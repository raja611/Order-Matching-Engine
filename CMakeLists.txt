cmake_minimum_required(VERSION 3.14)
project(OrderMatchingEngine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG -flto")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address,undefined")

add_library(matching_engine STATIC
    src/order_book.cpp
    src/matching_engine.cpp
)
target_include_directories(matching_engine PUBLIC include)

add_executable(ome_demo src/main.cpp)
target_link_libraries(ome_demo PRIVATE matching_engine)

add_executable(ome_bench bench/benchmark.cpp)
target_link_libraries(ome_bench PRIVATE matching_engine)
