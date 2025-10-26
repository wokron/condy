#include "condy/async_operations.hpp"
#include "condy/condition_variable.hpp"
#include "condy/strategies.hpp"
#include "condy/task.hpp"
#include <atomic>
#include <condy/event_loop.hpp>
#include <deque>
#include <doctest/doctest.h>

TEST_CASE("test condition_variable - basic") {
    condy::Mutex mutex;
    condy::ConditionVariable cv(mutex);
    bool ready = false;
    bool waiting = false;

    auto consumer = [&]() -> condy::Coro<void> {
        auto guard = co_await mutex.lock_guard();
        while (!ready) {
            waiting = true;
            co_await cv.wait();
        }
    };

    auto producer = [&]() -> condy::Coro<void> {
        __kernel_timespec ts = {.tv_sec = 0, .tv_nsec = 10000};
        co_await condy::async_timeout(&ts, 0, 0);
        {
            auto guard = co_await mutex.lock_guard();
            REQUIRE(waiting);
            ready = true;
        }
        cv.notify_one();
    };

    condy::EventLoop<condy::SimpleStrategy> loop(8);

    loop.run(consumer(), producer());

    REQUIRE(ready);
}

namespace {

class NoStopStrategy : public condy::SimpleStrategy {
public:
    using Base = condy::SimpleStrategy;
    using Base::Base;

    bool should_stop() const override { return false; }
};

} // namespace

TEST_CASE("test condition_variable - different thread") {
    condy::Mutex mutex;
    condy::ConditionVariable cv(mutex);
    bool ready = false;
    std::atomic_bool waiting = false;
    bool finished = false;

    auto consumer = [&]() -> condy::Coro<void> {
        auto guard = co_await mutex.lock_guard();
        while (!ready) {
            waiting = true;
            co_await cv.wait();
        }
        finished = true;
    };

    auto producer = [&]() -> condy::Coro<void> {
        __kernel_timespec ts = {.tv_sec = 0, .tv_nsec = 10000};
        co_await condy::async_timeout(&ts, 0, 0);
        {
            while (!waiting.load()) {
                co_await condy::async_nop();
            }
            auto guard = co_await mutex.lock_guard();
            ready = true;
        }
        cv.notify_one();
    };

    condy::EventLoop<condy::SimpleStrategy> loop1(8);
    condy::EventLoop<condy::SimpleStrategy> loop2(8);

    std::thread loop2_thread([&]() { loop2.run(consumer()); });

    loop1.run(producer());

    loop2_thread.join();

    REQUIRE(ready);
    REQUIRE(finished);
}

TEST_CASE("test condition_variable - notify_all") {
    const int num_consumers = 10;
    condy::Mutex mutex;
    condy::ConditionVariable cv(mutex);
    bool ready = false;
    std::atomic_int waiting_count = 0;
    int finished_count = 0;

    auto consumer = [&]() -> condy::Coro<void> {
        auto guard = co_await mutex.lock_guard();
        while (!ready) {
            waiting_count++;
            co_await cv.wait();
        }
        finished_count++;
    };

    auto producer = [&]() -> condy::Coro<void> {
        {
            while (waiting_count.load() < num_consumers) {
                co_await condy::async_nop();
            }
            auto guard = co_await mutex.lock_guard();
            ready = true;
        }
        cv.notify_all();
    };

    condy::EventLoop<condy::SimpleStrategy> loop(8);
    condy::EventLoop<condy::SimpleStrategy> loop2(8);

    auto main = [&]() -> condy::Coro<void> {
        for (int i = 0; i < num_consumers; i++) {
            condy::co_spawn(consumer()).detach();
        }
        co_return;
    };

    std::thread loop2_thread([&]() { loop2.run(main()); });

    loop.run(producer());

    loop2_thread.join();

    REQUIRE(ready);
    REQUIRE(finished_count == num_consumers);
}

TEST_CASE("test condition_variable - queue") {
    struct Queue {
        condy::Mutex mutex;
        condy::ConditionVariable cv{mutex};
        std::deque<int> data_queue;

        condy::Coro<void> push(int value) {
            auto guard = co_await mutex.lock_guard();
            data_queue.push_back(value);
            if (data_queue.size() == 1) {
                cv.notify_one();
            }
        }

        condy::Coro<int> pop() {
            auto guard = co_await mutex.lock_guard();
            while (data_queue.empty()) {
                co_await cv.wait();
            }
            int value = data_queue.front();
            data_queue.pop_front();
            co_return value;
        }
    };

    Queue queue;
    const int num_items = 100;
    int sum = 0;

    auto producer = [&]() -> condy::Coro<void> {
        for (int i = 1; i <= num_items; i++) {
            co_await queue.push(i);
        }
    };

    auto consumer = [&]() -> condy::Coro<void> {
        for (int i = 1; i <= num_items; i++) {
            int value = co_await queue.pop();
            sum += value;
        }
    };

    condy::EventLoop<condy::SimpleStrategy> loop1(8), loop2(8);

    std::thread consumer_thread([&]() { loop2.run(consumer()); });

    loop1.run(producer());

    consumer_thread.join();

    REQUIRE(sum == (num_items * (num_items + 1)) / 2);
}