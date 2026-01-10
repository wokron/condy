#include <condy/coro.hpp>
#include <condy/pmr.hpp>
#include <cstddef>
#include <doctest/doctest.h>
#include <memory>
#include <string>

TEST_CASE("test coro - run coro") {
    bool executed = false;
    auto func = [&]() -> condy::Coro<void> {
        executed = true;
        co_return;
    };

    auto coro = func();
    REQUIRE(!executed);

    coro.release().resume();
    REQUIRE(executed);
}

TEST_CASE("test coro - await coro") {
    bool executed = false;
    auto inner_func = [&]() -> condy::Coro<void> {
        executed = true;
        co_return;
    };
    auto outer_func = [&](condy::Coro<void> inner) -> condy::Coro<void> {
        co_await std::move(inner);
        co_return;
    };

    auto inner_coro = inner_func();
    auto outer_coro = outer_func(std::move(inner_coro));
    REQUIRE(!executed);

    outer_coro.release().resume();
    REQUIRE(executed);
}

TEST_CASE("test coro - nested await") {
    bool executed = false;
    auto inner_func = [&]() -> condy::Coro<void> {
        executed = true;
        co_return;
    };
    auto middle_func = [&](condy::Coro<void> inner) -> condy::Coro<void> {
        co_await std::move(inner);
        co_return;
    };
    auto outer_func = [&](condy::Coro<void> middle) -> condy::Coro<void> {
        co_await std::move(middle);
        co_return;
    };

    auto inner_coro = inner_func();
    auto middle_coro = middle_func(std::move(inner_coro));
    auto outer_coro = outer_func(std::move(middle_coro));
    REQUIRE(!executed);

    outer_coro.release().resume();
    REQUIRE(executed);
}

TEST_CASE("test coro - resume by awaiter") {
    struct Awaiter {
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) noexcept { handle = h; }
        void await_resume() noexcept {}
        std::coroutine_handle<> handle = nullptr;
    } awaiter;

    bool executed = false;
    auto func = [&]() -> condy::Coro<void> {
        co_await awaiter;
        executed = true;
        co_return;
    };

    auto coro = func();
    REQUIRE(!executed);
    REQUIRE(awaiter.handle == nullptr);

    coro.release().resume();
    REQUIRE(!executed);
    REQUIRE(awaiter.handle != nullptr);

    awaiter.handle.resume();
    REQUIRE(executed);
}

TEST_CASE("test coro - exception handling") {
    struct MyException : public std::exception {
        const char *what() const noexcept override {
            return "MyException occurred";
        }
    };

    bool caught = false;

    auto inner = [&]() -> condy::Coro<void> {
        throw MyException{};
        co_return;
    };
    auto func = [&]() -> condy::Coro<void> {
        try {
            co_await inner();
        } catch (const MyException &e) {
            caught = true;
            REQUIRE(std::string(e.what()) == "MyException occurred");
        }
        co_return;
    };

    auto coro = func();
    REQUIRE(!caught);

    coro.release().resume();
    REQUIRE(caught);
}

TEST_CASE("test coro - return value") {
    bool finished = false;

    auto inner = [&]() -> condy::Coro<int> { co_return 42; };
    auto func = [&]() -> condy::Coro<void> {
        int value = co_await inner();
        REQUIRE(value == 42);
        finished = true;
        co_return;
    };

    auto coro = func();
    REQUIRE(!finished);

    coro.release().resume();
    REQUIRE(finished);
}

TEST_CASE("test coro - return value with exception") {
    struct MyException : public std::exception {
        const char *what() const noexcept override {
            return "MyException occurred";
        }
    };

    bool caught = false;

    auto inner = [&]() -> condy::Coro<int> {
        throw MyException{};
        co_return 0;
    };
    auto func = [&]() -> condy::Coro<void> {
        try {
            int value = co_await inner();
            (void)value; // suppress unused variable warning
        } catch (const MyException &e) {
            caught = true;
            REQUIRE(std::string(e.what()) == "MyException occurred");
        }
        co_return;
    };

    auto coro = func();
    REQUIRE(!caught);

    coro.release().resume();
    REQUIRE(caught);
}

