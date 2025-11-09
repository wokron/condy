#include "condy/awaiter_operations.hpp"
#include "condy/channel.hpp"
#include "condy/coro.hpp"
#include "condy/sync_wait.hpp"
#include <condy/async_operations.hpp>
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
                               &addrlen, 0, session_func) ||
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