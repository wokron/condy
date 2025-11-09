cmake_minimum_required(VERSION 3.5)

project(condy VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(condy INTERFACE)
target_include_directories(condy INTERFACE include)

# doctest
add_subdirectory(third_party/doctest)

# liburing
set(LIBURING_SOURCE_DIR ${CMAKE_SOURCE_DIR}/third_party/liburing)
add_custom_target(
    build_uring
    ALL
    COMMAND ./configure --cc=gcc --cxx=g++
    COMMAND $(MAKE) -C src
    WORKING_DIRECTORY ${LIBURING_SOURCE_DIR}
)
add_library(uring STATIC IMPORTED GLOBAL)
set_target_properties(uring PROPERTIES IMPORTED_LOCATION ${LIBURING_SOURCE_DIR}/src/liburing.a)
target_include_directories(uring INTERFACE ${LIBURING_SOURCE_DIR}/src/include)
add_dependencies(uring build_uring)

add_subdirectory(examples)

add_subdirectory(benchmarks)

enable_testing()
add_subdirectory(tests)