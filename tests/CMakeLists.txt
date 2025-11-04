file(GLOB SRC CONFIGURE_DEPENDS "*.cpp")
add_executable(tests ${SRC})
target_link_libraries(tests PRIVATE doctest condy uring)
target_compile_options(tests PRIVATE -fsanitize=thread)
target_link_options(tests PRIVATE -fsanitize=thread)

add_test(NAME tests COMMAND tests)

add_subdirectory(test_crash)