add_executable(echo-server echo-server.cpp)
target_link_libraries(echo-server PRIVATE condy uring)

add_executable(link-cp link-cp.cpp)
target_link_libraries(link-cp PRIVATE condy uring)

add_executable(custom-allocator custom-allocator.cpp)
target_link_libraries(custom-allocator PRIVATE condy uring)

add_executable(file-server file-server.cpp)
target_link_libraries(file-server PRIVATE condy uring)

add_executable(queue-kernel-futex queue-kernel-futex.cpp)
target_link_libraries(queue-kernel-futex PRIVATE condy uring)

add_executable(queue-condy-futex queue-condy-futex.cpp)
target_link_libraries(queue-condy-futex PRIVATE condy uring)