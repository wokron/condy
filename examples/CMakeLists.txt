option(BUILD_EXAMPLES_BPF "Build BPF example" OFF)
option(EXAMPLES_STATIC_LINK "Use static linking for examples" OFF)

if (EXAMPLES_STATIC_LINK)
    add_link_options(-static)
endif()

add_executable(echo-server echo-server.cpp)
target_link_libraries(echo-server PRIVATE condy uring)

add_executable(link-cp link-cp.cpp)
target_link_libraries(link-cp PRIVATE condy uring)

add_executable(custom-allocator custom-allocator.cpp)
target_link_libraries(custom-allocator PRIVATE condy uring)

add_executable(file-server file-server.cpp)
target_link_libraries(file-server PRIVATE condy uring)

add_executable(futex-semaphore futex-semaphore.cpp)
target_link_libraries(futex-semaphore PRIVATE condy uring)

if (BUILD_EXAMPLES_BPF)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/vmlinux.h
        COMMAND bpftool btf dump file /sys/kernel/btf/vmlinux format c > ${CMAKE_CURRENT_BINARY_DIR}/vmlinux.h
    )
    add_custom_target(vmlinux_header DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/vmlinux.h)

    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/bpf_example.bpf.o
        COMMAND clang -O2 -g -target bpf -I${CMAKE_CURRENT_BINARY_DIR} -c ${CMAKE_CURRENT_SOURCE_DIR}/bpf_example.bpf.c -o ${CMAKE_CURRENT_BINARY_DIR}/bpf_example.bpf.o
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/bpf_example.bpf.c ${CMAKE_CURRENT_BINARY_DIR}/vmlinux.h
    )
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/bpf_example.skel.h
        COMMAND bpftool gen skeleton ${CMAKE_CURRENT_BINARY_DIR}/bpf_example.bpf.o > ${CMAKE_CURRENT_BINARY_DIR}/bpf_example.skel.h
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/bpf_example.bpf.o
    )
    add_custom_target(bpf_example_skel DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/bpf_example.skel.h)

    add_executable(bpf-example bpf-example.cpp)
    add_dependencies(bpf-example bpf_example_skel)
    target_include_directories(bpf-example PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(bpf-example PRIVATE condy uring bpf z elf zstd)
endif()
