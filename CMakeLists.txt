cmake_minimum_required(VERSION 3.20)
project(lease VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# lease — C++14 header-only object capability + access-control library.
#
# Consume from another project with:
#   add_subdirectory(lease)          # or FetchContent / CPM
#   target_link_libraries(app PRIVATE lease::lease)
#
# The library itself has no compiled artifacts; the interface target exports
# the include directory and the C++14 language requirement.
# ---------------------------------------------------------------------------

add_library(lease INTERFACE)
add_library(lease::lease ALIAS lease)

target_include_directories(lease INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lease>
    $<INSTALL_INTERFACE:include>)

target_compile_features(lease INTERFACE cxx_std_14)

# ---------------------------------------------------------------------------
# Tests live outside the library folder; enable CTest so `ctest` works.
# ---------------------------------------------------------------------------
option(LEASE_BUILD_TESTS "Build lease contract tests" ON)
option(LEASE_BUILD_BENCH "Build lease benchmark" ON)

if(LEASE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(LEASE_BUILD_BENCH)
    find_package(Threads REQUIRED)
    add_executable(lease_bench bench/bench.cpp)
    target_link_libraries(lease_bench PRIVATE lease::lease Threads::Threads)
    target_compile_features(lease_bench PRIVATE cxx_std_17)
endif()
