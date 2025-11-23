file(GLOB SRC CONFIGURE_DEPENDS "*.cpp")
add_executable(tests ${SRC})
target_link_libraries(tests PRIVATE doctest condy uring)

if  (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(tests PRIVATE -fsanitize=thread)
    target_link_options(tests PRIVATE -fsanitize=thread)
endif()

add_test(NAME tests COMMAND tests)

add_subdirectory(test_crash)