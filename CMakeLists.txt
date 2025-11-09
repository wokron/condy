cmake_minimum_required(VERSION 3.5)

project(condy VERSION 0.1.0 LANGUAGES CXX)

option(BUILD_EXAMPLES "Build examples" OFF)
option(BUILD_BENCHMARKS "Build benchmarks" OFF)
option(BUILD_TESTS "Build tests" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(condy INTERFACE)
target_include_directories(condy INTERFACE include)

if(BUILD_EXAMPLES OR BUILD_BENCHMARKS OR BUILD_TESTS)
    # liburing
    set(LIBURING_SOURCE_DIR ${CMAKE_SOURCE_DIR}/third_party/liburing)
    add_custom_target(
        build_uring
        ALL
        COMMAND ./configure --cc=${CMAKE_C_COMPILER} --cxx=${CMAKE_CXX_COMPILER}
        COMMAND $(MAKE) -C src
        WORKING_DIRECTORY ${LIBURING_SOURCE_DIR}
    )
    add_library(uring STATIC IMPORTED GLOBAL)
    set_target_properties(uring PROPERTIES IMPORTED_LOCATION ${LIBURING_SOURCE_DIR}/src/liburing.a)
    target_include_directories(uring INTERFACE ${LIBURING_SOURCE_DIR}/src/include)
    add_dependencies(uring build_uring)
endif()

if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(BUILD_TESTS)
    # doctest
    add_subdirectory(third_party/doctest)
    enable_testing()
    add_subdirectory(tests)
endif()