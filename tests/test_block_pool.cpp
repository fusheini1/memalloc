// MemAlloc — Custom Multithreaded Memory Allocator
// Author: Fusheini Abdul-Mumin <abdulmuminfusheini@gmail.com>
// License: MIT

#include "block_pool.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace memalloc {
// Defined in src/new_delete.cpp; routes C++ allocations through MemAlloc.
void init_allocator();
}

using memalloc::BlockPool;

namespace {

// True if every pointer in `v` is distinct.
bool all_distinct(const std::vector<void*>& v) {
    for (std::size_t i = 0; i < v.size(); ++i) {
        for (std::size_t j = i + 1; j < v.size(); ++j) {
            if (v[i] == v[j]) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

int main() {
    memalloc::init_allocator();  // warm the heap before any C++ allocation

    constexpr std::size_t BlockSize = 64;
    constexpr std::size_t N = 64;  // smaller than the default 256, faster tests

    // 1. Allocate all blocks: each BlockSize-aligned, free_count() -> 0.
    BlockPool<BlockSize, N> pool;
    assert(pool.capacity() == N);
    assert(pool.free_count() == N);

    std::vector<void*> blocks;
    blocks.reserve(N);
    for (std::size_t i = 0; i < N; ++i) {
        void* p = pool.allocate();
        assert(p != nullptr);
        assert(reinterpret_cast<std::uintptr_t>(p) % BlockSize == 0);
        blocks.push_back(p);
    }
    assert(pool.free_count() == 0);
    assert(all_distinct(blocks));

    // 2. Exhaustion: nothing left to hand out.
    assert(pool.allocate() == nullptr);
    assert(pool.free_count() == 0);

    // 3. Free one block, allocate again: must reuse that exact block.
    pool.deallocate(blocks[7]);
    assert(pool.free_count() == 1);
    void* reused = pool.allocate();
    assert(reused == blocks[7]);
    assert(pool.free_count() == 0);

    // 4. Free all, reallocate: every block valid again and still distinct.
    for (void* p : blocks) {
        pool.deallocate(p);
    }
    assert(pool.free_count() == N);

    std::vector<void*> again;
    again.reserve(N);
    for (std::size_t i = 0; i < N; ++i) {
        void* p = pool.allocate();
        assert(p != nullptr);
        assert(reinterpret_cast<std::uintptr_t>(p) % BlockSize == 0);
        again.push_back(p);
    }
    assert(pool.free_count() == 0);
    assert(all_distinct(again));

    // Edge: minimum legal block size (exactly sizeof(void*)).
    BlockPool<sizeof(void*), 32> tiny;
    assert(tiny.capacity() == 32);
    assert(tiny.free_count() == 32);
    void* t = tiny.allocate();
    assert(t != nullptr);
    tiny.deallocate(t);
    assert(tiny.free_count() == 32);

    printf("block_pool OK\n");
    return 0;
}
