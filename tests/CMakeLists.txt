file(GLOB SRC CONFIGURE_DEPENDS "*.cpp")
add_executable(tests ${SRC})
target_link_libraries(tests PRIVATE doctest condy uring)

if (TESTS_STATIC_LINK)
    target_link_options(tests PRIVATE -static)
elseif (TESTS_TSAN)
    target_compile_options(tests PRIVATE -fsanitize=thread)
    target_link_options(tests PRIVATE -fsanitize=thread)
elseif (TESTS_ASAN)
    target_compile_options(tests PRIVATE -fsanitize=address,undefined)
    target_link_options(tests PRIVATE -fsanitize=address,undefined)
endif()

add_test(NAME tests COMMAND tests)

add_subdirectory(test_crash)