cmake_minimum_required(VERSION 3.16)
project(exchange_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

# Download GoogleTest automatically
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
)
FetchContent_MakeAvailable(googletest)

enable_testing()
find_package(Threads REQUIRED)
add_executable(engine_tests
    tests/smoke_test.cpp
    tests/types_test.cpp
    tests/price_level_test.cpp
    tests/order_book_test.cpp
    tests/matching_engine_test.cpp
    tests/exchange_test.cpp
    tests/book_side_test.cpp
    tests/spsc_queue_test.cpp
    tests/engine_core_test.cpp
    tests/pipeline_test.cpp
    tests/journal_test.cpp
    tests/journaled_pipeline_test.cpp
    tests/protocol_test.cpp
    tests/tcp_server_test.cpp
    )
    add_executable(engine_bench bench/bench_engine.cpp)
    add_executable(engine_pipeline_bench bench/bench_pipeline.cpp)
target_include_directories(engine_pipeline_bench PRIVATE include)
target_link_libraries(engine_pipeline_bench Threads::Threads)
target_include_directories(engine_bench PRIVATE include)
target_include_directories(engine_tests PRIVATE include)
target_link_libraries(engine_tests GTest::gtest_main Threads::Threads)

add_executable(engine_crash_demo demo/crash_demo.cpp)
target_include_directories(engine_crash_demo PRIVATE include)
target_link_libraries(engine_crash_demo Threads::Threads)

add_executable(engine_journal_bench bench/bench_journal.cpp)
target_include_directories(engine_journal_bench PRIVATE include)
target_link_libraries(engine_journal_bench Threads::Threads)

add_executable(engine_server demo/engine_server.cpp)
target_include_directories(engine_server PRIVATE include)
target_link_libraries(engine_server Threads::Threads)

include(GoogleTest)
gtest_discover_tests(engine_tests)
