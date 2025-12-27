#include "condy/async_operations.hpp"
#include "condy/channel.hpp"
#include "condy/coro.hpp"
#include "condy/runtime.hpp"
#include "condy/runtime_options.hpp"
#include "condy/sync_wait.hpp"
#include "condy/task.hpp"
#include <atomic>
#include <cstddef>
#include <doctest/doctest.h>

namespace {

auto options = condy::RuntimeOptions().sq_size(8).cq_size(16);

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
    condy::Runtime runtime(options);
    condy::Channel<int> channel(2);

    const size_t max_items = 41;

    size_t finished = 0;
    auto producer = [&]() -> condy::Coro<void> {
        for (size_t i = 1; i <= max_items; ++i) {
            co_await channel.push(static_cast<int>(i));
        }
        finished++;
    };

    auto consumer = [&]() -> condy::Coro<void> {
        for (size_t i = 1; i <= max_items; ++i) {
            int item = co_await channel.pop();
            REQUIRE(item == i);
        }
        finished++;
    };

    auto t1 = condy::co_spawn(runtime, producer());
    auto t2 = condy::co_spawn(runtime, consumer());

    runtime.done();
    runtime.run();

    t1.wait();
    t2.wait();

    REQUIRE(finished == 2);
}

TEST_CASE("test channel - unbuffered channel") {
    condy::Runtime runtime(options);
    condy::Channel<int> channel(0);

    const size_t max_items = 10;

    size_t finished = 0;
    auto producer = [&]() -> condy::Coro<void> {
        for (size_t i = 1; i <= max_items; ++i) {
            co_await channel.push(static_cast<int>(i));
        }
        finished++;
    };

    auto consumer = [&]() -> condy::Coro<void> {
        for (size_t i = 1; i <= max_items; ++i) {
            int item = co_await channel.pop();
            REQUIRE(item == i);
        }
        finished++;
    };

    auto t1 = condy::co_spawn(runtime, producer());
    auto t2 = condy::co_spawn(runtime, consumer());

    runtime.done();
    runtime.run();

    t1.wait();
    t2.wait();

    REQUIRE(finished == 2);
}

