add_executable(spawn_tasks spawn_tasks.cpp)
target_link_libraries(spawn_tasks PRIVATE condy uring)

add_executable(channel channel.cpp)
target_link_libraries(channel PRIVATE condy uring)