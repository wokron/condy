cmake_minimum_required(VERSION 3.5)

project(condy LANGUAGES CXX)

option(BUILD_EXAMPLES "Build examples" OFF)
option(BUILD_BENCHMARKS "Build benchmarks" OFF)
option(BUILD_TESTS "Build tests" OFF)
option(ENABLE_CLANG_TIDY "Enable clang-tidy static analysis" OFF)
option(TESTS_STATIC_LINK "Use static linking for tests" OFF)
option(LINK_LIBURING "Use liburing from third_party and link it to condy" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES "clang-tidy")
    if(CLANG_TIDY_EXE)
        message(STATUS "Found clang-tidy: ${CLANG_TIDY_EXE}")
        set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
    else()
        message(WARNING "clang-tidy not found, static analysis will be disabled")
    endif()
endif()

add_library(condy INTERFACE)
target_include_directories(condy INTERFACE include)

if(BUILD_EXAMPLES OR BUILD_BENCHMARKS OR BUILD_TESTS OR LINK_LIBURING)
    # liburing
    set(LIBURING_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/liburing)
    message(STATUS "Configuring liburing in ${LIBURING_SOURCE_DIR}")
    execute_process(
        COMMAND ./configure --cc=${CMAKE_C_COMPILER} --cxx=${CMAKE_CXX_COMPILER}
        WORKING_DIRECTORY ${LIBURING_SOURCE_DIR}
        OUTPUT_QUIET
        RESULT_VARIABLE LIBURING_CONFIG_RESULT
    )
    if (NOT LIBURING_CONFIG_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to configure liburing")
    else()
        message(STATUS "Configuring liburing succeeded")
    endif()
    add_custom_target(
        build_uring
        ALL
        COMMAND $(MAKE) -C src
        WORKING_DIRECTORY ${LIBURING_SOURCE_DIR}
    )
    add_library(uring STATIC IMPORTED GLOBAL)
    set_target_properties(uring PROPERTIES IMPORTED_LOCATION ${LIBURING_SOURCE_DIR}/src/liburing.a)
    target_include_directories(uring INTERFACE ${LIBURING_SOURCE_DIR}/src/include)
    add_dependencies(uring build_uring)
endif()

if(LINK_LIBURING)
    target_link_libraries(condy INTERFACE uring)
endif()

# Warnings as errors
add_compile_options(-Wall -Wextra -Werror)

if(BUILD_EXAMPLES)
    message(STATUS "Configuring examples in ${CMAKE_CURRENT_SOURCE_DIR}/examples")
    add_subdirectory(examples)
endif()

if(BUILD_BENCHMARKS)
    message(STATUS "Configuring benchmarks in ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks")
    add_subdirectory(benchmarks)
endif()

if(BUILD_TESTS)
    # doctest
    add_subdirectory(third_party/doctest)
    message(STATUS "Configuring tests in ${CMAKE_CURRENT_SOURCE_DIR}/tests")
    enable_testing()
    add_subdirectory(tests)
endif()