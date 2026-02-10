#include "condy/awaiter_operations.hpp"
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
#include <thread>
#include <unistd.h>

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
    ssize_t r = read(sv[0], read_buf, sizeof(read_buf));
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
    ssize_t r = read(sv[0], read_buf, sizeof(read_buf));
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

        auto &buffer_table = condy::current_runtime().buffer_table();
        buffer_table.init(1);
        buffer_table.update(0, &iov, 1);

        condy::Channel<int> channel(1);
        ssize_t n = co_await condy::async_sendmsg_zc(
            sv[1], condy::fixed(0, &msg_hdr), 0, condy::will_push(channel));
        REQUIRE(n == msg.size());
        co_await channel.pop();
    };
    condy::sync_wait(func());

    char read_buf[2048];
    ssize_t r = read(sv[0], read_buf, sizeof(read_buf));
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

#if !IO_URING_CHECK_VERSION(2, 13) // >= 2.13
TEST_CASE("test async_operations - test nop128 - sqe 128") {
    condy::Runtime runtime(condy::RuntimeOptions().enable_sqe128());
    auto func = [&]() -> condy::Coro<void> {
        int r = co_await condy::async_nop128();
        REQUIRE(r == 0);
    };
    condy::sync_wait(runtime, func());
}
#endif

#if !IO_URING_CHECK_VERSION(2, 13) // >= 2.13
TEST_CASE("test async_operations - test nop128 - sqe mixed") {
    condy::Runtime runtime(condy::RuntimeOptions().enable_sqe_mixed());
    auto func = [&]() -> condy::Coro<void> {
        int r = co_await condy::async_nop128();
        REQUIRE(r == 0);
    };
    condy::sync_wait(runtime, func());
}
#endif

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

            if (i == 2) {
                char buf[32];
                ssize_t n = read(sockfd, buf, sizeof(buf));
                REQUIRE(n == 0); // EOF since no fd available
            }

            close(sockfd);
        }
    };

    auto main = [&]() -> condy::Coro<void> {
        std::jthread client_thread(client);

        condy::current_runtime().fd_table().init(2);

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

        int fd3 = co_await condy::async_accept_direct(
            listen_fd, (sockaddr *)&addr, &addrlen, 0, CONDY_FILE_INDEX_ALLOC);
        REQUIRE(fd3 == -ENFILE); // Should fail, no more fixed fds available

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

        auto &fd_table = condy::current_runtime().fd_table();
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
        auto &fd_table = condy::current_runtime().fd_table();
        fd_table.init(2);
        int r = co_await condy::async_files_update(pipe_fds, 2, 0);
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

        auto &fd_table = condy::current_runtime().fd_table();
        fd_table.init(4);

        for (int i = 0; i < 4; i++) {
            int sockfd = socket(AF_INET, SOCK_STREAM, 0);
            REQUIRE(sockfd >= 0);

            r = co_await condy::async_files_update(&sockfd, 1, i);
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
        int r = co_await condy::async_fallocate(fd, 0, 0, 1024ll * 1024ll);
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
        auto &fd_table = condy::current_runtime().fd_table();
        fd_table.init(1);
        int r = co_await condy::async_files_update(&fd, 1, 0);
        REQUIRE(r == 1);

        r = co_await condy::async_fallocate(condy::fixed(0), 0, 0,
                                            1024ll * 1024ll);
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
        auto &fd_table = condy::current_runtime().fd_table();
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
        auto &fd_table = condy::current_runtime().fd_table();
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
        auto &fd_table = condy::current_runtime().fd_table();
        fd_table.init(2);
        int r = co_await condy::async_files_update(pipe_fds, 2, 0);
        REQUIRE(r == 2);

        r = co_await condy::async_close(condy::fixed(0));
        REQUIRE(r == 0);
        r = co_await condy::async_close(pipe_fds[1]);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());
}

TEST_CASE("test async_operations - test read - basic") {
    ssize_t r;
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
    ssize_t r;
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    auto msg = generate_data(1024);
    r = write(pipe_fds[1], msg.data(), msg.size());
    REQUIRE(r == msg.size());
    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_runtime().fd_table();
        fd_table.init(2);
        r = co_await condy::async_files_update(pipe_fds, 2, 0);
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
    ssize_t r;
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    auto msg = generate_data(1024);
    r = write(pipe_fds[1], msg.data(), msg.size());
    REQUIRE(r == msg.size());
    auto func = [&]() -> condy::Coro<void> {
        char buf[2048];

        auto &buffer_table = condy::current_runtime().buffer_table();
        buffer_table.init(1);
        iovec register_iov{
            .iov_base = buf,
            .iov_len = sizeof(buf),
        };
        buffer_table.update(0, &register_iov, 1);

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
    ssize_t r;
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
    ssize_t r;
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
    ssize_t r;
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
    ssize_t r;
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    auto msg = generate_data(1024);
    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_runtime().fd_table();
        fd_table.init(2);
        r = co_await condy::async_files_update(pipe_fds, 2, 0);
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
    ssize_t r;
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    auto msg = generate_data(1024);
    auto func = [&]() -> condy::Coro<void> {
        auto &buffer_table = condy::current_runtime().buffer_table();
        buffer_table.init(1);
        iovec register_iov{
            .iov_base = msg.data(),
            .iov_len = msg.size(),
        };
        buffer_table.update(0, &register_iov, 1);

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
    ssize_t w = write(fd, msg.data(), msg.size());
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
