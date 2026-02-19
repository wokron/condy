# User Guide

@brief Step-by-step introduction to Condy’s concepts and usage.

## Running Coroutines

This section introduces how to define coroutine functions, as well as how to run and manage coroutines in Condy.

### Defining a Coroutine

To define a coroutine, declare a function that returns `condy::Coro<T>`, where `T` is the return type (default is `void`). Coroutines can use `co_await` to await asynchronous operations or other coroutines.

```cpp
#include <condy.hpp>

// Coroutine returning an integer
condy::Coro<int> get_answer() {
    co_return 42;
}

// Coroutine with no return value
condy::Coro<void> do_work() {
    std::cout << "Working...\n";
    co_return;
}

// Coroutines can co_await each other
condy::Coro<int> caller() {
    int answer = co_await get_answer();
    co_return answer * 2;  // 84
}
```

### Running a Coroutine

Use `condy::sync_wait()` to run a coroutine and block until all spawned tasks are finished.

In the following example, only a single coroutine task is created:

```cpp
int main() {
    condy::sync_wait(caller());
    return 0;
}
```

### Running on a Runtime

You can explicitly create a `condy::Runtime` object to manage the event loop and run coroutines on it. Pass the runtime to `condy::sync_wait(Runtime&, Coro)` to run the coroutine on the specified runtime.

The following code is equivalent to `condy::sync_wait(caller())`.

```cpp
// Equivalent to condy::sync_wait(caller())
condy::Runtime runtime(condy::default_runtime_options());
condy::sync_wait(runtime, caller());
```

`condy::default_runtime_options()` returns a global `condy::RuntimeOptions` object. You can also create your own `condy::RuntimeOptions` for custom configuration.

Besides `condy::sync_wait()`, you can also run the runtime directly. The difference is that running the runtime directly will not exit even if there are no tasks.

```cpp
condy::Runtime runtime;
runtime.run(); // Runs event loop; does not exit even if no tasks are present
```

Use the `condy::Runtime::allow_exit()` function to allow the runtime to exit when there are no tasks.

```cpp
runtime.allow_exit();
runtime.run();
```

### Task Management

You can use `condy::co_spawn()` to start a coroutine as a task. Different tasks within the same runtime will execute concurrently. The `condy::co_spawn()` function returns a `condy::Task<T>` object. The task object can be awaited inside a coroutine using `co_await`, or synchronously waited outside a coroutine using `condy::Task<T>::wait()`. You can also detach a task to let it run independently.

```cpp
condy::Coro<int> func() {
    co_return 42;
}

int main() {
    condy::Runtime runtime;
    condy::Task<int> task = condy::co_spawn(runtime, func());
    runtime.allow_exit();
    runtime.run();
    int r = task.wait();
    // ...
}
```

You can spawn tasks on different runtimes. Waiting for or detaching a task is thread-safe. 

> [!WARNING]  
> If a task is neither awaited nor detached, the program will terminate. If an uncaught exception occurs in a detached task, the program will also terminate.

Inside a coroutine, you can call `condy::co_spawn(func())` to run another task on the current runtime, without passing the `runtime` parameter.

```cpp
condy::Coro<void> parent() {
    condy::co_spawn(child()).detach();
    // Continue with parent logic...
}
```

## Asynchronous Operations

io_uring provides a rich set of asynchronous operations, covering not only I/O but also various system calls. Condy builds on top of these interfaces, offering well-designed abstractions and wrappers, making Condy a true asynchronous system call layer.

In addition to these asynchronous operations, Condy also provides a `condy::Channel` type, similar to the channel in Go. As a fundamental component of Condy, Channel can be combined with other mechanisms to implement more complex asynchronous control logic.

### io_uring Operations

Condy offers lightweight wrappers for most io_uring operations. Essentially, each `condy::async_*()` function corresponds to an `io_uring_prep_*()` function. In liburing, `io_uring_prep_*()` is used to prepare an asynchronous operation.

