#include "condy/buffers.hpp"
#include "condy/cqe_handler.hpp"
#include "condy/runtime.hpp"
#include "condy/runtime_options.hpp"
#include "condy/sync_wait.hpp"
#include "helpers.hpp"
#include <cerrno>
#include <condy/async_operations.hpp>
#include <cstdint>
#include <cstring>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <linux/nvme_ioctl.h>
#include <netinet/in.h>
#include <string_view>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

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
        auto &fd_table = condy::current_runtime().fd_table();
        fd_table.init(1);

        int r = co_await condy::async_socket_direct(AF_INET, SOCK_STREAM, 0,
                                                    CONDY_FILE_INDEX_ALLOC, 0);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());
}

#if !IO_URING_CHECK_VERSION(2, 5) // >= 2.5
TEST_CASE("test async_operations - test uring_cmd - basic") {
    // NOTE: cmd_sock available since 2.5
    auto my_async_cmd_sock = [](int cmd_op, int fd, int level, int optname,
                                void *optval, int optlen) {
        return condy::async_uring_cmd(cmd_op, fd, [=](io_uring_sqe *sqe) {
            sqe->optval = (unsigned long)(uintptr_t)optval;
            sqe->optname = optname;
            sqe->optlen = optlen;
            sqe->level = level;
        });
    };

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listen_fd >= 0);

    auto func = [&]() -> condy::Coro<void> {
        int val = 1;
        int r = co_await my_async_cmd_sock(SOCKET_URING_OP_SETSOCKOPT,
                                           listen_fd, SOL_SOCKET, SO_REUSEADDR,
                                           &val, sizeof(val));
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    close(listen_fd);
}
#endif

TEST_CASE("test async_operations - test uring_cmd - nvme passthrough") {
    const char *nvme_device_path = std::getenv("CONDY_TEST_NVME_DEVICE_PATH");
    const char *nvme_generic_char_device_path =
        std::getenv("CONDY_TEST_NVME_GENERIC_CHAR_DEVICE_PATH");
    if (nvme_device_path == nullptr) {
        MESSAGE("CONDY_TEST_NVME_DEVICE_PATH not set, skipping");
        return;
    }
    if (nvme_generic_char_device_path == nullptr) {
        MESSAGE("CONDY_TEST_NVME_GENERIC_CHAR_DEVICE_PATH not set, skipping");
        return;
    }

    int fd = open(nvme_device_path, O_WRONLY);
    REQUIRE(fd >= 0);

    std::string msg = "Hello, world!";
    ssize_t written = write(fd, msg.data(), msg.size());
    REQUIRE(written == (ssize_t)msg.size());
    fsync(fd);
    close(fd);

    fd = open(nvme_generic_char_device_path, O_RDONLY);
    REQUIRE(fd >= 0);

    condy::Runtime runtime(
        condy::RuntimeOptions().enable_sqe128().enable_cqe32());

    alignas(4096) char buffer[4096];
    auto func = [&]() -> condy::Coro<void> {
        condy::NVMeResult r =
            co_await my_cmd_nvme_read(fd, buffer, sizeof(buffer), 0);
        REQUIRE(r.status == 0);
        REQUIRE(r.result == 0);
        REQUIRE(std::string_view(buffer, msg.size()) == msg);
    };
    condy::sync_wait(runtime, func());
}

