// MemAlloc — Custom Multithreaded Memory Allocator
// Author: Fusheini Abdul-Mumin <abdulmuminfusheini@gmail.com>
// License: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace memalloc {

// Arena — a simple bump (linear) allocator over one contiguous region
// obtained from std::malloc. Allocations are cheap: align, check bounds,
// advance the cursor. Memory is released all at once on destruction.
//
// Not thread-safe. Non-copyable and non-movable.
class Arena {
public:
    // Allocates one contiguous region of `capacity` bytes via std::malloc.
    // Throws std::bad_alloc if the region cannot be obtained.
    explicit Arena(std::size_t capacity)
        : base_(static_cast<std::byte*>(std::malloc(capacity))),
          capacity_(capacity),
          used_(0),
          requested_(0),
          allocated_(0) {
        if (base_ == nullptr) {
            throw std::bad_alloc();
        }
    }

    ~Arena() { std::free(base_); }

    // Non-copyable, non-movable.
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&) = delete;
    Arena& operator=(Arena&&) = delete;

    // Returns a pointer to a block of at least `size` bytes aligned to
    // `alignment` (must be a power of two), or nullptr if the arena has
    // insufficient remaining space.
    void* allocate(std::size_t size,
                   std::size_t alignment = alignof(std::max_align_t)) {
        const std::uintptr_t cur = reinterpret_cast<std::uintptr_t>(base_ + used_);
        const std::uintptr_t aligned = (cur + alignment - 1) & ~(alignment - 1);
        const std::size_t padding = static_cast<std::size_t>(aligned - cur);
        if (used_ + padding + size > capacity_) {
            return nullptr;
        }
        used_ += padding + size;
        requested_ += size;
        allocated_ += padding + size;
        return reinterpret_cast<void*>(aligned);
    }

    // Rewinds the arena so all memory can be reused. Does not release the
    // backing region.
    void reset() {
        used_ = 0;
        requested_ = 0;
        allocated_ = 0;
    }

    // Fraction of allocated bytes lost to alignment padding.
    // (allocated_ - requested_) / allocated_, or 0 if nothing was allocated.
    double internal_fragmentation() const {
        if (allocated_ == 0) {
            return 0.0;
        }
        return static_cast<double>(allocated_ - requested_) /
               static_cast<double>(allocated_);
    }

    std::size_t used() const { return used_; }
    std::size_t capacity() const { return capacity_; }

    // True if `p` lies inside this arena's backing region. Used by the
    // global delete override to recognize large / over-aligned pointers,
    // which the Arena can never free individually.
    bool contains(const void* p) const {
        const std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(p);
        const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(base_);
        return addr >= begin && addr < begin + used_;
    }

private:
    std::byte* base_;
    std::size_t capacity_;
    std::size_t used_;       // bytes consumed from the region (incl. padding)
    std::size_t requested_;  // bytes actually requested by callers
    std::size_t allocated_;  // bytes handed out (requested + padding)
};

}  // namespace memalloc
