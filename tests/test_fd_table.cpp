#include "condy/async_operations.hpp"
#include "condy/awaiter_operations.hpp"
#include "condy/channel.hpp"
#include "condy/context.hpp"
#include "condy/coro.hpp"
#include "condy/ring.hpp"
#include "condy/runtime.hpp"
#include "condy/sync_wait.hpp"
#include <cerrno>
#include <doctest/doctest.h>
#include <stdexcept>
#include <variant>

TEST_CASE("test fd_table - init") {
    auto func = []() -> condy::Coro<void> {
        auto &fd_table = condy::detail::Context::current().ring()->fd_table();
        fd_table.init(4);
        co_return;
    };

    condy::sync_wait(func());
}

TEST_CASE("test fd_table - register/unregister fd") {
    auto func = []() -> condy::Coro<void> {
        auto &fd_table = condy::detail::Context::current().ring()->fd_table();
        fd_table.init(4);

        int pipes[2];
        int ret = pipe(pipes);
        REQUIRE(ret == 0);

        fd_table.update(0, pipes, 2);

        close(pipes[0]);
        close(pipes[1]);

        pipes[0] = -1;
        pipes[1] = -1;

        fd_table.update(0, pipes, 2);

        co_return;
    };

    condy::sync_wait(func());
}

TEST_CASE("test fd_table - use fixed fd") {
    auto func = []() -> condy::Coro<void> {
        auto &fd_table = condy::detail::Context::current().ring()->fd_table();
        fd_table.init(4);

        int pipes[2];
        int ret = pipe(pipes);
        REQUIRE(ret == 0);

        fd_table.update(0, pipes, 2);

        char write_buf[] = "hello";
        char read_buf[sizeof(write_buf)] = {0};

        auto write_op = condy::make_op_awaiter(io_uring_prep_write, 1,
                                               write_buf, sizeof(write_buf), 0);
        int write_res =
            co_await condy::flag<IOSQE_FIXED_FILE>(std::move(write_op));
        REQUIRE(write_res == sizeof(write_buf));

        auto read_op = condy::make_op_awaiter(io_uring_prep_read, 0, read_buf,
                                              sizeof(read_buf), 0);
        int read_res =
            co_await condy::flag<IOSQE_FIXED_FILE>(std::move(read_op));
        REQUIRE(read_res == sizeof(read_buf));
        REQUIRE(std::memcmp(write_buf, read_buf, sizeof(write_buf)) == 0);
        co_return;
    };

    condy::sync_wait(func());
}

#if !IO_URING_CHECK_VERSION(2, 4) // >= 2.4
TEST_CASE("test fd_table - send fd - basic") {
    condy::Runtime runtime1, runtime2;
    runtime1.fd_table().init(4);
    runtime2.fd_table().init(4);

    int next_fd = 0;
    runtime2.fd_table().set_fd_accepter(
        [&next_fd](int32_t received_fd) { REQUIRE(received_fd == next_fd++); });

    condy::Channel<std::monostate> chan(0);

    auto func1 = [&]() -> condy::Coro<void> {
        co_await chan.pop();
        int pipes[2][2];
        for (int i = 0; i < 2; i++) {
            int ret = pipe(pipes[i]);
            REQUIRE(ret == 0);
        }

        auto &fd_table = condy::current_runtime().fd_table();

        fd_table.update(0, reinterpret_cast<int *>(pipes), 4);
        for (int i = 0; i < 4; i++) {
            int r = co_await condy::async_fixed_fd_send(runtime2.fd_table(), i,
                                                        i, 0);
            REQUIRE(r == 0);
        }
    };

    auto func2 = [&]() -> condy::Coro<void> {
        co_await chan.push(std::monostate{});
        co_return;
    };

    condy::co_spawn(runtime2, func2()).detach();
    std::thread t2([&]() { runtime2.run(); });

    condy::sync_wait(runtime1, func1());
    runtime2.allow_exit();
    t2.join();

    REQUIRE(next_fd == 4);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 4) // >= 2.4
TEST_CASE("test fd_table - send fd - auto allocate") {
    condy::Runtime runtime1, runtime2;
    runtime1.fd_table().init(4);
    runtime2.fd_table().init(4);

    std::vector<int> received_fds;
    runtime2.fd_table().set_fd_accepter([&received_fds](int32_t received_fd) {
        received_fds.push_back(received_fd);
    });

    condy::Channel<std::monostate> chan(0);

    auto func1 = [&]() -> condy::Coro<void> {
        co_await chan.pop();
        int pipes[2][2];
        for (int i = 0; i < 2; i++) {
            int ret = pipe(pipes[i]);
            REQUIRE(ret == 0);
        }

        auto &fd_table = condy::current_runtime().fd_table();

        fd_table.update(0, reinterpret_cast<int *>(pipes), 4);
        for (int i = 0; i < 4; i++) {
            int r = co_await condy::async_fixed_fd_send(
                runtime2.fd_table(), i, CONDY_FILE_INDEX_ALLOC, 0);
            REQUIRE(r == i);
        }
    };

    auto func2 = [&]() -> condy::Coro<void> {
        co_await chan.push(std::monostate{});
        co_return;
    };

    condy::co_spawn(runtime2, func2()).detach();
    std::thread t2([&]() { runtime2.run(); });

    condy::sync_wait(runtime1, func1());
    runtime2.allow_exit();
    t2.join();

    REQUIRE(received_fds.size() == 4);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 4) // >= 2.4
TEST_CASE("test fd_table - send fd - throw without accepter") {
    condy::Runtime runtime1, runtime2;
    runtime1.fd_table().init(4);
    runtime2.fd_table().init(4);

    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    condy::Channel<std::monostate> chan(0);

    auto func1 = [&]() -> condy::Coro<void> {
        co_await chan.pop();

        auto &fd_table = condy::current_runtime().fd_table();

        fd_table.update(0, pipe_fds, 2);
        int r = co_await condy::async_fixed_fd_send(runtime2.fd_table(), 0,
                                                    CONDY_FILE_INDEX_ALLOC, 0);
        REQUIRE(r == 0);
    };

    auto func2 = [&]() -> condy::Coro<void> {
        co_await chan.push(std::monostate{});
        co_await chan.push(std::monostate{}); // Block here
        co_return;
    };

    condy::co_spawn(runtime2, func2()).detach();

    std::thread t2(
        [&]() { REQUIRE_THROWS_AS(runtime2.run(), std::logic_error); });

    condy::sync_wait(runtime1, func1());
    runtime2.allow_exit();
    t2.join();
}
#endif