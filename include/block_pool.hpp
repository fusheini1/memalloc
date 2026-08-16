// MemAlloc — Custom Multithreaded Memory Allocator
// Author: Fusheini Abdul-Mumin <abdulmuminfusheini@gmail.com>
// License: MIT

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <type_traits>
#include <vector>

namespace memalloc {
namespace detail {

// Compile-time probe: does this toolchain provide ::aligned_alloc?
// MSVC still lacks C11 aligned_alloc (verified on MSVC 19.50), so it is
// excluded explicitly; elsewhere the SFINAE detection idiom decides.
#if defined(_MSC_VER)
inline constexpr bool has_aligned_alloc_v = false;
#else
template <typename, typename = void>
struct has_aligned_alloc : std::false_type {};

template <typename T>
struct has_aligned_alloc<
    T, std::void_t<decltype(::aligned_alloc(std::size_t{0}, std::size_t{0}))>>
    : std::true_type {};

inline constexpr bool has_aligned_alloc_v = has_aligned_alloc<int>::value;
#endif

// A `size`-byte region aligned to `alignment` (a power of two), plus the raw
// pointer that must be handed to free().
struct SlabAlloc {
    void* aligned;
    void* raw;
};

// Attempts ::aligned_alloc, but only when the toolchain provides it. As a
// function template, the discarded branch of if constexpr is never
// instantiated, so the ::aligned_alloc call is not compiled on MSVC.
template <bool UseAlignedAlloc = has_aligned_alloc_v>
inline SlabAlloc try_aligned_alloc(std::size_t size, std::size_t alignment) {
    if constexpr (UseAlignedAlloc) {
        // C11 aligned_alloc: frees with free(). Our size is an integral
        // multiple of `alignment` (BlocksPerSlab * BlockSize).
        if (void* p = ::aligned_alloc(alignment, size)) {
            return {p, p};
        }
    }
    return {nullptr, nullptr};
}

inline SlabAlloc allocate_slab(std::size_t size, std::size_t alignment) {
    if (SlabAlloc s = try_aligned_alloc(size, alignment); s.aligned != nullptr) {
        return s;
    }
    // Fallback: over-allocate by `alignment` bytes, then round up. The raw
    // pointer is carried alongside, so no metadata needs to be stashed.
    void* raw = std::malloc(size + alignment);
    if (raw == nullptr) {
        return {nullptr, nullptr};
    }
    const std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(raw);
    const std::uintptr_t aligned = (addr + alignment - 1) & ~(alignment - 1);
    return {reinterpret_cast<void*>(aligned), raw};
}

inline void free_slab(SlabAlloc s) {
    std::free(s.raw);
}

}  // namespace detail

// BlockPool — a pool of fixed-size blocks carved from a single slab.
//
// Free blocks are linked into an intrusive free list: every free block's own
// first bytes hold a pointer to the next free block, so no separate metadata
// array is needed. allocate() pops the head; deallocate() pushes.
//
// Ownership: the pool OWNS only the slab it created in its constructor and
// frees exactly that slab on destruction. Slabs added later via
// add_external_slab() are linked into the free list but remain owned by
// whoever supplied them (the CentralHeap), so the pool never frees them.
//
// Not thread-safe. Non-copyable and non-movable.
template <std::size_t BlockSize, std::size_t BlocksPerSlab = 256>
class BlockPool {
    static_assert(BlockSize >= sizeof(void*),
                  "BlockSize must be at least sizeof(void*) to hold a free-list pointer");
    static_assert((BlockSize & (BlockSize - 1)) == 0,
                  "BlockSize must be a power of two");
    static_assert(BlocksPerSlab > 0, "BlocksPerSlab must be at least 1");
    static_assert(BlocksPerSlab <= SIZE_MAX / BlockSize,
                  "BlocksPerSlab * BlockSize overflows std::size_t");

public:
    BlockPool() {
        detail::SlabAlloc s =
            detail::allocate_slab(BlocksPerSlab * BlockSize, BlockSize);
        if (s.aligned == nullptr) {
            throw std::bad_alloc();
        }
        slab_ = s.aligned;
        raw_ = s.raw;

        // Link every block into the free list, last block terminates with
        // nullptr. The list ends up in address order, head_ = block 0.
        std::byte* b = static_cast<std::byte*>(slab_);
        for (std::size_t i = 0; i < BlocksPerSlab; ++i) {
            void** slot = reinterpret_cast<void**>(b);
            *slot = (i + 1 < BlocksPerSlab) ? static_cast<void*>(b + BlockSize)
                                            : nullptr;
            b += BlockSize;
        }
        head_ = slab_;
    }

    ~BlockPool() { detail::free_slab({slab_, raw_}); }

    // Non-copyable, non-movable.
    BlockPool(const BlockPool&) = delete;
    BlockPool& operator=(const BlockPool&) = delete;
    BlockPool(BlockPool&&) = delete;
    BlockPool& operator=(BlockPool&&) = delete;

    // Pops the free-list head, or nullptr if the pool is exhausted.
    void* allocate() {
        if (head_ == nullptr) {
            return nullptr;
        }
        void* p = head_;
        head_ = *reinterpret_cast<void**>(p);
        return p;
    }

