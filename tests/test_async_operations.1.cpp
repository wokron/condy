#include "condy/buffers.hpp"
#include "condy/channel.hpp"
#include "condy/provided_buffers.hpp"
#include "condy/runtime.hpp"
#include "condy/sync_wait.hpp"
#include "helpers.hpp"
#include <cerrno>
#include <condy/async_operations.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <netinet/in.h>
#include <string_view>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

TEST_CASE("test async_operations - splice fixed fd") {
    int pipe_fds1[2], pipe_fds2[2];
    REQUIRE(pipe(pipe_fds1) == 0);
    REQUIRE(pipe(pipe_fds2) == 0);

    const char *msg = "Hello, condy!";
    size_t msg_len = std::strlen(msg);

    // Write message to the first pipe
    ssize_t bytes_written = write(pipe_fds1[1], msg, msg_len);
    REQUIRE(bytes_written == msg_len);

    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_runtime().fd_table();
        fd_table.init(4);

        auto r = co_await condy::async_files_update(pipe_fds1, 2, 0);
        REQUIRE(r == 2);
        auto r2 = co_await condy::async_files_update(pipe_fds2, 2, 2);
        REQUIRE(r2 == 2);

        // Splice data from pipe_fds1[0] to pipe_fds2[1]
        ssize_t bytes_spliced = co_await condy::async_splice(
            condy::fixed(0), -1, condy::fixed(3), -1, msg_len, 0);
        REQUIRE(bytes_spliced == msg_len);
    };
    condy::sync_wait(func());

    // Read message from the second pipe
    char buffer[64] = {0};
    ssize_t bytes_read = read(pipe_fds2[0], buffer, sizeof(buffer));
    REQUIRE(bytes_read == msg_len);
    REQUIRE(std::strcmp(buffer, msg) == 0);
}

TEST_CASE("test async_operations - recvmsg multishot") {
    int sv[2];
    create_tcp_socketpair(sv);

    const size_t times = 5;

    const char *msg = "Hello, condy multishot!";
    size_t msg_len = std::strlen(msg);

    auto sender = [&]() -> condy::Coro<void> {
        for (size_t i = 0; i < times; ++i) {
            ssize_t n = co_await condy::async_send(
                sv[0], condy::buffer(msg, msg_len), 0);
            REQUIRE(n == msg_len);
        }
    };

    auto func = [&]() -> condy::Coro<void> {
        struct msghdr msg_hdr {};
        msg_hdr.msg_iov = nullptr;
        msg_hdr.msg_iovlen = 0;

        condy::Channel<std::pair<int, condy::ProvidedBuffer>> channel(8);

        condy::ProvidedBufferPool buf_pool(4, 256);

        auto t = condy::co_spawn(sender());

        auto [n, buf] = co_await condy::async_recvmsg_multishot(
            sv[1], &msg_hdr, 0, buf_pool, condy::will_push(channel));
        REQUIRE(n == -ENOBUFS);

        co_await std::move(t);

        REQUIRE(channel.size() == 4);

        for (size_t i = 0; i < 4; ++i) {
            auto [n, buf] = co_await channel.pop();
            auto *out = io_uring_recvmsg_validate(buf.data(), n, &msg_hdr);
            REQUIRE(n > msg_len);
            void *payload = io_uring_recvmsg_payload(out, &msg_hdr);
            size_t length = io_uring_recvmsg_payload_length(out, n, &msg_hdr);
            REQUIRE(length == msg_len);
            REQUIRE(std::memcmp(payload, msg, msg_len) == 0);
        }
    };
    condy::sync_wait(func());

    close(sv[0]);
    close(sv[1]);
}

TEST_CASE("test async_operations - cancel fd") {
    auto canceller = [&](int fd) -> condy::Coro<void> {
        int r = co_await condy::async_cancel_fd(fd, 0);
        REQUIRE(r == 0);
    };

    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    auto func = [&]() -> condy::Coro<void> {
        auto t = condy::co_spawn(canceller(pipe_fds[0]));
        char buffer[128];

        auto r = co_await condy::async_read(pipe_fds[0],
                                            condy::buffer(buffer, 128), 0);
        REQUIRE(r == -ECANCELED);

        co_await std::move(t);
    };

    condy::sync_wait(func());
}

