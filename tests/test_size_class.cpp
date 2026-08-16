// MemAlloc — Custom Multithreaded Memory Allocator
// Author: Fusheini Abdul-Mumin <abdulmuminfusheini@gmail.com>
// License: MIT

#include "single_thread_alloc.hpp"
#include "size_class.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace memalloc {
// Defined in src/new_delete.cpp; routes C++ allocations through MemAlloc.
void init_allocator();
}

using memalloc::block_for;
using memalloc::size_class_index;

// --- Compile-time verification of the size-class math (constexpr). ---

// block_for: smallest class >= n, 0 for the large path, 16 for n == 0.
static_assert(block_for(0) == 16, "0 -> smallest class");
static_assert(block_for(1) == 16, "1 -> 16");
static_assert(block_for(16) == 16, "16 -> 16");
static_assert(block_for(17) == 32, "17 -> 32");
static_assert(block_for(32) == 32, "32 -> 32");
static_assert(block_for(33) == 64, "33 -> 64");
static_assert(block_for(100) == 128, "100 -> 128");
static_assert(block_for(128) == 128, "128 -> 128");
static_assert(block_for(129) == 256, "129 -> 256");
static_assert(block_for(255) == 256, "255 -> 256");
static_assert(block_for(257) == 512, "257 -> 512");
static_assert(block_for(513) == 1024, "513 -> 1024");
static_assert(block_for(1024) == 1024, "1024 -> 1024");
static_assert(block_for(1025) == 0, "1025 -> 0 (large path)");
static_assert(block_for(1024 * 1024) == 0, "1 MiB -> 0 (large path)");

// size_class_index: index into SIZE_CLASSES.
static_assert(size_class_index(0) == 0, "0 -> index 0");
static_assert(size_class_index(1) == 0, "1 -> index 0 (16)");
static_assert(size_class_index(17) == 1, "17 -> index 1 (32)");
static_assert(size_class_index(33) == 2, "33 -> index 2 (64)");
static_assert(size_class_index(100) == 3, "100 -> index 3 (128)");
static_assert(size_class_index(200) == 4, "200 -> index 4 (256)");
static_assert(size_class_index(400) == 5, "400 -> index 5 (512)");
static_assert(size_class_index(800) == 6, "800 -> index 6 (1024)");
static_assert(size_class_index(1024) == 6, "1024 -> index 6 (1024)");

int main() {
    memalloc::init_allocator();  // warm the heap before any C++ allocation

    memalloc::SingleThreadAllocator alloc;

    // Small allocations from several different size classes.
    void* a = alloc.allocate(10);    // class 16
    void* b = alloc.allocate(50);    // class 64
    void* c = alloc.allocate(100);   // class 128
    void* d = alloc.allocate(500);   // class 512
    assert(a != nullptr && b != nullptr && c != nullptr && d != nullptr);

    // Blocks are aligned to at least their own size class.
    assert(reinterpret_cast<std::uintptr_t>(a) % 16 == 0);
    assert(reinterpret_cast<std::uintptr_t>(b) % 64 == 0);
    assert(reinterpret_cast<std::uintptr_t>(c) % 128 == 0);
    assert(reinterpret_cast<std::uintptr_t>(d) % 512 == 0);

    // Large request (> MAX_SMALL) is served by the Arena.
    void* big = alloc.allocate(2000);
    assert(big != nullptr);

    // Correct pool routing: a freed block is reused by the SAME size class
    // (LIFO within a pool), so pointers must never cross pools.
    alloc.deallocate(a, 10);
    alloc.deallocate(b, 50);
    alloc.deallocate(c, 100);
    alloc.deallocate(d, 500);
    assert(alloc.allocate(10) == a);
    assert(alloc.allocate(50) == b);
    assert(alloc.allocate(100) == c);
    assert(alloc.allocate(500) == d);

    // Deallocating a large block is a legal no-op (Arena has no free).
    alloc.deallocate(big, 2000);

    // A zero-size request lands in the smallest class.
    void* z = alloc.allocate(0);
    assert(z != nullptr);
    assert(reinterpret_cast<std::uintptr_t>(z) % 16 == 0);
    alloc.deallocate(z, 0);

    printf("size_class OK\n");
    return 0;
}
