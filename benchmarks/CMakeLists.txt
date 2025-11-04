add_executable(spawn_tasks spawn_tasks.cpp)
target_link_libraries(spawn_tasks PRIVATE condy uring)