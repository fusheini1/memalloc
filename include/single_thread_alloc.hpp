// MemAlloc — Custom Multithreaded Memory Allocator
// Author: Fusheini Abdul-Mumin <abdulmuminfusheini@gmail.com>
// License: MIT

#pragma once

#include "arena.hpp"
#include "block_pool.hpp"
#include "central_heap.hpp"
#include "size_class.hpp"
#include "stats.hpp"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <tuple>
#include <utility>

namespace memalloc {

// SingleThreadAllocator — routes requests <= MAX_SMALL to one of the seven
// fixed-size BlockPools and larger requests to a backing Arena.
//
// BlockPool is a template, so the pools live in a std::tuple and are
// selected with std::get<Index>(pools_). Because the index arrives at
// runtime, dispatch goes through a compile-time table of member-function
// pointers built from std::index_sequence.
//
// An optional chunk source can refill an exhausted pool with a fresh slab
// from the shared CentralHeap; with no source (the default) an exhausted
// pool simply returns nullptr.
//
// Not thread-safe. Non-copyable and non-movable.
class SingleThreadAllocator {
public:
    static constexpr std::size_t kArenaCapacity = 4 * 1024 * 1024;  // 4 MiB

    // Supplies a fresh chunk that can be carved into BlocksPerSlab blocks
    // of `size_class_bytes`, or nullptr if none could be obtained.
    using ChunkSource = void* (*)(std::size_t size_class_bytes, void* user);

    // `src` is an optional chunk source; `user` is passed through to it.
    // With src == nullptr (the default) allocate() keeps Step 3 behavior:
    // an exhausted pool returns nullptr instead of refilling.
    explicit SingleThreadAllocator(ChunkSource src = nullptr, void* user = nullptr)
        : chunk_source_(src), chunk_source_user_(user), arena_(kArenaCapacity) {}

    SingleThreadAllocator(const SingleThreadAllocator&) = delete;
    SingleThreadAllocator& operator=(const SingleThreadAllocator&) = delete;
    SingleThreadAllocator(SingleThreadAllocator&&) = delete;
    SingleThreadAllocator& operator=(SingleThreadAllocator&&) = delete;

    void* allocate(std::size_t n) {
        if (n > MAX_SMALL) {
            // Large path: the Arena's actual consumption is its used-delta
            // (requested bytes + alignment padding).
            const std::size_t before = arena_.used();
            void* p = arena_.allocate(n);
            if (p != nullptr) {
                stats_.record_alloc(n, arena_.used() - before);
            }
            return p;
        }
        const std::size_t index = size_class_index(n);
        void* p = pool_allocate(index);
        if (p != nullptr) {
            stats_.record_alloc(n, SIZE_CLASSES[index]);
            return p;
        }
        // Pool exhausted: refill it with a fresh chunk, then retry. This is
        // the rare slow path (one chunk per BlocksPerSlab allocations).
        if (chunk_source_ != nullptr) {
            void* chunk = chunk_source_(SIZE_CLASSES[index], chunk_source_user_);
            if (chunk != nullptr) {
                pool_add_external_slab(index, chunk);
                void* q = pool_allocate(index);
                if (q != nullptr) {
                    stats_.record_alloc(n, SIZE_CLASSES[index]);
                }
                return q;
            }
        }
        return nullptr;
    }

    void deallocate(void* p, std::size_t n) {
        if (n > MAX_SMALL) {
            return;  // the Arena has no per-object free
        }
        const std::size_t index = size_class_index(n);
        stats_.record_free(n, SIZE_CLASSES[index]);
        pool_deallocate(index, p);
    }

    // Like deallocate() but resolves the containing block start itself, for
    // pointers interior to a block (e.g. the delete[] array-cookie offset).
    // `n` only selects the pool; the actual block is computed by the pool.
    void deallocate_containing(void* p, std::size_t n) {
        if (n > MAX_SMALL) {
            return;  // the Arena has no per-object free
        }
        const std::size_t index = size_class_index(n);
        stats_.record_free(n, SIZE_CLASSES[index]);
        pool_deallocate_containing(index, p);
    }

    // Sentinel returned by classify() for pointers owned by the Arena
    // (large or over-aligned path): the Arena cannot free individual
    // objects, so such deallocations are a no-op.
    static constexpr std::size_t kArenaPointer =
        std::numeric_limits<std::size_t>::max();

    // Classifies `p` for the global delete override. Returns:
    //   0           - not owned by this allocator (a bootstrap/malloc
    //                 pointer): the caller must std::free it.
    //   kArenaPointer - owned by the Arena: no per-object free.
    //   anything else - the size class of the block containing `p`; for
    //                 ordinary allocations `p` IS the block start, and it
    //                 should be returned via deallocate(p, cls).
    std::size_t classify(void* p) const {
        if (p == nullptr) {
            return 0;
        }
        return classify_impl(p, std::make_index_sequence<NUM_CLASSES>{});
    }

