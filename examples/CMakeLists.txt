add_executable(echo-server echo-server.cpp)
target_link_libraries(echo-server PRIVATE condy uring)

add_executable(fast-cp fast-cp.cpp)
target_link_libraries(fast-cp PRIVATE condy uring)

add_executable(cancel-tasks cancel-tasks.cpp)
target_link_libraries(cancel-tasks PRIVATE condy uring)

add_executable(custom-allocator custom-allocator.cpp)
target_link_libraries(custom-allocator PRIVATE condy uring)