TEST_CASE("test async_operations - read fixed buffer") {
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    const char *msg = "Hello, condy!";
    size_t msg_len = std::strlen(msg);

    ssize_t r = ::write(pipe_fds[1], msg, msg_len);
    REQUIRE(r == msg_len);

    auto func = [&]() -> condy::Coro<void> {
        auto &buffer_table = condy::current_runtime().buffer_table();
        buffer_table.init(1);
        char buf_storage[64];
        iovec buf_storage_iov{
            .iov_base = buf_storage,
            .iov_len = sizeof(buf_storage),
        };
        buffer_table.update(0, &buf_storage_iov, 1);

        ssize_t n = co_await condy::async_read(
            pipe_fds[0], condy::fixed(0, condy::buffer(buf_storage, 64)), 0);
        REQUIRE(n == msg_len);
        REQUIRE(std::memcmp(buf_storage, msg, msg_len) == 0);
    };
    condy::sync_wait(func());

    close(pipe_fds[0]);
    close(pipe_fds[1]);
}

#if !IO_URING_CHECK_VERSION(2, 10) // >= 2.10
TEST_CASE("test async_operations - readv fixed buffer") {
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    const char *msg = "Hello, condy!";
    size_t msg_len = std::strlen(msg);

    ssize_t r = ::write(pipe_fds[1], msg, msg_len);
    REQUIRE(r == msg_len);

    auto func = [&]() -> condy::Coro<void> {
        auto &buffer_table = condy::current_runtime().buffer_table();
        buffer_table.init(1);
        char buf_storage[64];
        iovec buf_storage_iov{
            .iov_base = buf_storage,
            .iov_len = sizeof(buf_storage),
        };
        buffer_table.update(0, &buf_storage_iov, 1);

        size_t middle = msg_len / 2;
        iovec read_iovs[2] = {
            {
                .iov_base = buf_storage,
                .iov_len = middle,
            },
            {
                .iov_base = static_cast<char *>(buf_storage) + middle,
                .iov_len = msg_len - middle,
            },
        };

        ssize_t n = co_await condy::async_readv(
            pipe_fds[0], condy::fixed(0, read_iovs), 2, 0, 0);
        REQUIRE(n == msg_len);
        REQUIRE(std::memcmp(buf_storage, msg, msg_len) == 0);
    };
    condy::sync_wait(func());

    close(pipe_fds[0]);
    close(pipe_fds[1]);
}
#endif

TEST_CASE("test async_operations - read provided buffer") {
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    const char *msg = "Hello, condy provided buffer!";
    size_t msg_len = std::strlen(msg);

    ssize_t r = ::write(pipe_fds[1], msg, msg_len);
    REQUIRE(r == msg_len);

    auto func = [&]() -> condy::Coro<void> {
        condy::ProvidedBufferPool buf_pool(4, 64);

        auto [n, buf] = co_await condy::async_read(pipe_fds[0], buf_pool, 0);
        REQUIRE(n == msg_len);
        REQUIRE(std::memcmp(buf.data(), msg, msg_len) == 0);
    };
    condy::sync_wait(func());

    close(pipe_fds[0]);
    close(pipe_fds[1]);
}

