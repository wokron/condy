#include "condy/awaiter_operations.hpp"
#include "condy/context.hpp"
#include "condy/coro.hpp"
#include "condy/ring.hpp"
#include "condy/sync_wait.hpp"
#include <doctest/doctest.h>
#include <liburing.h>

TEST_CASE("test buffer_table - init") {
    auto func = []() -> condy::Coro<void> {
        auto &buffer_table = condy::Context::current().ring()->buffer_table();
        buffer_table.init(8);
        co_return;
    };

    condy::sync_wait(func());
}

TEST_CASE("test buffer_table - register/unregister buffer") {
    auto func = []() -> condy::Coro<void> {
        auto &buffer_table = condy::Context::current().ring()->buffer_table();
        buffer_table.init(8);

        char buffer1[16];
        char buffer2[32];

        iovec iov[2] = {
            {.iov_base = buffer1, .iov_len = sizeof(buffer1)},
            {.iov_base = buffer2, .iov_len = sizeof(buffer2)},
        };
        buffer_table.update_buffers(0, iov, 2);

        iov[0] = {.iov_base = nullptr, .iov_len = 0};
        iov[1] = {.iov_base = nullptr, .iov_len = 0};
        buffer_table.update_buffers(0, iov, 2);

        co_return;
    };

    condy::sync_wait(func());
}

TEST_CASE("test buffer_table - use registered buffer") {
    auto func = []() -> condy::Coro<void> {
        auto &buffer_table = condy::Context::current().ring()->buffer_table();
        buffer_table.init(8);

        int pipes[2];
        int ret = pipe(pipes);
        REQUIRE(ret == 0);

        const char write_buf[] = "hello";
        char read_buf[sizeof(write_buf)] = {0};

        iovec iovs[2] = {
            {.iov_base = read_buf, .iov_len = sizeof(read_buf)},
            {.iov_base = const_cast<char *>(write_buf),
             .iov_len = sizeof(write_buf)},
        };

        buffer_table.update_buffers(0, iovs, 2);

        auto write_op =
            condy::make_op_awaiter(io_uring_prep_write_fixed, pipes[1],
                                   write_buf, sizeof(write_buf), 0, 1);
        int write_res = co_await write_op;
        REQUIRE(write_res == sizeof(write_buf));

        auto read_op =
            condy::make_op_awaiter(io_uring_prep_read_fixed, pipes[0], read_buf,
                                   sizeof(read_buf), 0, 0);
        int read_res = co_await read_op;
        REQUIRE(read_res == sizeof(read_buf));
        REQUIRE(std::memcmp(write_buf, read_buf, sizeof(write_buf)) == 0);
        co_return;
    };

    condy::sync_wait(func());
}