The `condy::async_*` functions return awaitable objects. You need to use `co_await` to submit the operation to the io_uring backend and asynchronously wait for its completion.

The following example creates 5 coroutine tasks, each calling `condy::async_timeout()` to wait for 2 seconds in a non-blocking way. `condy::async_timeout()` corresponds to `io_uring_prep_timeout()` in liburing and uses the same parameters.

```cpp
#include <chrono>
#include <condy.hpp>

condy::Coro<void> sleep_task(int no, int seconds) {
    __kernel_timespec ts = {
        .tv_sec = seconds,
        .tv_nsec = 0,
    };

    int r = co_await condy::async_timeout(&ts, 0, 0);
    (void)r;
    std::printf("Task %d: Wake up\n", no);
}

condy::Coro<void> co_main(int times, int sleep_seconds) {
    auto start = std::chrono::steady_clock::now();
    std::vector<condy::Task<void>> tasks;
    tasks.reserve(times);
    for (int i = 0; i < times; ++i) {
        tasks.push_back(condy::co_spawn(sleep_task(i, sleep_seconds)));
    }
    for (auto &t : tasks) {
        co_await t;
    }
    auto end = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = end - start;
    std::printf("All tasks completed in %.2f seconds\n", elapsed.count());
}

int main() {
    condy::sync_wait(co_main(5, 2));
    return 0;
}
```

Since `condy::async_timeout()` is an asynchronous operation, it can be executed concurrently in each task. As a result, the total time for all tasks to complete is still 2 seconds.

```text
Task 4: Wake up
Task 3: Wake up
Task 2: Wake up
Task 1: Wake up
Task 0: Wake up
All tasks completed in 2.00 seconds
```

Condy is not just a simple wrapper around liburing functions. Through carefully designed mechanisms, it provides intuitive and expressive interfaces for many io_uring-specific features. These designs will be explained in detail in later sections.

### Channel

Condy introduces the `condy::Channel` type, which is a thread-safe, bounded, buffered or unbuffered queue. `condy::Channel` is a building block for many advanced features in Condy.

`condy::Channel` supports both synchronous (`condy::Channel::try_push()`/`condy::Channel::try_pop()`) and asynchronous (`condy::Channel::push()`/`condy::Channel::pop()`) operations. For asynchronous operations, `condy::Channel::push()` and `condy::Channel::pop()` return awaitable objects, which you can submit and wait for using `co_await`. This is similar to the `condy::async_*()` functions.

You can also close a channel using the `condy::Channel::push_close()` function. After closing, any subsequent `condy::Channel::try_push()` or `condy::Channel::push()` operations are invalid.

The following example creates a producer task and a consumer task.

```cpp
#include "condy/runtime.hpp"
#include <condy.hpp>

condy::Coro<void> producer(condy::Channel<int> &ch) {
    for (int i = 0; i < 10; ++i) {
        std::printf("Producing: %d\n", i);
        co_await ch.push(i);
    }
    co_return;
}

condy::Coro<void> consumer(condy::Channel<int> &ch) {
    for (int i = 0; i < 10; ++i) {
        int value = co_await ch.pop();
        std::printf("Consumed: %d\n", value);
    }
    co_return;
}

int main() {
    condy::Channel<int> ch(2);
    condy::Runtime runtime;
    condy::co_spawn(runtime, producer(ch)).detach();
    condy::co_spawn(runtime, consumer(ch)).detach();
    runtime.allow_exit();
    runtime.run();
    return 0;
}
```

Output:

```text
Producing: 0
Producing: 1
Producing: 2
Consumed: 0
Consumed: 1
Consumed: 2
Producing: 3
Producing: 4
Producing: 5
Producing: 6
Consumed: 3
Consumed: 4
Consumed: 5
Consumed: 6
Producing: 7
Producing: 8
Producing: 9
Consumed: 7
Consumed: 8
Consumed: 9
```

