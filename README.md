# Condy🍬

> *Modern C++20 Coroutine Runtime, Built **Natively** on io_uring*

Condy is designed to provide an intuitive, high-performance coroutine runtime on top of io_uring:

* 🔗 **Native io_uring Integration**
  Built directly on top of io_uring, integrating its unique features to fully leverage kernel-level asynchronous I/O.

* 🚀 **High Performance**
  Supports advanced features such as SQPOLL, multishot, zero-copy, and provided buffers to maximize hardware throughput.

* 💡 **Intuitive Programming Model**
  Replace callbacks with `co_await` to avoid callback hell. Combine complex async tasks and enable coroutine-to-coroutine communication using high-level components like `&&` / `||` / `>>` operators and `Channel`.

* 🏃 **Low Overhead**
  Minimize heap allocations. All awaiters are allocated only on the coroutine frame, and the runtime relies on the io_uring event loop for efficient lifecycle management.

* 📦 **Minimal Dependencies**
  Only depends on the C++20 standard library and liburing.

> [!WARNING]
> **Alpha Stage** - Condy is currently in early development. API is not stable and may change without notice.

> [!WARNING]
> **Outdated API in README**  
> Some of the APIs shown in this README are outdated. The documentation will be updated soon to reflect the latest API changes.  
> For the most up-to-date usage, please refer to the source code.

## Quick Start

```cpp
// hello.cpp
#include <condy.hpp>

condy::Coro<> co_main() {
    std::string msg = "Hello, Condy!\n";
    co_await condy::async_write(STDOUT_FILENO, condy::buffer(msg), 0);
}

int main() { condy::sync_wait(co_main()); }
```

```bash
# Make sure liburing (>=2.3) is installed on your system
# On Ubuntu: sudo apt install liburing-dev
clang++ hello.cpp -o hello -std=c++20 -luring -I./include
./hello
# Hello, Condy!
```

## Build and Usage

### Using Condy as a Dependency

You can include Condy in your project via Git Submodule:

```bash
git submodule add https://github.com/wokron/condy.git third_party/condy
git submodule update --init --recursive
```

In your `CMakeLists.txt`:

```cmake
# Add Condy as a subdirectory
add_subdirectory(third_party/condy)

# Create executable and link Condy
add_executable(my_app src/main.cpp)
target_link_libraries(my_app PRIVATE condy)
```

> [!NOTE]
> * C++20 is required because Condy uses coroutines.
> * Condy is a **header-only library** but depends on **liburing ≥ 2.3**.
> * If `LINK_LIBURING=ON` (default), Condy will build and link the bundled liburing from `third_party/liburing`.
> * If `LINK_LIBURING=OFF`, you need to install liburing in your system and link it explicitly.

### Building Examples / Benchmarks / Tests

Condy provides CMake options to build examples, benchmarks, and tests:

```bash
cmake -B build -S . \
    -DBUILD_EXAMPLES=ON \
    -DBUILD_BENCHMARKS=ON \
    -DBUILD_TESTS=ON \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j$(nproc)
```

After building:

* Run all tests:

```bash
ctest --test-dir build
```

* Run example programs directly:

```bash
./build/examples/fast-cp from.bin to.bin
```

## Core Features

### Coroutine

Define a coroutine by returning `Coro<T>` from a function, where `T` is the coroutine’s return type (defaults to `void`).

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

// Coroutines can co_await each other, with semantics similar to normal function calls
condy::Coro<int> caller() {
    int answer = co_await get_answer();
    co_return answer * 2;  // 84
}
```

The `sync_wait` function takes a coroutine object, runs it, and blocks until the coroutine and all tasks it spawns are complete.

```cpp
int main() {
    condy::sync_wait(caller());
    return 0;
}
```

### Task

`Task<T>` represents an independently running task and can be created using `co_spawn`:

```cpp
#include <condy.hpp>

condy::Coro<int> func() {
    // Do some work concurrently
    co_return 42;
}

