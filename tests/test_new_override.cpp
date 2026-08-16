// MemAlloc — Custom Multithreaded Memory Allocator
// Author: Fusheini Abdul-Mumin <abdulmuminfusheini@gmail.com>
// License: MIT

// Exercises the global operator new/delete overrides (linked from
// src/new_delete.cpp): scalar, array (with and without cookie), over-aligned,
// bootstrap-before-init, multithreaded churn, and a chunk-refill phase that
// forces the external-slab path through the override.

#include "central_heap.hpp"
#include "thread_pool_alloc.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#ifdef _DEBUG
#include <crtdbg.h>
#endif

namespace memalloc {
// Defined in src/new_delete.cpp; turns on the override after static init.
void init_allocator();
}  // namespace memalloc

namespace {

// Runs BEFORE main(): g_ready is still false, so this allocation must take
// the std::malloc bootstrap path, and its delete (during static destruction,
// after main) must classify it as not-ours and std::free it.
struct BootstrapProbe {
    int* p;
    BootstrapProbe() : p(new int(7)) {}
    ~BootstrapProbe() {
        assert(p != nullptr);
        assert(*p == 7);
        delete p;
    }
};
BootstrapProbe g_bootstrap_probe;

// Over-aligned type: new/delete for it must use the C++17 aligned forms,
// which we route to the Arena (no per-object free).
struct alignas(64) OverAligned {
    int v;
    explicit OverAligned(int x) : v(x) {}
};

constexpr int kThreads = 4;
constexpr int kIters = 10000;

}  // namespace

int main() {
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    memalloc::init_allocator();

    // Trivial scalar.
    {
        int* p = new int;
        *p = 42;
        assert(*p == 42);
        delete p;
    }

    // Class with a destructor.
    {
        std::string* s = new std::string("hello");
        assert(*s == "hello");
        delete s;
    }

    // Trivial array (no cookie). The compiler usually emits an UNSIZED
    // delete[] here, so this exercises pointer classification.
    {
        char* buf = new char[100];
        for (int i = 0; i < 100; ++i) {
            buf[i] = static_cast<char>(i);
        }
        for (int i = 0; i < 100; ++i) {
            assert(buf[i] == static_cast<char>(i));
        }
        delete[] buf;
    }

    // Non-trivial array (count cookie stored before the elements).
    {
        std::string* arr = new std::string[8];
        for (int i = 0; i < 8; ++i) {
            arr[i] = "arr" + std::to_string(i);
        }
        for (int i = 0; i < 8; ++i) {
            assert(arr[i] == "arr" + std::to_string(i));
        }
        delete[] arr;
    }

    // Over-aligned allocation (aligned new/delete forms).
    {
        OverAligned* oa = new OverAligned(9);
        assert(oa != nullptr);
        assert(reinterpret_cast<std::uintptr_t>(oa) % 64 == 0);
        assert(oa->v == 9);
        delete oa;  // classified as arena-owned: no-op
    }

    // Large allocation (rides the Arena; delete is a no-op by design).
    {
        char* big = new char[2000];
        assert(big != nullptr);
        big[0] = 'x';
        big[1999] = 'y';
        delete[] big;
    }

    // 4 threads x 10,000 std::string new/delete each. Every thread allocates
    // and frees its own strings, so all pointers stay thread-affine.
    {
        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([] {
                for (int i = 0; i < kIters; ++i) {
                    std::string* s = new std::string("thread");
                    assert(*s == "thread");
                    delete s;
                }
            });
        }
        for (auto& th : threads) {
            th.join();
        }
    }

    // Refill-forcing phase: hold ~300 buffers sized to the 1024 class so the
    // 1024 pool exhausts its constructor slab (256 blocks) and pulls at least
    // one CentralHeap chunk through the operator new path. All on this thread,
    // so every pointer stays thread-affine.
    {
        constexpr std::size_t kHold = 300;
        constexpr std::size_t kBufSize = 900;  // -> class 1024
        const std::size_t before =
            memalloc::CentralHeap::instance().total_chunks_acquired();

        std::vector<void*> held;
        held.reserve(kHold);
        for (std::size_t i = 0; i < kHold; ++i) {
            void* p = ::operator new(kBufSize);
            assert(p != nullptr);
            static_cast<char*>(p)[0] = 'r';
            static_cast<char*>(p)[kBufSize - 1] = 'z';
            held.push_back(p);
        }
        const std::size_t after =
            memalloc::CentralHeap::instance().total_chunks_acquired();
        assert(after > before);  // the slow (chunk-acquiring) path ran

        for (void* p : held) {
            ::operator delete(p);
        }
        std::printf("refill phase: chunks %zu -> %zu (+%zu)\n", before, after,
                    after - before);
    }

    std::printf("new_override OK\n");
    return 0;
}
