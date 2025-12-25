macro(add_crash_test test_name)
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE condy uring)
    add_test(NAME ${test_name}_expected COMMAND ${test_name})
    set_tests_properties(${test_name}_expected PROPERTIES WILL_FAIL TRUE)
endmacro()

add_crash_test(test_task_not_join)
add_crash_test(test_detached_task_exception)