#if !IO_URING_CHECK_VERSION(2, 5) // >= 2.5
TEST_CASE("test async_operations - test uring_cmd - fixed fd") {
    // NOTE: cmd_sock available since 2.5
    auto my_async_cmd_sock_fixed = [](int cmd_op, int fixed_fd, int level,
                                      int optname, void *optval, int optlen) {
        return condy::async_uring_cmd(
            cmd_op, condy::fixed(fixed_fd), [=](io_uring_sqe *sqe) {
                sqe->optval = (unsigned long)(uintptr_t)optval;
                sqe->optname = optname;
                sqe->optlen = optlen;
                sqe->level = level;
            });
    };

    int listen_fd = create_accept_socket();

    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_runtime().fd_table();
        fd_table.init(1);
        int r = co_await condy::async_files_update(&listen_fd, 1, 0);
        REQUIRE(r == 1);

        int val = 1;
        r = co_await my_async_cmd_sock_fixed(SOCKET_URING_OP_SETSOCKOPT, 0,
                                             SOL_SOCKET, SO_REUSEADDR, &val,
                                             sizeof(val));
        REQUIRE(r == 0);
    };

    condy::sync_wait(func());

    close(listen_fd);
}
#endif

// TODO: uring_cmd test case with sqe128 + cqe128 + nvme cmd

#if !IO_URING_CHECK_VERSION(2, 13) // >= 2.13
TEST_CASE("test async_operations - test uring_cmd128 - basic") {
    // TODO: Use nvme cmd instead of socket cmd
    condy::Runtime runtime(
        condy::RuntimeOptions().enable_sqe_mixed().enable_cqe_mixed());
    auto my_async_cmd_sock = [](int cmd_op, int fd, int level, int optname,
                                void *optval, int optlen) {
        return condy::async_uring_cmd128(cmd_op, fd, [=](io_uring_sqe *sqe) {
            sqe->optval = (unsigned long)(uintptr_t)optval;
            sqe->optname = optname;
            sqe->optlen = optlen;
            sqe->level = level;
        });
    };

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listen_fd >= 0);

    auto func = [&]() -> condy::Coro<void> {
        int val = 1;
        int r = co_await my_async_cmd_sock(SOCKET_URING_OP_SETSOCKOPT,
                                           listen_fd, SOL_SOCKET, SO_REUSEADDR,
                                           &val, sizeof(val));
        REQUIRE(r == 0);
    };
    condy::sync_wait(runtime, func());

    close(listen_fd);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 13) // >= 2.13
TEST_CASE("test async_operations - test uring_cmd128 - fixed fd") {
    // TODO: Use nvme cmd instead of socket cmd
    condy::Runtime runtime(
        condy::RuntimeOptions().enable_sqe_mixed().enable_cqe_mixed());
    auto my_async_cmd_sock_fixed = [](int cmd_op, int fixed_fd, int level,
                                      int optname, void *optval, int optlen) {
        return condy::async_uring_cmd128(
            cmd_op, condy::fixed(fixed_fd), [=](io_uring_sqe *sqe) {
                sqe->optval = (unsigned long)(uintptr_t)optval;
                sqe->optname = optname;
                sqe->optlen = optlen;
                sqe->level = level;
            });
    };

    int listen_fd = create_accept_socket();

    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_runtime().fd_table();
        fd_table.init(1);
        int r = co_await condy::async_files_update(&listen_fd, 1, 0);
        REQUIRE(r == 1);

        int val = 1;
        r = co_await my_async_cmd_sock_fixed(SOCKET_URING_OP_SETSOCKOPT, 0,
                                             SOL_SOCKET, SO_REUSEADDR, &val,
                                             sizeof(val));
        REQUIRE(r == 0);
    };

    condy::sync_wait(runtime, func());

    close(listen_fd);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 5) // >= 2.5
TEST_CASE("test async_operations - test cmd_sock - basic") {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listen_fd >= 0);

    auto func = [&]() -> condy::Coro<void> {
        int val = 1;
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
        auto &fd_table = condy::current_runtime().fd_table();
        fd_table.init(1);
        int r = co_await condy::async_files_update(&listen_fd, 1, 0);
        REQUIRE(r == 1);

        int val = 1;
        r = co_await condy::async_cmd_sock(SOCKET_URING_OP_SETSOCKOPT,
                                           condy::fixed(0), SOL_SOCKET,
                                           SO_REUSEADDR, &val, sizeof(val));
        REQUIRE(r == 0);
    };

    condy::sync_wait(func());
}
#endif