How to combine `condy::Channel` with other Condy features will be introduced in later sections.

## Composing and Controlling Asynchronous Operations

This section introduces methods for composing and controlling asynchronous operations in Condy. These methods provide support for certain io_uring features, enabling richer semantics and finer-grained control over program flow.

### Composing Operations

Condy provides a set of combinator functions to compose multiple asynchronous operations, allowing you to express complex async logic in an intuitive way.

#### Wait for All

You can use `condy::when_all()` or `condy::operators::operator&&` to wait for a group of operations to all complete. All operations will start concurrently, and the coroutine resumes when all have finished.

`condy::when_all()` can accept multiple different Awaiters as input, or a container of Awaiters of the same type.

The following example reads user input and writes the data to both a file and standard output concurrently.

```cpp
#include <condy.hpp>

condy::Coro<int> co_main() {
    using condy::operators::operator&&; // Import concurrent operator

    int fd = co_await condy::async_openat(AT_FDCWD, "result.txt",
                                          O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        co_return 1; // Fail to open file
    }

    char buffer[128];
    size_t offset = 0;

    while (true) {
        int r =
            co_await condy::async_read(STDIN_FILENO, condy::buffer(buffer), 0);
        if (r < 0) {
            co_return 1;
        } else if (r == 0) {
            break; // EOF
        }

        // Concurrently write to stdout and file
        auto [r1, r2] = co_await (
            condy::async_write(STDOUT_FILENO, condy::buffer(buffer, r), 0) &&
            condy::async_write(fd, condy::buffer(buffer, r), offset));
        if (r1 < 0 || r2 < 0) {
            co_return 1;
        }

        offset += r;
    }

    co_return 0;
}

int main() { return condy::sync_wait(co_main()); }
```

#### Wait for Any

You can use `condy::when_any()` or `condy::operators::operator||` to wait for any one of a group of operations to complete. All operations start concurrently, and once any operation completes, the others are cancelled.

The parameter types accepted by `condy::when_any()` are the same as those for `condy::when_all()`.

> [!NOTE]  
> `condy::when_any()` does not guarantee that only one operation will complete, but it only returns the result of the first completed operation. If you need the results of all completed operations, see `condy::parallel()` in later sections.

The following example waits for user input, and exits if there is no input within 5 seconds.

```cpp
#include <condy.hpp>

condy::Coro<int> co_main() {
    using condy::operators::operator||; // Import racing operator

    // Set a 5-second timeout
    __kernel_timespec ts = {
        .tv_sec = 5,
        .tv_nsec = 0,
    };

    char buffer[128];

    while (true) {
        // Wait concurrently for input or timeout
        std::variant<int, int> res = co_await (
            condy::async_read(STDIN_FILENO, condy::buffer(buffer), 0) ||
            condy::async_timeout(&ts, 0, 0));

        // Timeout
        if (res.index() == 1) {
            int r = co_await condy::async_write(
                STDOUT_FILENO, condy::buffer(std::string("Bye!\n")), 0);
            co_return r < 0 ? 1 : 0;
        }

        // Normal input
        int r = std::get<0>(res);
        if (r < 0) {
            co_return 1;
        }
        r = co_await condy::async_write(
            STDOUT_FILENO, condy::buffer(std::string("Got it!\n")), 0);
        if (r < 0) {
            co_return 1;
        }
    }
}

int main() { return condy::sync_wait(co_main()); }
```

The `push()` and `pop()` methods of `condy::Channel` are also asynchronous operations, so they can be passed to `condy::when_any()`. This allows `condy::Channel` to be used as a signal slot for cancellation.

```cpp
auto r = co_await (condy::async_read(...) || cancel_slot.pop());
```

#### Linking Operations

This is an io_uring feature. io_uring supports linking a group of asynchronous operations so that they are executed sequentially in the backend until all operations are complete. This reduces the number of syscalls and improves performance.