#if !IO_URING_CHECK_VERSION(2, 8) // >= 2.8
TEST_CASE("test async_operations - read incr provided buffer") {
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    const char *msg = "Hello, condy!";
    size_t msg_len = std::strlen(msg);

    ssize_t r = ::write(pipe_fds[1], msg, msg_len);
    REQUIRE(r == msg_len);

    auto func = [&]() -> condy::Coro<void> {
        condy::ProvidedBufferPool buf_pool(4, 64, IOU_PBUF_RING_INC);

        auto [n, buf] = co_await condy::async_read(pipe_fds[0], buf_pool, 0);
        REQUIRE(n == msg_len);
        REQUIRE(std::memcmp(buf.data(), msg, msg_len) == 0);
        REQUIRE(buf.owns_buffer() == false);

        ssize_t r = ::write(pipe_fds[1], msg, msg_len);
        REQUIRE(r == msg_len);
        auto [n2, buf2] = co_await condy::async_read(pipe_fds[0], buf_pool, 0);
        REQUIRE(n2 == msg_len);
        REQUIRE(std::memcmp(buf2.data(), msg, msg_len) == 0);
        REQUIRE(buf2.owns_buffer() == false);
    };
    condy::sync_wait(func());

    close(pipe_fds[0]);
    close(pipe_fds[1]);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
TEST_CASE("test async_operations - recv bundle provided buffer") {
    int sv[2];
    create_tcp_socketpair(sv);

    const char *msg = "Hello, condy!";
    size_t msg_len = std::strlen(msg);

    ssize_t r = ::write(sv[1], msg, msg_len);
    REQUIRE(r == msg_len);

    auto func = [&]() -> condy::Coro<void> {
        condy::ProvidedBufferPool buf_pool(4, 8);

        auto [n, bufs] =
            co_await condy::async_recv(sv[0], condy::bundled(buf_pool), 0);
        REQUIRE(n == msg_len);
        REQUIRE(bufs.size() == 2);
        char temp[64];
        std::memcpy(temp, bufs[0].data(), bufs[0].size());
        std::memcpy(temp + bufs[0].size(), bufs[1].data(), n - bufs[0].size());
        REQUIRE(std::memcmp(temp, msg, msg_len) == 0);
    };
    condy::sync_wait(func());

    close(sv[0]);
    close(sv[1]);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 8) // >= 2.8
TEST_CASE("test async_operations - recv incr and bundle provided buffer") {
    int sv[2];
    create_tcp_socketpair(sv);

    const char *msg = "Hello, condy!";
    size_t msg_len = std::strlen(msg);

    ssize_t r = ::write(sv[1], msg, msg_len);
    REQUIRE(r == msg_len);

    auto func = [&]() -> condy::Coro<void> {
        condy::ProvidedBufferPool buf_pool(4, 16, IOU_PBUF_RING_INC);

        auto [n, bufs] =
            co_await condy::async_recv(sv[0], condy::bundled(buf_pool), 0);
        REQUIRE(n == msg_len);
        REQUIRE(bufs.size() == 1);
        REQUIRE(bufs[0].owns_buffer() == false);
        REQUIRE(std::memcmp(bufs[0].data(), msg, msg_len) == 0);

        ssize_t r = ::write(sv[1], msg, msg_len);
        REQUIRE(r == msg_len);
        r = write(sv[1], msg, msg_len);
        REQUIRE(r == msg_len);

        auto [n2, bufs2] =
            co_await condy::async_recv(sv[0], condy::bundled(buf_pool), 0);
        REQUIRE(n2 == msg_len * 2);
        REQUIRE(bufs2.size() == 3); // 3 + 16 + 16
        REQUIRE(bufs2[0].size() == 3);
        REQUIRE(bufs2[1].size() == 16);
        REQUIRE(bufs2[2].size() == 16);

        std::string actual;
        ssize_t rest = n2;
        for (const auto &buf : bufs2) {
            REQUIRE(buf.owns_buffer());
            actual.append(static_cast<char *>(buf.data()),
                          std::min<size_t>(buf.size(), rest));
            rest -= static_cast<ssize_t>(buf.size());
        }
        std::string expected;
        expected.append(msg, msg_len);
        expected.append(msg, msg_len);
        REQUIRE(actual == expected);
    };
    condy::sync_wait(func());

    close(sv[0]);
    close(sv[1]);
}
#endif

TEST_CASE("test async_operations - write fixed buffer") {
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    char msg[] = "Hello, condy write fixed!";
    size_t msg_len = std::strlen(msg);

    auto func = [&]() -> condy::Coro<void> {
        auto &buffer_table = condy::current_runtime().buffer_table();
        buffer_table.init(1);
        iovec buf_storage_iov{
            .iov_base = const_cast<char *>(msg),
            .iov_len = msg_len,
        };
        buffer_table.update(0, &buf_storage_iov, 1);

        ssize_t n = co_await condy::async_write(
            pipe_fds[1], condy::fixed(0, condy::buffer(msg, msg_len)), 0);
        REQUIRE(n == msg_len);
    };
    condy::sync_wait(func());

    char read_buf[64];
    ssize_t n = ::read(pipe_fds[0], read_buf, sizeof(read_buf));
    REQUIRE(n == msg_len);
    REQUIRE(std::memcmp(read_buf, msg, msg_len) == 0);

    close(pipe_fds[0]);
    close(pipe_fds[1]);
}

#if !IO_URING_CHECK_VERSION(2, 10) // >= 2.10
TEST_CASE("test async_operations - writev fixed buffer") {
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    char msg[] = "Hello, condy write fixed!";
    size_t msg_len = std::strlen(msg);

    auto func = [&]() -> condy::Coro<void> {
        auto &buffer_table = condy::current_runtime().buffer_table();
        buffer_table.init(1);
        iovec buf_storage_iov{
            .iov_base = const_cast<char *>(msg),
            .iov_len = msg_len,
        };
        buffer_table.update(0, &buf_storage_iov, 1);

        size_t middle = msg_len / 2;
        iovec write_iovs[2] = {
            {
                .iov_base = msg,
                .iov_len = middle,
            },
            {
                .iov_base = msg + middle,
                .iov_len = msg_len - middle,
            },
        };

        ssize_t n = co_await condy::async_writev(
            pipe_fds[1], condy::fixed(0, write_iovs), 2, 0, 0);
        REQUIRE(n == msg_len);
    };
    condy::sync_wait(func());

    char read_buf[64];
    ssize_t n = ::read(pipe_fds[0], read_buf, sizeof(read_buf));
    REQUIRE(n == msg_len);
    REQUIRE(std::memcmp(read_buf, msg, msg_len) == 0);

    close(pipe_fds[0]);
    close(pipe_fds[1]);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
TEST_CASE("test async_operations - send provided buffer") {
    int sv[2];
    create_tcp_socketpair(sv);

    char msg[] = "Hello, condy!";
    size_t msg_len = std::strlen(msg);

    auto func = [&]() -> condy::Coro<void> {
        condy::ProvidedBufferQueue queue(4);
        auto bid = queue.push(condy::buffer(msg, msg_len));
        auto [r, binfo] = co_await condy::async_send(sv[1], queue, 0);
        REQUIRE(r == msg_len);
        REQUIRE(binfo.num_buffers == 1);
        REQUIRE(binfo.bid == bid);
    };
    condy::sync_wait(func());

    char read_buf[64];
    ssize_t n = ::read(sv[0], read_buf, sizeof(read_buf));
    REQUIRE(n == msg_len);
    REQUIRE(std::memcmp(read_buf, msg, msg_len) == 0);

    close(sv[0]);
    close(sv[1]);
}
#endif

TEST_CASE("test async_operations - sendto") {
    int sender_fd = socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(sender_fd >= 0);
    int receiver_fd = socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(receiver_fd >= 0);
    sockaddr_in recv_addr{};
    recv_addr.sin_family = AF_INET;
    recv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    recv_addr.sin_port = 0; // Let OS choose the port

    int r = bind(receiver_fd, (sockaddr *)&recv_addr, sizeof(recv_addr));
    REQUIRE(r == 0);

    socklen_t addrlen = sizeof(recv_addr);
    r = getsockname(receiver_fd, (sockaddr *)&recv_addr, &addrlen);
    REQUIRE(r == 0);

    const char *msg = "Hello, condy!";
    size_t msg_len = std::strlen(msg);

    auto func = [&]() -> condy::Coro<void> {
        ssize_t n = co_await condy::async_sendto(
            sender_fd, condy::buffer(msg, msg_len), 0, (sockaddr *)&recv_addr,
            sizeof(recv_addr));
        REQUIRE(n == msg_len);
    };
    condy::sync_wait(func());

    char recv_buf[64];
    ssize_t n = ::recv(receiver_fd, recv_buf, sizeof(recv_buf), 0);
    REQUIRE(n == msg_len);
    REQUIRE(std::memcmp(recv_buf, msg, msg_len) == 0);

    close(sender_fd);
    close(receiver_fd);
}

TEST_CASE("test async_operations - send zero copy") {
    int sv[2];
    create_tcp_socketpair(sv);

    const char *msg = "Hello, condy!";
    size_t msg_len = std::strlen(msg);

    auto func = [&]() -> condy::Coro<void> {
        condy::Channel<int> channel(1);
        ssize_t n =
            co_await condy::async_send_zc(sv[1], condy::buffer(msg, msg_len), 0,
                                          0, condy::will_push(channel));
        REQUIRE(n == msg_len);
        co_await channel.pop();
    };

    condy::sync_wait(func());
}

TEST_CASE("test async_operations - test splice - basic") {
    ssize_t r;
    int pipe_fds[2];
    int pipe_fds2[2];
    REQUIRE(pipe(pipe_fds) == 0);
    REQUIRE(pipe(pipe_fds2) == 0);

    auto msg = generate_data(1024);
    r = write(pipe_fds[1], msg.data(), msg.size());
    REQUIRE(r == msg.size());
    auto func = [&]() -> condy::Coro<void> {
        ssize_t n = co_await condy::async_splice(pipe_fds[0], -1, pipe_fds2[1],
                                                 -1, msg.size(), 0);
        REQUIRE(n == msg.size());
    };
    condy::sync_wait(func());

    char buffer[2048];
    r = read(pipe_fds2[0], buffer, sizeof(buffer));
    REQUIRE(r == msg.size());
    REQUIRE(std::string_view(buffer, r) == msg);

    close(pipe_fds[0]);
    close(pipe_fds[1]);
    close(pipe_fds2[0]);
    close(pipe_fds2[1]);
}

TEST_CASE("test async_operations - test splice - fixed fd") {
    ssize_t r;
    int pipe_fds[2];
    int pipe_fds2[2];
    REQUIRE(pipe(pipe_fds) == 0);
    REQUIRE(pipe(pipe_fds2) == 0);

    const size_t data_size = 1024;

    auto msg = generate_data(data_size);
    r = write(pipe_fds[1], msg.data(), msg.size());
    REQUIRE(r == msg.size());
    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_runtime().fd_table();
        fd_table.init(4);
        auto r1 = co_await condy::async_files_update(pipe_fds, 2, 0);
        REQUIRE(r1 == 2);
        auto r2 = co_await condy::async_files_update(pipe_fds2, 2, 2);
        REQUIRE(r2 == 2);

        // 1. in_fd fixed
        size_t data_size1 = data_size / 4;
        r = co_await condy::async_splice(condy::fixed(0), -1, pipe_fds2[1], -1,
                                         data_size1, 0);
        REQUIRE(r == data_size1);

        // 2. out_fd fixed
        size_t data_size2 = data_size / 4;
        r = co_await condy::async_splice(pipe_fds[0], -1, condy::fixed(3), -1,
                                         data_size2, 0);
        REQUIRE(r == data_size2);

        // 3. both fixed
        size_t data_size3 = data_size - data_size1 - data_size2;
        r = co_await condy::async_splice(condy::fixed(0), -1, condy::fixed(3),
                                         -1, data_size3, 0);
        REQUIRE(r == data_size3);
    };
    condy::sync_wait(func());

    char buffer[2048];
    r = read(pipe_fds2[0], buffer, sizeof(buffer));
    REQUIRE(r == msg.size());
    REQUIRE(std::string_view(buffer, r) == msg);

    close(pipe_fds[0]);
    close(pipe_fds[1]);
    close(pipe_fds2[0]);
    close(pipe_fds2[1]);
}

TEST_CASE("test async_operations - test tee - basic") {
    ssize_t r;
    int pipe_fds[2];
    int pipe_fds2[2];
    REQUIRE(pipe(pipe_fds) == 0);
    REQUIRE(pipe(pipe_fds2) == 0);

    auto msg = generate_data(1024);
    r = write(pipe_fds[1], msg.data(), msg.size());
    REQUIRE(r == msg.size());
    auto func = [&]() -> condy::Coro<void> {
        ssize_t n =
            co_await condy::async_tee(pipe_fds[0], pipe_fds2[1], msg.size(), 0);
        REQUIRE(n == msg.size());
    };
    condy::sync_wait(func());

    char buffer[2048];
    r = read(pipe_fds2[0], buffer, sizeof(buffer));
    REQUIRE(r == msg.size());
    REQUIRE(std::string_view(buffer, r) == msg);

    // Original pipe should still have the data
    r = read(pipe_fds[0], buffer, sizeof(buffer));
    REQUIRE(r == msg.size());
    REQUIRE(std::string_view(buffer, r) == msg);

    close(pipe_fds[0]);
    close(pipe_fds[1]);
    close(pipe_fds2[0]);
    close(pipe_fds2[1]);
}

TEST_CASE("test async_operations - test tee - fixed fd") {
    ssize_t r;
    int pipe_fds[2];
    int pipe_fds2[2];
    REQUIRE(pipe(pipe_fds) == 0);
    REQUIRE(pipe(pipe_fds2) == 0);

    const size_t data_size = 1024;

    auto msg = generate_data(data_size);
    r = write(pipe_fds[1], msg.data(), msg.size());
    REQUIRE(r == msg.size());
    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_runtime().fd_table();
        fd_table.init(4);
        auto r1 = co_await condy::async_files_update(pipe_fds, 2, 0);
        REQUIRE(r1 == 2);
        auto r2 = co_await condy::async_files_update(pipe_fds2, 2, 2);
        REQUIRE(r2 == 2);

        size_t tee_data_size = data_size / 4;

        // 1. in_fd fixed
        r = co_await condy::async_tee(condy::fixed(0), pipe_fds2[1],
                                      tee_data_size, 0);
        REQUIRE(r == tee_data_size);

        // 2. out_fd fixed
        r = co_await condy::async_tee(pipe_fds[0], condy::fixed(3),
                                      tee_data_size, 0);
        REQUIRE(r == tee_data_size);

        // 3. both fixed
        r = co_await condy::async_tee(condy::fixed(0), condy::fixed(3),
                                      tee_data_size, 0);
        REQUIRE(r == tee_data_size);
    };
    condy::sync_wait(func());

    char buffer[2048];
    r = read(pipe_fds2[0], buffer, sizeof(buffer));
    REQUIRE(r == 3 * (data_size / 4));
    std::string actual(buffer, r);
    std::string expected;
    for (size_t i = 0; i < 3; ++i) {
        expected.append(msg.data(), data_size / 4);
    }
    REQUIRE(actual == expected);

    // Original pipe should still have the data
    r = read(pipe_fds[0], buffer, sizeof(buffer));
    REQUIRE(r == msg.size());
    std::string actual_orig(buffer, r);
    REQUIRE(actual_orig == msg);

    close(pipe_fds[0]);
    close(pipe_fds[1]);
    close(pipe_fds2[0]);
    close(pipe_fds2[1]);
}

