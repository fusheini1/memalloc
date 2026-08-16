// MemAlloc — Custom Multithreaded Memory Allocator
// Author: Fusheini Abdul-Mumin <abdulmuminfusheini@gmail.com>
// License: MIT

// Fragmentation metrics test.
//
// Runtime: allocate many UNIFORM small requests that all land in one size
// class and check the aggregated internal fragmentation stays within the
// expected bound (< 0.30; a 30-byte request in the 32-byte class wastes
// 2/32 = 6.25%, and uniformity means no spread across classes to inflate
// the ratio). Also verifies the alloc/free counters balance.
//
// Compile-time: including frag_detect.hpp compiles its static_asserts —
// the very act of compiling this TU IS the compile-time test.

#include "frag_detect.hpp"
#include "stats.hpp"
#include "thread_pool_alloc.hpp"

#include <array>
#include <cassert>
#include <cstdio>
#include <cstddef>

namespace memalloc {
// Defined in src/new_delete.cpp; routes C++ allocations through MemAlloc.
void init_allocator();
}

namespace {

constexpr std::size_t kCount = 2000;   // allocations of the same class
constexpr std::size_t kSize = 30;      // rounds up to the 32-byte class

}  // namespace

int main() {
    memalloc::init_allocator();

    memalloc::ThreadPoolAlloc& alloc = memalloc::ThreadPoolAlloc::instance();

    // Allocate many uniform 30-byte blocks (class 32). 2000 blocks exceed
    // the constructor slab (256), so the slow path (chunk refill) runs too.
    // The held array is a fixed-size stack buffer: a std::vector buffer
    // (16 KB) would itself ride the arena and pollute the exact counters
    // asserted below, so only the kCount uniform blocks are recorded.
    std::array<void*, kCount> held{};
    for (std::size_t i = 0; i < kCount; ++i) {
        void* p = alloc.allocate(kSize);
        assert(p != nullptr);
        static_cast<char*>(p)[0] = static_cast<char>(i);
        held[i] = p;
    }

    const memalloc::Stats& s = alloc.stats();
    // Uniform sizes within one class: the only waste is the 2 bytes of
    // rounding in each 32-byte block, so 0.0625 — far under the 0.30 bound.
    assert(s.internal_fragmentation() < 0.30);
    assert(s.alloc_count == kCount);
    assert(s.requested == kCount * kSize);
    assert(s.allocated == kCount * 32);
    assert(s.peak_allocated >= kCount * 32);  // everything was live at once
    const double frag_before_free = s.internal_fragmentation();

    for (void* p : held) {
        alloc.deallocate(p, kSize);
    }
    assert(s.free_count == s.alloc_count);
    assert(s.internal_fragmentation() == frag_before_free);  // lifetime ratio

    // The compile-time machinery is exercised (and must compile): the
    // static_asserts in frag_detect.hpp already fired-or-passed when this TU
    // was compiled. Spot-check the values here too.
    static_assert(memalloc::compile_time_fragmentation<char>() == 15.0 / 16.0,
                  "char 1->16 wastes 15/16");
    static_assert(memalloc::compile_time_fragmentation<std::size_t>() == 0.5,
                  "size_t 8->16 wastes 8/16");

    std::printf("frag OK  allocs=%zu frag=%.4f peak=%zu\n", s.alloc_count,
                frag_before_free, s.peak_allocated);
    return 0;
}
