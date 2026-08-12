# Opt-in self-contained unit tests for the rocclr device layer.
# Enable with -DROCCLR_BUILD_TESTS=ON.
cmake_minimum_required(VERSION 3.14)

option(ROCCLR_BUILD_TESTS "Build rocclr device unit tests" OFF)

if(NOT ROCCLR_BUILD_TESTS)
  return()
endif()

find_package(GTest QUIET)
if(NOT GTest_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

enable_testing()
include(GoogleTest)

add_executable(rocclr_virtual_alloc_tests test_virtual_alloc_null_guard.cpp)
target_link_libraries(rocclr_virtual_alloc_tests PRIVATE GTest::gtest GTest::gtest_main)
gtest_discover_tests(rocclr_virtual_alloc_tests DISCOVERY_MODE PRE_TEST)