TEST_CASE("test async_operations - test readv - basic") {
    ssize_t r;
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    auto msg = generate_data(1024);
    r = write(pipe_fds[1], msg.data(), msg.size());
    REQUIRE(r == msg.size());
    auto func = [&]() -> condy::Coro<void> {
        // Test fixed fd as well
        auto &fd_table = condy::current_runtime().fd_table();
        fd_table.init(2);
        r = co_await condy::async_files_update(pipe_fds, 2, 0);
        REQUIRE(r == 2);

        char bufs[4][256];
        iovec iovs[4];
        for (int i = 0; i < 4; i++) {
            iovs[i].iov_base = bufs[i];
            iovs[i].iov_len = sizeof(bufs[i]);
        }

        ssize_t n = co_await condy::async_readv(condy::fixed(0), iovs, 4, 0, 0);
        REQUIRE(n == msg.size());

        std::string actual;
        for (int i = 0; i < 4; ++i) {
            actual.append(static_cast<char *>(iovs[i].iov_base),
                          iovs[i].iov_len);
        }
        REQUIRE(actual == msg);
    };
    condy::sync_wait(func());

    close(pipe_fds[0]);
    close(pipe_fds[1]);
}

#if !IO_URING_CHECK_VERSION(2, 10) // >= 2.10
TEST_CASE("test async_operations - test readv - fixed buffer") {
    ssize_t r;
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    auto msg = generate_data(1024);
    r = write(pipe_fds[1], msg.data(), msg.size());
    REQUIRE(r == msg.size());
    auto func = [&]() -> condy::Coro<void> {
        char bufs[4][256];

        auto &buffer_table = condy::current_runtime().buffer_table();
        buffer_table.init(1);
        iovec register_iov{
            .iov_base = bufs,
            .iov_len = sizeof(bufs),
        };
        buffer_table.update(0, &register_iov, 1);

        iovec iovs[4];
        for (int i = 0; i < 4; i++) {
            iovs[i].iov_base = bufs[i];
            iovs[i].iov_len = sizeof(bufs[i]);
        }
        ssize_t n = co_await condy::async_readv(pipe_fds[0],
                                                condy::fixed(0, iovs), 4, 0, 0);
        REQUIRE(n == msg.size());
        std::string actual;
        for (int i = 0; i < 4; ++i) {
            actual.append(static_cast<char *>(iovs[i].iov_base),
                          iovs[i].iov_len);
        }
        REQUIRE(actual == msg);
    };
    condy::sync_wait(func());

    close(pipe_fds[0]);
    close(pipe_fds[1]);
}
#endif

