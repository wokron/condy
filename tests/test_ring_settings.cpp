#include "condy/async_operations.hpp"
#include "condy/ring.hpp"
#include "condy/runtime.hpp"
#include "condy/runtime_options.hpp"
#include "condy/sync_wait.hpp"
#include <cerrno>
#include <doctest/doctest.h>
#include <fcntl.h>

#define USE_UID 1234

TEST_CASE("test ring_settings - personality") {
    int r;
    condy::Runtime runtime;
    auto &settings = runtime.settings();
    r = settings.apply_personality();
    REQUIRE(r > 0);
    int cred_id = r;

    // Apply twice is ok
    r = settings.apply_personality();
    REQUIRE(r > 0);
    REQUIRE(r != cred_id);
    r = settings.remove_personality(r);
    REQUIRE(r == 0);

    char name[32] = "XXXXXX";
    r = mkstemp(name);
    REQUIRE(r >= 0);
    close(r);
    auto d = condy::defer([&]() { unlink(name); });

    // Make temp file only accessible by current user
    r = chmod(name, S_IRUSR | S_IWUSR);
    REQUIRE(r == 0);

    auto func = [&]() -> condy::Coro<void> {
        // We can open the file as the original user
        r = co_await condy::async_open(name, O_RDONLY, 0);
        REQUIRE(r >= 0);
        co_await condy::async_close(r);

        uid_t orig_euid = geteuid();
        REQUIRE(orig_euid != USE_UID);
        if (seteuid(USE_UID) < 0) {
            MESSAGE("Can't switch to UID " << USE_UID << ", skipping");
            co_return;
        }
        auto d = condy::defer([&]() { REQUIRE(seteuid(orig_euid) == 0); });

        // Now we should be denied to open the file
        r = co_await condy::async_open(name, O_RDONLY, 0);
        REQUIRE(r == -EACCES);

        // We use the original user's credentials to open the file
        condy::set_current_cred_id(cred_id);
        r = co_await condy::async_open(name, O_RDONLY, 0);
        REQUIRE(r >= 0);
        co_await condy::async_close(r);
    };

    condy::sync_wait(runtime, func());

    REQUIRE(settings.remove_personality(cred_id) == 0);
}

TEST_CASE("test ring_settings - restrictions") {
    // Ring fd will be registered by default, disable it
    condy::Runtime runtime(condy::RuntimeOptions().disable_register_ring_fd());
    auto &settings = runtime.settings();
    io_uring_restriction res[2] = {};
    res[0].opcode = IORING_RESTRICTION_SQE_OP;
    res[0].sqe_op = IORING_OP_NOP;
    res[1].opcode = IORING_RESTRICTION_SQE_OP;
    res[1].sqe_op = IORING_OP_WRITE;
    REQUIRE(settings.set_restrictions(res, 2) == 0);

    // Restrictions cannot be changed once set
    io_uring_restriction res2 = {};
    res2.opcode = IORING_RESTRICTION_SQE_OP;
    res2.sqe_op = IORING_OP_READ;
    REQUIRE(settings.set_restrictions(&res2, 1) == -EBUSY);

    int pipefd[2];
    REQUIRE(pipe(pipefd) == 0);

    auto func = [&]() -> condy::Coro<void> {
        int r;

        r = co_await condy::async_nop();
        REQUIRE(r == 0);

        std::string msg = "Hello, world!";
        r = co_await condy::async_write(pipefd[1], condy::buffer(msg), 0);
        REQUIRE(r == msg.size());

        // This should be rejected by the restrictions
        char buffer[32];
        r = co_await condy::async_read(pipefd[0], condy::buffer(buffer), 0);
        REQUIRE(r == -EACCES);
    };

    condy::sync_wait(runtime, func());
}

TEST_CASE("test ring_settings - iowq_aff") {
    condy::Runtime runtime;
    auto &settings = runtime.settings();

    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(0, &mask);
    REQUIRE(settings.apply_iowq_aff(1, &mask) == 0);
    // Apply again is ok
    REQUIRE(settings.apply_iowq_aff(1, &mask) == 0);

    auto func = [&]() -> condy::Coro<void> {
        for (int i = 0; i < 5; i++) {
            co_await condy::async_nop();
        }
        // Change at runtime is ok
        REQUIRE(condy::current_runtime().settings().remove_iowq_aff() == 0);
        for (int i = 0; i < 5; i++) {
            co_await condy::async_nop();
        }
    };

    condy::sync_wait(runtime, func());
}

TEST_CASE("test ring_settings - iowq_max_workers") {
    condy::Runtime runtime;
    auto &settings = runtime.settings();
    unsigned int values[2] = {2, 4};
    REQUIRE(settings.set_iowq_max_workers(values) == 0);
    values[0] = values[1] = 0;
    REQUIRE(settings.set_iowq_max_workers(values) == 0);
    REQUIRE(values[0] == 2);
    REQUIRE(values[1] == 4);

    auto func = [&]() -> condy::Coro<void> {
        for (int i = 0; i < 5; i++) {
            co_await condy::async_nop();
        }
        values[0] = 3;
        values[1] = 6;
        REQUIRE(condy::current_runtime().settings().set_iowq_max_workers(
                    values) == 0);
        for (int i = 0; i < 5; i++) {
            co_await condy::async_nop();
        }
    };

    condy::sync_wait(runtime, func());
}

TEST_CASE("test ring_settings - probe") {
    condy::Runtime runtime;
    auto &settings = runtime.settings();
    io_uring_probe *probe = settings.get_probe();
    REQUIRE(probe != nullptr);
    REQUIRE(io_uring_opcode_supported(probe, IORING_OP_NOP));
}

#if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6
TEST_CASE("test ring_settings - napi") {
    condy::Runtime runtime;
    auto &settings = runtime.settings();
    io_uring_napi napi = {};
    napi.prefer_busy_poll = 1;
    napi.busy_poll_to = 50; // 50 us
    int r;
    r = settings.apply_napi(&napi);
    REQUIRE(r == 0);
    std::memset(&napi, 0, sizeof(napi));
    r = settings.remove_napi(&napi);
    REQUIRE(r == 0);
    REQUIRE(napi.prefer_busy_poll == 1);
    REQUIRE(napi.busy_poll_to == 50);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 8) // >= 2.8
TEST_CASE("test ring_settings - clock") {
    condy::Runtime runtime;
    auto &settings = runtime.settings();
    io_uring_clock_register clock_reg = {};
    clock_reg.clockid = CLOCK_MONOTONIC;
    REQUIRE(settings.set_clock(&clock_reg) == 0);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 9) // >= 2.9
TEST_CASE("test ring_settings - rings_size") {
    // Defer taskrun is required
    condy::Runtime runtime(condy::RuntimeOptions().enable_defer_taskrun());

    auto func = [&]() -> condy::Coro<void> {
        for (int i = 0; i < 5; i++) {
            co_await condy::async_nop();
        }
        auto &settings = runtime.settings();
        io_uring_params params = {};
        params.sq_entries = 2;
        params.cq_entries = 4;
        // Set at runtime is ok
        REQUIRE(settings.set_rings_size(&params) == 0);
        for (int i = 0; i < 5; i++) {
            co_await condy::async_nop();
        }
    };

    condy::sync_wait(runtime, func());
}
#endif

#if !IO_URING_CHECK_VERSION(2, 10) // >= 2.10
TEST_CASE("test ring_settings - iowait") {
    condy::Runtime runtime;
    auto &settings = runtime.settings();
    REQUIRE(settings.set_iowait(true) == 0);
    REQUIRE(settings.set_iowait(false) == 0);
}
#endif