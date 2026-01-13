add_executable(echo-server echo-server.cpp)
target_link_libraries(echo-server PRIVATE condy uring)

add_executable(link-cp link-cp.cpp)
target_link_libraries(link-cp PRIVATE condy uring)

add_executable(custom-allocator custom-allocator.cpp)
target_link_libraries(custom-allocator PRIVATE condy uring)
