#include "condy/runtime.hpp"
#include "condy/runtime_options.hpp"
#include "condy/sync_wait.hpp"
#include <doctest/doctest.h>

TEST_CASE("test sync_wait - with runtime") {
    bool finished = false;

    auto func = [&]() -> condy::Coro<int> {
        finished = true;
        co_return 42;
    };

    condy::Runtime runtime;

    auto result = condy::sync_wait(runtime, func());
    REQUIRE(result == 42);
    REQUIRE(finished);
}

TEST_CASE("test sync_wait - without runtime") {
    bool finished = false;

    auto func = [&]() -> condy::Coro<int> {
        finished = true;
        co_return 42;
    };

    auto result = condy::sync_wait(func());
    REQUIRE(result == 42);
    REQUIRE(finished);
}

TEST_CASE("test sync_wait - exception handling") {
    struct MyException : public std::exception {
        const char *what() const noexcept override {
            return "MyException occurred";
        }
    };

    auto func = [&]() -> condy::Coro<void> {
        throw MyException{};
        co_return;
    };

    try {
        condy::sync_wait(func());
        REQUIRE(false); // Should not reach here
    } catch (const MyException &e) {
        REQUIRE(std::string(e.what()) == "MyException occurred");
    }
}

namespace {

condy::pmr::Coro<int> test_pmr_func(auto &, bool &finished) {
    finished = true;
    co_return 42;
}

} // namespace

TEST_CASE("test sync_wait - with allocator") {
    bool finished = false;

    std::pmr::monotonic_buffer_resource pool;
    std::pmr::polymorphic_allocator<std::byte> allocator(&pool);

    auto result = condy::sync_wait(test_pmr_func(allocator, finished));
    REQUIRE(result == 42);
    REQUIRE(finished);
}