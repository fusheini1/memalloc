// MemAlloc — Custom Multithreaded Memory Allocator
// Author: Fusheini Abdul-Mumin <abdulmuminfusheini@gmail.com>
// License: MIT

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace memalloc {
namespace viz {

// Event kinds recorded in the ring. `kEventChunk` marks the central-heap
// slow path: a thread exhausted a pool and acquired a fresh 64 KiB chunk.
constexpr std::uint8_t kEventAlloc = 0;
constexpr std::uint8_t kEventFree = 1;
constexpr std::uint8_t kEventChunk = 2;

// One recorded allocator operation. `ts_ns` is a monotonic timestamp for
// ordering, `size` the requested bytes, `ptr` the block pointer, and
// `thread_id` the small per-thread id assigned when the thread's allocator
// was constructed (NOT the OS thread id — it stays small and stable for the
// visualizer's lifetime). `kind` is kEventAlloc/kEventFree/kEventChunk.
struct AllocEvent {
    std::uint64_t ts_ns;
    std::size_t size;
    std::uintptr_t ptr;
    std::uint32_t thread_id;
    std::uint8_t kind;
};

// Fixed-capacity ring buffer of allocator events. Producer threads push
// under a short mutex hold; the visualizer takes a best-effort snapshot.
class EventRing {
public:
    static constexpr std::size_t kCapacity = 65536;

    // Overwrites the oldest event when full.
    void push(const AllocEvent& e) {
        std::lock_guard<std::mutex> lock(mutex_);
        buf_[head_] = e;
        head_ = (head_ + 1) % kCapacity;
        if (count_ < kCapacity) {
            ++count_;
        }
    }

    // Copy of the current contents, oldest first. Vector growth happens OUT
    // of the lock (two passes): if the recording allocator's operator new
    // feeds the snapshot vector, re-entering push() while the ring lock is
    // held would deadlock the same thread. A slightly stale snapshot is
    // fine for a diagnostic display.
    std::vector<AllocEvent> snapshot() const {
        std::size_t n = 0;
        std::size_t start = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            n = count_;
            start = (count_ < kCapacity) ? 0 : head_;
        }
        std::vector<AllocEvent> out(n);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const std::size_t take = (n < count_) ? n : count_;
            for (std::size_t i = 0; i < take; ++i) {
                out[i] = buf_[(start + i) % kCapacity];
            }
        }
        return out;
    }

    // Copy of the most recent `max_count` events, oldest-first. Same
    // two-pass, growth-outside-the-lock structure as snapshot() but copies
    // at most max_count events, so the visualizer's per-frame event feed
    // stays cheap even when the ring is full. count() never decreases, so
    // the tail boundary (start + n - take) can be computed safely.
    std::vector<AllocEvent> snapshot_tail(std::size_t max_count) const {
        std::size_t n = 0;
        std::size_t start = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            n = count_;
            start = (count_ < kCapacity) ? 0 : head_;
        }
        const std::size_t take = std::min(n, max_count);
        std::vector<AllocEvent> out(take);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const std::size_t n2 = count_;
            const std::size_t start2 = (n2 < kCapacity) ? 0 : head_;
            const std::size_t take2 = std::min(take, n2);  // <= out.size()
            const std::size_t first = (start2 + n2 - take2) % kCapacity;
            for (std::size_t i = 0; i < take2; ++i) {
                out[i] = buf_[(first + i) % kCapacity];
            }
        }
        return out;
    }

    std::size_t count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }

private:
    mutable std::mutex mutex_;
    AllocEvent buf_[kCapacity];
    std::size_t head_ = 0;
    std::size_t count_ = 0;
};

// Master switch. The allocator checks it before every push, so a disabled
// build pays exactly one relaxed atomic load + branch per operation and
// nothing else (performance-neutral).
//
// A namespace-scope INLINE variable, not a function-local static: the
// latter's dynamic-initialization guard check would run on EVERY call
// (inlining would be defeated), costing far more than the promised single
// branch. C++17 inline semantics give one instance per process; MSVC folds
// it with selectany — reliable for plain (non-thread_local) inline
// variables, which is why the per-thread flags live in thread_state.cpp.
inline std::atomic<bool> g_enabled{false};

inline void set_enabled(bool on) {
    g_enabled.store(on, std::memory_order_relaxed);
}

inline bool enabled() {
    return g_enabled.load(std::memory_order_relaxed);
}

// The one ring per process. Heap-allocated and deliberately leaked so its
// mutex survives until process exit: exit-time operator delete calls may
// still push events after function-local statics would normally be torn
// down. A single leaked diagnostic buffer is the price for never touching a
// destroyed mutex during teardown. Only reached when enabled(), so the
// guard check is off the disabled hot path.
inline EventRing& ring() {
    static EventRing* r = new EventRing();
    return *r;
}

// Monotonic timestamp in nanoseconds, for ordering events.
inline std::uint64_t now_ns() {
    const auto t = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t).count());
}

// Small per-thread id for the visualizer, assigned once per allocator
// construction. Strictly increasing per process; unique per live thread.
inline std::uint32_t next_thread_id() {
    static std::atomic<std::uint32_t>* c =
        new std::atomic<std::uint32_t>(0);
    return c->fetch_add(1, std::memory_order_relaxed);
}

}  // namespace viz
}  // namespace memalloc