condy::Coro<void> co_main() {
    condy::Task<int> task = condy::co_spawn(func());
    // Do some other work...
    int result = co_await std::move(task);
}
```

`Task<T>` behaves similarly to `std::thread`. Unless explicitly detached via `detach`, a task must be awaited; otherwise, the program will terminate. Likewise, if an uncaught exception occurs in a detached task, the program will terminate.

Waiting for or detaching a `Task<T>` is thread-safe. Outside of coroutines, you can use `wait` to synchronously block until the task completes.

### Runtime

The `Runtime` object implements an io_uring event loop and can be configured via `RuntimeOptions`. Both `sync_wait` and `co_spawn` can accept a `Runtime` instance, in which case the coroutine will run on that specific runtime:

```cpp
auto options = condy::RuntimeOptions().enable_sqpoll();
condy::Runtime runtime(options);

condy::co_spawn(runtime, func1());
condy::sync_wait(runtime, func2());
```

You can also run a `Runtime` independently using the `run` method. The runtime does not exit even if there are no tasks. Calling `done` signals that the runtime should exit once all tasks are complete:

```cpp
condy::Runtime runtime;
runtime.done();
runtime.run(); // Exits immediately
```

### Async Operations

Condy wraps io_uring operations into a set of `async_*` functions. Typically, `async_{op}` is a wrapper around `io_uring_prep_{op}`. Calling `async_*` returns an awaitable object that must be `co_await`ed to submit and wait for completion:

```cpp
#include <condy.hpp>

condy::Coro<> co_main() {
    char buffer[128];
    while (true) {
        int r =
            co_await condy::async_read(STDIN_FILENO, condy::buffer(buffer), -1);
        if (r <= 0) {
            break;
        }
        co_await condy::async_write(STDOUT_FILENO, condy::buffer(buffer, r),
                                    -1);
    }
}

int main() { condy::sync_wait(co_main()); }
```

## Advanced Features

### Concurrent Operations

Condy provides multiple ways to execute asynchronous operations concurrently:

* `make_all_awaiter` / `operator&&`: wait for all tasks to complete.
* `make_ranged_all_awaiter`: concurrently execute an arbitrary number of identical tasks.
* `make_one_awaiter` / `operator||`: cancel other tasks when one completes.
* `make_ranged_one_awaiter`: similar for arbitrary numbers of identical tasks.

Example of using `operator&&` to write concurrently to stdout and a file:

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

Example of using `operator||` to wait for input or a timeout, cancelling the other when one completes:

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

> [!NOTE]
> `make_one_awaiter` does not guarantee that only a single task completes (see Q&A). To retrieve results of all tasks, use `make_parallel_awaiter` or `make_ranged_parallel_awaiter`. The result is a `std::pair`: the first element is the order of task completion, the second element contains the return values of each task.

### Link Operations (io_uring Feature)

Condy leverages io_uring to provide `make_link_awaiter`, which executes a series of asynchronous operations sequentially:

* Operations are executed in order; returns only when all complete or any operation fails.
* The return type is the same as `make_all_awaiter`.
* `operator>>` in `condy::operators` provides the same functionality.
* `make_ranged_link_awaiter` can execute an arbitrary number of identical operations concurrently.

Example code:

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

### Drain Operations (io_uring Feature)

Condy leverages the drain feature of io_uring:

* A drain operation is executed only after all previously submitted operations complete.
* Create a drain operation via `make_drained_op_awaiter`.
* `operator~` in `condy::operators` marks an operation as a drain.

Example code:

```cpp
#include <condy.hpp>

condy::Coro<int> co_main() {
    using condy::operators::operator&&;
    using condy::operators::operator~;  // Drain operator

    int fd = co_await condy::async_openat(AT_FDCWD, "output.txt",
                                          O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        co_return 1;
    }

    std::string msg = "Hello, condy!\n";

    // Prepare a series of write operations
    using WriteOp = decltype(condy::async_write(fd, condy::buffer(msg), 0));
    std::vector<WriteOp> write_ops;
    for (int i = 0; i < 20; ++i) {
        write_ops.push_back(
            condy::async_write(fd, condy::buffer(msg), i * msg.size()));
    }

    // Execute writes concurrently, then execute fsync after all writes have finished
    co_await (condy::make_ranged_all_awaiter(std::move(write_ops)) &&
              ~condy::async_fsync(fd, 0));

    co_await condy::async_close(fd);
    co_return 0;
}

