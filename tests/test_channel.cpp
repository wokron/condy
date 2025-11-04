#include "condy/async_operations.hpp"
#include "condy/channel.hpp"
#include "condy/coro.hpp"
#include "condy/runtime.hpp"
#include "condy/runtime_options.hpp"
#include "condy/task.hpp"
#include <atomic>
#include <cstddef>
#include <doctest/doctest.h>

namespace {

auto options = condy::SingleThreadOptions().sq_size(8).cq_size(16);

auto options2 =
    condy::MultiThreadOptions().sq_size(8).cq_size(16).num_threads(4);

} // namespace

TEST_CASE("test channel - try push and pop") {
    condy::Channel<int> channel(2);

    REQUIRE(channel.capacity() == 2);
    REQUIRE(channel.size() == 0);

    REQUIRE(channel.try_push(1) == true);
    REQUIRE(channel.size() == 1);

    REQUIRE(channel.try_push(2) == true);
    REQUIRE(channel.size() == 2);

    REQUIRE(channel.try_push(3) == false);
    REQUIRE(channel.size() == 2);

    auto item1 = channel.try_pop();
    REQUIRE(item1.has_value());
    REQUIRE(item1.value() == 1);
    REQUIRE(channel.size() == 1);

    auto item2 = channel.try_pop();
    REQUIRE(item2.has_value());
    REQUIRE(item2.value() == 2);
    REQUIRE(channel.size() == 0);

    auto item3 = channel.try_pop();
    REQUIRE(!item3.has_value());
    REQUIRE(channel.size() == 0);
}

TEST_CASE("test channel - push and pop with coroutines") {
    condy::SingleThreadRuntime runtime(options);
    condy::Channel<int> channel(2);

    const size_t max_items = 41;

    size_t finished = 0;
    auto producer = [&]() -> condy::Coro<void> {
        for (int i = 1; i <= max_items; ++i) {
            co_await channel.push(i);
        }
        finished++;
    };

    auto consumer = [&]() -> condy::Coro<void> {
        for (int i = 1; i <= max_items; ++i) {
            int item = co_await channel.pop();
            REQUIRE(item == i);
        }
        finished++;
    };

    auto t1 = condy::co_spawn(runtime, producer());
    auto t2 = condy::co_spawn(runtime, consumer());

    runtime.done();
    runtime.wait();

    t1.wait();
    t2.wait();

    REQUIRE(finished == 2);
}

TEST_CASE("test channel - multi producer and consumer") {
    condy::SingleThreadRuntime runtime(options);
    condy::Channel<int> channel(20);

    const size_t num_producers = 4;
    const size_t num_consumers = 4;
    const size_t items_per_producer = 25;

    size_t finished = 0;
    auto producer = [&](size_t id) -> condy::Coro<void> {
        for (int i = 1; i <= items_per_producer; ++i) {
            co_await channel.push(static_cast<int>(id * 100 + i));
        }
        finished++;
    };

    auto consumer = [&]() -> condy::Coro<void> {
        assert((num_producers * items_per_producer) % num_consumers == 0);
        for (size_t i = 0;
             i < (num_producers * items_per_producer) / num_consumers; ++i) {
            int item = co_await channel.pop();
            REQUIRE(item != 0); // Basic check to ensure item is received
        }
        finished++;
    };

    std::vector<condy::Task<void>> producer_tasks;
    for (size_t i = 0; i < num_producers; ++i) {
        producer_tasks.push_back(condy::co_spawn(runtime, producer(i)));
    }

    std::vector<condy::Task<void>> consumer_tasks;
    for (size_t i = 0; i < num_consumers; ++i) {
        consumer_tasks.push_back(condy::co_spawn(runtime, consumer()));
    }

    runtime.done();
    runtime.wait();

    for (auto &task : producer_tasks) {
        task.wait();
    }
    for (auto &task : consumer_tasks) {
        task.wait();
    }

    REQUIRE(finished == (num_producers + num_consumers));
}

TEST_CASE("test channel - wait two channel") {
    using condy::operators::operator&&;

    condy::SingleThreadRuntime runtime(options);
    condy::Channel<int> ch1(1), ch2(1);

    std::atomic_bool finished = false;

    auto func = [&]() -> condy::Coro<void> {
        auto [item1, item2] = co_await (ch1.pop() && ch2.pop());
        REQUIRE(item1 == 42);
        REQUIRE(item2 == 84);
        finished = true;
    };

    condy::co_spawn(runtime, func()).detach();

    std::thread t([&]() {
        runtime.done();
        runtime.wait();
    });

    REQUIRE(!finished);

    REQUIRE(ch1.try_push(42));
    REQUIRE(!finished);

    REQUIRE(ch2.try_push(84));
    t.join();
    REQUIRE(finished);
}