TEST_CASE("test async_operations - test writev - basic") {
    ssize_t r;
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    auto msg = generate_data(1024);
    auto func = [&]() -> condy::Coro<void> {
        // Test fixed fd as well
        auto &fd_table = condy::current_runtime().fd_table();
        fd_table.init(2);
        r = co_await condy::async_files_update(pipe_fds, 2, 0);
        REQUIRE(r == 2);

        char bufs[4][256];
        iovec iovs[4];
        for (int i = 0; i < 4; i++) {
            std::memcpy(bufs[i], msg.data() + static_cast<ptrdiff_t>(i * 256),
                        256);
            iovs[i].iov_base = bufs[i];
            iovs[i].iov_len = sizeof(bufs[i]);
        }

        ssize_t n =
            co_await condy::async_writev(condy::fixed(1), iovs, 4, 0, 0);
        REQUIRE(n == msg.size());
    };
    condy::sync_wait(func());

    char read_buf[2048];
    r = read(pipe_fds[0], read_buf, sizeof(read_buf));
    REQUIRE(r == msg.size());
    REQUIRE(std::string_view(read_buf, r) == msg);

    close(pipe_fds[0]);
    close(pipe_fds[1]);
}

#if !IO_URING_CHECK_VERSION(2, 10) // >= 2.10
TEST_CASE("test async_operations - test writev - fixed buffer") {
    ssize_t r;
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    auto msg = generate_data(1024);
    auto func = [&]() -> condy::Coro<void> {
        char bufs[4][256];

        auto &buffer_table = condy::current_runtime().buffer_table();
        buffer_table.init(1);
        iovec register_iov{
            .iov_base = bufs,
            .iov_len = sizeof(bufs),
        };
        buffer_table.update(0, &register_iov, 1);

        iovec iovs[4];
        for (int i = 0; i < 4; i++) {
            std::memcpy(bufs[i], msg.data() + static_cast<ptrdiff_t>(i * 256),
                        256);
            iovs[i].iov_base = bufs[i];
            iovs[i].iov_len = sizeof(bufs[i]);
        }

        ssize_t n = co_await condy::async_writev(
            pipe_fds[1], condy::fixed(0, iovs), 4, 0, 0);
        REQUIRE(n == msg.size());
    };
    condy::sync_wait(func());

    char read_buf[2048];
    r = read(pipe_fds[0], read_buf, sizeof(read_buf));
    REQUIRE(r == msg.size());
    REQUIRE(std::string_view(read_buf, r) == msg);

    close(pipe_fds[0]);
    close(pipe_fds[1]);
}
#endif

