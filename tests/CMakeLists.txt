file(GLOB_RECURSE SRC CONFIGURE_DEPENDS "*.cpp")
add_executable(tests ${SRC})
target_link_libraries(tests PRIVATE doctest condy uring)

add_test(NAME tests COMMAND tests)