TEST_CASE("test channel - multi producer and consumer") {
    condy::Runtime runtime(options);
    condy::Channel<int> channel(20);

    const size_t num_producers = 4;
    const size_t num_consumers = 4;
    const size_t items_per_producer = 25;

    size_t finished = 0;
    auto producer = [&](size_t id) -> condy::Coro<void> {
        for (size_t i = 1; i <= items_per_producer; ++i) {
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
    producer_tasks.reserve(num_producers);
    for (size_t i = 0; i < num_producers; ++i) {
        producer_tasks.push_back(condy::co_spawn(runtime, producer(i)));
    }

    std::vector<condy::Task<void>> consumer_tasks;
    consumer_tasks.reserve(num_consumers);
    for (size_t i = 0; i < num_consumers; ++i) {
        consumer_tasks.push_back(condy::co_spawn(runtime, consumer()));
    }

    runtime.done();
    runtime.run();

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

    condy::Runtime runtime(options);
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
        runtime.run();
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

    condy::Runtime runtime(options);
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
        runtime.run();
    });

    REQUIRE(!finished);
    REQUIRE(ch2.try_push(42));

    t.join();
    REQUIRE(finished);
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

TEST_CASE("test channel - move only in coroutine") {
    condy::Runtime runtime(options);
    condy::Channel<std::unique_ptr<int>> channel(2);

    const size_t max_items = 10;

    auto consumer = [&]() -> condy::Coro<void> {
        for (size_t i = 0; i < max_items; ++i) {
            auto item = co_await channel.pop();
            REQUIRE(item != nullptr);
            REQUIRE(*item == static_cast<int>(i));
        }
        co_return;
    };

    auto func = [&]() -> condy::Coro<void> {
        auto t = condy::co_spawn(consumer());
        for (size_t i = 0; i < max_items; ++i) {
            co_await channel.push(std::make_unique<int>(static_cast<int>(i)));
        }
        co_await std::move(t);
    };

    auto task = condy::co_spawn(runtime, func());

    runtime.done();
    runtime.run();

    task.wait();
}

TEST_CASE("test channel - no default constructor") {
    struct NoDefault {
        NoDefault(int v) : value(v) {}
        int value;
    };

    condy::Runtime runtime(options);
    // User should use type with default constructor when using
    // async operations
    condy::Channel<std::optional<NoDefault>> channel(2);

    const size_t max_items = 10;

    auto consumer = [&]() -> condy::Coro<void> {
        for (size_t i = 0; i < max_items; ++i) {
            auto item = *co_await channel.pop();
            REQUIRE(item.value == static_cast<int>(i));
        }
        co_return;
    };

    auto func = [&]() -> condy::Coro<void> {
        auto t = condy::co_spawn(consumer());
        for (size_t i = 0; i < max_items; ++i) {
            co_await channel.push(NoDefault(static_cast<int>(i)));
        }
        co_await std::move(t);
    };

    auto task = condy::co_spawn(runtime, func());

    runtime.done();
    runtime.run();

    task.wait();
}

TEST_CASE("test channel - close") {
    condy::Runtime runtime(options);
    condy::Channel<int> channel(2);

    const size_t max_items = 10;

    auto consumer = [&]() -> condy::Coro<void> {
        for (size_t i = 0; i < 2 * max_items; ++i) {
            auto item = co_await channel.pop();
            if (i < max_items) {
                REQUIRE(item == static_cast<int>(i + 1));
            } else {
                REQUIRE(item == 0); // Default indicates closed channel
            }
        }
    };

    auto func = [&]() -> condy::Coro<void> {
        auto t = condy::co_spawn(consumer());
        for (size_t i = 0; i < max_items; ++i) {
            co_await channel.push(static_cast<int>(i + 1));
        }
        channel.push_close();
        co_await std::move(t);
    };

    auto task = condy::co_spawn(runtime, func());

    runtime.done();
    runtime.run();

    task.wait();
}

TEST_CASE("test channel - close and broadcast") {
    condy::Runtime runtime(options);
    condy::Channel<int> channel(2);

    const size_t max_tasks = 5;

    size_t finished = 0;

    auto consumer = [&]() -> condy::Coro<void> {
        co_await channel.pop();
        finished++;
    };

    auto func = [&]() -> condy::Coro<void> {
        for (size_t i = 0; i < max_tasks; ++i) {
            condy::co_spawn(consumer()).detach();
        }
        co_await condy::co_switch(condy::current_runtime());
        channel.push_close();
        co_return;
    };

    auto task = condy::co_spawn(runtime, func());

    runtime.done();
    runtime.run();

    task.wait();

    REQUIRE(finished == max_tasks);
}

TEST_CASE("test channel - push to closed channel") {
    condy::Runtime runtime(options);
    condy::Channel<int> channel(2);

    channel.push_close();

    REQUIRE_THROWS(channel.try_push(42));

    auto func = [&]() -> condy::Coro<void> {
        REQUIRE_THROWS(co_await channel.push(42));
    };

    auto task = condy::co_spawn(runtime, func());

    runtime.done();
    runtime.run();

    task.wait();
}

TEST_CASE("test channel - push to closed channel with awaiters") {
    condy::Runtime runtime(options);
    condy::Channel<int> channel(1);

    auto close_func = [&]() -> condy::Coro<void> {
        channel.push_close();
        co_return;
    };

    auto func = [&]() -> condy::Coro<void> {
        co_await channel.push(1); // Fill the channel

        auto task = condy::co_spawn(close_func());

        REQUIRE_THROWS(co_await channel.push(2));

        co_await std::move(task);
    };

    auto task = condy::co_spawn(runtime, func());
    runtime.done();
    runtime.run();

    task.wait();
}

namespace {

struct int_deleter {
    void operator()(int *p) const {
        counter++;
        delete p;
    }
    inline static size_t counter = 0;
};

} // namespace

TEST_CASE("test channel - destruct items") {
    int_deleter::counter = 0;
    {
        condy::Channel<std::unique_ptr<int, int_deleter>> channel(5);

        for (size_t i = 0; i < 5; ++i) {
            REQUIRE(channel.try_push(std::unique_ptr<int, int_deleter>(
                new int(static_cast<int>(i)))));
        }

        REQUIRE(int_deleter::counter == 0);
    }
    REQUIRE(int_deleter::counter == 5);
}

TEST_CASE("test channel - destruct items after close") {
    int_deleter::counter = 0;
    {
        condy::Channel<std::unique_ptr<int, int_deleter>> channel(5);

        for (size_t i = 0; i < 5; ++i) {
            REQUIRE(channel.try_push(std::unique_ptr<int, int_deleter>(
                new int(static_cast<int>(i + 1)))));
        }

        channel.push_close();

        // Close should not destruct items yet, since we can still pop them
        REQUIRE(int_deleter::counter == 0);

        auto item = channel.try_pop();
        REQUIRE(item.has_value());
        REQUIRE(**item == 1);
    }
    REQUIRE(int_deleter::counter == 5);
}

TEST_CASE("test channel - cross runtimes") {
    condy::Runtime runtime1(options), runtime2(options);

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

    std::thread t1([&]() { runtime1.run(); });

    std::thread t2([&]() { runtime2.run(); });

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

TEST_CASE("test channel - cross runtimes with unbuffered channel") {
    condy::Runtime runtime1(options), runtime2(options);

    condy::Channel<int> channel(0);

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

    std::thread t1([&]() { runtime1.run(); });

    std::thread t2([&]() { runtime2.run(); });

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

TEST_CASE("test channel - force push") {
    condy::Channel<int> channel(2);

    REQUIRE(channel.try_push(1) == true);
    REQUIRE(channel.try_push(2) == true);
    REQUIRE(channel.try_push(3) == false);

    for (size_t i = 0; i < 10; i++) {
        channel.force_push(static_cast<int>(i + 3));
    }

    // Force pushed items act just like a waiting coroutine push
    REQUIRE(channel.size() == 2);

    for (size_t i = 0; i < 12; i++) {
        auto item = channel.try_pop();
        REQUIRE(item.has_value());
        REQUIRE(item.value() == static_cast<int>(i + 1));
    }

    REQUIRE(channel.size() == 0);

    channel.force_push(42);
    REQUIRE(channel.size() == 1);
}

namespace {

condy::Coro<void> producer_task(condy::Channel<int> &channel,
                                size_t num_messages) {
    for (size_t i = 0; i < num_messages; ++i) {
        co_await channel.push(static_cast<int>(i));
    }
    co_return;
}

condy::Coro<void> consumer_task(condy::Channel<int> &channel,
                                size_t num_messages) {
    for (size_t i = 0; i < num_messages; ++i) {
        auto value = co_await channel.pop();
        REQUIRE(value == static_cast<int>(i));
    }
    co_return;
}

condy::Coro<void>
launch_producers(std::vector<std::unique_ptr<condy::Channel<int>>> &channels,
                 size_t num_messages) {
    std::vector<condy::Task<void>> tasks;
    tasks.reserve(channels.size());
    for (auto &channel : channels) {
        tasks.emplace_back(
            condy::co_spawn(producer_task(*channel, num_messages)));
    }
    for (auto &task : tasks) {
        co_await std::move(task);
    }
    co_return;
}

condy::Coro<void>
launch_consumers(std::vector<std::unique_ptr<condy::Channel<int>>> &channels,
                 size_t num_messages) {
    std::vector<condy::Task<void>> tasks;
    tasks.reserve(channels.size());
    for (auto &channel : channels) {
        tasks.emplace_back(
            condy::co_spawn(consumer_task(*channel, num_messages)));
    }
    for (auto &task : tasks) {
        co_await std::move(task);
    }
    co_return;
}

} // namespace

TEST_CASE("test channel - two runtime") {
    const size_t num_pairs = 2;
    const size_t num_messages = 1025;
    const size_t buffer_size = 1024;

    std::vector<std::unique_ptr<condy::Channel<int>>> channels;
    channels.reserve(num_pairs);
    for (size_t i = 0; i < num_pairs; ++i) {
        channels.push_back(std::make_unique<condy::Channel<int>>(buffer_size));
    }

    condy::Runtime runtime1, runtime2;
    std::thread rt1([&]() {
        condy::sync_wait(runtime1, launch_producers(channels, num_messages));
    });

    condy::sync_wait(runtime2, launch_consumers(channels, num_messages));

    rt1.join();
}