#if !IO_URING_CHECK_VERSION(2, 13) // >= 2.13
TEST_CASE("test async_operations - test cmd_getsockname - basic") {
    int listen_fd = create_accept_socket();

    auto func = [&]() -> condy::Coro<void> {
        struct sockaddr_in addr {};
        socklen_t addrlen = sizeof(addr);
        int r = co_await condy::async_cmd_getsockname(
            listen_fd, (struct sockaddr *)&addr, &addrlen, 0);
        REQUIRE(r == 0);
        REQUIRE(addrlen == sizeof(addr));
        REQUIRE(addr.sin_family == AF_INET);
    };
    condy::sync_wait(func());

    close(listen_fd);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 13) // >= 2.13
TEST_CASE("test async_operations - test cmd_getsockname - fixed fd") {
    int listen_fd = create_accept_socket();

    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_runtime().fd_table();
        fd_table.init(1);
        int r = co_await condy::async_files_update(&listen_fd, 1, 0);
        REQUIRE(r == 1);

        struct sockaddr_in addr {};
        socklen_t addrlen = sizeof(addr);
        r = co_await condy::async_cmd_getsockname(
            condy::fixed(0), (struct sockaddr *)&addr, &addrlen, 0);
        REQUIRE(r == 0);
        REQUIRE(addrlen == sizeof(addr));
        REQUIRE(addr.sin_family == AF_INET);
    };
    condy::sync_wait(func());

    close(listen_fd);
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
    bool woken = false;

    auto waker = [&]() -> condy::Coro<void> {
        REQUIRE(!woken);
        int r = co_await condy::async_futex_wake(
            &futex_var, 1, FUTEX_BITSET_MATCH_ANY, FUTEX2_SIZE_U32, 0);
        REQUIRE(r >= 0);
    };

    auto func = [&]() -> condy::Coro<void> {
        auto t = condy::co_spawn(waker());
        int r = co_await condy::async_futex_wait(
            &futex_var, 0, FUTEX_BITSET_MATCH_ANY, FUTEX2_SIZE_U32, 0);
        woken = true;
        REQUIRE(r == 0);

        co_await std::move(t);
    };
    condy::sync_wait(func());
}
#endif

