#include <condy/coro.hpp>
#include <condy/event_loop.hpp>
#include <condy/semaphore.hpp>
#include <condy/strategies.hpp>
#include <condy/task.hpp>
#include <doctest/doctest.h>
#include <thread>

namespace {

class NoStopStrategy : public condy::SimpleStrategy {
public:
    using Base = condy::SimpleStrategy;
    using Base::Base;

    bool should_stop() const override { return false; }
};

} // namespace

TEST_CASE("test semaphore - release 1") {
    const int times = 5;
    condy::Semaphore sem(0), sem2(0);
    int count = 0;

    condy::EventLoop<NoStopStrategy> task_loop(8);

    std::thread task_thread([&]() { task_loop.run(); });

    auto func = [&]() -> condy::Coro<void> {
        co_await sem.acquire();
        count++;
        sem2.release();
    };

    auto main = [&]() -> condy::Coro<void> {
        for (int i = 0; i < times; i++) {
            (co_await condy::co_spawn(task_loop, func())).detach();
        }
        for (int i = 0; i < times; i++) {
            sem.release();
            co_await sem2.acquire();
            REQUIRE(count == i + 1);
        }
        co_return;
    };

    condy::EventLoop<condy::SimpleStrategy> loop(8);

    loop.run(main());

    task_loop.stop();
    task_thread.join();

    REQUIRE(count == times);
}

TEST_CASE("test semaphore - release n") {
    const int times = 5;

    condy::EventLoop<NoStopStrategy> loop1(8), loop2(8);
    condy::EventLoop<NoStopStrategy> *loops[] = {&loop1, &loop2};
    condy::Semaphore sem(0);

    std::vector<std::thread> threads;
    for (auto &l : loops) {
        threads.emplace_back([&l]() { l->run(); });
    }

    std::vector<bool> finished(times, false);

    auto func = [&](int no) -> condy::Coro<void> {
        co_await sem.acquire();
        finished[no] = true;
        co_return;
    };

    auto main = [&]() -> condy::Coro<void> {
        std::vector<condy::Task<void>> tasks;
        tasks.reserve(times);
        for (int i = 0; i < times; i++) {
            auto t = co_await condy::co_spawn(*loops[i % 2], func(i));
            tasks.emplace_back(std::move(t));
        }

        sem.release(times);
        for (auto &&task : std::move(tasks)) {
            co_await std::move(task);
        }

        REQUIRE(std::all_of(finished.begin(), finished.end(),
                            [](auto v) { return v; }));
    };

    condy::EventLoop<condy::SimpleStrategy> loop(8);

    loop.run(main());

    for (auto &l : loops) {
        l->stop();
    }

    for (auto &t : threads) {
        t.join();
    }
}

TEST_CASE("test semaphore - multi-thread release") {
    const int times = 1000;
    const int thread_count = 4;

    condy::Semaphore sem(0);
    int count = 0;

    condy::EventLoop<NoStopStrategy> task_loop(8);

    std::thread task_thread([&]() { task_loop.run(); });

    auto func = [&]() -> condy::Coro<void> {
        co_await sem.acquire();
        count++;
    };

    std::vector<std::thread> threads;
    auto main = [&]() -> condy::Coro<void> {
        std::vector<condy::Task<void>> tasks;
        tasks.reserve(thread_count * times);
        for (int i = 0; i < thread_count * times; i++) {
            auto t = co_await condy::co_spawn(task_loop, func());
            tasks.emplace_back(std::move(t));
        }

        for (int i = 0; i < thread_count; i++) {
            threads.emplace_back([&, i]() {
                for (int j = 0; j < times; j++) {
                    sem.release();
                }
            });
        }

        for (auto &&task : std::move(tasks)) {
            co_await std::move(task);
        }
    };

    condy::EventLoop<condy::SimpleStrategy> loop(8);

    loop.run(main());

    task_loop.stop();
    task_thread.join();

    for (auto &t : threads) {
        t.join();
    }

    REQUIRE(count == thread_count * times);
}