int main() { return condy::sync_wait(co_main()); }
```

### Channel

`Channel<T>` is a fixed-size, thread-safe queue:

* Use `try_push` / `try_pop` to attempt immediate insertion or removal.
* In coroutines, `push` / `pop` are asynchronous operations that wait until an element is available.
* Async `push` / `pop` can run concurrently with `async_*` operations, enabling cancellation logic.

Example code:

```cpp
#include <condy.hpp>

// Background task: prints "Tick" every second until main coroutine sets done = true
condy::Coro<void> background_task(condy::Channel<std::monostate> &chan,
                                  bool &done) {
    using condy::operators::operator||; // Concurrent operator

    __kernel_timespec ts = {
        .tv_sec = 1,
        .tv_nsec = 0,
    };

    while (true) {
        // Wait concurrently for timeout or channel pop
        co_await (condy::async_timeout(&ts, 0, 0) || chan.pop());

        if (done) {
            // Main coroutine set done, exit loop
            co_return;
        }

        // Timeout triggered: print Tick
        co_await condy::async_write(STDOUT_FILENO,
                                    condy::buffer(std::string("Tick\n")), 0);
    }
}

// Main coroutine: read from stdin and echo to stdout
condy::Coro<int> co_main() {
    bool done = false;
    condy::Channel<std::monostate> chan(1); // channel capacity 1

    // Spawn background task
    auto t = condy::co_spawn(background_task(chan, done));

    while (true) {
        char buf[16];
        int r = co_await condy::async_read(STDIN_FILENO, condy::buffer(buf), 0);

        if (r == 0) {
            // Input ended, notify background task to exit
            done = true;
            co_await chan.push(std::monostate{});
            break;
        }

        // Echo input back to stdout
        co_await condy::async_write(STDOUT_FILENO, condy::buffer(buf, r), 0);
    }

    // Wait for background task to complete
    co_await std::move(t);

    co_return 0;
}

int main() { return condy::sync_wait(co_main()); }
```

### Multishot Operation (io_uring Feature)

An io_uring Feature: a single asynchronous operation can produce multiple returns. Multishot operations require a callback function. After submitting a multishot operation, the current coroutine blocks until an error occurs; each normal return is handled via the callback.

Utility functions:

* `will_spawn`: spawns a new coroutine for each normal return.
* `will_push`: pushes the result into a `Channel` for each normal return.

Example:

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

### Zero Copy Operation (io_uring Feature)

An io_uring feature: zero-copy send allows sending data without copying the user buffer. When using zero-copy, a resource release function must be provided to ensure the buffer remains valid and unmodified until release.

Zero-copy operations can also be used with `will_spawn` / `will_push`.

Example:

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

### File Descriptor Registration (io_uring Feature)

An io_uring feature: Fixed file descriptors (fixed fd) reduce per-operation overhead. Normally, each async operation increments/decrements the file's kernel reference count. Registered files bypass these adjustments, improving performance.  

* Use `Runtime::fd_table()` to get the `FdTable` object of the current Runtime.
* Call `init` to initialize the table capacity before usage.
* Use `update_files` or `async_update_files` to register/unregister a file descriptor
* In asynchronous operations, pass a fixed fd using `fixed(fd)` to convert an `int` to `FixedFd`.
* Direct variants of async operations can register the fd at creation, or use `CONDY_FILE_INDEX_ALLOC` to let the system allocate a free slot. If no slot is available, the operation will fail and return `-ENFILE`.

Example:

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

### Buffer Registration (io_uring Feature)

An io_uring feature: fixed buffers reduce per-operation overhead, especially for O_DIRECT reads and writes. Registering a buffer maps it into the kernel once and avoids repeated page reference updates, improving performance.

* Use `Runtime::buffer_table()` to get the `BufferTable` object of the current Runtime.
* Call `init` to initialize the table capacity before usage.
* Use `update_buffers` to register/unregister a buffer.
* In asynchronous operations, pass a fixed buffer using `fixed(index, buf)` where `index` is the slot in the registration table. The memory region pointed to by buf must be within the range of the corresponding registered buffer.

Example:

```cpp
#include <condy.hpp>