TEST_CASE("test async_operations - test recvmsg - basic") {
    int sv[2];
    create_tcp_socketpair(sv);

    auto msg = generate_data(1024);
    ssize_t r = write(sv[1], msg.data(), msg.size());
    REQUIRE(r == msg.size());
    auto func = [&]() -> condy::Coro<void> {
        struct msghdr msg_hdr {};
        char buf_storage[1024];
        iovec iov{
            .iov_base = buf_storage,
            .iov_len = sizeof(buf_storage),
        };
        msg_hdr.msg_iov = &iov;
        msg_hdr.msg_iovlen = 1;

        ssize_t n = co_await condy::async_recvmsg(sv[0], &msg_hdr, 0);
        REQUIRE(n == msg.size());
        REQUIRE(std::string_view(buf_storage, n) == msg);
    };
    condy::sync_wait(func());

    close(sv[0]);
    close(sv[1]);
}

TEST_CASE("test async_operations - test recvmsg - multishot") {
    int sv[2];
    create_tcp_socketpair(sv);

    const size_t data_size = 1024;

    auto msg = generate_data(data_size);

    ssize_t r = write(sv[1], msg.data(), msg.size());
    REQUIRE(r == msg.size());

    close(sv[1]);

    auto func = [&]() -> condy::Coro<void> {
        struct msghdr msg_hdr {};
        size_t count = 0;
        std::string actual;

        condy::ProvidedBufferQueue queue(4);
        char bufs[2][256];
        uint16_t bid;
        bid = queue.push(condy::buffer(bufs[0]));
        REQUIRE(bid == 0);
        bid = queue.push(condy::buffer(bufs[1]));
        REQUIRE(bid == 1);

        auto [res, binfo] = co_await condy::async_recvmsg_multishot(
            sv[0], &msg_hdr, 0, queue, [&](auto r) {
                auto &[n, binfo] = r;
                REQUIRE(n == 256);
                REQUIRE(binfo.num_buffers == 1);
                REQUIRE(binfo.bid == count);

                auto *out =
                    io_uring_recvmsg_validate(bufs[binfo.bid], n, &msg_hdr);
                void *payload = io_uring_recvmsg_payload(out, &msg_hdr);
                size_t length =
                    io_uring_recvmsg_payload_length(out, n, &msg_hdr);
                actual.append(static_cast<char *>(payload), length);
                count++;
            });
        REQUIRE(res == -ENOBUFS);
        REQUIRE(count == 2);

        condy::ProvidedBufferPool buf_pool(16, 256);

        auto [res2, buf] = co_await condy::async_recvmsg_multishot(
            sv[0], &msg_hdr, 0, buf_pool, [&](auto r) {
                auto &[n, buf] = r;
                auto *out = io_uring_recvmsg_validate(buf.data(), n, &msg_hdr);
                void *payload = io_uring_recvmsg_payload(out, &msg_hdr);
                size_t length =
                    io_uring_recvmsg_payload_length(out, n, &msg_hdr);
                actual.append(static_cast<char *>(payload), length);
            });
        REQUIRE(res2 != -ENOBUFS);
        if (res2 > 0) {
            auto *out = io_uring_recvmsg_validate(buf.data(), res2, &msg_hdr);
            void *payload = io_uring_recvmsg_payload(out, &msg_hdr);
            size_t length =
                io_uring_recvmsg_payload_length(out, res2, &msg_hdr);
            actual.append(static_cast<char *>(payload), length);
        }

        REQUIRE(actual == msg);
    };
    condy::sync_wait(func());

    close(sv[0]);
}
