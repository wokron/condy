# Examples

@brief Practical Condy code samples.

- [custom-allocator.cpp](custom-allocator_8cpp_source.html)
    Demonstrates the use of `condy::pmr` and custom memory allocators to improve the performance of task creation and destruction.

- [echo-server.cpp](echo-server_8cpp_source.html)
    A high-concurrency TCP echo server utilizing features like fixed file descriptors.

- [file-server.cpp](file-server_8cpp_source.html)
    A simple HTTP file server using `condy::async_splice` for asynchronous file and network IO.

- [link-cp.cpp](link-cp_8cpp_source.html)
    Implements concurrent file copying using features like fixed file descriptors, fixed buffers, and link operations, supporting `O_DIRECT` IO. Achieves up to 2x performance improvement compared to `cp`.

- [queue-condy-futex.cpp](queue-condy-futex_8cpp_source.html)
    Builds a producer-consumer queue through `condy::Futex`, implementing asynchronous mutex and condition variable for synchronization.

- [queue-kernel-futex.cpp](queue-kernel-futex_8cpp_source.html)
    Builds a producer-consumer queue through asynchronous futex syscalls (`condy::async_futex_wait()`), implementing an asynchronous semaphore for synchronization.
