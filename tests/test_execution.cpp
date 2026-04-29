#include "condy/async_operations.hpp"
#include "condy/execution.hpp"
#include "condy/runtime.hpp"
#include <doctest/doctest.h>
#include <exec/when_any.hpp>

namespace ex = stdexec;

TEST_CASE("test execution - schedule") {
    condy::Runtime runtime;
    std::thread::id runtime_thread_id;
    std::thread runtime_thread([&] {
        runtime_thread_id = std::this_thread::get_id();
        runtime.run();
    });

    auto scheduler = condy::get_scheduler(runtime);

    bool executed = false;
    ex::sender auto sender = ex::schedule(scheduler) | ex::then([&] {
                                 executed = true;
                                 return std::this_thread::get_id();
                             });

    auto [thread_id] = ex::sync_wait(sender).value();
    REQUIRE(executed);
    REQUIRE(thread_id == runtime_thread_id);
    REQUIRE(runtime_thread_id != std::this_thread::get_id());

    runtime.allow_exit();
    runtime_thread.join();
}

TEST_CASE("test execution - sender") {
    condy::Runtime runtime;
    std::thread runtime_thread([&] { runtime.run(); });

    auto scheduler = condy::get_scheduler(runtime);

    bool executed = false;
    ex::sender auto sender = ex::schedule(scheduler) | ex::let_value([&] {
                                 executed = true;
                                 return condy::async_nop();
                             });

    auto [r] = ex::sync_wait(sender).value();
    REQUIRE(executed);
    REQUIRE(r == 0);

    runtime.allow_exit();
    runtime_thread.join();
}

TEST_CASE("test execution - when_all") {
    condy::Runtime runtime;
    std::thread runtime_thread([&] { runtime.run(); });

    auto scheduler = condy::get_scheduler(runtime);

    bool executed1 = false;
    bool executed2 = false;

    auto sender1 = ex::schedule(scheduler) | ex::then([&] {
                       executed1 = true;
                       return 42;
                   });
    auto sender2 = ex::schedule(scheduler) | ex::then([&] {
                       executed2 = true;
                       return 0;
                   });

    auto when_all_sender = ex::when_all(sender1, sender2);
    auto [r1, r2] = ex::sync_wait(when_all_sender).value();

    REQUIRE(executed1);
    REQUIRE(executed2);
    REQUIRE(r1 == 42);
    REQUIRE(r2 == 0);

    runtime.allow_exit();
    runtime_thread.join();
}

TEST_CASE("test execution - when_any") {
    condy::Runtime runtime;
    std::thread runtime_thread([&] { runtime.run(); });

    auto scheduler = condy::get_scheduler(runtime);

    __kernel_timespec ts = {
        .tv_sec = 60ll * 60ll,
        .tv_nsec = 0,
    };
    auto sender = ex::schedule(scheduler) | ex::let_value([&] {
                      return exec::when_any(condy::async_timeout(&ts, 0, 0),
                                            condy::async_nop());
                  });

    auto [r] = ex::sync_wait(sender).value();
    REQUIRE(r == 0);

    runtime.allow_exit();
    runtime_thread.join();
}

TEST_CASE("test execution - when_any with different thread") {
    condy::Runtime runtime1, runtime2;
    std::thread thread1([&] { runtime1.run(); });
    std::thread thread2([&] { runtime2.run(); });

    auto scheduler1 = condy::get_scheduler(runtime1);
    auto scheduler2 = condy::get_scheduler(runtime2);

    __kernel_timespec ts = {
        .tv_sec = 60ll * 60ll,
        .tv_nsec = 0,
    };
    auto sender1 = ex::schedule(scheduler1) | ex::let_value([&] {
                       return condy::async_timeout(&ts, 0, 0);
                   });
    auto sender2 = ex::schedule(scheduler2) | ex::then([] { return 42; });
    auto when_any_sender = exec::when_any(sender1, sender2);
    auto [r] = ex::sync_wait(when_any_sender).value();
    REQUIRE(r == 42);

    runtime1.allow_exit();
    runtime2.allow_exit();
    thread1.join();
    thread2.join();
}