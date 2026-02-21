#include "condy/execution.hpp"
#include "condy/runtime.hpp"
#include <doctest/doctest.h>

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