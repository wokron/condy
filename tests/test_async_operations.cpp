#include "condy/awaiter_operations.hpp"
#include "condy/buffers.hpp"
#include "condy/channel.hpp"
#include "condy/coro.hpp"
#include "condy/runtime.hpp"
#include "condy/sync_wait.hpp"
#include <condy/async_operations.hpp>
#include <cstddef>
#include <cstring>
#include <doctest/doctest.h>
#include <latch>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <variant>

namespace {

void event_loop(size_t &unfinished) {
    auto *ring = condy::Context::current().ring();
    while (unfinished > 0) {
        ring->submit();
        ring->reap_completions([&](io_uring_cqe *cqe) {
            auto handle_ptr = static_cast<condy::OpFinishHandle *>(
                io_uring_cqe_get_data(cqe));
            handle_ptr->set_result(cqe->res, 0);
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
            co_await condy::async_write(pipe_fds[1], condy::buffer(msg), 0);
        REQUIRE(bytes_written == sizeof(msg));
        --unfinished;
    };
    auto reader = [&]() -> condy::Coro<void> {
        int bytes_read = co_await condy::async_read(
            pipe_fds[0], condy::buffer(buf, sizeof(msg)), 0);
        REQUIRE(bytes_read == sizeof(msg));
        --unfinished;
    };

    writer().release().resume();
    reader().release().resume();
    REQUIRE(unfinished == 2);

    event_loop(unfinished);

    REQUIRE(unfinished == 0);
}

#if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2

namespace {

void server(std::latch &l, uint16_t port,
            condy::Channel<std::monostate> &cancel_channel) {
    int server_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    REQUIRE(server_fd > 0);

    // Make socket reusable
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    int r = bind(server_fd, (sockaddr *)&addr, sizeof(addr));
    REQUIRE(r == 0);

    r = listen(server_fd, 16);
    REQUIRE(r == 0);

    l.arrive_and_wait();

    int accpted_count = 0;

    auto session_func = [&](int client_fd) -> condy::Coro<void> {
        accpted_count++;
        close(client_fd);
        co_return;
    };

    auto server_func = [&]() -> condy::Coro<void> {
        using condy::operators::operator||;

        socklen_t addrlen = sizeof(addr);
        auto r = co_await (condy::async_multishot_accept(
                               server_fd, reinterpret_cast<sockaddr *>(&addr),
                               &addrlen, 0, condy::will_spawn(session_func)) ||
                           cancel_channel.pop());
        REQUIRE(r.index() == 1); // Cancelled
    };

    condy::sync_wait(server_func());

    REQUIRE(accpted_count == 2);
}

void client(uint16_t port) {
    for (int i = 0; i < 2; ++i) {
        int client_fd = socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(client_fd > 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);
        int r = connect(client_fd, (sockaddr *)&addr, sizeof(addr));
        REQUIRE(r == 0);
        char buf[20] = {0};
        int bytes_read = read(client_fd, buf, sizeof(buf));
        REQUIRE(bytes_read == 0); // Server closes immediately
        close(client_fd);
    }
}

} // namespace

TEST_CASE("test async_operations - multishot accept") {
    const uint16_t port = 12345;
    condy::Channel<std::monostate> cancel_channel(1);
    std::latch latch(2);
    std::thread server_thread([&]() { server(latch, port, cancel_channel); });

    // Ensure server is ready before client starts
    latch.arrive_and_wait();

    client(port);

    // Cancel the server
    cancel_channel.try_push(std::monostate{});
    server_thread.join();
}

#endif

TEST_CASE("test async_operations - fixed fd read write") {
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

    auto &fd_table = ring.fd_table();
    fd_table.init(2);
    fd_table.register_fd(0, pipe_fds[0]);
    fd_table.register_fd(1, pipe_fds[1]);

    size_t unfinished = 2;
    auto writer = [&]() -> condy::Coro<void> {
        int bytes_written =
            co_await condy::async_write(condy::fixed(1), condy::buffer(msg), 0);
        REQUIRE(bytes_written == sizeof(msg));
        --unfinished;
    };
    auto reader = [&]() -> condy::Coro<void> {
        int bytes_read = co_await condy::async_read(
            condy::fixed(0), condy::buffer(buf, sizeof(msg)), 0);
        REQUIRE(bytes_read == sizeof(msg));
        --unfinished;
    };

    writer().release().resume();
    reader().release().resume();
    REQUIRE(unfinished == 2);

    event_loop(unfinished);

    REQUIRE(unfinished == 0);
}

TEST_CASE("test async_operations - fixed buffer read write") {
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

    auto &buffer_table = ring.buffer_table();
    buffer_table.init(2);
    buffer_table.register_buffer(
        0, {.iov_base = (void *)msg, .iov_len = sizeof(msg)});
    buffer_table.register_buffer(1, {.iov_base = buf, .iov_len = sizeof(buf)});

    size_t unfinished = 2;
    auto writer = [&]() -> condy::Coro<void> {
        // Use buffer index 0
        int bytes_written = co_await condy::async_write(
            pipe_fds[1], condy::fixed(0, condy::buffer(msg)), 0);
        REQUIRE(bytes_written == sizeof(msg));
        --unfinished;
    };
    auto reader = [&]() -> condy::Coro<void> {
        // Use buffer index 1
        int bytes_read = co_await condy::async_read(
            pipe_fds[0], condy::fixed(1, condy::buffer(buf, sizeof(buf))), 0);
        REQUIRE(bytes_read == sizeof(msg));
        --unfinished;
    };

    writer().release().resume();
    reader().release().resume();
    REQUIRE(unfinished == 2);

    event_loop(unfinished);

    REQUIRE(unfinished == 0);
}

TEST_CASE("test async_operations - provided buffers read") {
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    const char msg[] = "Hello, condy!";

    condy::SingleThreadRuntime runtime;

    size_t unfinished = 2;
    auto writer = [&]() -> condy::Coro<void> {
        int bytes_written =
            co_await condy::async_write(pipe_fds[1], condy::buffer(msg), 0);
        REQUIRE(bytes_written == sizeof(msg));
        --unfinished;
    };
    auto reader = [&]() -> condy::Coro<void> {
        condy::ProvidedBuffers provided_buffers(2, 32);
        auto [bytes_read, buf] = co_await condy::async_read(
            pipe_fds[0], std::move(provided_buffers), 0);
        REQUIRE(bytes_read == sizeof(msg));
        REQUIRE(std::memcmp(buf.data(), msg, sizeof(msg)) == 0);
        --unfinished;
    };

    condy::co_spawn(runtime, writer()).detach();
    condy::co_spawn(runtime, reader()).detach();

    runtime.done();
    runtime.wait();

    REQUIRE(unfinished == 0);
}

TEST_CASE("test async_operations - multishot provided buffers read") {
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    char msg[16];

    condy::SingleThreadRuntime runtime;

    const int times = 5;

    size_t unfinished = 2;
    auto writer = [&]() -> condy::Coro<void> {
        for (size_t i = 0; i < times; i++) {
            std::fill_n(msg, 16, i + 1);
            int bytes_written = co_await condy::async_write(
                pipe_fds[1], condy::buffer(msg, 16), 0);
            REQUIRE(bytes_written == sizeof(msg));
        }
        close(pipe_fds[1]);
        --unfinished;
    };

    // TODO: Hard to use in this case, need to implement something like
    // condy::will_push(ch)
    int count = 0;
    auto multishot =
        [&](std::pair<int, condy::ProvidedBufferEntry> r) -> condy::Coro<void> {
        auto &[n, buf] = r;
        REQUIRE(n == 16);
        char *data = reinterpret_cast<char *>(buf.data());
        for (int i = 0; i < n; i++) {
            REQUIRE(data[i] == count + 1);
        }
        count++;
        co_return;
    };

    auto reader = [&]() -> condy::Coro<void> {
        condy::ProvidedBuffers provided_buffers(times, 16);
        auto [n, buf] = co_await condy::async_read_multishot(
            pipe_fds[0], provided_buffers, 0, condy::will_spawn(multishot));
        REQUIRE(n == 0);
        --unfinished;
    };

    condy::co_spawn(runtime, writer()).detach();
    condy::co_spawn(runtime, reader()).detach();

    runtime.done();
    runtime.wait();

    REQUIRE(unfinished == 0);
}

namespace {

void create_tcp_socketpair(int sv[2]) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listener >= 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // Let the OS choose the port

    int r = bind(listener, (sockaddr *)&addr, sizeof(addr));
    REQUIRE(r == 0);

    r = listen(listener, 1);
    REQUIRE(r == 0);

    socklen_t addrlen = sizeof(addr);
    r = getsockname(listener, (sockaddr *)&addr, &addrlen);
    REQUIRE(r == 0);

    sv[0] = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(sv[0] >= 0);

    r = connect(sv[0], (sockaddr *)&addr, sizeof(addr));
    REQUIRE(r == 0);

    sv[1] = accept(listener, nullptr, nullptr);
    REQUIRE(sv[1] >= 0);

    close(listener);
}

} // namespace

TEST_CASE("test async_operations - zero-copy read") {
    int socket_pair[2];
    create_tcp_socketpair(socket_pair);
    const char msg[] = "Hello, condy!";

    condy::SingleThreadRuntime runtime;

    size_t unfinished = 2;
    bool free_called = false;
    auto writer = [&]() -> condy::Coro<void> {
        int bytes_written = co_await condy::async_send_zc(
            socket_pair[0], condy::buffer(msg), 0, 0,
            [&](int r) { free_called = true; });
        REQUIRE(bytes_written == sizeof(msg));
        REQUIRE(!free_called);
        --unfinished;
    };
    auto reader = [&]() -> condy::Coro<void> {
        char buffer[128];
        auto bytes_read = co_await condy::async_read(socket_pair[1],
                                                     condy::buffer(buffer), 0);
        REQUIRE(bytes_read == sizeof(msg));
        REQUIRE(std::memcmp(buffer, msg, sizeof(msg)) == 0);
        --unfinished;
    };

    condy::co_spawn(runtime, writer()).detach();
    condy::co_spawn(runtime, reader()).detach();

    runtime.done();
    runtime.wait();

    REQUIRE(unfinished == 0);
    REQUIRE(free_called == true);
}