You can use `condy::link()` or `condy::operators::operator>>` to compose a group of asynchronous operations. All operations will be executed in the linked order, returning when all are complete or an error occurs.

The parameter types accepted by `condy::link()` are the same as those for `condy::when_all()`.

`condy::hard_link()` is a variant of `condy::link()`. Even if an intermediate operation fails, `condy::hard_link()` will continue to execute subsequent operations.

The following example copies data from a file `input.txt` to another file `output.txt`, with the read and write operations linked together.

```cpp
#include <condy.hpp>

condy::Coro<int> co_main() {
    using condy::operators::operator>>; // Linked operator
    using condy::operators::operator&&;

    int fd_in =
        co_await condy::async_openat(AT_FDCWD, "input.txt", O_RDONLY, 0);
    if (fd_in < 0) {
        co_return 1;
    }

    int fd_out = co_await condy::async_openat(
        AT_FDCWD, "output.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_out < 0) {
        co_await condy::async_close(fd_in);
        co_return 1;
    }

    char buffer[128];
    size_t offset = 0;

    while (true) {
        // Linked operations: read first, then write
        // If fewer bytes are read than the buffer size, EOF is reached
        // io_uring will automatically cancel the subsequent write operation
        auto [r1, r2] = co_await (
            condy::async_read(fd_in, condy::buffer(buffer), offset) >>
            condy::async_write(fd_out, condy::buffer(buffer), offset));
        if (r1 < 0) {
            co_return 1;
        }
        if (r1 < sizeof(buffer)) {
            break;
        }

        offset += sizeof(buffer);
    }

    // Handle remaining data
    int r = co_await condy::async_read(fd_in, condy::buffer(buffer), offset);
    co_await condy::async_write(fd_out, condy::buffer(buffer, r), offset);

    co_await (condy::async_close(fd_in) && condy::async_close(fd_out));
    co_return 0;
}

int main() { return condy::sync_wait(co_main()); }
```

#### Other Combinators

In addition to `condy::when_all()` and `condy::when_any()`, users may sometimes need more information, such as the results of all completed operations in `condy::when_any()`.

Condy provides the `condy::parallel()` function, which is a lower-level interface beneath `condy::when_all()` and `condy::when_any()`. Users can specify the Awaiter type to control the return result of the composed operation.

For example, you can set the Awaiter type to `condy::ParallelAnyAwaiter`. The return type of this Awaiter is `std::pair<std::array<size_t, N>, std::tuple<...>>`, where the first element is the completion order of all asynchronous operations, and the second is the results of all operations. This allows you to implement more complex control logic.

```cpp
auto [order, results] = co_await condy::parallel<condy::ParallelAnyAwaiter>(aw1, aw2);
```

### Controlling Single Operations

This is an io_uring feature. io_uring provides a series of flags to control the behavior of individual asynchronous operations, such as `IOSQE_IO_DRAIN` and `IOSQE_ASYNC`. The former delays the execution of the operation until all previously submitted operations have completed; the latter forces the operation to always execute asynchronously.

Condy wraps these configurations as `condy::drain()` and `condy::always_async()` functions.

The following example uses `condy::drain()` to decorate a `condy::async_fsync()` operation, ensuring it is executed only after all write operations have completed.

```cpp
#include <condy.hpp>

condy::Coro<int> co_main() {
    using condy::operators::operator&&;

    int fd = co_await condy::async_openat(AT_FDCWD, "output.txt",
                                          O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        co_return 1;
    }

    std::string msg = "Hello, condy!\n";

    // Prepare a series of write operations
    using WriteOp = decltype(condy::async_write(fd, condy::buffer(msg), 0));
    std::vector<WriteOp> write_ops;
    write_ops.reserve(20);
    for (int i = 0; i < 20; ++i) {
        write_ops.push_back(
            condy::async_write(fd, condy::buffer(msg), i * msg.size()));
    }

    // Execute writes concurrently, then execute fsync after all writes have finished
    co_await (condy::when_all(std::move(write_ops)) &&
              condy::drain(condy::async_fsync(fd, 0)));

    co_await condy::async_close(fd);
    co_return 0;
}

int main() { return condy::sync_wait(co_main()); }
```

