#include "condy/coro.hpp"
#include "condy/event_loop.hpp"
#include "condy/task.hpp"
#include <condy/async_operations.hpp>
#include <doctest/doctest.h>

TEST_CASE("test async_operations - simple read write") {
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    const char msg[] = "Hello, condy!";
    char buf[20] = {0};

    condy::EventLoop<condy::SimpleStrategy> loop(8);

    size_t unfinished = 1;
    auto writer = [&]() -> condy::Coro<void> {
        int bytes_written =
            co_await condy::async_write(pipe_fds[1], msg, sizeof(msg), 0);
        REQUIRE(bytes_written == sizeof(msg));
    };
    auto reader = [&]() -> condy::Coro<void> {
        int bytes_read =
            co_await condy::async_read(pipe_fds[0], buf, sizeof(msg), 0);
        REQUIRE(bytes_read == sizeof(msg));
    };

    auto func = [&]() -> condy::Coro<void> {
        auto w = condy::co_spawn(writer());
        auto r = condy::co_spawn(reader());
        co_await std::move(w);
        co_await std::move(r);
        REQUIRE(std::string(buf) == std::string(msg));
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    loop.run(std::move(coro));

    REQUIRE(unfinished == 0);
}