#include "condy/awaiter_operations.hpp"
#include "condy/context.hpp"
#include "condy/coro.hpp"
#include "condy/ring.hpp"
#include "condy/sync_wait.hpp"
#include <doctest/doctest.h>

TEST_CASE("test buffer_table - init/destroy") {
    auto func = []() -> condy::Coro<void> {
        char buf[32];
        iovec vec{.iov_base = buf, .iov_len = sizeof(buf)};
        auto &buffer_table =
            condy::detail::Context::current().ring()->buffer_table();

        REQUIRE(buffer_table.update(0, &vec, 1) < 0);

        REQUIRE(buffer_table.init(8) == 0);
        REQUIRE(buffer_table.init(8) != 0); // already initialized

        REQUIRE(buffer_table.update(0, &vec, 1) == 1);

        REQUIRE(buffer_table.destroy() == 0);
        REQUIRE(buffer_table.destroy() <= 0);
        co_return;
    };

    condy::sync_wait(func());
}

TEST_CASE("test buffer_table - register/unregister buffer") {
    auto func = []() -> condy::Coro<void> {
        auto &buffer_table =
            condy::detail::Context::current().ring()->buffer_table();
        buffer_table.init(8);

        char buffer1[16];
        char buffer2[32];

        iovec iov[2] = {
            {.iov_base = buffer1, .iov_len = sizeof(buffer1)},
            {.iov_base = buffer2, .iov_len = sizeof(buffer2)},
        };
        int n = buffer_table.update(0, iov, 2);
        REQUIRE(n == 2);

        iov[0] = {.iov_base = nullptr, .iov_len = 0};
        iov[1] = {.iov_base = nullptr, .iov_len = 0};
        n = buffer_table.update(0, iov, 2);
        REQUIRE(n == 2);

        co_return;
    };

    condy::sync_wait(func());
}

TEST_CASE("test buffer_table - use registered buffer") {
    auto func = []() -> condy::Coro<void> {
        auto &buffer_table =
            condy::detail::Context::current().ring()->buffer_table();
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

        buffer_table.update(0, iovs, 2);

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

#if !IO_URING_CHECK_VERSION(2, 10) // >= 2.10
TEST_CASE("test buffer_table - clone buffer table") {
    condy::Ring ring1, ring2;
    io_uring_params params = {};
    ring1.init(128, &params);
    ring2.init(128, &params);

    auto &table1 = ring1.buffer_table();
    auto &table2 = ring2.buffer_table();

    REQUIRE(table2.clone_buffers(table1) != 0);

    table1.init(16);

    REQUIRE(table2.clone_buffers(table1, 0, 8, 16) != 0);

    // capacity == 16
    REQUIRE(table2.clone_buffers(table1) == 0);

    // capacity == 16 + 8
    REQUIRE(table2.clone_buffers(table1, 8, 0, 16) == 0);

    table2.destroy();

    // capacity == 100 + 16
    REQUIRE(table2.clone_buffers(table1, 100, 0, 16) == 0);

    char buffer[32];
    iovec vec = {
        .iov_base = buffer,
        .iov_len = sizeof(buffer),
    };
    int r = table2.update(1, &vec, 1);
    REQUIRE(r == 1);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 10) // >= 2.10
TEST_CASE("test buffer_table - setup buffer before run") {
    condy::Runtime runtime1, runtime2;
    REQUIRE(runtime1.buffer_table().init(4) == 0);
    REQUIRE(runtime2.buffer_table().clone_buffers(runtime1.buffer_table()) ==
            0);

    runtime2.allow_exit();
    runtime2.run();

    runtime1.allow_exit();
    runtime1.run();
}
#endif