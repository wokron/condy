#include "condy/async_operations.hpp"
#include "condy/ring.hpp"
#include "condy/runtime.hpp"
#include "condy/runtime_options.hpp"
#include "condy/sync_wait.hpp"
#include <cerrno>
#include <doctest/doctest.h>
#include <fcntl.h>

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