## io_uring Feature Support

In addition to the io_uring features related to asynchronous operations mentioned above, Condy also supports many other io_uring-specific features. In most asynchronous frameworks, these features are difficult to fully utilize due to cross-platform requirements. By building directly on io_uring, Condy provides a wealth of easy-to-use interfaces, allowing users to leverage kernel features and fully exploit hardware performance.

### Multishot Operations

Multishot operations are a special type of io_uring operation. These operations only need to be submitted once but can produce multiple results. In liburing, multishot functions include `io_uring_prep_multishot_accept()`, `io_uring_prep_read_multishot()`, and others.

Condy supports multishot operations. Unlike regular operations, you need to pass an additional callback function to the multishot operation. For every result except the last, the callback is invoked for processing; only the last result resumes the coroutine.

Condy provides several helper functions to simplify writing callbacks, including:

- `condy::will_spawn()`: Each callback invocation spawns a new coroutine task.
- `condy::will_push()`: Each callback invocation pushes the result into a `condy::Channel` object.

The following example shows how to use `condy::async_multishot_accept()` to create a simple TCP server.

```cpp
#include <arpa/inet.h>
#include <condy.hpp>

// Session coroutine: send message to client and close connection
condy::Coro<void> session(int session_fd) {
    std::string msg = "Hello, Condy!\n";
    co_await condy::async_write(session_fd, condy::buffer(msg), 0);
    co_await condy::async_close(session_fd);
    co_return;
}

condy::Coro<int> co_main() {
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

    int server_fd = co_await condy::async_socket(AF_INET, SOCK_STREAM, 0, 0);
    if (server_fd < 0) {
        co_return 1;
    }

    int r = ::bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (r < 0) {
        co_await condy::async_close(server_fd);
        co_return 1;
    }

    r = ::listen(server_fd, 10);
    if (r < 0) {
        co_await condy::async_close(server_fd);
        co_return 1;
    }

    sockaddr_in client_addr{};
    socklen_t client_addr_len = sizeof(client_addr);

    // Multishot accept: for each new client connection, spawn a session coroutine
    co_await condy::async_multishot_accept(
        server_fd, (struct sockaddr *)&client_addr, &client_addr_len, 0,
        condy::will_spawn(session));

    co_await condy::async_close(server_fd);
    co_return 0;
}

int main() { return condy::sync_wait(co_main()); }
```

For asynchronous operations like `condy::async_read_multishot()`, spawning a coroutine for each result may not be ideal. Instead, you can use `condy::will_push()` to push results into a `condy::Channel`, and have another coroutine process them sequentially.

> [!NOTE]  
> While keeping callbacks in coroutines may seem odd, it ensures sufficient extensibility. For intermediate results, some operations (like accept) may want to process them immediately, while others (like read) may prefer to process results sequentially. We retain callbacks to allow users to flexibly handle intermediate results according to their needs, without introducing extra overhead.

### Zero Copy Tx

Zero Copy Tx is another special type of io_uring operation. When successful, such operations return twice: the first time indicates the operation is complete, and the second time indicates the corresponding buffer is no longer needed.

When supported by hardware, using such operations allows the NIC to send data directly from user memory, avoiding user-to-kernel data copies. In liburing, zero copy Tx include functions like `io_uring_prep_send_zc()`.

Similar to multishot operations, these asynchronous operations in Condy also require a callback function. Condy manages the callback's lifetime and invokes it when the buffer is no longer needed.

The following example shows how to use `condy::async_send_zc()` and a `condy::Channel` to ensure the buffer is not released before the callback. You can also provide a custom callback, such as using `delete` or `free()` to release memory.

