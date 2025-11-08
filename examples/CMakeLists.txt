add_executable(echo-server echo-server.cpp)
target_link_libraries(echo-server PRIVATE condy uring)

add_executable(condy-cp condy-cp.cpp)
target_link_libraries(condy-cp PRIVATE condy uring)

add_executable(cancel-tasks cancel-tasks.cpp)
target_link_libraries(cancel-tasks PRIVATE condy uring)