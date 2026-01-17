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

TEST_CASE("test async_operations - test fadvise - basic") {
    char name[32] = "XXXXXX";
    int fd = mkstemp(name);
    REQUIRE(fd >= 0);

    auto msg = generate_data(1024);
    ssize_t w = write(fd, msg.data(), msg.size());
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
    ssize_t w = write(fd, msg.data(), msg.size());
    REQUIRE(w == msg.size());

    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_runtime().fd_table();
        fd_table.init(1);
        int r = co_await condy::async_files_update(&fd, 1, 0);
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
    ssize_t w = write(fd, msg.data(), msg.size());
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
    ssize_t w = write(fd, msg.data(), msg.size());
    REQUIRE(w == msg.size());

    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_runtime().fd_table();
        fd_table.init(1);
        int r = co_await condy::async_files_update(&fd, 1, 0);
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
    ssize_t r = recv(sv[0], read_buf, sizeof(read_buf), 0);
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
        auto &fd_table = condy::current_runtime().fd_table();
        fd_table.init(2);
        int r = co_await condy::async_files_update(sv, 2, 0);
        REQUIRE(r == 2);

        ssize_t n =
            co_await condy::async_send(condy::fixed(1), condy::buffer(msg), 0);
        REQUIRE(n == msg.size());
    };
    condy::sync_wait(func());

    char read_buf[2048];
    ssize_t r = recv(sv[0], read_buf, sizeof(read_buf), 0);
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
            queue.push(condy::buffer(
                msg.data() + static_cast<ptrdiff_t>(i * (1024 / 4)), 1024 / 4));
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
    ssize_t r = recv(sv[0], read_buf, sizeof(read_buf), 0);
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
            queue.push(condy::buffer(
                msg.data() + static_cast<ptrdiff_t>(i * (1024 / 4)), 1024 / 4));
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
    ssize_t r = recv(sv[0], read_buf, sizeof(read_buf), 0);
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
    ssize_t r = recv(sv[0], read_buf, sizeof(read_buf), 0);
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
        auto &buffer_table = condy::current_runtime().buffer_table();
        buffer_table.init(1);
        iovec register_iov{
            .iov_base = const_cast<char *>(msg.data()),
            .iov_len = msg.size(),
        };
        buffer_table.update(0, &register_iov, 1);

        size_t n = co_await condy::async_send_zc(
            sv[1], condy::fixed(0, condy::buffer(msg)), 0, 0,
            [&](auto) { called = true; });
        REQUIRE(n == msg.size());
    };
    condy::sync_wait(func());
    REQUIRE(called);

    char read_buf[2048];
    ssize_t r = recv(sv[0], read_buf, sizeof(read_buf), 0);
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

    ssize_t r = bind(receiver_fd, (sockaddr *)&recv_addr, sizeof(recv_addr));
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

    ssize_t r = bind(receiver_fd, (sockaddr *)&recv_addr, sizeof(recv_addr));
    REQUIRE(r == 0);

    socklen_t addrlen = sizeof(recv_addr);
    r = getsockname(receiver_fd, (sockaddr *)&recv_addr, &addrlen);
    REQUIRE(r == 0);

    auto msg = generate_data(1024);
    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_runtime().fd_table();
        fd_table.init(2);
        r = co_await condy::async_files_update(&sender_fd, 1, 0);
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
            queue.push(condy::buffer(
                msg.data() + static_cast<ptrdiff_t>(i * (1024 / 4)), 1024 / 4));
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
        ssize_t r = recvfrom(receiver_fd, read_buf, sizeof(read_buf), 0,
                             nullptr, nullptr);
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

    ssize_t r = bind(receiver_fd, (sockaddr *)&recv_addr, sizeof(recv_addr));
    REQUIRE(r == 0);

    socklen_t addrlen = sizeof(recv_addr);
    r = getsockname(receiver_fd, (sockaddr *)&recv_addr, &addrlen);
    REQUIRE(r == 0);

    auto msg = generate_data(1024);
    auto func = [&]() -> condy::Coro<void> {
        condy::ProvidedBufferQueue queue(4);
        for (int i = 0; i < 4; i++) {
            queue.push(condy::buffer(
                msg.data() + static_cast<ptrdiff_t>(i * (1024 / 4)), 1024 / 4));
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

    ssize_t r = bind(receiver_fd, (sockaddr *)&recv_addr, sizeof(recv_addr));
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

    ssize_t r = bind(receiver_fd, (sockaddr *)&recv_addr, sizeof(recv_addr));
    REQUIRE(r == 0);

    socklen_t addrlen = sizeof(recv_addr);
    r = getsockname(receiver_fd, (sockaddr *)&recv_addr, &addrlen);
    REQUIRE(r == 0);

    auto msg = generate_data(1024);
    bool called = false;
    auto func = [&]() -> condy::Coro<void> {
        auto &buffer_table = condy::current_runtime().buffer_table();
        buffer_table.init(1);
        iovec register_iov{
            .iov_base = const_cast<char *>(msg.data()),
            .iov_len = msg.size(),
        };
        buffer_table.update(0, &register_iov, 1);

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
    ssize_t r = send(sv[1], msg.data(), msg.size(), 0);
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
    ssize_t r = send(sv[1], msg.data(), msg.size(), 0);
    REQUIRE(r == msg.size());

    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_runtime().fd_table();
        fd_table.init(2);
        r = co_await condy::async_files_update(sv, 2, 0);
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
    ssize_t r = send(sv[1], msg.data(), msg.size(), 0);
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
    ssize_t r = send(sv[1], msg.data(), msg.size(), 0);
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
    ssize_t r = send(sv[1], msg.data(), msg.size(), 0);
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
        ssize_t r;
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
        auto &fd_table = condy::current_runtime().fd_table();
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

    ssize_t r = recv(sv[0], nullptr, 0, 0);
    REQUIRE(r == 0); // EOF

    close(sv[0]);
    close(sv[1]);
}

TEST_CASE("test async_operations - test shutdown - fixed fd") {
    int sv[2];
    create_tcp_socketpair(sv);

    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_runtime().fd_table();
        fd_table.init(2);
        int r = co_await condy::async_files_update(sv, 2, 0);
        REQUIRE(r == 2);

        r = co_await condy::async_shutdown(condy::fixed(1), SHUT_RDWR);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    ssize_t r = recv(sv[0], nullptr, 0, 0);
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
    ssize_t w = write(fd, msg.data(), msg.size());
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
