cmake_minimum_required(VERSION 3.10)

project(condy VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(condy INTERFACE)
target_include_directories(condy INTERFACE include)

add_subdirectory(third_party/doctest)
add_subdirectory(examples)
enable_testing()
add_subdirectory(tests)
add_subdirectory(benchmarks)