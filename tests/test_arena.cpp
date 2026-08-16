// MemAlloc — Custom Multithreaded Memory Allocator
// Author: Fusheini Abdul-Mumin <abdulmuminfusheini@gmail.com>
// License: MIT

#include "arena.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace memalloc {
// Defined in src/new_delete.cpp; routes C++ allocations through MemAlloc.
void init_allocator();
}

using memalloc::Arena;

int main() {
    memalloc::init_allocator();  // warm the heap before any C++ allocation
    // 1. Successful allocation returns non-null.
    Arena a(4096);
    void* p = a.allocate(100);
    assert(p != nullptr);

    // 2. Alignment: allocate(64, 64) returns a pointer aligned to 64.
    void* q = a.allocate(64, 64);
    assert(q != nullptr);
    assert(reinterpret_cast<std::uintptr_t>(q) % 64 == 0);

    // 3. OOM: allocating more than the remaining capacity returns nullptr.
    Arena small(100);
    assert(small.allocate(200) == nullptr);
    assert(small.allocate(100) != nullptr);
    assert(small.allocate(1) == nullptr);

    // 4. Fragmentation: a misaligned cursor forces padding, so frag > 0.
    Arena frag(4096);
    frag.allocate(1);                       // cursor now 1 byte past a 16-aligned base
    void* r = frag.allocate(64, 64);        // must skip up to 63 bytes of padding
    assert(r != nullptr);
    assert(reinterpret_cast<std::uintptr_t>(r) % 64 == 0);
    assert(frag.internal_fragmentation() > 0.0);

    // 5. reset() zeroes the counters and allows new allocations afterward.
    Arena b(4096);
    b.allocate(512);
    assert(b.used() == 512);
    b.reset();
    assert(b.used() == 0);
    assert(b.internal_fragmentation() == 0.0);
    void* s = b.allocate(256);
    assert(s != nullptr);
    assert(b.used() == 256);

    printf("arena OK  used=%zu frag=%.4f\n", frag.used(), frag.internal_fragmentation());
    return 0;
}
