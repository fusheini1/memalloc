// MemAlloc — Custom Multithreaded Memory Allocator
// Author: Fusheini Abdul-Mumin <abdulmuminfusheini@gmail.com>
// License: MIT

#pragma once

#include <cstddef>

namespace memalloc {

// Fixed size classes used by the small-allocation path.
constexpr std::size_t SIZE_CLASSES[] = {16, 32, 64, 128, 256, 512, 1024};

// Largest request served by the size-class pools; larger requests go to the
// arena ("large path").
constexpr std::size_t MAX_SMALL = 1024;

constexpr std::size_t NUM_CLASSES = 7;

// Index of the smallest size class >= n. n == 0 maps to class 0.
// Precondition: n <= MAX_SMALL; returns NUM_CLASSES as a "not found"
// sentinel when the precondition is violated.
constexpr std::size_t size_class_index(std::size_t n) {
    if (n == 0) {
        return 0;
    }
    for (std::size_t i = 0; i < NUM_CLASSES; ++i) {
        if (SIZE_CLASSES[i] >= n) {
            return i;
        }
    }
    return NUM_CLASSES;
}

// Block size that satisfies a request of `n` bytes. n == 0 returns the
// smallest class (16); n > MAX_SMALL returns 0 as a sentinel meaning
// "use the large path".
constexpr std::size_t block_for(std::size_t n) {
    if (n == 0) {
        return SIZE_CLASSES[0];
    }
    if (n > MAX_SMALL) {
        return 0;
    }
    return SIZE_CLASSES[size_class_index(n)];
}

}  // namespace memalloc
