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

        buffer_table.register_buffer(0, condy::buffer(buffer1));
        buffer_table.register_buffer(1, condy::buffer(buffer2));

        buffer_table.unregister_buffer(0);
        buffer_table.unregister_buffer(1);

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

        buffer_table.register_buffer(0, condy::buffer(read_buf));
        buffer_table.register_buffer(1, condy::buffer(write_buf));

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