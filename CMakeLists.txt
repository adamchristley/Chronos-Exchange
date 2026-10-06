cmake_minimum_required(VERSION 3.20)
project(chronos_exchange VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(chronos_core
    src/order_book.cpp
    src/protocol.cpp
    src/event_log.cpp
)
target_include_directories(chronos_core PUBLIC include)
target_compile_options(chronos_core PRIVATE -Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)

add_executable(chronos_server src/exchange_server.cpp)
target_link_libraries(chronos_server PRIVATE chronos_core Threads::Threads)
add_executable(chronos_client src/trading_client.cpp)
target_link_libraries(chronos_client PRIVATE chronos_core)
add_executable(chronos_subscriber src/market_data_subscriber.cpp)
target_link_libraries(chronos_subscriber PRIVATE chronos_core)
add_executable(chronos_replay src/replay.cpp)
target_link_libraries(chronos_replay PRIVATE chronos_core)
add_executable(chronos_bench src/benchmark.cpp)
target_link_libraries(chronos_bench PRIVATE chronos_core)

include(CTest)
if(BUILD_TESTING)
    add_executable(chronos_tests tests/test_matching.cpp)
    target_link_libraries(chronos_tests PRIVATE chronos_core Threads::Threads)
    add_test(NAME chronos_tests COMMAND chronos_tests)
endif()
