#include "condy/awaiter_operations.hpp"
#include "condy/buffers.hpp"
#include "condy/channel.hpp"
#include "condy/provided_buffers.hpp"
#include "condy/runtime.hpp"
#include "condy/sync_wait.hpp"
#include <cerrno>
#include <condy/async_operations.hpp>
#include <cstddef>
#include <cstring>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <liburing.h>
#include <linux/futex.h>
#include <netinet/in.h>
#include <string_view>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <thread>
#include <unistd.h>

namespace {

int create_accept_socket() {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(sockfd >= 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // Let the OS choose the port

    int r = bind(sockfd, (sockaddr *)&addr, sizeof(addr));
    REQUIRE(r == 0);

    r = listen(sockfd, 1);
    REQUIRE(r == 0);

    return sockfd;
}

void create_tcp_socketpair(int sv[2]) {
    int r;
    int listener = create_accept_socket();

    sockaddr_in addr{};
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

std::string generate_data(size_t size) {
    std::string data;
    data.resize(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = 'A' + (i % 26);
    }
    return data;
}

} // namespace

TEST_CASE("test async_operations - splice fixed fd") {
    int pipe_fds1[2], pipe_fds2[2];
    REQUIRE(pipe(pipe_fds1) == 0);
    REQUIRE(pipe(pipe_fds2) == 0);

    const char *msg = "Hello, condy!";
    ssize_t msg_len = std::strlen(msg);

    // Write message to the first pipe
    ssize_t bytes_written = write(pipe_fds1[1], msg, msg_len);
    REQUIRE(bytes_written == msg_len);

    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_fd_table();
        fd_table.init(4);

        auto r = co_await fd_table.async_update_files(pipe_fds1, 2, 0);
        REQUIRE(r == 2);
        auto r2 = co_await fd_table.async_update_files(pipe_fds2, 2, 2);
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
    ssize_t msg_len = std::strlen(msg);

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
    ssize_t msg_len = std::strlen(msg);

    int r = ::write(pipe_fds[1], msg, msg_len);
    REQUIRE(r == msg_len);

    auto func = [&]() -> condy::Coro<void> {
        auto &buffer_table = condy::current_buffer_table();
        buffer_table.init(1);
        char buf_storage[64];
        iovec buf_storage_iov{
            .iov_base = buf_storage,
            .iov_len = sizeof(buf_storage),
        };
        buffer_table.update_buffers(0, &buf_storage_iov, 1);

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
    ssize_t msg_len = std::strlen(msg);

    int r = ::write(pipe_fds[1], msg, msg_len);
    REQUIRE(r == msg_len);

    auto func = [&]() -> condy::Coro<void> {
        auto &buffer_table = condy::current_buffer_table();
        buffer_table.init(1);
        char buf_storage[64];
        iovec buf_storage_iov{
            .iov_base = buf_storage,
            .iov_len = sizeof(buf_storage),
        };
        buffer_table.update_buffers(0, &buf_storage_iov, 1);

        ssize_t middle = msg_len / 2;
        iovec read_iovs[2] = {
            {
                .iov_base = buf_storage,
                .iov_len = static_cast<size_t>(middle),
            },
            {
                .iov_base = static_cast<char *>(buf_storage) + middle,
                .iov_len = static_cast<size_t>(msg_len - middle),
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
    ssize_t msg_len = std::strlen(msg);

    int r = ::write(pipe_fds[1], msg, msg_len);
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
    ssize_t msg_len = std::strlen(msg);

    int r = ::write(pipe_fds[1], msg, msg_len);
    REQUIRE(r == msg_len);

    auto func = [&]() -> condy::Coro<void> {
        condy::ProvidedBufferPool buf_pool(4, 64, IOU_PBUF_RING_INC);

        auto [n, buf] = co_await condy::async_read(pipe_fds[0], buf_pool, 0);
        REQUIRE(n == msg_len);
        REQUIRE(std::memcmp(buf.data(), msg, msg_len) == 0);
        REQUIRE(buf.owns_buffer() == false);

        int r = ::write(pipe_fds[1], msg, msg_len);
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
    ssize_t msg_len = std::strlen(msg);

    int r = ::write(sv[1], msg, msg_len);
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
    ssize_t msg_len = std::strlen(msg);

    int r = ::write(sv[1], msg, msg_len);
    REQUIRE(r == msg_len);

    auto func = [&]() -> condy::Coro<void> {
        condy::ProvidedBufferPool buf_pool(4, 16, IOU_PBUF_RING_INC);

        auto [n, bufs] =
            co_await condy::async_recv(sv[0], condy::bundled(buf_pool), 0);
        REQUIRE(n == msg_len);
        REQUIRE(bufs.size() == 1);
        REQUIRE(bufs[0].owns_buffer() == false);
        REQUIRE(std::memcmp(bufs[0].data(), msg, msg_len) == 0);

        int r = ::write(sv[1], msg, msg_len);
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
        int rest = n2;
        for (const auto &buf : bufs2) {
            REQUIRE(buf.owns_buffer());
            actual.append(static_cast<char *>(buf.data()),
                          std::min<size_t>(buf.size(), rest));
            rest -= buf.size();
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
        auto &buffer_table = condy::current_buffer_table();
        buffer_table.init(1);
        iovec buf_storage_iov{
            .iov_base = const_cast<char *>(msg),
            .iov_len = msg_len,
        };
        buffer_table.update_buffers(0, &buf_storage_iov, 1);

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
        auto &buffer_table = condy::current_buffer_table();
        buffer_table.init(1);
        iovec buf_storage_iov{
            .iov_base = const_cast<char *>(msg),
            .iov_len = msg_len,
        };
        buffer_table.update_buffers(0, &buf_storage_iov, 1);

        ssize_t middle = msg_len / 2;
        iovec write_iovs[2] = {
            {
                .iov_base = msg,
                .iov_len = static_cast<size_t>(middle),
            },
            {
                .iov_base = msg + middle,
                .iov_len = static_cast<size_t>(msg_len - middle),
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
    ssize_t msg_len = std::strlen(msg);

    auto func = [&]() -> condy::Coro<void> {
        condy::Channel<int> channel(1);
        char buffer[64];
        std::memcpy(buffer, msg, msg_len);
        ssize_t n =
            co_await condy::async_send_zc(sv[1], condy::buffer(buffer, msg_len),
                                          0, 0, condy::will_push(channel));
        REQUIRE(n == msg_len);
        co_await channel.pop();
    };

    condy::sync_wait(func());
}

TEST_CASE("test async_operations - test splice - basic") {
    int r;
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
    int r;
    int pipe_fds[2];
    int pipe_fds2[2];
    REQUIRE(pipe(pipe_fds) == 0);
    REQUIRE(pipe(pipe_fds2) == 0);

    const size_t data_size = 1024;

    auto msg = generate_data(data_size);
    r = write(pipe_fds[1], msg.data(), msg.size());
    REQUIRE(r == msg.size());
    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_fd_table();
        fd_table.init(4);
        auto r1 = co_await fd_table.async_update_files(pipe_fds, 2, 0);
        REQUIRE(r1 == 2);
        auto r2 = co_await fd_table.async_update_files(pipe_fds2, 2, 2);
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
    int r;
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
    int r;
    int pipe_fds[2];
    int pipe_fds2[2];
    REQUIRE(pipe(pipe_fds) == 0);
    REQUIRE(pipe(pipe_fds2) == 0);

    const size_t data_size = 1024;

    auto msg = generate_data(data_size);
    r = write(pipe_fds[1], msg.data(), msg.size());
    REQUIRE(r == msg.size());
    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_fd_table();
        fd_table.init(4);
        auto r1 = co_await fd_table.async_update_files(pipe_fds, 2, 0);
        REQUIRE(r1 == 2);
        auto r2 = co_await fd_table.async_update_files(pipe_fds2, 2, 2);
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
    int r;
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    auto msg = generate_data(1024);
    r = write(pipe_fds[1], msg.data(), msg.size());
    REQUIRE(r == msg.size());
    auto func = [&]() -> condy::Coro<void> {
        // Test fixed fd as well
        auto &fd_table = condy::current_fd_table();
        fd_table.init(2);
        r = co_await fd_table.async_update_files(pipe_fds, 2, 0);
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
    int r;
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    auto msg = generate_data(1024);
    r = write(pipe_fds[1], msg.data(), msg.size());
    REQUIRE(r == msg.size());
    auto func = [&]() -> condy::Coro<void> {
        char bufs[4][256];

        auto &buffer_table = condy::current_buffer_table();
        buffer_table.init(1);
        iovec register_iov{
            .iov_base = bufs,
            .iov_len = sizeof(bufs),
        };
        buffer_table.update_buffers(0, &register_iov, 1);

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
    int r;
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    auto msg = generate_data(1024);
    auto func = [&]() -> condy::Coro<void> {
        // Test fixed fd as well
        auto &fd_table = condy::current_fd_table();
        fd_table.init(2);
        r = co_await fd_table.async_update_files(pipe_fds, 2, 0);
        REQUIRE(r == 2);

        char bufs[4][256];
        iovec iovs[4];
        for (int i = 0; i < 4; i++) {
            std::memcpy(bufs[i], msg.data() + i * 256, 256);
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
    int r;
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    auto msg = generate_data(1024);
    auto func = [&]() -> condy::Coro<void> {
        char bufs[4][256];

        auto &buffer_table = condy::current_buffer_table();
        buffer_table.init(1);
        iovec register_iov{
            .iov_base = bufs,
            .iov_len = sizeof(bufs),
        };
        buffer_table.update_buffers(0, &register_iov, 1);

        iovec iovs[4];
        for (int i = 0; i < 4; i++) {
            std::memcpy(bufs[i], msg.data() + i * 256, 256);
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
    int r = write(sv[1], msg.data(), msg.size());
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

    int r = write(sv[1], msg.data(), msg.size());
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

TEST_CASE("test async_operations - test sendmsg - basic") {
    int sv[2];
    create_tcp_socketpair(sv);

    auto msg = generate_data(1024);
    auto func = [&]() -> condy::Coro<void> {
        struct msghdr msg_hdr {};
        iovec iov{
            .iov_base = const_cast<char *>(msg.data()),
            .iov_len = msg.size(),
        };
        msg_hdr.msg_iov = &iov;
        msg_hdr.msg_iovlen = 1;

        ssize_t n = co_await condy::async_sendmsg(sv[1], &msg_hdr, 0);
        REQUIRE(n == msg.size());
    };
    condy::sync_wait(func());

    char read_buf[2048];
    int r = read(sv[0], read_buf, sizeof(read_buf));
    REQUIRE(r == msg.size());
    REQUIRE(std::string_view(read_buf, r) == msg);

    close(sv[0]);
    close(sv[1]);
}

TEST_CASE("test async_operations - test sendmsg - zero copy") {
    int sv[2];
    create_tcp_socketpair(sv);

    auto msg = generate_data(1024);
    auto func = [&]() -> condy::Coro<void> {
        struct msghdr msg_hdr {};
        iovec iov{
            .iov_base = const_cast<char *>(msg.data()),
            .iov_len = msg.size(),
        };
        msg_hdr.msg_iov = &iov;
        msg_hdr.msg_iovlen = 1;

        condy::Channel<int> channel(1);
        ssize_t n = co_await condy::async_sendmsg_zc(sv[1], &msg_hdr, 0,
                                                     condy::will_push(channel));
        REQUIRE(n == msg.size());
        co_await channel.pop();
    };
    condy::sync_wait(func());

    char read_buf[2048];
    int r = read(sv[0], read_buf, sizeof(read_buf));
    REQUIRE(r == msg.size());
    REQUIRE(std::string_view(read_buf, r) == msg);

    close(sv[0]);
    close(sv[1]);
}

#if !IO_URING_CHECK_VERSION(2, 10) // >= 2.10
TEST_CASE("test async_operations - test sendmsg - zero copy fixed buffer") {
    int sv[2];
    create_tcp_socketpair(sv);

    auto msg = generate_data(1024);
    auto func = [&]() -> condy::Coro<void> {
        struct msghdr msg_hdr {};
        iovec iov{
            .iov_base = msg.data(),
            .iov_len = msg.size(),
        };
        msg_hdr.msg_iov = &iov;
        msg_hdr.msg_iovlen = 1;

        auto &buffer_table = condy::current_buffer_table();
        buffer_table.init(1);
        buffer_table.update_buffers(0, &iov, 1);

        condy::Channel<int> channel(1);
        ssize_t n = co_await condy::async_sendmsg_zc(
            sv[1], condy::fixed(0, &msg_hdr), 0, condy::will_push(channel));
        REQUIRE(n == msg.size());
        co_await channel.pop();
    };
    condy::sync_wait(func());

    char read_buf[2048];
    int r = read(sv[0], read_buf, sizeof(read_buf));
    REQUIRE(r == msg.size());
    REQUIRE(std::string_view(read_buf, r) == msg);

    close(sv[0]);
    close(sv[1]);
}
#endif

TEST_CASE("test async_operations - test fsync") {
    char name[32] = "XXXXXX";
    int fd = mkstemp(name);
    REQUIRE(fd >= 0);
    auto d = condy::defer([&] {
        close(fd);
        unlink(name);
    });

    auto func = [&]() -> condy::Coro<void> {
        int r = co_await condy::async_fsync(fd, 0);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());
    close(fd);
}

TEST_CASE("test async_operations - test nop") {
    auto func = [&]() -> condy::Coro<void> {
        int r = co_await condy::async_nop();
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());
}

TEST_CASE("test async_operations - test timeout - basic") {
    auto func = [&]() -> condy::Coro<void> {
        __kernel_timespec ts = {
            .tv_sec = 0,
            .tv_nsec = 100,
        };
        int r = co_await condy::async_timeout(&ts, 0, 0);
        REQUIRE(r == -ETIME);
    };
    condy::sync_wait(func());
}

#if !IO_URING_CHECK_VERSION(2, 4) // >= 2.4
TEST_CASE("test async_operations - test timeout - multishot") {
    auto func = [&]() -> condy::Coro<void> {
        __kernel_timespec ts = {
            .tv_sec = 0,
            .tv_nsec = 100,
        };
        size_t count = 0;
        int r = co_await condy::async_timeout_multishot(&ts, 5, 0, [&](int r) {
            REQUIRE(r == -ETIME);
            count++;
        });
        REQUIRE(r == -ETIME);
        count++;
        REQUIRE(count == 5);
    };
    condy::sync_wait(func());
}
#endif

TEST_CASE("test async_operations - test accept - basic") {
    int listen_fd = create_accept_socket();
    sockaddr_in addr{};
    socklen_t addrlen = sizeof(addr);
    int r = getsockname(listen_fd, (sockaddr *)&addr, &addrlen);
    REQUIRE(r == 0);

    auto client = [&]() {
        for (int i = 0; i < 4; i++) {
            int sockfd = socket(AF_INET, SOCK_STREAM, 0);
            REQUIRE(sockfd >= 0);

            int r = connect(sockfd, (sockaddr *)&addr, sizeof(addr));
            REQUIRE(r == 0);

            close(sockfd);
        }
    };

    auto main = [&]() -> condy::Coro<void> {
        std::jthread client_thread(client);

        struct sockaddr_in addr {};
        socklen_t addrlen = sizeof(addr);
        for (int i = 0; i < 4; i++) {
            int conn_fd = co_await condy::async_accept(
                listen_fd, (sockaddr *)&addr, &addrlen, 0);
            REQUIRE(conn_fd >= 0);
            co_await condy::async_close(conn_fd);
        }
    };

    condy::sync_wait(main());
    close(listen_fd);
}

TEST_CASE("test async_operations - test accept - direct") {
    int listen_fd = create_accept_socket();
    sockaddr_in addr{};
    socklen_t addrlen = sizeof(addr);
    int r = getsockname(listen_fd, (sockaddr *)&addr, &addrlen);
    REQUIRE(r == 0);

    auto client = [&]() {
        for (int i = 0; i < 4; i++) {
            int sockfd = socket(AF_INET, SOCK_STREAM, 0);
            REQUIRE(sockfd >= 0);

            int r = connect(sockfd, (sockaddr *)&addr, sizeof(addr));
            REQUIRE(r == 0);

            close(sockfd);
        }
    };

    auto main = [&]() -> condy::Coro<void> {
        std::jthread client_thread(client);

        condy::current_fd_table().init(2);

        struct sockaddr_in addr {};
        socklen_t addrlen = sizeof(addr);
        int fd1 = co_await condy::async_accept_direct(
            listen_fd, (sockaddr *)&addr, &addrlen, 0, CONDY_FILE_INDEX_ALLOC);
        REQUIRE(fd1 >= 0);
        REQUIRE(fd1 < 2);

        int fd2 = co_await condy::async_accept_direct(
            listen_fd, (sockaddr *)&addr, &addrlen, 0, CONDY_FILE_INDEX_ALLOC);
        REQUIRE(fd2 >= 0);
        REQUIRE(fd2 < 2);

        // TODO: Where is the third connection?
        int fd3 = co_await condy::async_accept_direct(
            listen_fd, (sockaddr *)&addr, &addrlen, 0, CONDY_FILE_INDEX_ALLOC);
        REQUIRE(fd3 < 0); // Should fail, no more fixed fds available

        int r = co_await condy::async_close(condy::fixed(fd1));
        REQUIRE(r == 0);

        int fd4 = co_await condy::async_accept_direct(
            listen_fd, (sockaddr *)&addr, &addrlen, 0, CONDY_FILE_INDEX_ALLOC);
        REQUIRE(fd4 >= 0); // Should succeed now
        REQUIRE(fd4 < 2);
    };

    condy::sync_wait(main());
}

TEST_CASE("test async_operations - test accept - multishot") {
    int listen_fd = create_accept_socket();
    sockaddr_in addr{};
    socklen_t addrlen = sizeof(addr);
    int r = getsockname(listen_fd, (sockaddr *)&addr, &addrlen);
    REQUIRE(r == 0);

    auto client = [&]() {
        for (int i = 0; i < 4; i++) {
            int sockfd = socket(AF_INET, SOCK_STREAM, 0);
            REQUIRE(sockfd >= 0);

            int r = connect(sockfd, (sockaddr *)&addr, sizeof(addr));
            REQUIRE(r == 0);

            close(sockfd);
        }
    };

    auto main = [&]() -> condy::Coro<void> {
        using condy::operators::operator||;
        std::jthread client_thread(client);

        size_t count = 0;
        struct sockaddr_in addr {};
        socklen_t addrlen = sizeof(addr);

        condy::Channel<std::monostate> done_channel(1);

        auto r = co_await (
            condy::async_multishot_accept(
                listen_fd, (sockaddr *)&addr, &addrlen, 0,
                [&](int conn_fd) {
                    REQUIRE(conn_fd >= 0);
                    count++;
                    if (count == 4) {
                        REQUIRE(done_channel.try_push(std::monostate{}));
                    }
                    close(conn_fd);
                }) ||
            done_channel.pop());
        REQUIRE(r.index() == 1);
        REQUIRE(count == 4);
    };

    condy::sync_wait(main());
    close(listen_fd);
}

TEST_CASE("test async_operations - test accept - multishot direct") {
    int listen_fd = create_accept_socket();
    sockaddr_in addr{};
    socklen_t addrlen = sizeof(addr);
    int r = getsockname(listen_fd, (sockaddr *)&addr, &addrlen);
    REQUIRE(r == 0);

    auto client = [&]() {
        for (int i = 0; i < 4; i++) {
            int sockfd = socket(AF_INET, SOCK_STREAM, 0);
            REQUIRE(sockfd >= 0);

            int r = connect(sockfd, (sockaddr *)&addr, sizeof(addr));
            REQUIRE(r == 0);

            close(sockfd);
        }
    };

    auto main = [&]() -> condy::Coro<void> {
        using condy::operators::operator||;
        std::jthread client_thread(client);

        auto &fd_table = condy::current_fd_table();
        fd_table.init(4);

        size_t count = 0;
        struct sockaddr_in addr {};
        socklen_t addrlen = sizeof(addr);

        condy::Channel<std::monostate> done_channel(1);

        auto r = co_await (
            condy::async_multishot_accept_direct(
                listen_fd, (sockaddr *)&addr, &addrlen, 0,
                [&](int conn_fd) {
                    REQUIRE(conn_fd >= 0);
                    REQUIRE(conn_fd < 4);
                    count++;
                    if (count == 4) {
                        REQUIRE(done_channel.try_push(std::monostate{}));
                    }
                }) ||
            done_channel.pop());
        REQUIRE(r.index() == 1);
        REQUIRE(count == 4);
    };

    condy::sync_wait(main());
    close(listen_fd);
}

TEST_CASE("test async_operations - test cancel fd - basic") {
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    auto func = [&]() -> condy::Coro<void> {
        auto cancel_func = [&]() -> condy::Coro<void> {
            int r = co_await condy::async_cancel_fd(pipe_fds[0], 0);
            REQUIRE(r == 0);
        };

        auto t = condy::co_spawn(cancel_func());

        char buf[16];
        ssize_t n =
            co_await condy::async_read(pipe_fds[0], condy::buffer(buf), 0);
        REQUIRE(n == -ECANCELED);

        co_await std::move(t);
    };
    condy::sync_wait(func());
}

TEST_CASE("test async_operations - test cancel fd - fixed fd") {
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_fd_table();
        fd_table.init(2);
        int r = co_await fd_table.async_update_files(pipe_fds, 2, 0);
        REQUIRE(r == 2);

        auto cancel_func = [&]() -> condy::Coro<void> {
            int r = co_await condy::async_cancel_fd(condy::fixed(0), 0);
            REQUIRE(r == 0);
        };

        auto t = condy::co_spawn(cancel_func());

        char buf[16];
        ssize_t n =
            co_await condy::async_read(condy::fixed(0), condy::buffer(buf), 0);
        REQUIRE(n == -ECANCELED);

        co_await std::move(t);
    };
    condy::sync_wait(func());
}

TEST_CASE("test async_operations - test link timeout") {
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);
    using condy::operators::operator>>;
    auto func = [&]() -> condy::Coro<void> {
        char buffer[128];
        __kernel_timespec ts = {
            .tv_sec = 0,
            .tv_nsec = 1,
        };
        auto [r1, r2] = co_await (
            condy::async_read(pipe_fds[0], condy::buffer(buffer, 128), 0) >>
            condy::async_link_timeout(&ts, 0));
        REQUIRE(r1 == -ECANCELED);
        REQUIRE(r2 == -ETIME);
    };

    condy::sync_wait(func());
}

TEST_CASE("test async_operations - test connect - basic") {
    int listen_fd = create_accept_socket();
    sockaddr_in addr{};
    socklen_t addrlen = sizeof(addr);
    int r = getsockname(listen_fd, (sockaddr *)&addr, &addrlen);
    REQUIRE(r == 0);

    auto server = [&] {
        for (int i = 0; i < 4; i++) {
            int conn_fd = accept(listen_fd, nullptr, nullptr);
            REQUIRE(conn_fd >= 0);
            close(conn_fd);
        }
    };

    auto main = [&]() -> condy::Coro<void> {
        std::jthread server_thread(server);

        for (int i = 0; i < 4; i++) {
            int sockfd = socket(AF_INET, SOCK_STREAM, 0);
            REQUIRE(sockfd >= 0);

            int r = co_await condy::async_connect(sockfd, (sockaddr *)&addr,
                                                  sizeof(addr));
            REQUIRE(r == 0);

            co_await condy::async_close(sockfd);
        }
    };

    condy::sync_wait(main());

    close(listen_fd);
}

TEST_CASE("test async_operations - test connect - fixed fd") {
    int listen_fd = create_accept_socket();
    sockaddr_in addr{};
    socklen_t addrlen = sizeof(addr);
    int r = getsockname(listen_fd, (sockaddr *)&addr, &addrlen);
    REQUIRE(r == 0);

    auto server = [&] {
        for (int i = 0; i < 4; i++) {
            int conn_fd = accept(listen_fd, nullptr, nullptr);
            REQUIRE(conn_fd >= 0);
            close(conn_fd);
        }
    };

    auto main = [&]() -> condy::Coro<void> {
        std::jthread server_thread(server);

        auto &fd_table = condy::current_fd_table();
        fd_table.init(4);

        for (int i = 0; i < 4; i++) {
            int sockfd = socket(AF_INET, SOCK_STREAM, 0);
            REQUIRE(sockfd >= 0);

            r = co_await fd_table.async_update_files(&sockfd, 1, i);
            REQUIRE(r == 1);

            int r = co_await condy::async_connect(
                condy::fixed(i), (sockaddr *)&addr, sizeof(addr));
            REQUIRE(r == 0);

            co_await condy::async_close(condy::fixed(i));
        }
    };

    condy::sync_wait(main());

    close(listen_fd);
}

TEST_CASE("test async_operations - test fallocate - basic") {
    char name[32] = "XXXXXX";
    int fd = mkstemp(name);
    REQUIRE(fd >= 0);
    auto d = condy::defer([&] {
        close(fd);
        unlink(name);
    });

    auto func = [&]() -> condy::Coro<void> {
        int r = co_await condy::async_fallocate(fd, 0, 0, 1024 * 1024);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    struct stat st {};
    int r = fstat(fd, &st);
    REQUIRE(r == 0);
    REQUIRE(st.st_size == 1024 * 1024);
}

TEST_CASE("test async_operations - test fallocate - fixed fd") {
    char name[32] = "XXXXXX";
    int fd = mkstemp(name);
    REQUIRE(fd >= 0);
    auto d = condy::defer([&] {
        close(fd);
        unlink(name);
    });

    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_fd_table();
        fd_table.init(1);
        int r = co_await fd_table.async_update_files(&fd, 1, 0);
        REQUIRE(r == 1);

        r = co_await condy::async_fallocate(condy::fixed(0), 0, 0, 1024 * 1024);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    struct stat st {};
    int r = fstat(fd, &st);
    REQUIRE(r == 0);
    REQUIRE(st.st_size == 1024 * 1024);
}

TEST_CASE("test async_operations - test openat - basic") {
    char name[32] = "XXXXXX";
    int fd = mkstemp(name);
    REQUIRE(fd >= 0);
    close(fd);
    auto d = condy::defer([&] { unlink(name); });

    auto func = [&]() -> condy::Coro<void> {
        int rfd = co_await condy::async_openat(AT_FDCWD, name, O_RDONLY, 0);
        REQUIRE(rfd >= 0);
        co_await condy::async_close(rfd);
    };
    condy::sync_wait(func());
}

TEST_CASE("test async_operations - test openat - direct") {
    char name[32] = "XXXXXX";
    int fd = mkstemp(name);
    REQUIRE(fd >= 0);
    close(fd);
    auto d = condy::defer([&] { unlink(name); });

    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_fd_table();
        fd_table.init(8);

        int rfd =
            co_await condy::async_openat_direct(AT_FDCWD, name, O_RDONLY, 0, 0);
        REQUIRE(rfd == 0);

        co_await condy::async_close(condy::fixed(rfd));
    };
    condy::sync_wait(func());
}

TEST_CASE("test async_operations - test open - basic") {
    char name[32] = "XXXXXX";
    int fd = mkstemp(name);
    REQUIRE(fd >= 0);
    close(fd);
    auto d = condy::defer([&] { unlink(name); });

    auto func = [&]() -> condy::Coro<void> {
        int rfd = co_await condy::async_open(name, O_RDONLY, 0);
        REQUIRE(rfd >= 0);
        co_await condy::async_close(rfd);
    };
    condy::sync_wait(func());
}

TEST_CASE("test async_operations - test open - direct") {
    char name[32] = "XXXXXX";
    int fd = mkstemp(name);
    REQUIRE(fd >= 0);
    close(fd);
    auto d = condy::defer([&] { unlink(name); });

    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_fd_table();
        fd_table.init(8);

        int rfd = co_await condy::async_open_direct(name, O_RDONLY, 0, 0);
        REQUIRE(rfd == 0);

        co_await condy::async_close(condy::fixed(rfd));
    };
    condy::sync_wait(func());
}

TEST_CASE("test async_operations - test close") {
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_fd_table();
        fd_table.init(2);
        int r = co_await fd_table.async_update_files(pipe_fds, 2, 0);
        REQUIRE(r == 2);

        r = co_await condy::async_close(condy::fixed(0));
        REQUIRE(r == 0);
        r = co_await condy::async_close(pipe_fds[1]);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());
}

TEST_CASE("test async_operations - test read - basic") {
    int r;
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    auto msg = generate_data(1024);
    r = write(pipe_fds[1], msg.data(), msg.size());
    REQUIRE(r == msg.size());
    auto func = [&]() -> condy::Coro<void> {
        char buf[2048];
        ssize_t n =
            co_await condy::async_read(pipe_fds[0], condy::buffer(buf), 0);
        REQUIRE(n == msg.size());
        REQUIRE(std::string_view(buf, n) == msg);
    };
    condy::sync_wait(func());

    close(pipe_fds[0]);
    close(pipe_fds[1]);
}

TEST_CASE("test async_operations - test read - fixed fd") {
    int r;
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    auto msg = generate_data(1024);
    r = write(pipe_fds[1], msg.data(), msg.size());
    REQUIRE(r == msg.size());
    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_fd_table();
        fd_table.init(2);
        r = co_await fd_table.async_update_files(pipe_fds, 2, 0);
        REQUIRE(r == 2);

        char buf[2048];
        ssize_t n =
            co_await condy::async_read(condy::fixed(0), condy::buffer(buf), 0);
        REQUIRE(n == msg.size());
        REQUIRE(std::string_view(buf, n) == msg);
    };
    condy::sync_wait(func());

    close(pipe_fds[0]);
    close(pipe_fds[1]);
}

TEST_CASE("test async_operations - test read - fixed buffer") {
    int r;
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    auto msg = generate_data(1024);
    r = write(pipe_fds[1], msg.data(), msg.size());
    REQUIRE(r == msg.size());
    auto func = [&]() -> condy::Coro<void> {
        char buf[2048];

        auto &buffer_table = condy::current_buffer_table();
        buffer_table.init(1);
        iovec register_iov{
            .iov_base = buf,
            .iov_len = sizeof(buf),
        };
        buffer_table.update_buffers(0, &register_iov, 1);

        ssize_t n = co_await condy::async_read(
            pipe_fds[0], condy::fixed(0, condy::buffer(buf)), 0);
        REQUIRE(n == msg.size());
        REQUIRE(std::string_view(buf, n) == msg);
    };
    condy::sync_wait(func());

    close(pipe_fds[0]);
    close(pipe_fds[1]);
}

TEST_CASE("test async_operations - test read - provided buffer") {
    int r;
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    auto msg = generate_data(1024);
    r = write(pipe_fds[1], msg.data(), msg.size());
    REQUIRE(r == msg.size());
    auto func = [&]() -> condy::Coro<void> {
        std::string actual;

        char buf[2][256];
        condy::ProvidedBufferQueue queue(2);
        uint16_t bid = queue.push(condy::buffer(buf[0]));
        REQUIRE(bid == 0);
        bid = queue.push(condy::buffer(buf[1]));
        REQUIRE(bid == 1);

        auto [r, binfo] = co_await condy::async_read(pipe_fds[0], queue, 0);
        REQUIRE(r == 256);
        REQUIRE(binfo.num_buffers == 1);
        REQUIRE(binfo.bid == 0);
        actual.append(static_cast<char *>(buf[binfo.bid]), r);

        auto [r2, binfo2] = co_await condy::async_read(pipe_fds[0], queue, 0);
        REQUIRE(r2 == 256);
        REQUIRE(binfo2.num_buffers == 1);
        REQUIRE(binfo2.bid == 1);
        actual.append(static_cast<char *>(buf[binfo2.bid]), r2);

        condy::ProvidedBufferPool buf_pool(2, 256);
        auto [r3, buf3] = co_await condy::async_read(pipe_fds[0], buf_pool, 0);
        REQUIRE(r3 == 256);
        actual.append(static_cast<char *>(buf3.data()), r3);

        auto [r4, buf4] = co_await condy::async_read(pipe_fds[0], buf_pool, 0);
        REQUIRE(r4 == 256);
        actual.append(static_cast<char *>(buf4.data()), r4);

        REQUIRE(actual == msg);
    };
    condy::sync_wait(func());

    close(pipe_fds[0]);
    close(pipe_fds[1]);
}

#if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6
TEST_CASE("test async_operations - test read - multishot") {
    int r;
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    auto msg = generate_data(1024);
    r = write(pipe_fds[1], msg.data(), msg.size());
    REQUIRE(r == msg.size());
    close(pipe_fds[1]);

    auto func = [&]() -> condy::Coro<void> {
        size_t count = 0;
        std::string actual;

        condy::ProvidedBufferPool pool(2, 256);

        condy::Channel<condy::ProvidedBuffer> channel(2);

        auto [n, buf] = co_await condy::async_read_multishot(
            pipe_fds[0], pool, 0, [&](auto res) {
                auto &[n, buf] = res;
                REQUIRE(n == 256);
                actual.append(static_cast<char *>(buf.data()), n);
                count++;
                REQUIRE(channel.try_push(std::move(buf)));
            });
        REQUIRE(n == -ENOBUFS);
        REQUIRE(count == 2);

        auto tmp = co_await channel.pop();
        tmp.reset(); // Release the buffer back to the pool
        tmp = co_await channel.pop();
        tmp.reset(); // Release the buffer back to the pool

        auto [n2, buf2] = co_await condy::async_read_multishot(
            pipe_fds[0], pool, 0, [&](auto res) {
                auto &[n, buf] = res;
                REQUIRE(n == 256);
                actual.append(static_cast<char *>(buf.data()), n);
                count++;
            });
        REQUIRE(n2 == -ENOBUFS);
        REQUIRE(count == 4);

        REQUIRE(actual == msg);
    };
    condy::sync_wait(func());

    close(pipe_fds[0]);
}
#endif

TEST_CASE("test async_operations - test write - basic") {
    int r;
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    auto msg = generate_data(1024);
    auto func = [&]() -> condy::Coro<void> {
        ssize_t n =
            co_await condy::async_write(pipe_fds[1], condy::buffer(msg), 0);
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

TEST_CASE("test async_operations - test write - fixed fd") {
    int r;
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    auto msg = generate_data(1024);
    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_fd_table();
        fd_table.init(2);
        r = co_await fd_table.async_update_files(pipe_fds, 2, 0);
        REQUIRE(r == 2);

        ssize_t n =
            co_await condy::async_write(condy::fixed(1), condy::buffer(msg), 0);
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

TEST_CASE("test async_operations - test write - fixed buffer") {
    int r;
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    auto msg = generate_data(1024);
    auto func = [&]() -> condy::Coro<void> {
        auto &buffer_table = condy::current_buffer_table();
        buffer_table.init(1);
        iovec register_iov{
            .iov_base = msg.data(),
            .iov_len = msg.size(),
        };
        buffer_table.update_buffers(0, &register_iov, 1);

        ssize_t n = co_await condy::async_write(
            pipe_fds[1], condy::fixed(0, condy::buffer(msg)), 0);
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

TEST_CASE("test async_operations - test statx") {
    char name[32] = "XXXXXX";
    int fd = mkstemp(name);
    REQUIRE(fd >= 0);

    auto msg = generate_data(1024);
    int w = write(fd, msg.data(), msg.size());
    REQUIRE(w == msg.size());

    close(fd);
    auto d = condy::defer([&] { unlink(name); });

    auto func = [&]() -> condy::Coro<void> {
        struct statx stx {};
        int r =
            co_await condy::async_statx(AT_FDCWD, name, 0, STATX_SIZE, &stx);
        REQUIRE(r == 0);
        REQUIRE(stx.stx_size == msg.size());
    };
    condy::sync_wait(func());
}

TEST_CASE("test async_operations - test fadvise - basic") {
    char name[32] = "XXXXXX";
    int fd = mkstemp(name);
    REQUIRE(fd >= 0);

    auto msg = generate_data(1024);
    int w = write(fd, msg.data(), msg.size());
    REQUIRE(w == msg.size());

    auto func = [&]() -> condy::Coro<void> {
        int r = co_await condy::async_fadvise(fd, 0, 1024, POSIX_FADV_NOREUSE);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    close(fd);
    unlink(name);
}

TEST_CASE("test async_operations - test fadvise - fixed fd") {
    char name[32] = "XXXXXX";
    int fd = mkstemp(name);
    REQUIRE(fd >= 0);

    auto msg = generate_data(1024);
    int w = write(fd, msg.data(), msg.size());
    REQUIRE(w == msg.size());

    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_fd_table();
        fd_table.init(1);
        int r = co_await fd_table.async_update_files(&fd, 1, 0);
        REQUIRE(r == 1);

        r = co_await condy::async_fadvise(condy::fixed(0), 0, 1024,
                                          POSIX_FADV_NOREUSE);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    close(fd);
    unlink(name);
}

#if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
TEST_CASE("test async_operations - test fadvise64 - basic") {
    char name[32] = "XXXXXX";
    int fd = mkstemp(name);
    REQUIRE(fd >= 0);

    auto msg = generate_data(1024);
    int w = write(fd, msg.data(), msg.size());
    REQUIRE(w == msg.size());

    auto func = [&]() -> condy::Coro<void> {
        int r =
            co_await condy::async_fadvise64(fd, 0, 1024, POSIX_FADV_NOREUSE);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    close(fd);
    unlink(name);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
TEST_CASE("test async_operations - test fadvise64 - fixed fd") {
    char name[32] = "XXXXXX";
    int fd = mkstemp(name);
    REQUIRE(fd >= 0);

    auto msg = generate_data(1024);
    int w = write(fd, msg.data(), msg.size());
    REQUIRE(w == msg.size());

    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_fd_table();
        fd_table.init(1);
        int r = co_await fd_table.async_update_files(&fd, 1, 0);
        REQUIRE(r == 1);

        r = co_await condy::async_fadvise64(condy::fixed(0), 0, 1024,
                                            POSIX_FADV_NOREUSE);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    close(fd);
    unlink(name);
}
#endif

TEST_CASE("test async_operations - test madvise") {
    void *addr = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    REQUIRE(addr != MAP_FAILED);

    auto func = [&]() -> condy::Coro<void> {
        int r = co_await condy::async_madvise(addr, 4096, MADV_DONTNEED);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    munmap(addr, 4096);
}

#if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
TEST_CASE("test async_operations - test madvise64") {
    void *addr = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    REQUIRE(addr != MAP_FAILED);

    auto func = [&]() -> condy::Coro<void> {
        int r = co_await condy::async_madvise64(addr, 4096, MADV_DONTNEED);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    munmap(addr, 4096);
}
#endif

TEST_CASE("test async_operations - test send - basic") {
    int sv[2];
    create_tcp_socketpair(sv);

    auto msg = generate_data(1024);
    auto func = [&]() -> condy::Coro<void> {
        ssize_t n = co_await condy::async_send(sv[1], condy::buffer(msg), 0);
        REQUIRE(n == msg.size());
    };
    condy::sync_wait(func());

    char read_buf[2048];
    int r = recv(sv[0], read_buf, sizeof(read_buf), 0);
    REQUIRE(r == msg.size());
    REQUIRE(std::string_view(read_buf, r) == msg);

    close(sv[0]);
    close(sv[1]);
}

TEST_CASE("test async_operations - test send - fixed fd") {
    int sv[2];
    create_tcp_socketpair(sv);

    auto msg = generate_data(1024);
    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_fd_table();
        fd_table.init(2);
        int r = co_await fd_table.async_update_files(sv, 2, 0);
        REQUIRE(r == 2);

        ssize_t n =
            co_await condy::async_send(condy::fixed(1), condy::buffer(msg), 0);
        REQUIRE(n == msg.size());
    };
    condy::sync_wait(func());

    char read_buf[2048];
    int r = recv(sv[0], read_buf, sizeof(read_buf), 0);
    REQUIRE(r == msg.size());
    REQUIRE(std::string_view(read_buf, r) == msg);

    close(sv[0]);
    close(sv[1]);
}

#if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
TEST_CASE("test async_operations - test send - provided buffer") {
    int sv[2];
    create_tcp_socketpair(sv);

    auto msg = generate_data(1024);
    auto func = [&]() -> condy::Coro<void> {
        condy::ProvidedBufferQueue queue(4);
        for (int i = 0; i < 4; i++) {
            queue.push(condy::buffer(msg.data() + i * (1024 / 4), 1024 / 4));
        }

        for (int i = 0; i < 4; i++) {
            auto [n, binfo] = co_await condy::async_send(sv[1], queue, 0);
            REQUIRE(n == msg.size() / 4);
            REQUIRE(binfo.num_buffers == 1);
            REQUIRE(binfo.bid == i);
            REQUIRE(queue.size() == static_cast<size_t>(4 - i - 1));
        }
    };
    condy::sync_wait(func());

    char read_buf[2048];
    int r = recv(sv[0], read_buf, sizeof(read_buf), 0);
    REQUIRE(r == msg.size());
    REQUIRE(std::string_view(read_buf, r) == msg);

    close(sv[0]);
    close(sv[1]);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
TEST_CASE("test async_operations - test send - bundled provided buffer") {
    int sv[2];
    create_tcp_socketpair(sv);

    auto msg = generate_data(1024);
    auto func = [&]() -> condy::Coro<void> {
        condy::ProvidedBufferQueue queue(4);
        for (int i = 0; i < 4; i++) {
            queue.push(condy::buffer(msg.data() + i * (1024 / 4), 1024 / 4));
        }

        auto [n, binfo] =
            co_await condy::async_send(sv[1], condy::bundled(queue), 0);
        REQUIRE(n == msg.size());
        REQUIRE(binfo.num_buffers == 4);
        REQUIRE(binfo.bid == 0);
        REQUIRE(queue.size() == 0);
    };
    condy::sync_wait(func());

    char read_buf[2048];
    int r = recv(sv[0], read_buf, sizeof(read_buf), 0);
    REQUIRE(r == msg.size());
    REQUIRE(std::string_view(read_buf, r) == msg);

    close(sv[0]);
    close(sv[1]);
}
#endif

TEST_CASE("test async_operations - test send - zero copy") {
    int sv[2];
    create_tcp_socketpair(sv);

    auto msg = generate_data(1024);
    bool called = false;
    auto func = [&]() -> condy::Coro<void> {
        size_t n = co_await condy::async_send_zc(
            sv[1], condy::buffer(msg), 0, 0, [&](auto) { called = true; });
        REQUIRE(n == msg.size());
    };
    condy::sync_wait(func());
    REQUIRE(called);

    char read_buf[2048];
    int r = recv(sv[0], read_buf, sizeof(read_buf), 0);
    REQUIRE(r == msg.size());
    REQUIRE(std::string_view(read_buf, r) == msg);

    close(sv[0]);
    close(sv[1]);
}

TEST_CASE("test async_operations - test send - zero copy fixed buffer") {
    int sv[2];
    create_tcp_socketpair(sv);

    auto msg = generate_data(1024);
    bool called = false;
    auto func = [&]() -> condy::Coro<void> {
        auto &buffer_table = condy::current_buffer_table();
        buffer_table.init(1);
        iovec register_iov{
            .iov_base = const_cast<char *>(msg.data()),
            .iov_len = msg.size(),
        };
        buffer_table.update_buffers(0, &register_iov, 1);

        size_t n = co_await condy::async_send_zc(
            sv[1], condy::fixed(0, condy::buffer(msg)), 0, 0,
            [&](auto) { called = true; });
        REQUIRE(n == msg.size());
    };
    condy::sync_wait(func());
    REQUIRE(called);

    char read_buf[2048];
    int r = recv(sv[0], read_buf, sizeof(read_buf), 0);
    REQUIRE(r == msg.size());
    REQUIRE(std::string_view(read_buf, r) == msg);

    close(sv[0]);
    close(sv[1]);
}

TEST_CASE("test async_operations - test sendto - basic") {
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

    auto msg = generate_data(1024);
    auto func = [&]() -> condy::Coro<void> {
        ssize_t n = co_await condy::async_sendto(sender_fd, condy::buffer(msg),
                                                 0, (sockaddr *)&recv_addr,
                                                 sizeof(recv_addr));
        REQUIRE(n == msg.size());
    };
    condy::sync_wait(func());

    char read_buf[2048];
    r = recvfrom(receiver_fd, read_buf, sizeof(read_buf), 0, nullptr, nullptr);
    REQUIRE(r == msg.size());
    REQUIRE(std::string_view(read_buf, r) == msg);
    close(sender_fd);
    close(receiver_fd);
}

TEST_CASE("test async_operations - test sendto - fixed fd") {
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

    auto msg = generate_data(1024);
    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_fd_table();
        fd_table.init(2);
        r = co_await fd_table.async_update_files(&sender_fd, 1, 0);
        REQUIRE(r == 1);

        ssize_t n = co_await condy::async_sendto(
            condy::fixed(0), condy::buffer(msg), 0, (sockaddr *)&recv_addr,
            sizeof(recv_addr));
        REQUIRE(n == msg.size());
    };
    condy::sync_wait(func());

    char read_buf[2048];
    r = recvfrom(receiver_fd, read_buf, sizeof(read_buf), 0, nullptr, nullptr);
    REQUIRE(r == msg.size());
    REQUIRE(std::string_view(read_buf, r) == msg);
    close(sender_fd);
    close(receiver_fd);
}

#if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
TEST_CASE("test async_operations - test sendto - provided buffer") {
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

    auto msg = generate_data(1024);
    auto func = [&]() -> condy::Coro<void> {
        condy::ProvidedBufferQueue queue(4);
        for (int i = 0; i < 4; i++) {
            queue.push(condy::buffer(msg.data() + i * (1024 / 4), 1024 / 4));
        }

        for (int i = 0; i < 4; i++) {
            auto [n, binfo] = co_await condy::async_sendto(
                sender_fd, queue, 0, (sockaddr *)&recv_addr, sizeof(recv_addr));
            REQUIRE(n == msg.size() / 4);
            REQUIRE(binfo.num_buffers == 1);
            REQUIRE(binfo.bid == i);
            REQUIRE(queue.size() == static_cast<size_t>(4 - i - 1));
        }
    };
    condy::sync_wait(func());

    char read_buf[2048];
    std::string actual;
    for (int i = 0; i < 4; i++) {
        int r = recvfrom(receiver_fd, read_buf, sizeof(read_buf), 0, nullptr,
                         nullptr);
        REQUIRE(r == msg.size() / 4);
        actual.append(static_cast<char *>(read_buf), r);
    }
    REQUIRE(actual == msg);
    close(sender_fd);
    close(receiver_fd);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
TEST_CASE("test async_operations - test sendto - bundled provided buffer") {
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

    auto msg = generate_data(1024);
    auto func = [&]() -> condy::Coro<void> {
        condy::ProvidedBufferQueue queue(4);
        for (int i = 0; i < 4; i++) {
            queue.push(condy::buffer(msg.data() + i * (1024 / 4), 1024 / 4));
        }

        auto [n, binfo] = co_await condy::async_sendto(
            sender_fd, condy::bundled(queue), 0, (sockaddr *)&recv_addr,
            sizeof(recv_addr));
        REQUIRE(n == msg.size());
        REQUIRE(binfo.num_buffers == 4);
        REQUIRE(binfo.bid == 0);
        REQUIRE(queue.size() == 0);
    };
    condy::sync_wait(func());

    // NOTE: io_uring will merge the packets into one if possible in bundled
    // mode, so we only need to recv once here.
    char read_buf[2048];
    r = recvfrom(receiver_fd, read_buf, sizeof(read_buf), 0, nullptr, nullptr);
    REQUIRE(r == msg.size());
    REQUIRE(std::string_view(read_buf, r) == msg);

    close(sender_fd);
    close(receiver_fd);
}
#endif

TEST_CASE("test async_operations - test sendto - zero copy") {
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

    auto msg = generate_data(1024);
    bool called = false;
    auto func = [&]() -> condy::Coro<void> {
        size_t n = co_await condy::async_sendto_zc(
            sender_fd, condy::buffer(msg), 0, (sockaddr *)&recv_addr,
            sizeof(recv_addr), 0, [&](auto) { called = true; });
        REQUIRE(n == msg.size());
    };
    condy::sync_wait(func());
    REQUIRE(called);

    char read_buf[2048];
    r = recvfrom(receiver_fd, read_buf, sizeof(read_buf), 0, nullptr, nullptr);
    REQUIRE(r == msg.size());
    REQUIRE(std::string_view(read_buf, r) == msg);
    close(sender_fd);
    close(receiver_fd);
}

TEST_CASE("test async_operations - test sendto - zero copy fixed buffer") {
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

    auto msg = generate_data(1024);
    bool called = false;
    auto func = [&]() -> condy::Coro<void> {
        auto &buffer_table = condy::current_buffer_table();
        buffer_table.init(1);
        iovec register_iov{
            .iov_base = const_cast<char *>(msg.data()),
            .iov_len = msg.size(),
        };
        buffer_table.update_buffers(0, &register_iov, 1);

        size_t n = co_await condy::async_sendto_zc(
            sender_fd, condy::fixed(0, condy::buffer(msg)), 0,
            (sockaddr *)&recv_addr, sizeof(recv_addr), 0,
            [&](auto) { called = true; });
        REQUIRE(n == msg.size());
    };
    condy::sync_wait(func());
    REQUIRE(called);

    char read_buf[2048];
    r = recvfrom(receiver_fd, read_buf, sizeof(read_buf), 0, nullptr, nullptr);
    REQUIRE(r == msg.size());
    REQUIRE(std::string_view(read_buf, r) == msg);
    close(sender_fd);
    close(receiver_fd);
}

TEST_CASE("test async_operations - test recv - basic") {
    int sv[2];
    create_tcp_socketpair(sv);

    auto msg = generate_data(1024);
    int r = send(sv[1], msg.data(), msg.size(), 0);
    REQUIRE(r == msg.size());

    auto func = [&]() -> condy::Coro<void> {
        char buf[2048];
        ssize_t n = co_await condy::async_recv(sv[0], condy::buffer(buf), 0);
        REQUIRE(n == msg.size());
        REQUIRE(std::string_view(buf, n) == msg);
    };
    condy::sync_wait(func());

    close(sv[0]);
    close(sv[1]);
}

TEST_CASE("test async_operations - test recv - fixed fd") {
    int sv[2];
    create_tcp_socketpair(sv);

    auto msg = generate_data(1024);
    int r = send(sv[1], msg.data(), msg.size(), 0);
    REQUIRE(r == msg.size());

    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_fd_table();
        fd_table.init(2);
        r = co_await fd_table.async_update_files(sv, 2, 0);
        REQUIRE(r == 2);

        char buf[2048];
        ssize_t n =
            co_await condy::async_recv(condy::fixed(0), condy::buffer(buf), 0);
        REQUIRE(n == msg.size());
        REQUIRE(std::string_view(buf, n) == msg);
    };
    condy::sync_wait(func());

    close(sv[0]);
    close(sv[1]);
}

TEST_CASE("test async_operations - test recv - provided buffer") {
    int sv[2];
    create_tcp_socketpair(sv);

    auto msg = generate_data(1024);
    int r = send(sv[1], msg.data(), msg.size(), 0);
    REQUIRE(r == msg.size());

    auto func = [&]() -> condy::Coro<void> {
        std::string actual;

        char buf[2][256];
        condy::ProvidedBufferQueue queue(2);
        uint16_t bid = queue.push(condy::buffer(buf[0]));
        REQUIRE(bid == 0);
        bid = queue.push(condy::buffer(buf[1]));
        REQUIRE(bid == 1);

        auto [r, binfo] = co_await condy::async_recv(sv[0], queue, 0);
        REQUIRE(r == 256);
        REQUIRE(binfo.num_buffers == 1);
        REQUIRE(binfo.bid == 0);
        REQUIRE(queue.size() == 1);
        actual.append(static_cast<char *>(buf[binfo.bid]), r);

        auto [r2, binfo2] = co_await condy::async_recv(sv[0], queue, 0);
        REQUIRE(r2 == 256);
        REQUIRE(binfo2.num_buffers == 1);
        REQUIRE(binfo2.bid == 1);
        REQUIRE(queue.size() == 0);
        actual.append(static_cast<char *>(buf[binfo2.bid]), r2);

        auto [rx, binfox] = co_await condy::async_recv(sv[0], queue, 0);
        REQUIRE(rx == -ENOBUFS);

        condy::ProvidedBufferPool buf_pool(2, 256);
        auto [r3, buf3] = co_await condy::async_recv(sv[0], buf_pool, 0);
        REQUIRE(r3 == 256);
        actual.append(static_cast<char *>(buf3.data()), r3);

        auto [r4, buf4] = co_await condy::async_recv(sv[0], buf_pool, 0);
        REQUIRE(r4 == 256);
        actual.append(static_cast<char *>(buf4.data()), r4);

        REQUIRE(actual == msg);
    };
    condy::sync_wait(func());

    close(sv[0]);
    close(sv[1]);
}

#if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
TEST_CASE("test async_operations - test recv - bundled provided buffer") {
    int sv[2];
    create_tcp_socketpair(sv);

    auto msg = generate_data(1024);
    int r = send(sv[1], msg.data(), msg.size(), 0);
    REQUIRE(r == msg.size());

    auto func = [&]() -> condy::Coro<void> {
        condy::ProvidedBufferPool pool(4, 256);

        auto [n, bufs] =
            co_await condy::async_recv(sv[0], condy::bundled(pool), 0);
        REQUIRE(n == msg.size());
        REQUIRE(bufs.size() == 4);
        std::string actual;
        for (size_t i = 0; i < bufs.size(); i++) {
            actual.append(static_cast<char *>(bufs[i].data()), bufs[i].size());
        }
        REQUIRE(actual == msg);
    };
    condy::sync_wait(func());

    close(sv[0]);
    close(sv[1]);
}
#endif

TEST_CASE("test async_operations - test recv - multishot") {
    int sv[2];
    create_tcp_socketpair(sv);

    auto msg = generate_data(1024);
    int r = send(sv[1], msg.data(), msg.size(), 0);
    REQUIRE(r == msg.size());
    close(sv[1]);

    auto func = [&]() -> condy::Coro<void> {
        size_t count = 0;
        std::string actual;

        condy::ProvidedBufferPool pool(2, 256);

        condy::Channel<condy::ProvidedBuffer> channel(2);

        auto [n, buf] =
            co_await condy::async_recv_multishot(sv[0], pool, 0, [&](auto res) {
                auto &[n, buf] = res;
                REQUIRE(n == 256);
                actual.append(static_cast<char *>(buf.data()), n);
                count++;
                REQUIRE(channel.try_push(std::move(buf)));
            });
        REQUIRE(n == -ENOBUFS);
        REQUIRE(count == 2);

        auto tmp = co_await channel.pop();
        tmp.reset(); // Release the buffer back to the pool
        tmp = co_await channel.pop();
        tmp.reset(); // Release the buffer back to the pool

        auto [n2, buf2] =
            co_await condy::async_recv_multishot(sv[0], pool, 0, [&](auto res) {
                auto &[n, buf] = res;
                REQUIRE(n == 256);
                actual.append(static_cast<char *>(buf.data()), n);
                count++;
            });
        REQUIRE(n2 == -ENOBUFS);
        REQUIRE(count == 4);

        REQUIRE(actual == msg);
    };
    condy::sync_wait(func());

    close(sv[0]);
}

#if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
TEST_CASE("test async_operations - test recv - bundled multishot") {
    int sv[2];
    create_tcp_socketpair(sv);

    auto msg = generate_data(1024);

    auto func = [&]() -> condy::Coro<void> {
        int r;
        size_t count = 0;
        std::string actual;

        condy::ProvidedBufferPool pool(2, 256);

        r = send(sv[1], msg.data(), msg.size() / 4, 0);
        REQUIRE(r == msg.size() / 4);

        auto [n, bufs] = co_await condy::async_recv_multishot(
            sv[0], condy::bundled(pool), 0, [&](auto res) {
                auto &[n, bufs] = res;
                REQUIRE(n == 256);
                REQUIRE(bufs.size() == 1);
                actual.append(static_cast<char *>(bufs[0].data()), n);
                count++;
                r = send(sv[1], msg.data() + count * 256, 256, 0);
                REQUIRE(r == 256);
            });
        // This behavior seems strange...
        REQUIRE(n == 256);
        REQUIRE(bufs.size() == 1);
        actual.append(static_cast<char *>(bufs[0].data()), n);
        count++;
        REQUIRE(count == 2);
        bufs[0].reset(); // Release the buffer back to the pool

        r = send(sv[1], msg.data() + count * 256, msg.size() - count * 256, 0);
        REQUIRE(r == msg.size() - count * 256);
        close(sv[1]);

        auto [n2, bufs2] = co_await condy::async_recv_multishot(
            sv[0], condy::bundled(pool), 0, [&](auto) {
                assert(false); // Should not be called
            });
        REQUIRE(n2 == 512);
        REQUIRE(bufs2.size() == 2);
        for (size_t i = 0; i < bufs2.size(); i++) {
            actual.append(static_cast<char *>(bufs2[i].data()), 256);
        }
        count += bufs2.size();
        REQUIRE(count == 4);

        REQUIRE(actual == msg);
    };
    condy::sync_wait(func());

    close(sv[0]);
    close(sv[1]);
}
#endif

TEST_CASE("test async_operations - test openat2 - basic") {
    char name[32] = "XXXXXX";
    int fd = mkstemp(name);
    REQUIRE(fd >= 0);
    close(fd);
    auto d = condy::defer([&] { unlink(name); });

    auto func = [&]() -> condy::Coro<void> {
        struct open_how how {};
        how.flags = O_RDONLY;
        how.mode = 0;

        int r = co_await condy::async_openat2(AT_FDCWD, name, &how);
        REQUIRE(r >= 0);
        close(r);
    };
    condy::sync_wait(func());
}

TEST_CASE("test async_operations - test openat2 - direct") {
    char name[32] = "XXXXXX";
    int fd = mkstemp(name);
    REQUIRE(fd >= 0);
    close(fd);
    auto d = condy::defer([&] { unlink(name); });

    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_fd_table();
        fd_table.init(1);

        struct open_how how {};
        how.flags = O_RDONLY;
        how.mode = 0;

        int r = co_await condy::async_openat2_direct(AT_FDCWD, name, &how,
                                                     CONDY_FILE_INDEX_ALLOC);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());
}

TEST_CASE("test async_operations - test shutdown - basic") {
    int sv[2];
    create_tcp_socketpair(sv);

    auto func = [&]() -> condy::Coro<void> {
        int r = co_await condy::async_shutdown(sv[1], SHUT_RDWR);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    int r = recv(sv[0], nullptr, 0, 0);
    REQUIRE(r == 0); // EOF

    close(sv[0]);
    close(sv[1]);
}

TEST_CASE("test async_operations - test shutdown - fixed fd") {
    int sv[2];
    create_tcp_socketpair(sv);

    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_fd_table();
        fd_table.init(2);
        int r = co_await fd_table.async_update_files(sv, 2, 0);
        REQUIRE(r == 2);

        r = co_await condy::async_shutdown(condy::fixed(1), SHUT_RDWR);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    int r = recv(sv[0], nullptr, 0, 0);
    REQUIRE(r == 0); // EOF

    close(sv[0]);
    close(sv[1]);
}

TEST_CASE("test async_operations - test unlinkat") {
    char name[32] = "XXXXXX";
    int fd = mkstemp(name);
    REQUIRE(fd >= 0);
    close(fd);

    auto func = [&]() -> condy::Coro<void> {
        int r = co_await condy::async_unlinkat(AT_FDCWD, name, 0);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    struct stat st {};
    int r = stat(name, &st);
    REQUIRE(r == -1);
    REQUIRE(errno == ENOENT);
}

TEST_CASE("test async_operations - test unlink") {
    char name[32] = "XXXXXX";
    int fd = mkstemp(name);
    REQUIRE(fd >= 0);
    close(fd);

    auto func = [&]() -> condy::Coro<void> {
        int r = co_await condy::async_unlink(name, 0);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    struct stat st {};
    int r = stat(name, &st);
    REQUIRE(r == -1);
    REQUIRE(errno == ENOENT);
}

TEST_CASE("test async_operations - test renameat") {
    char old_name[] = "XXXXXX";
    int old_fd = mkstemp(old_name);
    REQUIRE(old_fd >= 0);
    close(old_fd);

    char new_name[32] = {};
    snprintf(new_name, sizeof(new_name), "%s_renamed", old_name);
    auto d = condy::defer([&] {
        unlink(old_name); // Ensure cleanup
        unlink(new_name);
    });

    auto func = [&]() -> condy::Coro<void> {
        int r = co_await condy::async_renameat(AT_FDCWD, old_name, AT_FDCWD,
                                               new_name, 0);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    struct stat st {};
    int r = stat(old_name, &st);
    REQUIRE(r == -1);
    REQUIRE(errno == ENOENT);

    r = stat(new_name, &st);
    REQUIRE(r == 0);
}

TEST_CASE("test async_operations - test rename") {
    char old_name[] = "XXXXXX";
    int old_fd = mkstemp(old_name);
    REQUIRE(old_fd >= 0);
    close(old_fd);

    char new_name[32] = {};
    snprintf(new_name, sizeof(new_name), "%s_renamed", old_name);
    auto d = condy::defer([&] {
        unlink(old_name); // Ensure cleanup
        unlink(new_name);
    });

    auto func = [&]() -> condy::Coro<void> {
        int r = co_await condy::async_rename(old_name, new_name);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    struct stat st {};
    int r = stat(old_name, &st);
    REQUIRE(r == -1);
    REQUIRE(errno == ENOENT);

    r = stat(new_name, &st);
    REQUIRE(r == 0);
}

TEST_CASE("test async_operations - test sync_file_range") {
    char name[32] = "XXXXXX";
    int fd = mkstemp(name);
    REQUIRE(fd >= 0);

    auto msg = generate_data(4096);
    int w = write(fd, msg.data(), msg.size());
    REQUIRE(w == msg.size());

    auto func = [&]() -> condy::Coro<void> {
        int r = co_await condy::async_sync_file_range(
            fd, 0, 4096, SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    close(fd);
    unlink(name);
}

TEST_CASE("test async_operations - test mkdirat") {
    char name[] = "temp_dir";
    auto d = condy::defer([&] { rmdir(name); });

    auto func = [&]() -> condy::Coro<void> {
        int r = co_await condy::async_mkdirat(AT_FDCWD, name, 0755);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    struct stat st {};
    int r = stat(name, &st);
    REQUIRE(r == 0);
    REQUIRE(S_ISDIR(st.st_mode));
}

TEST_CASE("test async_operations - test mkdir") {
    char name[] = "temp_dir";
    auto d = condy::defer([&] { rmdir(name); });

    auto func = [&]() -> condy::Coro<void> {
        int r = co_await condy::async_mkdir(name, 0755);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    struct stat st {};
    int r = stat(name, &st);
    REQUIRE(r == 0);
    REQUIRE(S_ISDIR(st.st_mode));
}

TEST_CASE("test async_operations - test symlinkat") {
    char target_name[] = "XXXXXX";
    int target_fd = mkstemp(target_name);
    REQUIRE(target_fd >= 0);
    close(target_fd);
    auto d1 = condy::defer([&] { unlink(target_name); });

    char link_name[32] = {};
    snprintf(link_name, sizeof(link_name), "%s_link", target_name);
    auto d2 = condy::defer([&] { unlink(link_name); });

    auto func = [&]() -> condy::Coro<void> {
        int r =
            co_await condy::async_symlinkat(target_name, AT_FDCWD, link_name);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    char buf[256];
    ssize_t r = readlink(link_name, buf, sizeof(buf) - 1);
    REQUIRE(r >= 0);
    buf[r] = '\0';
    REQUIRE(std::string_view(buf) == target_name);
}

TEST_CASE("test async_operations - test symlink") {
    char target_name[] = "XXXXXX";
    int target_fd = mkstemp(target_name);
    REQUIRE(target_fd >= 0);
    close(target_fd);
    auto d1 = condy::defer([&] { unlink(target_name); });

    char link_name[32] = {};
    snprintf(link_name, sizeof(link_name), "%s_link", target_name);
    auto d2 = condy::defer([&] { unlink(link_name); });

    auto func = [&]() -> condy::Coro<void> {
        int r = co_await condy::async_symlink(target_name, link_name);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    char buf[256];
    ssize_t r = readlink(link_name, buf, sizeof(buf) - 1);
    REQUIRE(r >= 0);
    buf[r] = '\0';
    REQUIRE(std::string_view(buf) == target_name);
}

TEST_CASE("test async_operations - test linkat") {
    char target_name[] = "XXXXXX";
    int target_fd = mkstemp(target_name);
    REQUIRE(target_fd >= 0);
    close(target_fd);
    auto d1 = condy::defer([&] { unlink(target_name); });

    char link_name[32] = {};
    snprintf(link_name, sizeof(link_name), "%s_link", target_name);
    auto d2 = condy::defer([&] { unlink(link_name); });

    auto func = [&]() -> condy::Coro<void> {
        int r = co_await condy::async_linkat(AT_FDCWD, target_name, AT_FDCWD,
                                             link_name, 0);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    struct stat st1 {
    }, st2{};
    int r = stat(target_name, &st1);
    REQUIRE(r == 0);
    r = stat(link_name, &st2);
    REQUIRE(r == 0);
    REQUIRE(st1.st_ino == st2.st_ino);
}

TEST_CASE("test async_operations - test link") {
    char target_name[] = "XXXXXX";
    int target_fd = mkstemp(target_name);
    REQUIRE(target_fd >= 0);
    close(target_fd);
    auto d1 = condy::defer([&] { unlink(target_name); });

    char link_name[32] = {};
    snprintf(link_name, sizeof(link_name), "%s_link", target_name);
    auto d2 = condy::defer([&] { unlink(link_name); });

    auto func = [&]() -> condy::Coro<void> {
        int r = co_await condy::async_link(target_name, link_name, 0);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    struct stat st1 {
    }, st2{};
    int r = stat(target_name, &st1);
    REQUIRE(r == 0);
    r = stat(link_name, &st2);
    REQUIRE(r == 0);
    REQUIRE(st1.st_ino == st2.st_ino);
}

TEST_CASE("test async_operations - test getxattr") {
    char name[32] = "XXXXXX";
    int fd = mkstemp(name);
    REQUIRE(fd >= 0);
    close(fd);
    auto d = condy::defer([&] { unlink(name); });

    const char *attr_name = "user.test_attr";
    const char *attr_value = "test_value";
    int w = setxattr(name, attr_name, attr_value, strlen(attr_value), 0);
    REQUIRE(w == 0);

    auto func = [&]() -> condy::Coro<void> {
        char buf[256];
        ssize_t r =
            co_await condy::async_getxattr(attr_name, buf, name, sizeof(buf));
        REQUIRE(r == static_cast<ssize_t>(strlen(attr_value)));
        REQUIRE(std::string_view(buf, r) == attr_value);
    };
    condy::sync_wait(func());
}

TEST_CASE("test async_operations - test setxattr") {
    char name[32] = "XXXXXX";
    int fd = mkstemp(name);
    REQUIRE(fd >= 0);
    close(fd);
    auto d = condy::defer([&] { unlink(name); });

    const char *attr_name = "user.test_attr";
    const char *attr_value = "test_value";

    auto func = [&]() -> condy::Coro<void> {
        int r = co_await condy::async_setxattr(attr_name, attr_value, name, 0,
                                               strlen(attr_value));
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    char buf[256];
    ssize_t r = getxattr(name, attr_name, buf, sizeof(buf));
    REQUIRE(r == static_cast<ssize_t>(strlen(attr_value)));
    REQUIRE(std::string_view(buf, r) == attr_value);
}

TEST_CASE("test async_operations - test fgetxattr") {
    char name[32] = "XXXXXX";
    int fd = mkstemp(name);
    REQUIRE(fd >= 0);
    auto d = condy::defer([&] {
        close(fd);
        unlink(name);
    });

    const char *attr_name = "user.test_attr";
    const char *attr_value = "test_value";
    int w = fsetxattr(fd, attr_name, attr_value, strlen(attr_value), 0);
    REQUIRE(w == 0);

    auto func = [&]() -> condy::Coro<void> {
        char buf[256];
        ssize_t r =
            co_await condy::async_fgetxattr(fd, attr_name, buf, sizeof(buf));
        REQUIRE(r == static_cast<ssize_t>(strlen(attr_value)));
        REQUIRE(std::string_view(buf, r) == attr_value);
    };
    condy::sync_wait(func());
}

TEST_CASE("test async_operations - test fsetxattr") {
    char name[32] = "XXXXXX";
    int fd = mkstemp(name);
    REQUIRE(fd >= 0);
    auto d = condy::defer([&] {
        close(fd);
        unlink(name);
    });

    const char *attr_name = "user.test_attr";
    const char *attr_value = "test_value";

    auto func = [&]() -> condy::Coro<void> {
        int r = co_await condy::async_fsetxattr(fd, attr_name, attr_value, 0,
                                                strlen(attr_value));
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    char buf[256];
    ssize_t r = fgetxattr(fd, attr_name, buf, sizeof(buf));
    REQUIRE(r == static_cast<ssize_t>(strlen(attr_value)));
    REQUIRE(std::string_view(buf, r) == attr_value);
}

TEST_CASE("test async_operations - test socket - basic") {
    auto func = [&]() -> condy::Coro<void> {
        int fd = co_await condy::async_socket(AF_INET, SOCK_STREAM, 0, 0);
        REQUIRE(fd >= 0);
        close(fd);
    };
    condy::sync_wait(func());
}

TEST_CASE("test async_operations - test socket - direct") {
    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_fd_table();
        fd_table.init(1);

        int r = co_await condy::async_socket_direct(AF_INET, SOCK_STREAM, 0,
                                                    CONDY_FILE_INDEX_ALLOC, 0);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());
}

#if !IO_URING_CHECK_VERSION(2, 5) // >= 2.5
TEST_CASE("test async_operations - test cmd_sock - basic") {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listen_fd >= 0);

    auto func = [&]() -> condy::Coro<void> {
        int val;
        int r = co_await condy::async_cmd_sock(SOCKET_URING_OP_SETSOCKOPT,
                                               listen_fd, SOL_SOCKET,
                                               SO_REUSEADDR, &val, sizeof(val));
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    close(listen_fd);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 5) // >= 2.5
TEST_CASE("test async_operations - test cmd_sock - fixed fd") {
    int listen_fd = create_accept_socket();

    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_fd_table();
        fd_table.init(1);
        int r = co_await fd_table.async_update_files(&listen_fd, 1, 0);
        REQUIRE(r == 1);

        int val;
        r = co_await condy::async_cmd_sock(SOCKET_URING_OP_SETSOCKOPT,
                                           condy::fixed(0), SOL_SOCKET,
                                           SO_REUSEADDR, &val, sizeof(val));
        REQUIRE(r == 0);
    };

    condy::sync_wait(func());
}
#endif

#if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6
TEST_CASE("test async_operations - test waitid") {
    pid_t pid = fork();
    REQUIRE(pid >= 0);

    if (pid == 0) {
        // Child process
        _exit(42);
    }

    auto func = [&]() -> condy::Coro<void> {
        siginfo_t info{};
        int r = co_await condy::async_waitid(P_PID, pid, &info, WEXITED, 0);
        REQUIRE(r == 0);
        REQUIRE(info.si_pid == pid);
        REQUIRE(info.si_code == CLD_EXITED);
        REQUIRE(info.si_status == 42);
    };
    condy::sync_wait(func());
}
#endif

#if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6
TEST_CASE("test async_operations - test futex - wait/wake") {
    uint32_t futex_var = 0;

    auto waker = [&]() -> condy::Coro<void> {
        futex_var = 1;
        int r = co_await condy::async_futex_wake(
            &futex_var, 1, FUTEX_BITSET_MATCH_ANY, FUTEX2_SIZE_U32, 0);
        REQUIRE(r >= 0);
        co_return;
    };

    auto func = [&]() -> condy::Coro<void> {
        auto t = condy::co_spawn(waker());
        int r = co_await condy::async_futex_wait(
            &futex_var, 1, FUTEX_BITSET_MATCH_ANY, FUTEX2_SIZE_U32, 0);
        REQUIRE(r == 0);

        co_await std::move(t);
    };
    condy::sync_wait(func());
}
#endif

#if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6
TEST_CASE("test async_operations - test fixed_fd_install") {
    int sv[2];
    create_tcp_socketpair(sv);

    auto msg = generate_data(512);
    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_fd_table();
        fd_table.init(2);

        int r = co_await fd_table.async_update_files(sv, 2, 0);
        REQUIRE(r == 2);

        int write_fd = co_await fd_table.async_fixed_fd_install(1, 0);
        REQUIRE(write_fd >= 0);
        REQUIRE(write_fd != sv[1]);

        ssize_t n = co_await condy::async_send(write_fd, condy::buffer(msg), 0);
        REQUIRE(n == msg.size());
    };
    condy::sync_wait(func());

    char read_buf[1024];
    ssize_t r = recv(sv[0], read_buf, sizeof(read_buf), 0);
    REQUIRE(r == static_cast<ssize_t>(msg.size()));
    REQUIRE(std::string_view(read_buf, r) == msg);

    close(sv[0]);
    close(sv[1]);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6
TEST_CASE("test async_operations - test ftruncate - basic") {
    char name[32] = "XXXXXX";
    int fd = mkstemp(name);
    REQUIRE(fd >= 0);
    auto d = condy::defer([&] { unlink(name); });

    auto func = [&]() -> condy::Coro<void> {
        int r = co_await condy::async_ftruncate(fd, 4096);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    struct stat st {};
    int r = stat(name, &st);
    REQUIRE(r == 0);
    REQUIRE(st.st_size == 4096);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6
TEST_CASE("test async_operations - test ftruncate - fixed fd") {
    char name[32] = "XXXXXX";
    int fd = mkstemp(name);
    REQUIRE(fd >= 0);
    auto d = condy::defer([&] { unlink(name); });

    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_fd_table();
        fd_table.init(1);
        int r = co_await fd_table.async_update_files(&fd, 1, 0);
        REQUIRE(r == 1);

        r = co_await condy::async_ftruncate(condy::fixed(0), 4096);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    struct stat st {};
    int r = stat(name, &st);
    REQUIRE(r == 0);
    REQUIRE(st.st_size == 4096);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
TEST_CASE("test async_operations - test bind - basic") {
    int sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(sock_fd >= 0);

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind_addr.sin_port = 0; // Let OS choose the port

    auto func = [&]() -> condy::Coro<void> {
        int r = co_await condy::async_bind(sock_fd, (sockaddr *)&bind_addr,
                                           sizeof(bind_addr));
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    close(sock_fd);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
TEST_CASE("test async_operations - test bind - fixed fd") {
    int sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(sock_fd >= 0);

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind_addr.sin_port = 0; // Let OS choose the port

    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_fd_table();
        fd_table.init(1);
        int r = co_await fd_table.async_update_files(&sock_fd, 1, 0);
        REQUIRE(r == 1);

        r = co_await condy::async_bind(condy::fixed(0), (sockaddr *)&bind_addr,
                                       sizeof(bind_addr));
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    close(sock_fd);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
TEST_CASE("test async_operations - test listen - basic") {
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(sock_fd >= 0);

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind_addr.sin_port = 0; // Let OS choose the port
    int r = bind(sock_fd, (sockaddr *)&bind_addr, sizeof(bind_addr));
    REQUIRE(r == 0);

    auto func = [&]() -> condy::Coro<void> {
        int r = co_await condy::async_listen(sock_fd, 10);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    close(sock_fd);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
TEST_CASE("test async_operations - test listen - fixed fd") {
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(sock_fd >= 0);

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind_addr.sin_port = 0; // Let OS choose the port
    int r = bind(sock_fd, (sockaddr *)&bind_addr, sizeof(bind_addr));
    REQUIRE(r == 0);

    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_fd_table();
        fd_table.init(1);
        int r = co_await fd_table.async_update_files(&sock_fd, 1, 0);
        REQUIRE(r == 1);

        r = co_await condy::async_listen(condy::fixed(0), 10);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    close(sock_fd);
}
#endif

// TODO: Unit tests for cmd_discard

#if !IO_URING_CHECK_VERSION(2, 12) // >= 2.12
TEST_CASE("test async_operations - test pipe - basic") {
    int pipe_fds[2];

    auto func = [&]() -> condy::Coro<void> {
        int r = co_await condy::async_pipe(pipe_fds, 0);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    auto msg = generate_data(128);
    ssize_t w = write(pipe_fds[1], msg.data(), msg.size());
    REQUIRE(w == static_cast<ssize_t>(msg.size()));

    char buf[128];
    ssize_t r = read(pipe_fds[0], buf, sizeof(buf));
    REQUIRE(r == w);
    REQUIRE(std::string_view(buf, r) == msg);

    close(pipe_fds[0]);
    close(pipe_fds[1]);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 12) // >= 2.12
TEST_CASE("test async_operations - test pipe - direct") {
    int pipe_fds[2];

    auto msg = generate_data(128);
    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_fd_table();
        fd_table.init(2);

        int r = co_await condy::async_pipe_direct(pipe_fds, 0, 0);
        REQUIRE(r == 0);

        r = co_await condy::async_write(condy::fixed(1), condy::buffer(msg), 0);
        REQUIRE(r == static_cast<ssize_t>(msg.size()));

        char buf[128];
        r = co_await condy::async_read(condy::fixed(0), condy::buffer(buf), 0);
        REQUIRE(r == static_cast<ssize_t>(msg.size()));
        REQUIRE(std::string_view(buf, r) == msg);
    };
    condy::sync_wait(func());
}
#endif