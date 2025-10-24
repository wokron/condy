#include <condy/coro.hpp>
#include <condy/event_loop.hpp>
#include <condy/semaphore.hpp>
#include <condy/strategies.hpp>
#include <condy/task.hpp>
#include <doctest/doctest.h>

TEST_CASE("test semaphore - BinarySemaphore") {
    const int times = 5;
    condy::BinarySemaphore sem(0);
    condy::BinarySemaphore sem2(0);
    int count = 0;

    auto func = [&]() -> condy::Coro<void> {
        co_await sem.acquire();
        count++;
        sem2.release();
    };

    auto main = [&]() -> condy::Coro<void> {
        for (int i = 0; i < times; i++) {
            condy::co_spawn(func()).detach();
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

    REQUIRE(count == times);
}

TEST_CASE("test semaphore - Semaphore") {
    const int times = 5;
    condy::Semaphore sem(times, 0);

    std::vector<bool> finished(times, false);

    auto func = [&](int no) -> condy::Coro<void> {
        sem.acquire();
        finished[no] = true;
        co_return;
    };

    auto main = [&]() -> condy::Coro<void> {
        std::vector<condy::Task<void>> tasks;
        tasks.reserve(times);
        for (int i = 0; i < times; i++) {
            tasks.emplace_back(condy::co_spawn(func(i)));
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
}