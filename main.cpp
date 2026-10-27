#include <algorithm>
#include <barrier>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <atomic>
#include <vector>
#include <vector>

#include "lock_free_vector.h"
#include <cstddef>
#include <cstdint>
#include <utility>
#include <atomic>
#include <thread>
#include <cstdlib>
#include <memory>
#include <vector>

using usize = size_t;

int main() {
    LockFreeArray<usize> l(1024);

    std::vector<usize> data(10, 1);
    l.Write(10, 20, data);
    assert(l.Read(15) == 1);
}
