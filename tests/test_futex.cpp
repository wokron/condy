#include "condy/async_operations.hpp"
#include "condy/coro.hpp"
#include "condy/futex.hpp"
#include "condy/sync_wait.hpp"
#include "condy/task.hpp"
#include <algorithm>
#include <atomic>
#include <doctest/doctest.h>
#include <thread>

TEST_CASE("test async_futex - basic wait and notify") {
    int counter = 0;
    std::atomic_ref<int> atomic_counter(counter);
    condy::AsyncFutex<int> futex(atomic_counter);

    bool finished = false;

    auto wake_func = [&]() -> condy::Coro<void> {
        REQUIRE(!finished);
        REQUIRE(atomic_counter.load() == 0);
        atomic_counter++;
        futex.notify_one();
        co_return;
    };

    auto wait_func = [&]() -> condy::Coro<void> {
        auto t = condy::co_spawn(wake_func());
        co_await futex.wait(0);
        REQUIRE(atomic_counter.load() == 1);
        finished = true;
        co_await t;
    };

    condy::sync_wait(wait_func());
    REQUIRE(finished);
}

TEST_CASE("test async_futex - notify all") {
    int counter = 0;
    std::atomic_ref<int> atomic_counter(counter);
    condy::AsyncFutex<int> futex(atomic_counter);

    std::vector<int> finished(10, false);

    auto wake_func = [&]() -> condy::Coro<void> {
        REQUIRE(atomic_counter.load() == 0);
        REQUIRE(std::all_of(finished.begin(), finished.end(),
                            [](int f) { return !f; }));
        atomic_counter++;
        futex.notify_all();
        co_return;
    };

    auto wait_func = [&](size_t no) -> condy::Coro<void> {
        co_await futex.wait(0);
        REQUIRE(atomic_counter.load() == 1);
        finished[no] = true;
    };

    auto wait_func_all = [&]() -> condy::Coro<void> {
        auto t = condy::co_spawn(wake_func());
        std::vector<condy::Task<void>> tasks;
        tasks.reserve(finished.size());
        for (size_t i = 0; i < finished.size(); i++) {
            tasks.push_back(condy::co_spawn(wait_func(i)));
        }
        for (auto &t : tasks) {
            co_await t;
        }
        co_await t;
    };

    condy::sync_wait(wait_func_all());
    REQUIRE(
        std::all_of(finished.begin(), finished.end(), [](int f) { return f; }));
}

TEST_CASE("test async_futex - cross thread") {
    int counter = 0;
    std::atomic_ref<int> atomic_counter(counter);
    condy::AsyncFutex<int> futex(atomic_counter);

    bool finished = false;

    std::atomic_bool ready_to_wait = false;

    auto wait_func = [&]() -> condy::Coro<void> {
        ready_to_wait.store(true);
        ready_to_wait.notify_one();

        co_await futex.wait(0);
        REQUIRE(atomic_counter.load() == 1);
        finished = true;
    };

    std::thread t1([&] { condy::sync_wait(wait_func()); });

    ready_to_wait.wait(false);

    atomic_counter++;
    futex.notify_one();

    t1.join();

    REQUIRE(finished);
}

TEST_CASE("test async_futex - cancel") {
    int counter = 0;
    std::atomic_ref<int> atomic_counter(counter);
    condy::AsyncFutex<int> futex(atomic_counter);

    auto wait_func = [&]() -> condy::Coro<void> {
        auto r = co_await condy::when_any(futex.wait(0), condy::async_nop());
        REQUIRE(r.index() == 1);
        REQUIRE(atomic_counter.load() == 0);
    };

    condy::sync_wait(wait_func());
}