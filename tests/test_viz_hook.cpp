// MemAlloc — Custom Multithreaded Memory Allocator
// Author: Fusheini Abdul-Mumin <abdulmuminfusheini@gmail.com>
// License: MIT

// Verifies the Step 9.2 instrumentation layer: with the viz hook enabled,
// every allocator operation is recorded into the ring buffer with matching
// alloc/free flags and non-null pointers; occupancy() reflects live pool
// state; for_each_live() can enumerate the live allocators; and disabling
// the hook freezes the ring (the disabled path is a pure no-op).
//
// Links memalloc_core only — deliberately NOT the new/delete override — so
// the test drives ThreadPoolAlloc through its plain API and system malloc
// stays the baseline for the test's own machinery.

#include "stats.hpp"
#include "thread_pool_alloc.hpp"
#include "viz_hook.hpp"

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <vector>

int main() {
    memalloc::ThreadPoolAlloc& alloc = memalloc::ThreadPoolAlloc::instance();

    // --- Phase 1: recording enabled ---
    memalloc::viz::set_enabled(true);

    constexpr std::size_t kSizes[] = {16, 64, 256, 1024};
    constexpr std::size_t kIterations = 1000;
    std::vector<void*> held;
    held.reserve(128);  // system allocation: test links no override
    for (std::size_t i = 0; i < kIterations; ++i) {
        const std::size_t size = kSizes[i % 4];
        void* p = alloc.allocate(size);
        assert(p != nullptr);
        if (i % 8 == 0) {
            held.push_back(p);
        } else {
            alloc.deallocate(p, size);
        }
    }

    // Occupancy: while blocks are still held, at least one used class must
    // show free < cap, and every class must satisfy free <= cap.
    const memalloc::SingleThreadAllocator::Occupancy occ = alloc.occupancy();
    bool saw_live_class = false;
    for (std::size_t i = 0; i < memalloc::NUM_CLASSES; ++i) {
        assert(occ.free_count[i] <= occ.cap[i]);
        if (occ.cap[i] > 0 && occ.free_count[i] < occ.cap[i]) {
            saw_live_class = true;
        }
    }
    assert(saw_live_class);
    assert(occ.arena_cap > 0);

    // Release the held blocks through the classify path.
    for (void* p : held) {
        const std::size_t cls = alloc.classify(p);
        assert(cls != 0);
        alloc.deallocate_containing(p, cls);
    }
    held.clear();

    // Snapshot: events were recorded, every event carries a non-null block
    // pointer, and both alloc and free events exist.
    const std::vector<memalloc::viz::AllocEvent> events =
        memalloc::viz::ring().snapshot();
    assert(!events.empty());
    std::size_t allocs = 0, frees = 0;
    for (const memalloc::viz::AllocEvent& e : events) {
        assert(e.ptr != 0);
        assert(e.thread_id == alloc.thread_id());
        // This workload never exhausts a 256-block pool, so only plain
        // alloc/free events may be recorded (no chunk events).
        assert(e.kind == memalloc::viz::kEventAlloc ||
               e.kind == memalloc::viz::kEventFree);
        if (e.kind == memalloc::viz::kEventAlloc) {
            ++allocs;
        } else {
            ++frees;
        }
    }
    assert(allocs > 0 && frees > 0);

    // for_each_live() must enumerate this thread's allocator.
    std::size_t live_count = 0;
    memalloc::for_each_live(
        [](const memalloc::ThreadPoolAlloc& a, void* user) {
            ++(*static_cast<std::size_t*>(user));
            (void)a;
        },
        &live_count);
    assert(live_count >= 1);

    // --- Phase 2: disabled path is a pure no-op ---
    memalloc::viz::set_enabled(false);
    const std::size_t before = memalloc::viz::ring().count();
    for (std::size_t i = 0; i < kIterations; ++i) {
        const std::size_t size = kSizes[i % 4];
        void* p = alloc.allocate(size);
        assert(p != nullptr);
        alloc.deallocate(p, size);
    }
    const std::size_t after = memalloc::viz::ring().count();
    assert(after == before);  // disabled: nothing new recorded

    std::printf("viz_hook OK  events=%zu allocs=%zu frees=%zu live_threads=%zu\n",
                events.size(), allocs, frees, live_count);
    return 0;
}
