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

    condy::SingleThreadRuntime runtime;

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