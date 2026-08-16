// MemAlloc — Custom Multithreaded Memory Allocator
// Author: Fusheini Abdul-Mumin <abdulmuminfusheini@gmail.com>
// License: MIT

// Global operator new/delete overrides that route every C++ allocation
// through the per-thread allocator. Linked into every test executable; tests
// opt into the warm heap by calling memalloc::init_allocator() in main.
//
// Deallocation contract: free on the owning thread. Cross-thread frees of
// pool blocks are intentionally deferred/leaked (never corrupted) in this
// version; a production design would add a central transfer cache
// (tcmalloc-style). Pre-init (bootstrap) allocations use malloc and are
// released via the fallback registry.

#include "single_thread_alloc.hpp"
#include "thread_pool_alloc.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <utility>
#include <vector>

#ifdef _DEBUG
#include <crtdbg.h>
#endif

namespace {

// Set true by memalloc::init_allocator() at the start of main. Before that,
// static initialization is still running and the thread-local allocator may
// not exist yet, so every operator new falls back to std::malloc; such
// bootstrap pointers are later recognized by the ownership check in
// do_delete() and released with std::free.
std::atomic<bool> g_ready{false};

// Re-entrancy guard lives in the headers (detail::g_allocator_active) so it
// covers BOTH entry points: this operator new AND the direct
// ThreadPoolAlloc API used by the raw-allocator tests. While the allocator
// is working (thread-local construction, CentralHeap chunk bookkeeping, pool
// slab vectors), nested operator new calls take the malloc path instead of
// re-entering.

// Belt-and-braces record of pre-ready / in-recursion OVER-aligned pointers,
// whose aligned payload is not the raw malloc pointer and so cannot be
// std::free'd directly. This path exists only for static-initialization
// corner cases; the test never hits it. {raw malloc pointer, payload}.
thread_local std::vector<std::pair<void*, void*>> g_aligned_scratch;

// Every pointer our malloc FALLBACK path returns (bootstrap before g_ready,
// or nested new while the allocator is re-entered) is recorded here. These
// are the only pointers the global delete override may legitimately
// std::free when it cannot classify them: a "not ours" pointer that is NOT
// in this registry belongs to another thread's per-thread pools, and freeing
// it would corrupt the heap (cross-thread deallocation is out of scope for
// the per-thread design, so such deletes are safely ignored and the block
// leaks).
//
// An insert-only lock-free linked list: nodes are obtained with raw
// std::malloc (never operator new), so registering cannot recurse into this
// allocator. Entries are never removed; a pointer can only be freed once, so
// stale membership is harmless.
struct MallocRec {
    void* ptr;
    MallocRec* next;
};
std::atomic<MallocRec*> g_malloc_recs{nullptr};

void record_malloc_ptr(void* p) {
    MallocRec* n = static_cast<MallocRec*>(std::malloc(sizeof(MallocRec)));
    if (n == nullptr) {
        return;  // registration is best-effort; worst case the pointer leaks
    }
    n->ptr = p;
    n->next = g_malloc_recs.load(std::memory_order_relaxed);
    g_malloc_recs.store(n, std::memory_order_relaxed);
}

bool is_registered_malloc_ptr(void* p) {
    for (MallocRec* n = g_malloc_recs.load(std::memory_order_relaxed);
         n != nullptr; n = n->next) {
        if (n->ptr == p) {
            return true;
        }
    }
    return false;
}

// Belt-and-braces around every std::free in do_delete. In Debug builds the
// CRT validates the pointer against its heap (debug_heap.cpp,
// _CrtIsValidHeapPointer) and asserts if it is not a heap block — a hard
// "Debug Assertion Failed!" dialog. The registry gates above already make
// that impossible for pointers that came from our malloc fallback; this
// second gate makes it structurally impossible even if a misrouted pointer
// (pool block, foreign memory) ever reaches a free: it leaks with a
// one-time diagnostic instead of corrupting the heap. Leaking is exactly
// what the deallocation contract prescribes for unclassifiable pointers.
void safe_free(void* p) {
#ifdef _DEBUG
    if (p != nullptr && !_CrtIsValidHeapPointer(p)) {
        static std::atomic<bool> reported{false};
        bool expected = false;
        if (reported.compare_exchange_strong(expected, true)) {
            std::fprintf(stderr,
                         "MemAlloc: refusing to std::free non-heap pointer "
                         "%p (leaked per deallocation contract)\n",
                         p);
        }
        return;
    }
#endif
    std::free(p);
}

// Over-aligned allocation before the heap is ready (or during allocator
// re-entrancy): malloc + manual alignment, stashing the raw pointer so the
// matching delete can free the right address.
void* bootstrap_aligned_new(std::size_t size, std::size_t alignment) {
    void* raw = std::malloc(size + alignment);
    if (raw == nullptr) {
        throw std::bad_alloc();
    }
    const std::uintptr_t addr =
        (reinterpret_cast<std::uintptr_t>(raw) + alignment - 1) &
        ~static_cast<std::uintptr_t>(alignment - 1);
    void* payload = reinterpret_cast<void*>(addr);
    g_aligned_scratch.push_back({raw, payload});
    return payload;
}

void* do_new(std::size_t size) {
    if (size == 0) {
        size = 1;  // the standard requires a distinct non-null pointer
    }
    if (!g_ready.load(std::memory_order_acquire) ||
        memalloc::detail::allocator_active_flag() ||
        memalloc::detail::allocator_gone()) {
        void* p = std::malloc(size);
        record_malloc_ptr(p);
        return p;
    }
    // Guard the WHOLE entry, not just the pool allocation: the first call on
    // a fresh thread constructs the thread_local ThreadPoolAlloc, and in
    // Debug builds constructing the allocator's own STL containers
    // allocates _Container_proxy objects through this very operator new.
    // Without the guard that nested new would re-enter instance() while the
    // thread_local is still being constructed and recurse forever.
    memalloc::detail::AllocatorGuard guard;
    void* p = memalloc::ThreadPoolAlloc::instance().allocate(size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

void* do_aligned_new(std::size_t size, std::size_t alignment) {
    if (size == 0) {
        size = 1;
    }
    // Alignment at or below the default is satisfied by the normal path:
    // every pool block is at least 16-byte aligned (constructor slabs are
    // class-aligned, chunk blocks 64).
    if (alignment <= alignof(std::max_align_t)) {
        return do_new(size);
    }
    if (!g_ready.load(std::memory_order_acquire) ||
        memalloc::detail::allocator_active_flag() ||
        memalloc::detail::allocator_gone()) {
        return bootstrap_aligned_new(size, alignment);
    }
    // Same construction-time re-entrancy protection as do_new: the aligned
    // path may run during thread_local allocator construction.
    memalloc::detail::AllocatorGuard guard;
    void* p =
        memalloc::ThreadPoolAlloc::instance().allocate_aligned(size, alignment);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

// Uniform delete path. The pointer's origin decides the action — the size
// the compiler passes (sized forms) is deliberately ignored because
// classify() is authoritative and also handles pointers whose size is not
// known (unsized delete[] and bootstrap pointers).
void do_delete(void* p) noexcept {
    if (p == nullptr) {
        return;
    }
    // Stashed over-aligned bootstrap pointers first (payload != raw).
    {
        auto& stash = g_aligned_scratch;
        for (auto it = stash.begin(); it != stash.end(); ++it) {
            if (it->second == p) {
                safe_free(it->first);
                stash.erase(it);
                return;
            }
        }
    }
    if (!g_ready.load(std::memory_order_acquire)) {
        // Pre-main delete of a bootstrap pointer: every allocation before
        // the warm-up flag took the malloc fallback and was registered.
        if (is_registered_malloc_ptr(p)) {
            safe_free(p);
        }
        return;
    }
    if (memalloc::detail::allocator_gone()) {
        // This thread's allocator has already been destroyed (exit-time
        // teardown of TLS vectors, static destructors, CRT cleanup): its
        // pools/arena no longer exist, so the pointer can only be a malloc
        // fallback pointer (registered) or foreign memory to leave alone.
        if (is_registered_malloc_ptr(p)) {
            safe_free(p);
        }
        return;
    }
    // Guard the whole delete too: the first delete on a fresh thread (e.g.
    // the std::thread callable freed at thread exit) constructs the
    // thread_local allocator, whose Debug _Container_proxy allocations must
    // take the malloc fallback instead of re-entering instance().
    memalloc::detail::AllocatorGuard guard;
    memalloc::ThreadPoolAlloc& alloc = memalloc::ThreadPoolAlloc::instance();
    if (alloc.is_dying()) {
        // Thread-local teardown: only the allocator's own malloc-backed
        // bookkeeping buffers reach this path (they were allocated through
        // the re-entrancy fallback and are in the registry). Anything else
        // — a pool block, foreign memory — must NOT be std::free'd: that
        // would corrupt the CRT heap (_CrtIsValidHeapPointer).
        if (is_registered_malloc_ptr(p)) {
            safe_free(p);
        }
        return;
    }
    const std::size_t cls = alloc.classify(p);
    if (cls == 0) {
        // Not owned by THIS thread's allocator. Only pointers that came from
        // our malloc fallback (bootstrap / re-entrancy) may be std::free'd;
        // anything else is a pool block owned by another thread, which the
        // per-thread free list cannot safely accept — so it is leaked rather
        // than corrupting the heap.
        if (is_registered_malloc_ptr(p)) {
            safe_free(p);
        }
        return;
    }
    if (cls == memalloc::SingleThreadAllocator::kArenaPointer) {
        return;  // Arena has no per-object free
    }
    // deallocate_containing resolves the block start itself: `p` may be
    // offset from it by the delete[] array-count cookie.
    alloc.deallocate_containing(p, cls);
}

}  // namespace

// --- Plain forms -----------------------------------------------------------

void* operator new(std::size_t size) { return do_new(size); }
void* operator new[](std::size_t size) { return do_new(size); }
void operator delete(void* p) noexcept { do_delete(p); }
void operator delete[](void* p) noexcept { do_delete(p); }
void operator delete(void* p, std::size_t) noexcept { do_delete(p); }
void operator delete[](void* p, std::size_t) noexcept { do_delete(p); }

// --- Aligned forms (C++17) -------------------------------------------------

void* operator new(std::size_t size, std::align_val_t alignment) {
    return do_aligned_new(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return do_aligned_new(size, static_cast<std::size_t>(alignment));
}
void operator delete(void* p, std::align_val_t) noexcept { do_delete(p); }
void operator delete[](void* p, std::align_val_t) noexcept { do_delete(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
    do_delete(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
    do_delete(p);
}

namespace memalloc {

// Warms the heap and flips the bootstrap flag. Call at the very start of
// main(), before any user allocation. Constructing the allocator eagerly
// guarantees it exists before the flag turns on, and nothing in its
// construction recurses into operator new (pools and arena are malloc-only).
void init_allocator() {
    (void)ThreadPoolAlloc::instance();
    g_ready.store(true, std::memory_order_release);
}

}  // namespace memalloc
