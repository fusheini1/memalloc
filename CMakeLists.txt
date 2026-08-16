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

enable_testing()
add_test(NAME test_arena COMMAND test_arena)
add_test(NAME test_block_pool COMMAND test_block_pool)
add_test(NAME test_size_class COMMAND test_size_class)
add_test(NAME test_threaded COMMAND test_threaded)
add_test(NAME test_new_override COMMAND test_new_override)
