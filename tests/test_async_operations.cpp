#include "condy/coro.hpp"
#include <condy/async_operations.hpp>
#include <cstring>
#include <doctest/doctest.h>

namespace {

void event_loop(size_t &unfinished) {
    auto *ring = condy::Context::current().ring();
    while (unfinished > 0) {
        ring->submit();
        ring->reap_completions([&](io_uring_cqe *cqe) {
            auto handle_ptr = static_cast<condy::OpFinishHandle *>(
                io_uring_cqe_get_data(cqe));
            handle_ptr->set_result(cqe->res);
            (*handle_ptr)();
        });
    }
}

} // namespace

TEST_CASE("test async_operations - simple read write") {
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    const char msg[] = "Hello, condy!";
    char buf[20] = {0};

    condy::Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);
    auto &context = condy::Context::current();
    context.init(&ring);

    size_t unfinished = 2;
    auto writer = [&]() -> condy::Coro<void> {
        int bytes_written =
            co_await condy::async_write(pipe_fds[1], msg, sizeof(msg), 0);
        REQUIRE(bytes_written == sizeof(msg));
        --unfinished;
    };
    auto reader = [&]() -> condy::Coro<void> {
        int bytes_read =
            co_await condy::async_read(pipe_fds[0], buf, sizeof(msg), 0);
        REQUIRE(bytes_read == sizeof(msg));
        --unfinished;
    };

    writer().release().resume();
    reader().release().resume();
    REQUIRE(unfinished == 2);

    event_loop(unfinished);

    REQUIRE(unfinished == 0);
}