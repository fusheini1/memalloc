// MemAlloc — Custom Multithreaded Memory Allocator
// Author: Fusheini Abdul-Mumin <abdulmuminfusheini@gmail.com>
// License: MIT

// Benchmarks: MemAlloc (ThreadPoolAlloc) vs the system allocator.
//
// IMPORTANT: this executable links ONLY memalloc_core (which transitively
// carries the tiny thread-state TU) and deliberately NOT memalloc_newdel —
// there are no operator new/delete overrides in this binary, so
// std::malloc and ::operator new here are the SYSTEM allocators. Every
// comparison is therefore a fair custom-vs-system race.
//
// Run in Release (-O2), from the memalloc/ directory:
//   cmake --build build --config Release
//   ./build/Release/bench.exe
//
// Methodology: one untimed warmup run, then R = 5 timed trials; the MIN and
// MEDIAN are reported for every number.

#include "thread_pool_alloc.hpp"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::high_resolution_clock;

constexpr std::size_t kTotalOps = 2000000;  // (a) and (b) total alloc/free pairs
constexpr std::size_t kSizes[] = {16, 64, 256, 1024};
constexpr std::size_t kNumSizes = sizeof(kSizes) / sizeof(kSizes[0]);

// One line to stdout AND to the results file.
void print_line(FILE* file, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::fputs(buf, stdout);
    if (file != nullptr) {
        std::fputs(buf, file);
    }
}

// min & median of the R timed trials (ns).
struct Timing {
    double min;
    double median;
};

// One untimed warmup run, then kTrials timed runs of f(); returns min/median.
template <typename F>
Timing measure(F&& f) {
    constexpr std::size_t kTrials = 5;
    f();  // untimed warmup
    std::vector<double> samples;
    samples.reserve(kTrials);
    for (std::size_t i = 0; i < kTrials; ++i) {
        const auto t0 = Clock::now();
        f();
        const auto t1 = Clock::now();
        samples.push_back(
            std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
    std::sort(samples.begin(), samples.end());
    return {samples.front(), samples[samples.size() / 2]};
}

// `count` alloc/free pairs, sizes cycling {16,64,256,1024}. Custom = the
// MemAlloc per-thread pools; else system malloc/free.
template <bool Custom>
void op_loop(std::size_t count) {
    if constexpr (Custom) {
        memalloc::ThreadPoolAlloc& alloc =
            memalloc::ThreadPoolAlloc::instance();
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t n = kSizes[i % kNumSizes];
            void* p = alloc.allocate(n);
            alloc.deallocate(p, n);
        }
    } else {
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t n = kSizes[i % kNumSizes];
            void* p = std::malloc(n);
            std::free(p);
        }
    }
}

// kTotalOps pairs split evenly across `threads`; every thread allocates and
// frees only its own blocks (owning-thread contract). Returns min/median
// wall time in ns.
template <bool Custom>
Timing scaling(std::size_t threads) {
    return measure([threads] {
        const std::size_t per = kTotalOps / threads;
        if (threads == 1) {
            op_loop<Custom>(per);
        } else {
            std::vector<std::thread> pool;
            pool.reserve(threads);
            for (std::size_t t = 0; t < threads; ++t) {
                pool.emplace_back([per] { op_loop<Custom>(per); });
            }
            for (std::thread& th : pool) {
                th.join();
            }
        }
    });
}

// Cache-locality: 500,000 x 64-byte structs. Path A carves them from the
// per-thread pools (contiguous within slabs/chunks); Path B from std::malloc
// (scattered). Times only the linear traversal; returns ns/elem.
struct Node {
    int field;
    char pad[60];
};
static_assert(sizeof(Node) == 64, "Node must be exactly one cache line");

template <bool Custom>
Timing locality(std::int64_t& sink_out) {
    constexpr std::size_t kM = 500000;
    auto run = [kM] {
        std::vector<Node*> ptrs;
        ptrs.reserve(kM);
        if constexpr (Custom) {
            memalloc::ThreadPoolAlloc& alloc =
                memalloc::ThreadPoolAlloc::instance();
            for (std::size_t i = 0; i < kM; ++i) {
                Node* p = static_cast<Node*>(alloc.allocate(sizeof(Node)));
                p->field = 1;
                ptrs.push_back(p);
            }
        } else {
            for (std::size_t i = 0; i < kM; ++i) {
                Node* p = static_cast<Node*>(std::malloc(sizeof(Node)));
                p->field = 1;
                ptrs.push_back(p);
            }
        }
        // Linear traversal; the volatile sink defeats dead-code elimination.
        volatile std::int64_t sink = 0;
        const auto t0 = Clock::now();
        for (const Node* p : ptrs) {
            sink += p->field;
        }
        const auto t1 = Clock::now();
        const double ns_elem =
            std::chrono::duration<double, std::nano>(t1 - t0).count() /
            static_cast<double>(kM);
        if constexpr (Custom) {
            for (const Node* p : ptrs) {
                memalloc::ThreadPoolAlloc::instance().deallocate(
                    const_cast<Node*>(p), sizeof(Node));
            }
        } else {
            for (const Node* p : ptrs) {
                std::free(const_cast<Node*>(p));
            }
        }
        return ns_elem;
    };
    run();  // untimed warmup
    std::vector<double> samples;
    for (std::size_t i = 0; i < 5; ++i) {
        samples.push_back(run());
    }
    std::sort(samples.begin(), samples.end());
    sink_out = 1;  // the real sum is folded into the volatile sink; keep the
                   // reference so no run can ever be optimized away
    return {samples.front(), samples[samples.size() / 2]};
}