    // Links `num_blocks` blocks starting at `base` into the free list and
    // records the region so deallocate() can validate blocks from it. The
    // caller keeps ownership of `base`: the pool records it but NEVER frees
    // it (the CentralHeap frees the underlying chunk at process exit).
    //
    // `num_blocks` defaults to BlocksPerSlab but may be smaller: an external
    // chunk (e.g. CentralHeap's 64 KiB) may not be big enough for the full
    // slab of the largest classes, so the caller caps the count at what
    // actually fits and the walk must stay inside the chunk.
    void add_external_slab(void* base, std::size_t num_blocks = BlocksPerSlab) {
        assert(base != nullptr);
        assert(num_blocks > 0 && num_blocks <= BlocksPerSlab);
        std::byte* b = static_cast<std::byte*>(base);
        for (std::size_t i = 0; i < num_blocks; ++i) {
            void** slot = reinterpret_cast<void**>(b);
            *slot = (i + 1 < num_blocks) ? static_cast<void*>(b + BlockSize)
                                         : head_;
            b += BlockSize;
        }
        head_ = base;
        external_slabs_.push_back({base, num_blocks});
    }

    // Pushes `p` back onto the free list. `p` must be a block previously
    // handed out by this pool (from the constructor slab or any external
    // slab, BlockSize-aligned within it); debug builds assert this,
    // Release relies on the caller.
    void deallocate(void* p) {
        assert(p != nullptr);
        assert(owns_block(p));
        *reinterpret_cast<void**>(p) = head_;
        head_ = p;
    }

    // True if `p` lies anywhere inside a slab region this pool manages
    // (block-aligned or not). delete[] pointers can be offset from the block
    // start by the array-count cookie the compiler stores before the
    // elements, so the global new/delete override must accept interior
    // pointers and resolve the containing block itself.
    bool contains(void* p) const {
        const std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(p);
        const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(slab_);
        if (addr >= begin && addr < begin + BlocksPerSlab * BlockSize) {
            return true;
        }
        for (const ExternalSlab& s : external_slabs_) {
            const std::uintptr_t b = reinterpret_cast<std::uintptr_t>(s.base);
            if (addr >= b && addr < b + s.num_blocks * BlockSize) {
                return true;
            }
        }
        return false;
    }

    // Deallocates the block CONTAINING `p`, where `p` may be interior to the
    // block (e.g. the delete[] array-cookie offset). Computes the containing
    // block start, then pushes it exactly like deallocate().
    void deallocate_containing(void* p) {
        const std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(p);
        const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(slab_);
        void* block = nullptr;
        if (addr >= begin && addr < begin + BlocksPerSlab * BlockSize) {
            block = reinterpret_cast<void*>(begin +
                                           ((addr - begin) / BlockSize) * BlockSize);
        } else {
            for (const ExternalSlab& s : external_slabs_) {
                const std::uintptr_t b = reinterpret_cast<std::uintptr_t>(s.base);
                if (addr >= b && addr < b + s.num_blocks * BlockSize) {
                    block = reinterpret_cast<void*>(b +
                                                   ((addr - b) / BlockSize) * BlockSize);
                    break;
                }
            }
        }
        assert(block != nullptr);
        assert(owns_block(block));
        deallocate(block);
    }

    // Number of currently free blocks. Walks the list, O(n).
    std::size_t free_count() const {
        std::size_t n = 0;
        for (void* p = head_; p != nullptr; p = *reinterpret_cast<void**>(p)) {
            ++n;
        }
        return n;
    }

    std::size_t capacity() const { return BlocksPerSlab; }

    // Total number of blocks this pool manages: the constructor slab plus
    // every external slab linked via add_external_slab(). Used by the
    // visualizer's occupancy snapshot — used = total - free_count stays
    // non-negative even after chunk refills have grown the pool past its
    // original slab. capacity() deliberately remains BlocksPerSlab for
    // tests that assert the initial slab size.
    std::size_t total_blocks() const {
        std::size_t total = BlocksPerSlab;
        for (const ExternalSlab& s : external_slabs_) {
            total += s.num_blocks;
        }
        return total;
    }

    // True if `p` is a block start inside the constructor slab or any
    // external slab. Used by deallocate() to validate, and exposed publicly
    // so the global new/delete override can classify a pointer before
    // routing it back to the right pool.
    bool owns_block(void* p) const {
        const std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(p);
        const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(slab_);
        if (addr >= begin && addr < begin + BlocksPerSlab * BlockSize &&
            (addr - begin) % BlockSize == 0) {
            return true;
        }
        for (const ExternalSlab& s : external_slabs_) {
            const std::uintptr_t b = reinterpret_cast<std::uintptr_t>(s.base);
            const std::uintptr_t end = b + s.num_blocks * BlockSize;
            if (addr >= b && addr < end && (addr - b) % BlockSize == 0) {
                return true;
            }
        }
        return false;
    }

private:
    // A chunk-linked block region: where it starts and how many blocks were
    // linked into it. Kept for deallocate() validation only.
    struct ExternalSlab {
        void* base;
        std::size_t num_blocks;
    };

    void* slab_;               // BlockSize-aligned start of the block region
    void* raw_;                // pointer to hand back to free()
    void* head_;               // head of the intrusive free list
    std::vector<ExternalSlab> external_slabs_;  // regions we link but do NOT free
};

}  // namespace memalloc
