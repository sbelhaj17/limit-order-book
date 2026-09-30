cmake_minimum_required(VERSION 3.20)
project(limit_order_book LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(LOB_SANITIZE "Build with AddressSanitizer and UBSan" OFF)
if(LOB_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_library(lob INTERFACE)
target_include_directories(lob INTERFACE include)
target_compile_options(lob INTERFACE -Wall -Wextra -Wpedantic)

add_executable(itch_replay src/itch_replay.cpp)
target_link_libraries(itch_replay PRIVATE lob)

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_executable(lob_tests
  tests/differential_test.cpp
  tests/engine_test.cpp
  tests/id_map_test.cpp
  tests/itch_test.cpp
  tests/market_test.cpp
)
target_link_libraries(lob_tests PRIVATE lob GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(lob_tests)