condy::Coro<int> co_main() {
    // Get the buffer registration table of the current Runtime
    auto &table = condy::current_runtime().buffer_table();
    table.init(4);  // Initialize capacity to 4

    int fd = co_await condy::async_openat(AT_FDCWD, "result.txt",
                                          O_CREAT | O_WRONLY, 0644);
    if (fd < 0) {
        co_return 1;
    }

    // Register string in fixed buffer slot 0
    std::string msg = "Hello, condy!\n";
    iovec iov = {
        .iov_base = msg.data(),
        .iov_len = msg.size();
    }
    table.update_buffers(0, &iov, 1);

    // Use fixed buffer in async write operation
    int r = co_await condy::async_write(fd, condy::fixed(0, condy::buffer(msg)), 0);
    if (r < 0) {
        co_return 1;
    }

    // Unregister fixed buffer
    iov = {.iov_base = nullptr, .iov_len = 0};;
    table.update_buffers(0, &iov, 1);

    co_return 0;
}

int main() { return condy::sync_wait(co_main()); }
```

### Provided Buffers (io_uring Feature)

An io_uring feature: `ProvidedBufferPool` allows defining a pool of fixed-size buffers for asynchronous reads. Unlike completion-based models where buffers must be chosen upfront, provided buffers let the kernel select a suitable buffer when data is ready, reducing memory usage and improving performance for high-concurrency workloads.

* Define a buffer pool: `ProvidedBufferPool pool(log_num_buffers, buffer_size)`

  * `log_num_buffers`: logarithm (base 2) of the number of buffers; total buffers = 2^log_num_buffers
  * `buffer_size`: size of each buffer
* During async operations, a buffer is taken from the pool and returned as a `ProvidedBuffer` object.
* When the `ProvidedBuffer` is destructed or `reset` is called, the buffer is automatically returned to the pool.
* If no free buffer is available in the pool, the async operation will fail and return `-ENOBUFS`.
* The lifetime of `ProvidedBuffer` objects does not need to be shorter than the `ProvidedBufferPool`. The buffer pool will only be released after all `ProvidedBuffer` objects and the `ProvidedBufferPool` object are destructed.

Example:

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
        co_await condy::async_write(session_fd, condy::buffer(buffer.data(), r), 0);
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
            co_await ch.push(std::make_pair(0, condy::ProvidedBuffer{}));
            break;
        }

        // Error handling (excluding buffer pool full)
        if (res < 0 && res != -ENOBUFS) {
            co_await ch.push(std::make_pair(0, condy::ProvidedBuffer{}));
            co_return 1;
        }
    }

    co_return 0;
}

int main() { return condy::sync_wait(co_main()); }
```

### Custom Coroutine Allocator

The second template parameter of `Coro<T, Allocator>` can be used to specify a custom allocator for the coroutine frame. The default is `void`, which uses the system default allocator.

* When using a custom allocator, the coroutine function’s first argument must match the allocator type.
* `Coro` also provides support for `pmr` allocators, through the type `condy::pmr::Coro<T>`.

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

### Switching Runtime for a Coroutine

Condy provides `co_switch` to move a coroutine to a different runtime. After `co_await co_switch(other_runtime)`, the current coroutine will continue execution in the specified runtime. This is useful for load balancing across multiple runtimes:

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

## Q&A

### Q: Does Condy support Windows or macOS?

**No.**

io_uring is a Linux-only API.

To achieve performance close to native io_uring, Condy is built directly on top of liburing instead of designing a cross-platform abstraction. Therefore Condy currently supports Linux only.

### Q: Why use callbacks for multishot and zero-copy operations?

Keeping callbacks inside a coroutine library may seem unusual, but it is the most scalable and efficient approach for io_uring's “**submit once, complete many times**” operations.

For **multishot**:

* In theory we could design an awaitable that allows multiple `co_await` calls, but such a design is not practical. The runtime cannot guarantee that after the first `co_await` returns, the user will call `co_await` again.
This forces the runtime to buffer all unconsumed multishot completions, adding memory and complexity.

* In several real scenarios (e.g., `accept`), it is common and natural to spawn a new task for each event, making buffering unnecessary.

For **zero-copy**:

* Zero-copy completion events are not tied to the coroutine’s lifetime—buffers are often heap-allocated rather than stored in coroutine frames. Forcing co_await introduces unnecessary suspension and overhead.

Therefore, Condy processes multishot and Zero-copy events via callbacks and provides `will_spawn` and `will_push` to satisfy different needs—keeping the design both efficient and simple.

### Q: Why is there no multi-threaded runtime? Why is there no work-stealing?

Axboe (the author of io_uring) strongly advises **against sharing a single ring across multiple threads**. Multithreaded submissions require locking to maintain consistency, which can easily lead to severe contention. Moreover, several io_uring optimizations assume that each ring is accessed by only one thread.

Using one ring per thread in a thread pool is possible, but it introduces significant challenges. Multithreading is useful because threads can pull work from shared global resources, naturally balancing the load. However, a one-ring-per-thread architecture complicates this: cross-thread wakeups must be delivered explicitly via mechanisms like `msg_ring` or `eventfd`, requiring the sender to specify the target thread. This eliminates implicit load balancing and forces manual routing of tasks, greatly increasing runtime complexity.

Work stealing is another common benefit of thread pools, helping to smooth out long-tail tasks in concurrent workloads. However, io_uring offers a large set of asynchronous system calls, and Condy already provides thread-safe `Task` and `Channel` that allow efficient load balancing across runtimes and offloading of CPU-bound work to threads when needed.

As a result, genuine long-tail scenarios are rare. If long tails still occur, they may be caused by third-party libraries performing blocking synchronous work, not by io_uring itself. Forcing io_uring to adapt to synchronous programming would be counterproductive.

Therefore, instead of building a complex multithreaded runtime on top of io_uring, Condy adopts a simpler, robust design: **one ring per runtime, no ring sharing, and concurrency expressed through async operations and explicit task offloading**.

### Q: Why can asynchronous operations run concurrently, but tasks cannot be awaited concurrently?

Asynchronous operations start executing when awaited, so Condy must allow:

* Starting multiple operations simultaneously
* Waiting for them together

Tasks behave differently: A task starts running immediately when spawned (`co_spawn`), not when awaited. Thus:

* `make_all_awaiter(tasks)` has the same semantics as awaiting tasks in sequence
* The only meaningful primitive is `make_one_awaiter`: wait for one task/operation, cancel the rest

But Condy tasks are **non-cancelable**, so it is **impossible** to implement `make_one_awaiter` for tasks.

### Q: Why can’t a task be canceled?

Condy tasks do not support forced cancellation. To support forced cancellation of a task, the runtime would need to:

* Cancel all currently pending async operations within the task.
* Track the cancellation state of the task itself.
* Upon completion of each async operation, check the cancellation state and destroy all remaining coroutine frames if the task has been canceled.

Implementing this requires storing extra metadata per task (e.g., cancellation flags, active async operations, all coroutine frames, etc.), which introduces significant overhead and contradicts Condy’s design goal.

Instead, Condy adopts a model similar to `std::thread`: tasks themselves are not cancelable. Users can implement cooperative cancellation by setting and checking their own flags within the task. Tasks may check these flags at appropriate points to decide whether to exit gracefully.

### Q: Why can’t `make_one_awaiter` guarantee exactly one completion?

This is an inherent property of asynchronous cancellation. Consider a simplified model that illustrates the race condition:

* Thread A pushes an item into a locked queue.
* Thread B pops from the queue.
* Thread A may try to remove the item.

Once A releases the lock after pushing, there is no guarantee that the item is still present when removal occurs. Similarly, with io_uring, issuing a cancellation request does not guarantee the operation will fail—after the request, the operation may still succeed.