const char* os_name() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#else
    return "Unknown";
#endif
}

void print_compiler(FILE* file) {
    const char* name =
#if defined(_MSC_VER)
        "MSVC";
#elif defined(__clang__)
        "Clang";
#elif defined(__GNUC__)
        "GCC";
#else
        "Unknown";
#endif
    print_line(file, "compiler = %s", name);
#if defined(_MSC_VER)
    print_line(file, " (_MSC_VER=%d)", static_cast<int>(_MSC_VER));
#elif defined(__clang__)
    print_line(file, " (%d.%d)", static_cast<int>(__clang_major__),
               static_cast<int>(__clang_minor__));
#elif defined(__GNUC__)
    print_line(file, " (%d.%d)", static_cast<int>(__GNUC__),
               static_cast<int>(__GNUC_MINOR__));
#endif
    print_line(file, "\n");
}

}  // namespace

int main() {
    FILE* out = std::fopen("assets/bench_results.txt", "w");
    if (out == nullptr) {
        std::fprintf(stderr,
                     "warning: cannot open assets/bench_results.txt (run from "
                     "the memalloc/ directory)\n");
    }

    print_line(out, "=== MemAlloc benchmarks ===\n");
    print_line(out, "hardware_concurrency = %u\n",
               std::thread::hardware_concurrency());
    print_line(out, "OS = %s\n", os_name());
    print_compiler(out);
    print_line(out, "config = Release (-O2)\n\n");

    // (a) single-thread alloc/free latency.
    print_line(out, "(a) single-thread alloc/free latency   (%zu pairs, ns/op)\n",
               static_cast<std::size_t>(kTotalOps));
    print_line(out, "  %-24s %10s %10s\n", "allocator", "min", "median");

    const Timing custom_a = measure([] {
        op_loop<true>(kTotalOps);
    });
    print_line(out, "  %-24s %10.2f %10.2f\n", "ThreadPoolAlloc",
               custom_a.min / kTotalOps, custom_a.median / kTotalOps);

    const Timing malloc_a = measure([] { op_loop<false>(kTotalOps); });
    print_line(out, "  %-24s %10.2f %10.2f\n", "std::malloc/free",
               malloc_a.min / kTotalOps, malloc_a.median / kTotalOps);

    const Timing new_a = measure([] {
        for (std::size_t i = 0; i < kTotalOps; ++i) {
            const std::size_t n = kSizes[i % kNumSizes];
            void* p = ::operator new(n);
            ::operator delete(p);
        }
    });
    print_line(out, "  %-24s %10.2f %10.2f\n", "::operator new/delete",
               new_a.min / kTotalOps, new_a.median / kTotalOps);
    print_line(out, "\n");

    // (b) multi-thread scaling.
    print_line(out, "(b) multi-thread scaling   (%zu pairs total, split evenly)\n",
               static_cast<std::size_t>(kTotalOps));
    print_line(out, "  %-7s %-16s %-15s %-16s %-15s\n", "threads",
               "custom ns/op", "custom speedup", "malloc ns/op",
               "malloc speedup");
    const std::size_t threads[] = {1, 2, 4, 8};
    // T=1 wall time is the speedup baseline for every other thread count.
    const double base_custom = scaling<true>(1).median;
    const double base_malloc = scaling<false>(1).median;
    for (const std::size_t t : threads) {
        const Timing c = scaling<true>(t);
        const Timing m = scaling<false>(t);
        const double per_thread_ops = static_cast<double>(kTotalOps) / t;
        print_line(out, "  %-7zu %-16.2f %-15.2f %-16.2f %-15.2f\n", t,
                   c.median / per_thread_ops,
                   base_custom / c.median,
                   m.median / per_thread_ops,
                   base_malloc / m.median);
    }
    print_line(out, "\n");

    // (c) cache-locality traversal.
    print_line(out,
               "(c) cache-locality traversal   (500,000 x 64 B structs, ns/elem)\n");
    print_line(out, "  %-24s %10s %10s\n", "path", "min", "median");
    std::int64_t sink = 0;
    const Timing c_a = locality<true>(sink);
    print_line(out, "  %-24s %10.3f %10.3f\n", "ThreadPoolAlloc (slabs)",
               c_a.min, c_a.median);
    const Timing c_b = locality<false>(sink);
    print_line(out, "  %-24s %10.3f %10.3f\n", "std::malloc (scattered)",
               c_b.min, c_b.median);
    (void)sink;

    if (out != nullptr) {
        std::fclose(out);
        std::printf("\n(results written to assets/bench_results.txt)\n");
    }
    return 0;
}
