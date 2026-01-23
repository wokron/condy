# Examples

@brief Practical Condy code samples.

- [custom-allocator.cpp](custom-allocator_8cpp_source.html)
    Demonstrates the use of `condy::pmr` and custom memory allocators to improve the performance of task creation and destruction.

- [echo-server.cpp](echo-server_8cpp_source.html)
    A high-concurrency TCP echo server utilizing features like fixed file descriptors.

- [file-server.cpp](file-server_8cpp_source.html)
    A simple HTTP file server using `condy::async_splice` for asynchronous file and network IO.

- [futex-semaphore.cpp](futex-semaphore_8cpp_source.html)
    Implements simple asynchronous semaphores and mutexes based on `condy::async_futex_wait` and `condy::async_futex_wake`, and further builds a high-concurrency producer-consumer queue.

- [link-cp.cpp](link-cp_8cpp_source.html)
    Implements concurrent file copying using features like fixed file descriptors, fixed buffers, and link operations, supporting `O_DIRECT` IO. Achieves up to 2x performance improvement compared to `cp`.