```cpp
// This example is modified from the Multishot example,
// only the session function is changed to use zero-copy
condy::Coro<void> session(int session_fd) {
    condy::Channel<int> ch(1);  // Channel to wait for zero-copy send completion
    std::string msg = "Hello, Condy!\n";

    // Zero-copy send: push result into Channel after send completes
    co_await condy::async_send_zc(session_fd, condy::buffer(msg), 0, 0,
                                  condy::will_push(ch));

    co_await condy::async_close(session_fd);

    // Wait until zero-copy send finishes
    co_await ch.pop();

    co_return;
}
```

> [!NOTE]  
> io_uring also supports Zero Copy Rx. Condy will support this feature in the future.

### File Registration

io_uring allows you to register files with the kernel. Normally, each asynchronous operation increments/decrements the file's reference count, but registering files with the kernel can skip this process and improve performance.

Condy abstracts file registration as operations on the `condy::FdTable` type. Each `condy::Runtime` has an `condy::FdTable` object, accessible via `condy::Runtime::fd_table()`.

- Call `condy::FdTable::init()` to initialize the table's capacity.
- Call `condy::FdTable::update()` to register or unregister specific files with the kernel.
- See the API documentation for more details on other available `condy::FdTable` functions.

For files registered with the kernel, you can use their index in the `condy::FdTable` instead of the file descriptor for asynchronous operations. Use `condy::fixed(int)` to convert an `int` to a `FixedFd` type, then pass it to async operation functions. io_uring will treat the argument as a registered file index.

Some async operations have Direct variants. These operations, which would normally return a file descriptor, instead register the file with the kernel and return its index. You need to specify the index or use `CONDY_FILE_INDEX_ALLOC` to let the operation choose a free slot.

The following example uses a fixed fd instead of a regular fd to open and write to a file.

```cpp
#include <condy.hpp>

condy::Coro<int> co_main() {
    // Initialize fd table with capacity 4
    condy::current_runtime().fd_table().init(4);

    // Create file and register to FdTable; CONDY_FILE_INDEX_ALLOC allocates a free slot
    int fd = co_await condy::async_openat_direct(
        AT_FDCWD, "result.txt", O_CREAT | O_WRONLY, 0644,
        CONDY_FILE_INDEX_ALLOC);

    if (fd < 0) {
        // If returns -ENOBUFS, no free slot is available in the FdTable
        co_return 1;
    }

    // Use fixed fd in async write operation
    std::string msg = "Hello, condy!\n";
    int r = co_await condy::async_write(condy::fixed(fd), condy::buffer(msg), 0);
    if (r < 0) {
        co_return 1;
    }

    co_return 0;
}

int main() { return condy::sync_wait(co_main()); }
```

### Buffer Registration

Similar to files, each io_uring async operation needs to acquire a reference to memory pages. Pre-registering memory regions with the kernel can avoid this overhead and improve performance.

Condy abstracts buffer registration as operations on the `condy::BufferTable` type. Each `condy::Runtime` has a `condy::BufferTable` object, accessible via `condy::Runtime::buffer_table()`.

- Call `condy::BufferTable::init()` to initialize the table's capacity.
- Call `condy::BufferTable::update()` to register or unregister specific memory regions with the kernel.
- See the API documentation for more details on other available `condy::BufferTable` functions.

For async operations that require a buffer, you can optionally pass the index of a registered region to optimize the operation. Note that the buffer used in the async operation does not need to be the entire registered region, only within it. Use `condy::fixed(index, buf)` to attach registration info to the buffer.

The following example demonstrates how to use buffer registration.

