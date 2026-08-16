// MemAlloc — Custom Multithreaded Memory Allocator
// Author: Fusheini Abdul-Mumin <abdulmuminfusheini@gmail.com>
// License: MIT

#pragma once

#include "central_heap.hpp"
#include "single_thread_alloc.hpp"
#include "stats.hpp"
#include "viz_hook.hpp"

#include <atomic>
#include <cstddef>

namespace memalloc {

// Per-thread allocator front-end. Every thread gets its own
// SingleThreadAllocator via thread_local storage; when one of its pools runs
// dry, the chunk-source callback refills it from the shared CentralHeap.
//
// Deallocation contract: free on the owning thread. Cross-thread frees of
// pool blocks are intentionally deferred/leaked (never corrupted) in this
// version; a production design would add a central transfer cache
// (tcmalloc-style). Pre-init (bootstrap) allocations use malloc and are
// released via the fallback registry in new_delete.cpp.
//
// False-sharing prevention: the class is over-aligned to a full 64-byte
// cache line, so each thread's hot allocator state lives on a separate cache
// line and cores never ping-pong allocator metadata.
class alignas(64) ThreadPoolAlloc {
public:
    static ThreadPoolAlloc& instance() {
        // The guard must be active while the thread_local below is being
        // CONSTRUCTED: in Debug builds, constructing this object's STL
        // members (the pools' vectors, etc.) allocates _Container_proxy
        // objects through the overridden operator new. Without a guard set
        // before the declaration, that nested new would re-enter instance()
        // mid-construction and recurse forever. The guard is a plain stack
        // object, so it only covers the construction and this call.
        detail::AllocatorGuard guard;
        thread_local ThreadPoolAlloc t;
        return t;
    }

    // Set at the very start of the destructor, BEFORE any member is torn
    // down. While a thread's allocator is dying, the only operator delete
    // calls that can still occur are for the allocator's own bookkeeping
    // buffers (std::malloc-backed vectors), so the global delete override
    // must std::free them instead of classifying them against the dying
    // pools. Also keeps the main thread's teardown at process exit safe.
    bool is_dying() const { return dying_.load(std::memory_order_relaxed); }

    void* allocate(std::size_t n) {
        ++alloc_count_;  // plain counter: only the owning thread touches it
        // Guard the ENTIRE entry: pool refill links the chunk and grows the
        // pool's external-slab vector (operator new under the hood); without
        // the guard that nested new would re-enter the allocator (recursion
        // / deadlock) and, worse, its buffer would be a pool block that
        // teardown would wrongly std::free.
        detail::AllocatorGuard guard;
        void* p = alloc_.allocate(n);
        // Viz instrumentation (Step 9.2): one relaxed atomic load when
        // disabled, so the default build pays ~nothing. Only successful
        // allocations are recorded.
        if (memalloc::viz::enabled() && p != nullptr) {
            memalloc::viz::ring().push(
                {memalloc::viz::now_ns(), n,
                 reinterpret_cast<std::uintptr_t>(p), thread_id_,
                 memalloc::viz::kEventAlloc});
        }
        return p;
    }

    // Over-aligned requests ride the Arena (no per-object free).
    void* allocate_aligned(std::size_t n, std::size_t alignment) {
        detail::AllocatorGuard guard;
        return alloc_.allocate_aligned(n, alignment);
    }

    void deallocate(void* p, std::size_t n) {
        ++free_count_;  // ditto — no synchronization needed
        alloc_.deallocate(p, n);
        if (memalloc::viz::enabled() && p != nullptr) {
            memalloc::viz::ring().push(
                {memalloc::viz::now_ns(), n,
                 reinterpret_cast<std::uintptr_t>(p), thread_id_,
                 memalloc::viz::kEventFree});
        }
    }

    // Frees the block containing `p` (handles delete[] cookie offsets).
    void deallocate_containing(void* p, std::size_t n) {
        ++free_count_;
        alloc_.deallocate_containing(p, n);
        if (memalloc::viz::enabled() && p != nullptr) {
            memalloc::viz::ring().push(
                {memalloc::viz::now_ns(), n,
                 reinterpret_cast<std::uintptr_t>(p), thread_id_,
                 memalloc::viz::kEventFree});
        }
    }

    // Ownership classification used by the global new/delete override
    // (0 = not ours, kArenaPointer = arena-owned, else the size class).
    std::size_t classify(void* p) const { return alloc_.classify(p); }

    // Read-only accessors; the counters are aggregated globally in Step 6.
    std::size_t alloc_count() const { return alloc_count_; }
    std::size_t free_count() const { return free_count_; }

    // This thread's allocation statistics (the live entry in the global
    // registry; retired as a snapshot when this allocator is destroyed).
    const Stats& stats() const { return alloc_.stats(); }

    // Per-class pool occupancy snapshot, forwarded from the underlying
    // SingleThreadAllocator (for the visualizer).
    using Occupancy = SingleThreadAllocator::Occupancy;
    Occupancy occupancy() const { return alloc_.occupancy(); }

    // Stable small id for the visualizer, unique per live thread.
    std::uint32_t thread_id() const { return thread_id_; }

private:
    // The `user` pointer carries this thread's viz id (set in the ctor) so
    // the slow-path chunk event is tagged with the acquiring thread.
    static void* acquire_chunk_cb(std::size_t size_class_bytes, void* user) {
        const std::uint32_t tid = static_cast<std::uint32_t>(
            reinterpret_cast<std::uintptr_t>(user));
        return CentralHeap::instance().acquire_chunk(size_class_bytes, tid);
    }

    ThreadPoolAlloc()
        : thread_id_(memalloc::viz::next_thread_id()),
          alloc_(acquire_chunk_cb,
                 reinterpret_cast<void*>(
                     static_cast<std::uintptr_t>(thread_id_))) {
        // Registered while the construction-time guard (set by instance())
        // is active, so the registry's vector growth takes the malloc
        // fallback instead of recursing into this very allocator.
        detail::register_stats(&alloc_.stats(), this);
    }

    ~ThreadPoolAlloc() {
        dying_.store(true, std::memory_order_relaxed);
        // Sticky per-thread flag: after this destructor (and member
        // teardown) the allocator object no longer exists, but exit-time
        // operator delete calls (TLS vector destructors, static-destructor
        // frees) still arrive on this thread. They must take the malloc
        // fallback path instead of calling instance()/classify() on the
        // destroyed object.
        detail::mark_allocator_gone();
        // Teardown publishing: snapshot this thread's Stats into the global
        // retired list (the Stats object dies with alloc_ below). The guard
        // makes the registry's vector growth use the malloc fallback — no
        // recursion into the allocator mid-destruction.
        detail::AllocatorGuard guard;
        detail::retire_stats(&alloc_.stats());
    }

    // Declared BEFORE alloc_ so it outlives it: member destruction runs in
    // reverse declaration order, and the deletes during alloc_'s teardown
    // (vector buffers) must still observe the flag as true.
    std::atomic<bool> dying_{false};
    // Declared before alloc_ because the ctor passes this thread's viz id
    // to the chunk source (via the `user` pointer).
    std::uint32_t thread_id_ = 0;  // viz id, assigned once in the ctor
    SingleThreadAllocator alloc_;
    std::size_t alloc_count_ = 0;
    std::size_t free_count_ = 0;
};

// Each thread's hot allocator state must occupy its own cache line so cores
// never ping-pong allocator metadata (prevents false sharing).
static_assert(alignof(ThreadPoolAlloc) >= 64,
              "per-thread allocator state must occupy its own cache line");

}  // namespace memalloc
