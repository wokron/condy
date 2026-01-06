# Condy

> *C++ Asynchronous System Call Layer, Powered by io_uring and C++20 Coroutines*

Condy is designed to provide an intuitive, high-performance coroutine runtime on top of io_uring:

- 🛠️ **Comprehensive io_uring Integration**
  Designed to integrate and maintain support for most io_uring features, with ongoing updates to track kernel and liburing advancements.

- 🏃 **Low Overhead**
  Efficient template-based abstractions and precise lifetime management eliminate nearly all heap allocations outside coroutine frames, resulting in extremely low runtime overhead.

- 💡 **Intuitive Programming Model**
  Write asynchronous code in a direct, readable style using C++20 coroutines—no callbacks. Friendly APIs, high-level combinators, and channels make complex async flows easy to express.

> [!WARNING]
> **Alpha Stage** - Condy is currently in early development. API is not stable and may change without notice.

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

See [Documentation](#documentation) for more details.

## Documentation

- **[Building and Usage](docs/build.md):** How to build and integrate Condy in your project.
- **[User Guide](docs/guide.md):** Step-by-step introduction to Condy’s concepts and usage.
- **[Async Operation Types](docs/ops.md):** Overview and classification of supported io_uring async operation variants.

## Support

- For questions, bug reports, or feature requests, please open an [issue](https://github.com/wokron/condy/issues).
- [Pull requests](https://github.com/wokron/condy/pulls) are welcome!