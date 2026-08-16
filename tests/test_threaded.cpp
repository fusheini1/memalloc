// MemAlloc — Custom Multithreaded Memory Allocator
// Author: Fusheini Abdul-Mumin <abdulmuminfusheini@gmail.com>
// License: MIT

// Exercises the multithreaded core: 8 threads churn through the per-thread
// allocator, then hold pointers and publish them to a shared set to prove
// no two threads ever received the same block (duplicate = bug).
//
// Phase 2 lifetime note: the shared std::unordered_set's nodes and bucket
// arrays are carved from the WORKER threads' per-thread pools (every C++
// allocation in this test flows through the new/delete override). Those
// pools are freed when each worker exits, so the set must be destroyed while
// the workers are still alive — the test waits for every thread to finish
// inserting, destroys the set, and only then joins. This honors the
// deallocation contract (free on the owning thread) and avoids walking
// memory whose owning allocator has already died.

#include "central_heap.hpp"
#include "thread_pool_alloc.hpp"

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

namespace memalloc {
// Defined in src/new_delete.cpp; routes C++ allocations through MemAlloc.
void init_allocator();
}

namespace {

constexpr std::size_t kThreads = 8;
constexpr std::size_t kChurnIters = 100000;
constexpr std::size_t kHoldCount = 2000;
constexpr std::size_t kSizes[] = {16, 64, 256, 1024};

// Blocks from a pool's own slab are aligned to their size class; blocks from
// external chunks are only guaranteed 64-byte aligned (chunk base + 64).
// Assert the minimum that holds in both cases.
void check_aligned(void* p, std::size_t size) {
    const std::size_t need = (size >= 64) ? 64 : 16;
    assert(reinterpret_cast<std::uintptr_t>(p) % need == 0);
}

// Phase 1: rapid alloc/free churn across all size classes. Pairs never
// exhaust a pool, so this is the contention-free fast path.
void churn() {
    memalloc::ThreadPoolAlloc& alloc = memalloc::ThreadPoolAlloc::instance();
    for (std::size_t i = 0; i < kChurnIters; ++i) {
        const std::size_t size = kSizes[i % 4];
        void* p = alloc.allocate(size);
        assert(p != nullptr);
        check_aligned(p, size);
        alloc.deallocate(p, size);
    }
}

// Phase 2: hold kHoldCount mixed-size blocks, publish every pointer to the
// shared set (duplicate = bug), wait until all threads have finished
// inserting, then free our own blocks on this thread. Finally the worker
// parks on `release` until main has destroyed the shared set: the set's
// nodes/buckets were carved from this thread's pools, so the thread (and its
// allocator) must outlive the set. Only after main signals release may the
// worker return and let its thread_local allocator be torn down.
void hold_and_verify(std::unordered_set<void*>& seen, std::mutex& mutex,
                     std::atomic<int>& done, std::atomic<bool>& release) {
    memalloc::ThreadPoolAlloc& alloc = memalloc::ThreadPoolAlloc::instance();
    std::vector<void*> held;
    held.reserve(kHoldCount);
    for (std::size_t i = 0; i < kHoldCount; ++i) {
        const std::size_t size = kSizes[i % 4];
        void* p = alloc.allocate(size);
        assert(p != nullptr);
        check_aligned(p, size);
        held.push_back(p);
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (void* p : held) {
            assert(seen.insert(p).second);  // duplicate address = bug
        }
    }
    done.fetch_add(1, std::memory_order_release);
    while (done.load(std::memory_order_acquire) != static_cast<int>(kThreads)) {
        // spin until every thread has finished inserting
    }
    for (std::size_t i = 0; i < kHoldCount; ++i) {
        alloc.deallocate(held[i], kSizes[i % 4]);
    }
    while (!release.load(std::memory_order_acquire)) {
        // park: main is destroying the shared set; do not exit (and do not
        // let this thread's per-thread allocator die) until it is done
    }
}

}  // namespace

int main() {
    memalloc::init_allocator();  // warm the heap before any C++ allocation

    const auto start = std::chrono::steady_clock::now();

    // Phase 1: churn.
    {
        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (std::size_t t = 0; t < kThreads; ++t) {
            threads.emplace_back(churn);
        }
        for (auto& th : threads) {
            th.join();
        }
    }

    // Phase 2: uniqueness across threads. The shared set's nodes and bucket
    // arrays are carved from the WORKERS' per-thread pools, so the set must
    // be destroyed (end of its scope below) while every worker is still
    // alive — after all have inserted, but BEFORE any worker exits. Workers
    // park on `release` so their allocators survive until the set is gone.
    std::atomic<int> done{0};
    std::atomic<bool> release{false};
    std::mutex mutex;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    {
        std::unordered_set<void*> seen;
        for (std::size_t t = 0; t < kThreads; ++t) {
            threads.emplace_back(hold_and_verify, std::ref(seen), std::ref(mutex),
                                 std::ref(done), std::ref(release));
        }
        while (done.load(std::memory_order_acquire) !=
               static_cast<int>(kThreads)) {
            // spin until every worker has finished inserting
        }
        // `seen` is destroyed here while every worker is parked on release,
        // so the pools/arenas owning the set's memory are still alive.
    }
    release.store(true, std::memory_order_release);
    // Workers now free their held blocks (if not already) and exit; their
    // thread_local allocators are torn down only after the set is gone.
    for (auto& th : threads) {
        th.join();
    }

    // Directly verify the chunk masking technique used by header_for().
    void* chunk = memalloc::CentralHeap::instance().acquire_chunk(256);
    assert(chunk != nullptr);
    assert(reinterpret_cast<std::uintptr_t>(chunk) % 64 == 0);
    const memalloc::CentralHeap::ChunkHeader* header =
        memalloc::CentralHeap::instance().header_for(chunk);
    assert(header != nullptr);
    assert(header->size_class == 256);
    assert(header->block_size == 256);

    const std::size_t chunks =
        memalloc::CentralHeap::instance().total_chunks_acquired();
    assert(chunks > 0);  // proves the slow (chunk-acquiring) path ran

    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    printf("threaded OK  chunks=%zu  threads=%zu  time=%.2fs\n", chunks,
           static_cast<std::size_t>(kThreads), seconds);
    return 0;
}
