// MemAlloc — Custom Multithreaded Memory Allocator
// Author: Fusheini Abdul-Mumin <abdulmuminfusheini@gmail.com>
// License: MIT

// Sole definitions of the per-thread allocator state. The thread_locals
// live in exactly ONE translation unit (this one), so every TU that uses
// them — the new/delete override in new_delete.cpp, the allocator entry
// points in the headers, and the standalone benchmark — shares one instance
// per thread through the non-inline accessors declared in central_heap.hpp.
//
// This file deliberately contains NO operator new/delete overrides: the
// benchmark links only memalloc_core (which carries this TU) and must keep
// ::operator new / std::malloc as the SYSTEM allocators.

#include "central_heap.hpp"

namespace memalloc {
namespace detail {

thread_local bool g_allocator_active = false;
thread_local bool g_allocator_gone = false;

bool& allocator_active_flag() { return g_allocator_active; }

// Sticky: once a thread's allocator is destroyed, it stays "gone" for the
// rest of that thread's life (thread exit / process exit). Exit-time
// deletes that would otherwise call instance() on the destroyed object are
// routed to the malloc-fallback path instead.
void mark_allocator_gone() { g_allocator_gone = true; }
bool allocator_gone() { return g_allocator_gone; }

}  // namespace detail
}  // namespace memalloc
