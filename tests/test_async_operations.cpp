#include "condy/coro.hpp"
#include "condy/task.hpp"
#include <condy/async_operations.hpp>
#include <doctest/doctest.h>

namespace {

void event_loop(size_t &unfinished) {
    auto &context = condy::Context::current();
    auto ring = context.get_ring();
    while (unfinished > 0) {
        io_uring_submit_and_wait(ring, 1);

        io_uring_cqe *cqe;
        io_uring_peek_cqe(ring, &cqe);
        if (cqe == nullptr) {
            continue;
        }

        auto handle_ptr =
            static_cast<condy::OpFinishHandle *>(io_uring_cqe_get_data(cqe));
        if (handle_ptr) {
            handle_ptr->finish(cqe->res);
        }

        io_uring_cqe_seen(ring, cqe);
    }
}

} // namespace

TEST_CASE("test async_operations - simple read write") {
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    const char msg[] = "Hello, condy!";
    char buf[20] = {0};

    auto &context = condy::Context::current();
    context.init({.io_uring_entries = 8});
    auto ring = context.get_ring();

    size_t unfinished = 1;
    auto writer = [&]() -> condy::Coro {
        int bytes_written =
            co_await condy::async_write(pipe_fds[1], msg, sizeof(msg), 0);
        REQUIRE(bytes_written == sizeof(msg));
    };
    auto reader = [&]() -> condy::Coro {
        int bytes_read =
            co_await condy::async_read(pipe_fds[0], buf, sizeof(msg), 0);
        REQUIRE(bytes_read == sizeof(msg));
    };

    auto func = [&]() -> condy::Coro {
        auto w = condy::co_spawn(writer());
        auto r = condy::co_spawn(reader());
        co_await std::move(w);
        co_await std::move(r);
        REQUIRE(std::string(buf) == std::string(msg));
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    coro.release().resume();
    REQUIRE(unfinished == 1);

    event_loop(unfinished);
    REQUIRE(unfinished == 0);

    context.destroy();
}