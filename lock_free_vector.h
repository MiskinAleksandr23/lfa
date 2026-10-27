#pragma once
#include <vector>
#include <atomic>
#include <barrier>


namespace multicas {
    struct McasDescriptor;

    bool mcas(McasDescriptor *desc);

    struct WordDescriptor {
        std::atomic<uintptr_t> *address;
        uintptr_t old;
        uintptr_t new_;
        McasDescriptor *parent;

        static_assert(std::pointer_traits<decltype(address)>::element_type::is_always_lock_free);
    };

    enum StatusType { ACTIVE, SUCCESSFUL, FAILED };

    struct McasDescriptor {
        std::atomic<StatusType> status;
        // NOTE: It's suggested to order the word descriptors by address.  Then,
        //   should two threads try to mutate the same words, one will always
        //   succeed, potentially avoiding repeated collisions.
        std::vector<WordDescriptor> words;

        static_assert(decltype(status)::is_always_lock_free);
    };

    // for placing descriptor sentinel in unused address bits-- but see comments below
    const uintptr_t DESCRIPTOR_MASK = 0xFFFF000000000000;
    const uintptr_t DESCRIPTOR_SENTINEL = 0xDCBA000000000000;

    // (The approach of in-band descriptors seems naive.  Assuming CAS locations
    // can contain non-pointer values, even if a value exactly matches an active
    // descriptor, we don't know it's not by chance.  While mcas() can verify that
    // expected and new values don't look like a descriptor, coordination between
    // this library and the application is necessary to avoid conflicts.)
    inline bool isDescriptorValue(uintptr_t val) {
        return (val & DESCRIPTOR_MASK) == DESCRIPTOR_SENTINEL;
    }

    inline uintptr_t descriptorPtrToValue(const WordDescriptor *ptr) {
        auto value = reinterpret_cast<uintptr_t>(ptr);
        assert(!(value & DESCRIPTOR_MASK));
        return value | DESCRIPTOR_SENTINEL;
    }

    inline WordDescriptor *descriptorValueToPtr(uintptr_t value) {
        return reinterpret_cast<WordDescriptor *>(value & ~DESCRIPTOR_MASK);
    }

    std::pair<uintptr_t, uintptr_t>
    readInternal(const std::atomic<uintptr_t> *addr, McasDescriptor *self) {
        while (true) {
            auto content = addr->load();
            auto value = content;
            if (isDescriptorValue(content)) {
                auto desc = descriptorValueToPtr(content);
                auto parent = desc->parent;
                if (parent != self && parent->status == ACTIVE) {
                    mcas(parent);
                    continue;
                }
                value = parent->status == SUCCESSFUL ? desc->new_ : desc->old;
            }
            return std::make_pair(content, value);
        }
    }

    uintptr_t read(const std::atomic<uintptr_t> &addr) {
        return readInternal(&addr, nullptr).second;
    }

    bool mcas(McasDescriptor *desc) {
        auto status = SUCCESSFUL;
        for (const auto &wordDesc: desc->words) {
            assert(!isDescriptorValue(wordDesc.old));
            assert(!isDescriptorValue(wordDesc.new_));
            auto desired_desc = descriptorPtrToValue(&wordDesc);
        retry_word:
            auto result = readInternal(wordDesc.address, desc);
            auto current_desc = result.first;
            // if this word already points to the right place, move on
            if (current_desc == desired_desc) continue;
            // if the expected value is different, the MCAS fails
            if (result.second != wordDesc.old) {
                status = FAILED;
                break;
            }
            if (desc->status != ACTIVE) break;
            // try to install the pointer to my descriptor; if failed, retry
            if (!wordDesc.address->compare_exchange_weak(current_desc, desired_desc)) {
                goto retry_word;
            }
        }
        auto expected_status = ACTIVE;
        if (desc->status.compare_exchange_strong(expected_status, status)) {
            // if I finalized this descriptor, mark it for reclamation
            //retireForCleanup(desc);
        }
        return (desc->status == SUCCESSFUL);
    }

    bool cas2(std::atomic<uintptr_t> *a_addr, uintptr_t a_old, uintptr_t a_new,
              std::atomic<uintptr_t> *b_addr, uintptr_t b_old, uintptr_t b_new) {
        // TODO: sort entries by address
        auto desc = new McasDescriptor{
            .status = ACTIVE,
            .words = {
                {
                    .address = a_addr,
                    .old = a_old,
                    .new_ = a_new,
                },
                {
                    .address = b_addr,
                    .old = b_old,
                    .new_ = b_new,
                },
            },
        };
        for (auto &wordDesc: desc->words) {
            wordDesc.parent = desc;
        }
        return mcas(desc);
    }
}


