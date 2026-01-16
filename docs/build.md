# Building and Usage

@brief How to build and integrate Condy in your project.

## Direct Header-Only Usage

Condy is a header-only library. You can use it by simply including the header file in your project.  
Make sure `liburing` (≥2.3) is installed on your system.

```cpp
#include <condy.hpp>
```

Compile your code:

```bash
clang++ main.cpp \
    -std=c++20 \        # Enable C++20 for coroutine support
    -luring \           # Link against liburing
    -I/path/to/condy    # Specify Condy include path
```

## Using Condy as a Submodule (Recommended)

You can add Condy to your project via Git submodule:

```bash
git submodule add https://github.com/wokron/condy.git third_party/condy
git submodule update --init --recursive
```

In your `CMakeLists.txt`:

```cmake
add_subdirectory(third_party/condy)
add_executable(my_app src/main.cpp)
target_link_libraries(my_app PRIVATE condy)
```

> [!NOTE]
> - C++20 is required for coroutine support.  
> - Condy depends on **liburing ≥ 2.3**.  
> - By default, Condy builds and links the bundled liburing in `third_party/liburing` (`LINK_LIBURING=ON`). If you need a specific version of liburing, you can manually check out the desired commit in that directory before building.
> - To link against the system liburing, set `LINK_LIBURING=OFF` and install liburing manually.

## Building Examples / Benchmarks / Tests

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

- Run all tests:

```bash
ctest --test-dir build
```

- Run example programs:

```bash
./build/examples/link-cp from.bin to.bin
```

## Version Compatibility

Condy supports **liburing ≥ 2.3**. When building with a specific version of liburing, Condy assumes that all related interfaces provided by that version are already supported by your Linux kernel.

> [!NOTE]
> If your kernel version is older than the liburing version you are using, some features may not be available at runtime, even if compilation succeeds.  
> For best compatibility and feature support, it is recommended to use a Linux kernel version that matches or exceeds the requirements of your chosen liburing version.