```cpp
#include <condy.hpp>

condy::Coro<int> co_main() {
    // Get the buffer registration table of the current Runtime
    auto &table = condy::current_runtime().buffer_table();
    table.init(4); // Initialize capacity to 4

    int fd = co_await condy::async_openat(AT_FDCWD, "result.txt",
                                          O_CREAT | O_WRONLY, 0644);
    if (fd < 0) {
        co_return 1;
    }

    // Register string in fixed buffer slot 0
    std::string msg = "Hello, condy!\n";
    iovec iov = {.iov_base = msg.data(), .iov_len = msg.size()};
    table.update(0, &iov, 1);

    // Use fixed buffer in async write operation
    int r =
        co_await condy::async_write(fd, condy::fixed(0, condy::buffer(msg)), 0);
    if (r < 0) {
        co_return 1;
    }

    // Unregister fixed buffer
    iov = {.iov_base = nullptr, .iov_len = 0};
    table.update(0, &iov, 1);

    co_return 0;
}

int main() { return condy::sync_wait(co_main()); }
```

### Provided Buffers

io_uring supports pre-providing a set of buffers for I/O operations. Condy provides the `condy::ProvidedBufferQueue` and `condy::ProvidedBufferPool` types to support this feature.

`condy::ProvidedBufferQueue` wraps the liburing interface. It is a bounded queue; you can add a buffer to the queue using `condy::ProvidedBufferQueue::push()`, which returns an incrementing id to identify the buffer.

You can pass a `condy::ProvidedBufferQueue` object as a substitute for a normal buffer in async operations. After the operation completes, it returns a `condy::BufferInfo` object indicating which buffers in the queue were consumed.

`condy::ProvidedBufferPool` provides more advanced functionality. It manages a set of buffers internally. After an async operation using this type completes, it returns a `condy::ProvidedBuffer` object. This is an RAII type; when the object is destroyed, its buffer is returned to the pool.

You can use `condy::bundled()` to decorate `condy::ProvidedBufferQueue` and `condy::ProvidedBufferPool` objects. In this case, the async operation may consume multiple buffers at once, and the return type will change accordingly.

See the API documentation for details on how to use `condy::ProvidedBufferQueue` and `condy::ProvidedBufferPool`.

The following example demonstrates using a `condy::ProvidedBufferPool` as a buffer pool for async operations to implement an echo server.

```cpp
#include <arpa/inet.h>
#include <condy.hpp>

// Background coroutine: handle buffers from the Channel
condy::Coro<void>
handle_buffers(condy::Channel<std::pair<int, condy::ProvidedBuffer>> &ch,
               int session_fd) {
    while (true) {
        auto [r, buffer] = co_await ch.pop(); // Asynchronously pop data
        if (r == 0) {
            break; // Termination signal
        }
        co_await condy::async_write(session_fd, condy::buffer(buffer.data(), r),
                                    0);
    }
}

// Main coroutine: receive data and use buffer pool
condy::Coro<int> co_main() {
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

    int server_fd = co_await condy::async_socket(AF_INET, SOCK_STREAM, 0, 0);
    if (server_fd < 0) {
        co_return 1;
    }

    int r =
        ::bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (r < 0) {
        co_await condy::async_close(server_fd);
        co_return 1;
    }

    r = ::listen(server_fd, 10);
    if (r < 0) {
        co_await condy::async_close(server_fd);
        co_return 1;
    }

    // Accept one client connection
    sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    int session_fd = co_await condy::async_accept(
        server_fd, (struct sockaddr *)&client_addr, &client_addr_len, 0);
    if (session_fd < 0) {
        co_await condy::async_close(server_fd);
        co_return 1;
    }

    // Create buffer pool: 4 buffers of 32 bytes
    condy::ProvidedBufferPool pool(4, 32);
    condy::Channel<std::pair<int, condy::ProvidedBuffer>> ch(16);

    // Spawn background task to handle writes
    condy::co_spawn(handle_buffers(ch, session_fd)).detach();

    while (true) {
        // Multishot receive: callback handles normal results;
        // coroutine resumes on final error or termination
        auto [res, buf] = co_await condy::async_recv_multishot(
            session_fd, pool, 0, condy::will_push(ch));

        if (res == 0) {
            // Termination signal
            ch.push_close();
            break;
        }

        // Error handling (excluding buffer pool full)
        if (res < 0 && res != -ENOBUFS) {
            ch.push_close();
            co_return 1;
        }
    }

    co_return 0;
}

int main() { return condy::sync_wait(co_main()); }
```

