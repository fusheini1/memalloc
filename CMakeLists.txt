cmake_minimum_required(VERSION 3.16)

project(MemAlloc VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# MinGW/Windows-friendly flags (also fine on GCC/Clang on other platforms).
# Optimization applies to Release only so Debug keeps /Od + /RTC1 intact.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra $<$<CONFIG:Release>:-O2>)
elseif(MSVC)
    add_compile_options(/W4 $<$<CONFIG:Release>:/O2>)
endif()

# Header-only core: every consumer gets include/ on the include path.
add_library(memalloc_core INTERFACE)
target_include_directories(memalloc_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Per-thread allocator state (re-entrancy guard flags). A single-TU static
# library so the thread_locals have exactly one definition per process;
# every memalloc_core consumer (tests, frag_report, bench) links it. It
# contains NO operator new/delete overrides — the benchmark links only this
# and memalloc_core, so its ::operator new and std::malloc stay the SYSTEM
# allocators for the custom-vs-system comparison.
add_library(memalloc_state STATIC src/thread_state.cpp)
target_include_directories(memalloc_state PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(memalloc_core INTERFACE memalloc_state)

# std::thread on non-Windows needs an explicit threading library.
find_package(Threads REQUIRED)

# Global new/delete overrides. Every test links this so that all C++
# allocations (operator new/delete, STL containers, std::thread internals)
# flow through MemAlloc; tests opt into the warm heap by calling
# memalloc::init_allocator() at the top of main.
add_library(memalloc_newdel STATIC src/new_delete.cpp)
target_link_libraries(memalloc_newdel PRIVATE memalloc_core)

# Each test in tests/ is its own executable.
function(memalloc_add_test name)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE memalloc_core memalloc_newdel)
    if(NOT WIN32)
        target_link_libraries(${name} PRIVATE Threads::Threads)
    endif()
    # Keep <cassert> asserts alive even under Release (-DNDEBUG).
    if(MSVC)
        target_compile_options(${name} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${name} PRIVATE -UNDEBUG)
    endif()
endfunction()

memalloc_add_test(test_arena)
memalloc_add_test(test_block_pool)
memalloc_add_test(test_size_class)
memalloc_add_test(test_threaded)
memalloc_add_test(test_new_override)
memalloc_add_test(test_frag)

# Benchmarks: links ONLY memalloc_core (which transitively brings the tiny
# thread-state TU). Deliberately NOT memalloc_newdel, so ::operator new and
# std::malloc remain the SYSTEM allocators for the comparison. Built and run
# in Release (-O2); it is a benchmark, not a ctest.
add_executable(bench bench/bench.cpp)
target_link_libraries(bench PRIVATE memalloc_core)
if(NOT WIN32)
    target_link_libraries(bench PRIVATE Threads::Threads)
endif()

# Fragmentation report: a standalone executable (not a ctest) that prints
# the compile-time table and the runtime global stats.
add_executable(frag_report src/frag_report.cpp)
target_link_libraries(frag_report PRIVATE memalloc_core memalloc_newdel)
if(NOT WIN32)
    target_link_libraries(frag_report PRIVATE Threads::Threads)
endif()
# Keep <cassert> asserts alive even under Release (-DNDEBUG).
if(MSVC)
    target_compile_options(frag_report PRIVATE /UNDEBUG)
else()
    target_compile_options(frag_report PRIVATE -UNDEBUG)
endif()

enable_testing()
add_test(NAME test_arena COMMAND test_arena)
add_test(NAME test_block_pool COMMAND test_block_pool)
add_test(NAME test_size_class COMMAND test_size_class)
add_test(NAME test_threaded COMMAND test_threaded)
add_test(NAME test_new_override COMMAND test_new_override)
add_test(NAME test_frag COMMAND test_frag)
