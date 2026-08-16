// MemAlloc — Custom Multithreaded Memory Allocator
// Author: Fusheini Abdul-Mumin <abdulmuminfusheini@gmail.com>
// License: MIT

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <vector>

namespace memalloc {

// Aggregated allocation statistics. The size counters are LIFETIME totals:
// every successful allocate()/deallocate() adds its sizes, so
// internal_fragmentation() is the fraction of every byte handed out that was
// lost to size-class rounding and alignment padding.
struct Stats {
    std::size_t requested = 0;       // bytes the user asked for
    std::size_t allocated = 0;       // bytes actually consumed (incl. padding/round-up)
    std::size_t peak_allocated = 0;  // peak live bytes outstanding
    std::size_t alloc_count = 0;
    std::size_t free_count = 0;

    // `got` is the number of bytes actually consumed to satisfy a `req`-byte
    // request (the size class for pool allocations, the arena's used-delta
    // for large ones).
    void record_alloc(std::size_t req, std::size_t got) {
        requested += req;
        allocated += got;
        live_ += got;
        if (live_ > peak_allocated) {
            peak_allocated = live_;
        }
        ++alloc_count;
    }

    // Mirrors record_alloc for a deallocation of a `req`-byte object that
    // occupied a `got`-byte block. Large (arena) frees are a no-op in the
    // allocator, so they are never recorded — the arena's bytes stay counted
    // as live, which matches reality (the arena only releases memory when
    // the owning thread dies).
    void record_free(std::size_t req, std::size_t got) {
        requested += req;
        allocated += got;
        live_ -= got;
        ++free_count;
    }

    // (allocated - requested) / allocated, or 0 if nothing was allocated.
    double internal_fragmentation() const {
        if (allocated == 0) {
            return 0.0;
        }
        return static_cast<double>(allocated - requested) /
               static_cast<double>(allocated);
    }

    void print(const char* label, FILE* f = stdout) const {
        std::fprintf(f,
                     "%s: requested=%zu allocated=%zu peak=%zu allocs=%zu "
                     "frees=%zu frag=%.4f\n",
                     label, requested, allocated, peak_allocated, alloc_count,
                     free_count, internal_fragmentation());
    }

private:
    std::size_t live_ = 0;  // current live bytes; drives peak_allocated
};

namespace detail {

// Mutex-protected registry of per-thread Stats. Each thread's allocator
// registers a pointer to its Stats when constructed; when the allocator is
// destroyed the thread RETIRES a snapshot (the Stats object dies with the
// allocator) and unregisters. global_stats() sums the retired snapshots plus
// all still-live Stats.
//
// Function-local static inside an inline function: exactly one registry per
// process, constructed on first use. Its vectors grow through the overridden
// operator new, so every caller (the allocator ctor/dtor) invokes these
// under the re-entrancy guard — growth then takes the malloc fallback
// instead of recursing into the allocator.
struct StatsRegistry {
    std::mutex mutex;
    std::vector<const Stats*> live;
    std::vector<Stats> retired;
};

inline StatsRegistry& stats_registry() {
    static StatsRegistry r;
    return r;
}

inline void register_stats(const Stats* s) {
    std::lock_guard<std::mutex> lock(stats_registry().mutex);
    stats_registry().live.push_back(s);
}

// Snapshots `s` into the retired list (its storage is about to die) and
// removes it from the live registry.
inline void retire_stats(const Stats* s) {
    std::lock_guard<std::mutex> lock(stats_registry().mutex);
    StatsRegistry& reg = stats_registry();
    reg.retired.push_back(*s);
    const auto it = std::find(reg.live.begin(), reg.live.end(), s);
    if (it != reg.live.end()) {
        reg.live.erase(it);
    }
}

}  // namespace detail

// Sum of every thread's Stats: retired snapshots plus live threads. Safe to
// call from any thread at any time. A live Stats may be modified by its
// owning thread concurrently, so the sums are best-effort (aligned size_t
// reads never tear on the supported platforms, so a snapshot is at worst
// slightly stale — never torn).
//
// Inline so the single definition is shared across all TUs that include
// this header (including the new/delete override TU).
inline Stats global_stats() {
    std::lock_guard<std::mutex> lock(detail::stats_registry().mutex);
    detail::StatsRegistry& reg = detail::stats_registry();
    Stats total;
    const auto add = [&total](const Stats& s) {
        total.requested += s.requested;
        total.allocated += s.allocated;
        total.alloc_count += s.alloc_count;
        total.free_count += s.free_count;
        if (s.peak_allocated > total.peak_allocated) {
            total.peak_allocated = s.peak_allocated;
        }
    };
    for (const Stats* s : reg.live) {
        add(*s);
    }
    for (const Stats& s : reg.retired) {
        add(s);
    }
    return total;
}

}  // namespace memalloc
