// MemAlloc — Custom Multithreaded Memory Allocator
// Author: Fusheini Abdul-Mumin <abdulmuminfusheini@gmail.com>
// License: MIT

// Fragmentation report: prints the compile-time per-allocation worst-case
// fragmentation of representative types (computed entirely at compile time),
// then runs a mixed workload through the per-thread allocator and prints the
// aggregated runtime stats via memalloc::global_stats().

#include "frag_detect.hpp"
#include "size_class.hpp"
#include "stats.hpp"
#include "thread_pool_alloc.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <string>
#include <thread>

namespace memalloc {
void init_allocator();
}

namespace {

// One row of the compile-time table. The name is the stringized type name.
struct FragRow {
    const char* name;
    std::size_t size;
    double frag;
};

template <typename T>
constexpr FragRow frag_row(const char* name) {
    return {name, sizeof(T), memalloc::compile_time_fragmentation<T>()};
}

#define FRAG_ROW(T) frag_row<T>(#T)

// Workload: a bounded large-allocation phase plus a pool-size churn phase.
// Keeps a few pool allocations held (every 8th) so the peak counter is
// non-trivial; frees the rest immediately. Returns the total allocation
// count.
//
// The arena has no per-object free, so large blocks accumulate until the
// owning thread dies. 200 x ~5 KB is ~1 MB — comfortably inside the 4 MiB
// arena — so the large phase never exhausts it.
std::size_t run_workload(std::size_t iterations) {
    memalloc::ThreadPoolAlloc& alloc = memalloc::ThreadPoolAlloc::instance();

    constexpr std::size_t kArenaAllocs = 200;
    std::array<void*, kArenaAllocs> arena_blocks{};
    std::size_t arena_count = 0;
    for (std::size_t i = 0; i < kArenaAllocs; ++i) {
        void* p = alloc.allocate(5000);
        if (p == nullptr) {
            std::fprintf(stderr, "frag_report: arena exhausted\n");
            break;
        }
        static_cast<char*>(p)[0] = 'A';  // touch so it is resident
        arena_blocks[arena_count++] = p;
    }

    constexpr std::size_t kSizes[] = {17, 65, 129, 500, 900};
    constexpr std::size_t kNumSizes = sizeof(kSizes) / sizeof(kSizes[0]);
    std::array<void*, 32> held{};
    std::size_t held_count = 0;
    for (std::size_t i = 0; i < iterations; ++i) {
        const std::size_t size = kSizes[i % kNumSizes];
        void* p = alloc.allocate(size);
        if (p == nullptr) {
            std::fprintf(stderr, "frag_report: allocation of %zu failed\n",
                         size);
            break;
        }
        static_cast<char*>(p)[0] = static_cast<char>(i);  // touch
        // Keep roughly every 8th allocation held (bounded); free the rest.
        if (i % 8 == 0 && held_count < held.size()) {
            held[held_count++] = p;
        } else {
            alloc.deallocate(p, size);
        }
    }
    // Release the held pool blocks. Their allocation sizes were recycled, so
    // the exact size is no longer known; classify() recovers the containing
    // block and the size class. Arena blocks (arena_blocks) are left live by
    // design — the arena has no per-object free.
    for (std::size_t i = 0; i < held_count; ++i) {
        const std::size_t cls = alloc.classify(held[i]);
        if (cls != 0 && cls != memalloc::SingleThreadAllocator::kArenaPointer) {
            alloc.deallocate_containing(held[i], cls);
        }
    }
    (void)arena_blocks;  // kept live until the thread dies
    return iterations + arena_count;
}

}  // namespace

int main() {
    memalloc::init_allocator();

    std::printf("=== compile-time per-allocation fragmentation ===\n");
    constexpr FragRow rows[] = {
        FRAG_ROW(char),      FRAG_ROW(short),    FRAG_ROW(int),
        FRAG_ROW(long long), FRAG_ROW(float),    FRAG_ROW(double),
        FRAG_ROW(void*),     FRAG_ROW(std::string)};
    for (const FragRow& r : rows) {
        const std::size_t cls = memalloc::block_for(r.size);
        std::printf("  %-12s sizeof=%2zu  class=%3zu  frag=%.4f (%.2f%%)\n",
                    r.name, r.size, cls, r.frag, r.frag * 100.0);
    }

    std::printf("\n=== runtime workload ===\n");
    run_workload(100000);

    // Two short-lived threads that allocate and exit: their Stats are
    // retired (snapshotted) when their allocators die and are summed into
    // global_stats() by the aggregation below.
    std::thread t1([] { run_workload(50000); });
    std::thread t2([] { run_workload(50000); });
    t1.join();
    t2.join();

    const memalloc::Stats total = memalloc::global_stats();
    std::printf("\n=== runtime global stats (all threads) ===\n");
    total.print("global_stats");
    std::printf("frag_report OK\n");
    return 0;
}

#undef FRAG_ROW