### Initialization Options

As mentioned earlier, the `condy::Runtime` type can accept a `condy::RuntimeOptions` object, which contains a series of configurable initialization parameters for `condy::Runtime`. These parameters can be set using chained calls as shown below:

```cpp
auto options = condy::RuntimeOptions()
    .sq_size(32)
    .enable_sqpoll();
condy::Runtime runtime(options);
```

`condy::RuntimeOptions` provides wrappers for io_uring setup options. For details, see the API documentation and liburing documentation.

### Runtime Configuration

After creating a `condy::Runtime` object, you may need to adjust some settings dynamically. Condy associates each `condy::Runtime` with a `condy::RingSettings` object, accessible via `condy::Runtime::settings()`.

The `condy::RingSettings` object wraps various io_uring configuration options, providing features such as NAPI, Clock, and more. For details, see the API documentation and liburing documentation.

## Others

This section describes features that are not directly related to io_uring.

### Basic Buffers

In previous examples, you may have noticed the use of `condy::buffer()` when passing buffer arguments to asynchronous operations. Since io_uring supports multiple advanced buffer-related features (such as buffer registration and provided buffers), Condy overloads the `async_*` functions so they can accept ordinary buffers, fixed buffers, or provided buffers directly. This approach keeps the API clean and consistent.

All asynchronous operations in Condy that require a buffer accept a single buffer parameter, regardless of the underlying buffer type or io_uring feature being used. For ordinary buffers (such as a pointer and size, or a `std::string`/`std::vector`), you can use `condy::buffer()` to convert them into a single basic buffer object (`condy::MutableBuffer` or `condy::ConstBuffer`) suitable for async operations.

This design allows you to write concise and flexible code, and makes it easy to switch between different buffer management strategies as needed.

Example:

```cpp
char buf[128];
co_await condy::async_read(fd, condy::buffer(buf, sizeof(buf)), 0);

std::string msg = "Hello, Condy!";
co_await condy::async_write(fd, condy::buffer(msg), 0);
```

For advanced scenarios (such as using fixed buffers or provided buffers), you can pass the corresponding buffer object directly to the same async function, thanks to function overloading.

### Custom Coroutine Allocator

The second template parameter of `condy::Coro<T, Allocator>` can be used to specify a custom allocator for the coroutine frame. The default is `void`, which uses the system default allocator.

* When using a custom allocator, the first argument of the coroutine function must match the allocator type.
* `Coro` also provides support for `pmr` allocators through the type `condy::pmr::Coro<T>`.

Example:

```cpp
#include <condy.hpp>
#include <memory_resource> // std::pmr

// Coroutine using a custom allocator
// The coroutine's first argument must match the allocator type
condy::pmr::Coro<int> co_main(auto &allocator) {
    co_return 42;
}

int main() {
    // Create PMR memory resource (lifetime must cover coroutine execution)
    std::pmr::monotonic_buffer_resource pool;

    // Create polymorphic allocator from the memory resource
    std::pmr::polymorphic_allocator<std::byte> allocator(&pool);

    // Pass the allocator to the coroutine to allocate its frame
    // This allows customizing memory management of the coroutine for performance or other needs
    return condy::sync_wait(co_main(allocator));
}
```

### Switching Runtime

Condy provides `condy::co_switch` to move a coroutine to a different runtime. After `co_await condy::co_switch(other_runtime)`, the current coroutine will continue execution in the specified runtime. This is useful for load balancing across multiple runtimes:

```cpp
condy::Runtime runtime1, runtime2;

condy::Coro<void> func() {
    // Running on runtime1
    co_await condy::co_switch(runtime2);
    // Now running on runtime2
    co_return;
}

condy::co_spawn(runtime1, func());
```