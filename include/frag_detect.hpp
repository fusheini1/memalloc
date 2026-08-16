// MemAlloc — Custom Multithreaded Memory Allocator
// Author: Fusheini Abdul-Mumin <abdulmuminfusheini@gmail.com>
// License: MIT

#pragma once

#include "size_class.hpp"

#include <cstddef>

namespace memalloc {

// Per-allocation WORST-CASE internal fragmentation of allocating a T: the
// size class T rounds up to, minus sizeof(T), as a fraction of that class.
//
// THE MATH: because the size classes double (16, 32, 64, ...), a request
// just past class/2 + 1 rounds up to the next class and wastes almost half
// the block. Worst case approaches 50% per allocation:
//   worst fragmentation = (class - (class/2 + 1)) / class  ->  1/2
// A request of exactly class/2 fits the lower class exactly (0% waste);
// class/2 + 1 does not, so the waste jumps discontinuously toward 50%.
// A type smaller than the smallest class (16) can waste even more: a 1-byte
// char in a 16-byte block wastes 15/16 = 93.75%.
//
// Note this is the per-ALLOCATION worst case; a runtime workload that
// spreads requests across classes generally fragments less because the
// rounding waste averages out.
template <typename T>
constexpr double compile_time_fragmentation() {
    constexpr std::size_t cls = block_for(sizeof(T));
    return cls == 0 ? 0.0
                    : static_cast<double>(cls - sizeof(T)) /
                          static_cast<double>(cls);
}

// True iff every type in Ts has per-allocation fragmentation below
// `threshold`.
template <typename... Ts>
constexpr bool all_below(double threshold) {
    return ((compile_time_fragmentation<Ts>() < threshold) && ...);
}

// Compile-time tests: the very act of compiling these asserts IS the test.
// Actual values: int 4->16 = 0.75, double 8->16 = 0.50, char 1->16 = 0.9375.
// (The thresholds below are chosen to hold for those values; the negative
// assert proves the check really fires on the 0.94-wasteful char.)
static_assert(compile_time_fragmentation<int>() < 0.80,
              "int frag too high");
static_assert(compile_time_fragmentation<double>() < 0.60,
              "double frag too high");
static_assert(all_below<int, double, char>(0.95),
              "type list has high-frag member");
static_assert(!all_below<int, double, char>(0.80),
              "char (frag ~0.94) must trip a strict threshold");

}  // namespace memalloc