TEST_CASE("test coro - return move-only type") {
    bool finished = false;

    auto inner = [&]() -> condy::Coro<std::unique_ptr<int>> {
        co_return std::make_unique<int>(99);
    };
    auto func = [&]() -> condy::Coro<void> {
        std::unique_ptr<int> mo = co_await inner();
        REQUIRE(*mo == 99);
        finished = true;
        co_return;
    };

    auto coro = func();
    REQUIRE(!finished);

    coro.release().resume();
    REQUIRE(finished);
}

TEST_CASE("test coro - return no default constructible type") {
    struct NoDefault {
        NoDefault(int v) : value(v) {}
        int value;
    };

    bool finished = false;

    auto inner = [&]() -> condy::Coro<NoDefault> { co_return NoDefault{123}; };
    auto func = [&]() -> condy::Coro<void> {
        NoDefault nd = co_await inner();
        REQUIRE(nd.value == 123);
        finished = true;
        co_return;
    };

    auto coro = func();
    REQUIRE(!finished);

    coro.release().resume();
    REQUIRE(finished);
}

namespace {

struct CustomAllocator {
    using value_type = char;

    value_type *allocate(size_t size) {
        allocated_size = size;
        allocated = true;
        return reinterpret_cast<value_type *>(::malloc(size));
    }

    void deallocate(value_type *ptr, size_t size) {
        REQUIRE(size == allocated_size);
        ::free(ptr);
    }

    size_t allocated_size = 0;
    bool allocated = false;
};

condy::Coro<void, CustomAllocator> test_custom_allocator_func(CustomAllocator &,
                                                              bool &finished) {
    finished = true;
    co_return;
}

} // namespace

TEST_CASE("test coro - custom allocator") {
    bool finished = false;
    CustomAllocator allocator;
    auto coro = test_custom_allocator_func(allocator, finished);
    coro.release().resume();
    REQUIRE(finished);
    REQUIRE(allocator.allocated);
}

namespace {

condy::pmr::Coro<void> test_pmr_func(auto &, bool &finished) {
    finished = true;
    co_return;
}

} // namespace

TEST_CASE("test coro - pmr allocator") {
    std::pmr::monotonic_buffer_resource pool;
    std::pmr::polymorphic_allocator<std::byte> allocator(&pool);
    bool finished = false;
    auto coro = test_pmr_func(allocator, finished);
    coro.release().resume();
    REQUIRE(finished);
}

namespace {
struct AllocatorA {
    using value_type = char;
    value_type *allocate(size_t size) {
        allocated_size = size;
        allocated = true;
        return reinterpret_cast<value_type *>(::malloc(size));
    }
    void deallocate(value_type *ptr, size_t size) {
        REQUIRE(size == allocated_size);
        ::free(ptr);
    }
    size_t allocated_size = 0;
    bool allocated = false;
};

struct AllocatorB {
    using value_type = char;
    value_type *allocate(size_t size) {
        allocated_size = size;
        allocated = true;
        return reinterpret_cast<value_type *>(::malloc(size));
    }
    void deallocate(value_type *ptr, size_t size) {
        REQUIRE(size == allocated_size);
        ::free(ptr);
    }
    size_t allocated_size = 0;
    bool allocated = false;
};

condy::Coro<void, AllocatorA> test_allocator_func1(AllocatorA &,
                                                   bool &finished1) {
    finished1 = true;
    co_return;
}

condy::Coro<void, AllocatorB> test_allocator_func2(AllocatorB &,
                                                   AllocatorA &allocatorA,
                                                   bool &finished2,
                                                   bool &finished1) {
    finished2 = true;
    co_await test_allocator_func1(allocatorA, finished1);
    co_return;
}

} // namespace

TEST_CASE("test coro - different allocators") {
    bool finished1 = false;
    bool finished2 = false;

    AllocatorA allocatorA;
    AllocatorB allocatorB;

    auto coro =
        test_allocator_func2(allocatorB, allocatorA, finished2, finished1);
    REQUIRE(!finished1);
    REQUIRE(!finished2);
    coro.release().resume();
    REQUIRE(finished1);
    REQUIRE(finished2);
    REQUIRE(allocatorA.allocated);
    REQUIRE(allocatorB.allocated);
}