#if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6
TEST_CASE("test async_operations - test futex - waitv") {
    uint32_t futex_var1 = 0;
    uint32_t futex_var2 = 0;
    bool woken = false;

    auto waker = [&]() -> condy::Coro<void> {
        REQUIRE(!woken);
        int r = co_await condy::async_futex_wake(
            &futex_var2, 1, FUTEX_BITSET_MATCH_ANY, FUTEX2_SIZE_U32, 0);
        REQUIRE(r >= 0);
        REQUIRE(!woken);
        r = co_await condy::async_futex_wake(
            &futex_var1, 1, FUTEX_BITSET_MATCH_ANY, FUTEX2_SIZE_U32, 0);
        REQUIRE(r >= 0);
    };

    auto func = [&]() -> condy::Coro<void> {
        auto t = condy::co_spawn(waker());

        REQUIRE(!woken);
        futex_waitv waitv[2] = {};
        waitv[0].uaddr = reinterpret_cast<uint64_t>(&futex_var1);
        waitv[0].val = 0;
        waitv[0].flags = FUTEX2_SIZE_U32;
        waitv[1].uaddr = reinterpret_cast<uint64_t>(&futex_var2);
        waitv[1].val = 0;
        waitv[1].flags = FUTEX2_SIZE_U32;

        int r = co_await condy::async_futex_waitv(waitv, 2, 0);
        REQUIRE(r >= 0);
        woken = true;

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
        auto &fd_table = condy::current_runtime().fd_table();
        fd_table.init(2);

        int r = co_await condy::async_files_update(sv, 2, 0);
        REQUIRE(r == 2);

        int write_fd = co_await condy::async_fixed_fd_install(1, 0);
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
        auto &fd_table = condy::current_runtime().fd_table();
        fd_table.init(1);
        int r = co_await condy::async_files_update(&fd, 1, 0);
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

#if !IO_URING_CHECK_VERSION(2, 8) // >= 2.8
TEST_CASE("test async_operations - test cmd_discard - basic") {
    BlkDevice blkdev;
    if (blkdev.path().empty()) {
        MESSAGE("Can't create loop device, skipping");
        return;
    }

    int fd = open(blkdev.path().c_str(), O_RDWR);
    REQUIRE(fd >= 0);

    auto func = [&]() -> condy::Coro<void> {
        int r = co_await condy::async_cmd_discard(fd, 0, 4096);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());
    close(fd);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 8) // >= 2.8
TEST_CASE("test async_operations - test cmd_discard - fixed fd") {
    BlkDevice blkdev;
    if (blkdev.path().empty()) {
        MESSAGE("Can't create loop device, skipping");
        return;
    }

    int fd = open(blkdev.path().c_str(), O_RDWR);
    REQUIRE(fd >= 0);

    auto func = [&]() -> condy::Coro<void> {
        auto &fd_table = condy::current_runtime().fd_table();
        fd_table.init(1);
        int r = co_await condy::async_files_update(&fd, 1, 0);
        REQUIRE(r == 1);

        r = co_await condy::async_cmd_discard(condy::fixed(0), 0, 4096);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());
    close(fd);
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
        auto &fd_table = condy::current_runtime().fd_table();
        fd_table.init(1);
        int r = co_await condy::async_files_update(&sock_fd, 1, 0);
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
        auto &fd_table = condy::current_runtime().fd_table();
        fd_table.init(1);
        int r = co_await condy::async_files_update(&sock_fd, 1, 0);
        REQUIRE(r == 1);

        r = co_await condy::async_listen(condy::fixed(0), 10);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    close(sock_fd);
}
#endif

TEST_CASE("test async_operations - test epoll_ctl") {
    int epoll_fd = epoll_create1(0);
    REQUIRE(epoll_fd >= 0);

    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    auto func = [&]() -> condy::Coro<void> {
        epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = pipe_fds[0];
        int r = co_await condy::async_epoll_ctl(epoll_fd, pipe_fds[0],
                                                EPOLL_CTL_ADD, &ev);
        REQUIRE(r == 0);
    };
    condy::sync_wait(func());

    close(epoll_fd);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
}

#if !IO_URING_CHECK_VERSION(2, 10) // >= 2.10
TEST_CASE("test async_operations - test epoll_wait") {
    int epoll_fd = epoll_create1(0);
    REQUIRE(epoll_fd >= 0);

    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);

    auto msg = generate_data(128);

    auto writer = [&]() -> condy::Coro<void> {
        co_await condy::async_write(pipe_fds[1], condy::buffer(msg), 0);
    };

    auto func = [&]() -> condy::Coro<void> {
        int r;
        epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = pipe_fds[0];
        r = co_await condy::async_epoll_ctl(epoll_fd, pipe_fds[0],
                                            EPOLL_CTL_ADD, &ev);
        REQUIRE(r == 0);

        epoll_event events[4];

        auto t = condy::co_spawn(writer());

        r = co_await condy::async_epoll_wait(epoll_fd, events, 4, 0);
        REQUIRE(r == 1);

        co_await std::move(t);
    };
    condy::sync_wait(func());

    char buf[128];
    ssize_t r_read = read(pipe_fds[0], buf, sizeof(buf));
    REQUIRE(r_read == static_cast<ssize_t>(msg.size()));
    REQUIRE(std::string_view(buf, r_read) == msg);

    close(epoll_fd);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
}
#endif

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
        auto &fd_table = condy::current_runtime().fd_table();
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