    // Over-aligned requests (alignment > alignof(max_align_t)) ride the
    // Arena: bump allocation natively honors arbitrary power-of-two
    // alignment. Trade-off: arena memory is only released when the
    // allocator dies (no per-object free) — acceptable for this workload.
    // Recorded like the large path; the matching free is a no-op by design.
    void* allocate_aligned(std::size_t n, std::size_t alignment) {
        const std::size_t before = arena_.used();
        void* p = arena_.allocate(n, alignment);
        if (p != nullptr) {
            stats_.record_alloc(n, arena_.used() - before);
        }
        return p;
    }

    // Aggregated allocation statistics for this thread's allocator.
    const Stats& stats() const { return stats_; }

private:
    using Pools = std::tuple<
        BlockPool<16>, BlockPool<32>, BlockPool<64>, BlockPool<128>,
        BlockPool<256>, BlockPool<512>, BlockPool<1024>>;

    static_assert(std::tuple_size<Pools>::value == NUM_CLASSES,
                  "pools_ must have exactly one pool per size class");

    template <std::size_t I>
    void* pool_allocate_at() {
        return std::get<I>(pools_).allocate();
    }

    template <std::size_t I>
    void pool_deallocate_at(void* p) {
        std::get<I>(pools_).deallocate(p);
    }

    template <std::size_t I>
    void pool_deallocate_containing_at(void* p) {
        std::get<I>(pools_).deallocate_containing(p);
    }

    template <std::size_t I>
    void pool_add_external_slab_at(void* body) {
        auto& pool = std::get<I>(pools_);
        // Link at most what the pool's own slab holds AND what fits inside
        // the chunk (CentralHeap::CHUNK_SIZE). Capping is essential for the
        // largest classes: 256 blocks of 1 KiB would run 256 KiB past a
        // 64 KiB chunk into other threads' memory.
        const std::size_t max_blocks = std::min(
            pool.capacity(), CentralHeap::blocks_per_chunk(SIZE_CLASSES[I]));
        pool.add_external_slab(body, max_blocks);
    }

    using AllocFn = void* (SingleThreadAllocator::*)();
    using FreeFn = void (SingleThreadAllocator::*)(void*);
    using AddSlabFn = void (SingleThreadAllocator::*)(void*);

    template <std::size_t... Is>
    void* pool_allocate_impl(std::size_t index, std::index_sequence<Is...>) {
        static constexpr AllocFn table[] = {
            &SingleThreadAllocator::pool_allocate_at<Is>...};
        return (this->*table[index])();
    }

    template <std::size_t... Is>
    void pool_deallocate_impl(std::size_t index, void* p,
                              std::index_sequence<Is...>) {
        static constexpr FreeFn table[] = {
            &SingleThreadAllocator::pool_deallocate_at<Is>...};
        (this->*table[index])(p);
    }

    template <std::size_t... Is>
    void pool_deallocate_containing_impl(std::size_t index, void* p,
                                         std::index_sequence<Is...>) {
        static constexpr FreeFn table[] = {
            &SingleThreadAllocator::pool_deallocate_containing_at<Is>...};
        (this->*table[index])(p);
    }

    template <std::size_t... Is>
    std::size_t classify_impl(void* p, std::index_sequence<Is...>) const {
        std::size_t found = 0;
        // Slab regions are disjoint, so at most one pool can contain p; the
        // fold keeps probing (harmlessly) even after a hit. `contains` (not
        // owns_block) so delete[] cookie offsets are still classified.
        ((std::get<Is>(pools_).contains(p) ? found = SIZE_CLASSES[Is] : 0),
         ...);
        if (found != 0) {
            return found;
        }
        return arena_.contains(p) ? kArenaPointer : 0;
    }

    template <std::size_t... Is>
    void pool_add_external_slab_impl(std::size_t index, void* base,
                                     std::index_sequence<Is...>) {
        static constexpr AddSlabFn table[] = {
            &SingleThreadAllocator::pool_add_external_slab_at<Is>...};
        (this->*table[index])(base);
    }

    void* pool_allocate(std::size_t index) {
        return pool_allocate_impl(index, std::make_index_sequence<NUM_CLASSES>{});
    }

    void pool_deallocate(std::size_t index, void* p) {
        pool_deallocate_impl(index, p, std::make_index_sequence<NUM_CLASSES>{});
    }

    void pool_deallocate_containing(std::size_t index, void* p) {
        pool_deallocate_containing_impl(index, p,
                                        std::make_index_sequence<NUM_CLASSES>{});
    }

    void pool_add_external_slab(std::size_t index, void* base) {
        pool_add_external_slab_impl(index, base,
                                    std::make_index_sequence<NUM_CLASSES>{});
    }

    Pools pools_;
    Arena arena_;
    Stats stats_;
    ChunkSource chunk_source_;
    void* chunk_source_user_;
};

}  // namespace memalloc
