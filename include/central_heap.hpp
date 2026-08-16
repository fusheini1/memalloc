// MemAlloc — Custom Multithreaded Memory Allocator
// Author: Fusheini Abdul-Mumin <abdulmuminfusheini@gmail.com>
// License: MIT

#pragma once

#include "block_pool.hpp"  // reuse detail::allocate_slab / free_slab
#include "viz_hook.hpp"     // slow-path chunk events for the visualizer

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <new>
#include <vector>

namespace memalloc {
namespace detail {

// True while the allocator itself is running, regardless of entry point
// (operator new OR the direct ThreadPoolAlloc API). Nested operator new
// calls triggered by the allocator's own bookkeeping — e.g. std::vector
// growth inside a locked CentralHeap chunk acquisition — must take the
// malloc fallback instead of re-entering the allocator, which would deadlock
// on the CentralHeap mutex.
//
// Declared here but DEFINED in src/thread_state.cpp: the thread_local lives
// in exactly one translation unit. Inline thread_local data (whether a
// namespace-scope variable or a static local of an inline function) is
// unreliable on MSVC when the header is included from several TUs, so the
// single definition lives in a dedicated state TU that every memalloc_core
// consumer links.
bool& allocator_active_flag();

// Sticky per-thread flags recorded when a thread's ThreadPoolAlloc is
// destroyed. The allocator object itself is gone afterwards, so exit-time
// operator delete calls (TLS vector destructors, static-destructor frees)
// must not call instance()/classify() on it anymore — they take the malloc
// fallback path instead. Defined in src/new_delete.cpp.
void mark_allocator_gone();
bool allocator_gone();

// RAII save/restore so nested entries restore the outer state.
class AllocatorGuard {
public:
    AllocatorGuard() : prev_(allocator_active_flag()) {
        allocator_active_flag() = true;
    }
    ~AllocatorGuard() { allocator_active_flag() = prev_; }
    AllocatorGuard(const AllocatorGuard&) = delete;
    AllocatorGuard& operator=(const AllocatorGuard&) = delete;

private:
    bool prev_;
};

}  // namespace detail

// CentralHeap — the shared, locked source of large chunks for the per-thread
// allocators. Chunks are never recycled while the program runs: every
// acquire_chunk() hands out a fresh 64 KiB region.
//
// Ownership: CentralHeap OWNS every chunk it creates and frees them all in
// its destructor (process exit). The pools that consume chunks only borrow
// them and never free them, which is what prevents use-after-free bugs.
class CentralHeap {
public:
    // One chunk = 64 KiB. A power of two — that is what makes the
    // address-masking in header_for() recover the chunk base exactly.
    static constexpr std::size_t CHUNK_SIZE = 65536;

    // The chunk header lives at the base; the body (where blocks are carved)
    // starts 64 bytes in so the first block keeps 64-byte alignment.
    static constexpr std::size_t kHeaderOffset = 64;

    // How many `block_size`-byte blocks fit in one chunk after the header
    // offset. External slabs must be capped at this: a full slab of the
    // largest classes (256 * 1 KiB = 256 KiB) would walk far past a 64 KiB
    // chunk and alias other threads' memory.
    static std::size_t blocks_per_chunk(std::size_t block_size) {
        return (CHUNK_SIZE - kHeaderOffset) / block_size;
    }

    struct ChunkHeader {
        std::size_t size_class;  // bytes per block in this chunk
        std::size_t block_size;  // identical to size_class; kept explicit
    };

    // Meyers singleton: thread-safe construction, one heap per process.
    static CentralHeap& instance() {
        static CentralHeap heap;
        return heap;
    }

    // Allocates a fresh chunk and returns a pointer to its BODY (base + 64),
    // which keeps 64-byte alignment for the blocks carved out of it.
    // The re-entrancy guard is set because chunks_.push_back() below may
    // grow the vector through operator new while the mutex is held; without
    // the guard that nested new would re-enter the allocator and deadlock.
    // `thread_id` is the acquiring thread's viz id (from the chunk-source
    // callback), used only to tag the kind=2 event in the visualizer feed.
    void* acquire_chunk(std::size_t size_class_bytes,
                        std::uint32_t thread_id = 0) {
        detail::AllocatorGuard guard;
        std::lock_guard<std::mutex> lock(mutex_);
        detail::SlabAlloc s = detail::allocate_slab(CHUNK_SIZE, CHUNK_SIZE);
        if (s.aligned == nullptr) {
            throw std::bad_alloc();
        }
        void* base = s.aligned;
        ChunkHeader* header = static_cast<ChunkHeader*>(base);
        header->size_class = size_class_bytes;
        header->block_size = size_class_bytes;
        chunks_.push_back(s.raw);  // the raw pointer we must free
        total_chunks_.fetch_add(1, std::memory_order_relaxed);
        void* body = static_cast<char*>(base) + kHeaderOffset;
        // Slow-path event for the visualizer (only when recording is on).
        if (memalloc::viz::enabled()) {
            memalloc::viz::ring().push(
                {memalloc::viz::now_ns(), CHUNK_SIZE,
                 reinterpret_cast<std::uintptr_t>(body), thread_id,
                 memalloc::viz::kEventChunk});
        }
        return body;
    }

    // Recover the header that owns a body pointer by masking off the low
    // bits: chunk bases are CHUNK_SIZE-aligned, so
    //   base = (uintptr_t)p & ~(CHUNK_SIZE - 1).
    const ChunkHeader* header_for(void* p) const {
        const std::uintptr_t base =
            reinterpret_cast<std::uintptr_t>(p) & ~(CHUNK_SIZE - 1);
        return reinterpret_cast<const ChunkHeader*>(base);
    }

    // Atomic counter — safe to read without holding the mutex.
    std::size_t total_chunks_acquired() const {
        return total_chunks_.load(std::memory_order_relaxed);
    }

    // Frees every chunk. Runs at process exit, after all threads have been
    // joined, so no pool references any chunk body anymore.
    ~CentralHeap() {
        for (void* raw : chunks_) {
            std::free(raw);
        }
    }

    CentralHeap(const CentralHeap&) = delete;
    CentralHeap& operator=(const CentralHeap&) = delete;

private:
    CentralHeap() = default;

    std::mutex mutex_;
    std::vector<void*> chunks_;  // raw malloc pointers to free at exit
    std::atomic<std::size_t> total_chunks_{0};
};

}  // namespace memalloc