TEST_CASE("test channel - channel cancel pop") {
    using condy::operators::operator||;

    condy::SingleThreadRuntime runtime(options);
    condy::Channel<int> ch1(1), ch2(1);

    std::atomic_bool finished = false;

    auto func = [&]() -> condy::Coro<void> {
        __kernel_timespec ts{
            .tv_sec = 60 * 60,
            .tv_nsec = 0,
        };
        auto r = co_await (ch1.pop() || ch2.pop() ||
                           condy::async_timeout(&ts, 0, 0));
        REQUIRE(r.index() == 1);
        REQUIRE(std::get<1>(r) == 42);
        finished = true;
    };

    condy::co_spawn(runtime, func()).detach();

    std::thread t([&]() {
        runtime.done();
        runtime.wait();
    });

    REQUIRE(!finished);
    REQUIRE(ch2.try_push(42));

    t.join();
    REQUIRE(finished);
}

TEST_CASE("test channel - multi thread runtime push and pop") {
    condy::MultiThreadRuntime runtime(options2);

    condy::Channel<int> channel(2);

    const size_t num_items = 101;

    std::atomic_size_t finished = 0;

    auto producer = [&]() -> condy::Coro<void> {
        for (int i = 1; i <= num_items; ++i) {
            co_await channel.push(i);
        }
        finished++;
    };

    auto consumer = [&]() -> condy::Coro<void> {
        for (int i = 1; i <= num_items; ++i) {
            int item = co_await channel.pop();
            REQUIRE(item == i);
        }
        finished++;
    };

    auto t1 = condy::co_spawn(runtime, producer());
    auto t2 = condy::co_spawn(runtime, consumer());

    t1.wait();
    t2.wait();

    REQUIRE(finished == 2);
}

TEST_CASE("test channel - multi thread runtime multi producer and consumer") {
    condy::MultiThreadRuntime runtime(options2);
    condy::Channel<int> channel(20);

    std::atomic_size_t finished = 0;

    const size_t num_producers = 5;
    const size_t num_consumers = 5;
    const size_t items_per_producer = 25;

    auto producer = [&](size_t id) -> condy::Coro<void> {
        for (size_t i = 0; i < items_per_producer; ++i) {
            co_await channel.push(static_cast<int>(id * 100 + i));
        }
        finished++;
    };

    auto consumer = [&]() -> condy::Coro<void> {
        for (size_t i = 0; i < items_per_producer; ++i) {
            co_await channel.pop();
        }
        finished++;
    };

    std::vector<condy::Task<void>> producer_tasks;
    for (size_t i = 0; i < num_producers; ++i) {
        producer_tasks.push_back(condy::co_spawn(runtime, producer(i)));
    }

    std::vector<condy::Task<void>> consumer_tasks;
    for (size_t i = 0; i < num_consumers; ++i) {
        consumer_tasks.push_back(condy::co_spawn(runtime, consumer()));
    }

    for (auto &task : producer_tasks) {
        task.wait();
    }

    for (auto &task : consumer_tasks) {
        task.wait();
    }

    REQUIRE(finished == (num_producers + num_consumers));
}

TEST_CASE("test channel - move only type") {
    condy::Channel<std::unique_ptr<int>> channel(2);

    REQUIRE(channel.try_push(std::make_unique<int>(42)));
    REQUIRE(channel.try_push(std::make_unique<int>(43)));

    auto item = channel.try_pop();
    REQUIRE(item.has_value());
    REQUIRE(**item == 42);

    item = channel.try_pop();
    REQUIRE(item.has_value());
    REQUIRE(**item == 43);
}

TEST_CASE("test channel - cross runtimes") {
    condy::SingleThreadRuntime runtime1(options), runtime2(options);

    condy::Channel<int> channel(2);

    const size_t max_items = 41;

    std::atomic_size_t finished = 0;
    auto producer = [&]() -> condy::Coro<void> {
        for (size_t i = 0; i < max_items; ++i) {
            co_await channel.push(static_cast<int>(i));
        }
        finished++;
    };

    auto consumer = [&]() -> condy::Coro<void> {
        for (size_t i = 0; i < max_items; ++i) {
            int item = co_await channel.pop();
            REQUIRE(item == static_cast<int>(i));
        }
        finished++;
    };

    std::thread t1([&]() { runtime1.wait(); });

    std::thread t2([&]() { runtime2.wait(); });

    auto task1 = condy::co_spawn(runtime1, consumer());
    auto task2 = condy::co_spawn(runtime2, producer());

    task1.wait();
    task2.wait();

    REQUIRE(finished == 2);

    runtime1.done();
    runtime2.done();

    REQUIRE(finished == 2);

    t1.join();
    t2.join();
}