// TODO: memory_orders and fences
template<typename Value, const int kBatchSize = 64>
struct LockFreeArray {
    using usize = size_t;
    struct Block;

    using SnapShotRange = std::vector<const Block *>;

    explicit LockFreeArray(usize size): size_(size),
                                        entries_(size / kBatchSize) {
        assert(size % kBatchSize == 0 && "Current implementation expects equal block sizes");

        for (usize idx = 0; idx < size / kBatchSize; ++idx) {
            entries_[idx].store(reinterpret_cast<uintptr_t>(Block::Allocate()));
        }
    }

    [[nodiscard]] usize BlocksCount() const {
        return entries_.size();
    }

    const Block *ReadBlock(usize index) const {
        return reinterpret_cast<const Block *>(multicas::read(entries_[index]));
    }

    auto ReadRange(usize left_entry, usize right_entry) -> SnapShotRange {
        assert(left_entry <= right_entry && right_entry <= BlocksCount());

        if (left_entry == right_entry) [[unlikely]] {
            return {};
        }

        const auto count = right_entry - left_entry;
        SnapShotRange snap(count, nullptr);
        // TODO: fix ABA
    repeat:
        for (usize idx = 0; idx < count; ++idx) {
            snap[idx] = ReadBlock(left_entry + idx);
        }

        for (usize idx = 0; idx < count; ++idx) {
            if (snap[idx] != ReadBlock(left_entry + idx)) {
                goto repeat;
            }
        }

        return snap;
    }

    auto Write(usize left, usize right, const std::vector<Value> &values) {
        assert(left <= right && right <= size_);
        assert(values.size() == right - left);

        if (left == right) return;

        const auto li = left / kBatchSize;
        const auto ri = (right + kBatchSize - 1) / kBatchSize;

        std::vector<Block *> blocks(ri - li, nullptr);

        std::vector<const Block *> expected(ri - li);
        for (auto &block: blocks) {
            block = Block::Allocate();
        }
        for (;;) {
            for (usize idx = 0; idx < ri - li; ++idx) {
                expected[idx] = reinterpret_cast<const Block *>(
                    multicas::read(entries_[li + idx])
                );

                blocks[idx]->Store(expected[idx]);
            }

            for (usize idx = 0; idx < values.size(); ++idx) {
                const auto position = left + idx;

                blocks[position / kBatchSize - li]
                        ->data_[position % kBatchSize] = values[idx];
            }

            if (WriteRange(li, ri, expected, blocks)) {
                return;
            }
        }
    }

    auto WriteRange(usize left, usize right, const std::vector<const Block *> &expected,
                    const std::vector<Block *> &values) -> bool {
        assert(left <= right && right <= BlocksCount());
        assert(values.size() == right - left);
        assert(expected.size() == right - left);

        if (left == right) {
            return true;
        }

        const auto count = right - left;

        auto *m = new multicas::McasDescriptor{
            .status = multicas::ACTIVE,
            .words = std::vector<multicas::WordDescriptor>(count)
        };

        for (usize idx = 0; idx < count; ++idx) {
            m->words[idx] = {
                .address = &entries_[left + idx],
                .old = reinterpret_cast<uintptr_t>(expected[idx]),
                .new_ = reinterpret_cast<uintptr_t>(values[idx]),
                .parent = m
            };
        }

        return multicas::mcas(m);
    }


    [[nodiscard]] Value Read(usize position) const {
        assert(position < size_);
        return ReadBlock(position / kBatchSize)->data_[position % kBatchSize];
    }

    auto Write(usize index, const Value &value) -> void {
        assert(index < size_);
        Write(index, index + 1, std::vector<Value>{value});
    }


    struct Block {
        Value data_[kBatchSize]{};

        Block() = default;

        static Block *Allocate() {
            return new Block();
        }

        void Store(const Block *previous) {
            for (usize idx = 0; idx < kBatchSize; ++idx) {
                data_[idx] = previous->data_[idx];
            }
        }
    };

private:
    usize size_;
    std::vector<std::atomic<uintptr_t> > entries_;
};
