#include <condy/coro.hpp>
#include <doctest/doctest.h>
#include <string>

TEST_CASE("test coro - run coro") {
    bool executed = false;
    auto func = [&]() -> condy::Coro {
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
    auto inner_func = [&]() -> condy::Coro {
        executed = true;
        co_return;
    };
    auto outer_func = [&](condy::Coro inner) -> condy::Coro {
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
    auto inner_func = [&]() -> condy::Coro {
        executed = true;
        co_return;
    };
    auto middle_func = [&](condy::Coro inner) -> condy::Coro {
        co_await std::move(inner);
        co_return;
    };
    auto outer_func = [&](condy::Coro middle) -> condy::Coro {
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
    auto func = [&]() -> condy::Coro {
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

    auto inner = [&]() -> condy::Coro {
        throw MyException{};
        co_return;
    };
    auto func = [&]() -> condy::Coro {
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