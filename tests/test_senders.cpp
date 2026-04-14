#include "condy/awaiter_operations.hpp"
#include "condy/channel.hpp"
#include "condy/sender_operations.hpp"
#include "condy/senders.hpp"
#include "condy/sync_wait.hpp"
#include <atomic>
#include <cerrno>
#include <doctest/doctest.h>
#include <thread>

TEST_CASE("test senders - basic") {
    auto f = []() -> condy::Coro<void> {
        auto r = co_await condy::detail::make_op_awaiter(io_uring_prep_nop);
        REQUIRE(r == 0);
    };
    condy::sync_wait(f());
}

TEST_CASE("test senders - when_all") {
    auto f = []() -> condy::Coro<void> {
        auto [r1, r2] = co_await (
            condy::when_all(condy::detail::make_op_awaiter(io_uring_prep_nop),
                            condy::detail::make_op_awaiter(io_uring_prep_nop)));
        REQUIRE(r1 == 0);
        REQUIRE(r2 == 0);
    };
    condy::sync_wait(f());
}

TEST_CASE("test senders - empty when_all") {
    auto f = []() -> condy::Coro<void> { co_await (condy::when_all()); };
    condy::sync_wait(f());
}

TEST_CASE("test senders - ranged when_all") {
    auto f = []() -> condy::Coro<void> {
        using Op = decltype(condy::detail::make_op_awaiter(io_uring_prep_nop));
        std::vector<Op> senders;
        senders.reserve(5);
        for (int i = 0; i < 5; ++i) {
            senders.push_back(
                condy::detail::make_op_awaiter(io_uring_prep_nop));
        }
        auto results = co_await (condy::when_all(std::move(senders)));
        for (auto r : results) {
            REQUIRE(r == 0);
        }
    };
    condy::sync_wait(f());
}

TEST_CASE("test senders - ranged empty when_all") {
    auto f = []() -> condy::Coro<void> {
        using Op = decltype(condy::detail::make_op_awaiter(io_uring_prep_nop));
        std::vector<Op> senders;
        auto results = co_await (condy::when_all(std::move(senders)));
        REQUIRE(results.empty());
    };
    condy::sync_wait(f());
}

TEST_CASE("test senders - parallel all") {
    auto f = []() -> condy::Coro<void> {
        auto [order, r] = co_await (condy::parallel<condy::ParallelAllSender>(
            condy::detail::make_op_awaiter(io_uring_prep_nop),
            condy::detail::make_op_awaiter(io_uring_prep_nop)));
        REQUIRE(order[0] == 0);
        REQUIRE(order[1] == 1);
        REQUIRE(std::get<0>(r) == 0);
        REQUIRE(std::get<1>(r) == 0);
    };
    condy::sync_wait(f());
}

TEST_CASE("test senders - empty parallell all") {
    auto f = []() -> condy::Coro<void> {
        [[maybe_unused]] auto [order, r] =
            co_await (condy::parallel<condy::ParallelAllSender>());
    };
    condy::sync_wait(f());
}

TEST_CASE("test senders - ranged parallel all") {
    auto f = []() -> condy::Coro<void> {
        using Op = decltype(condy::detail::make_op_awaiter(io_uring_prep_nop));
        std::vector<Op> senders;
        senders.reserve(5);
        for (int i = 0; i < 5; ++i) {
            senders.push_back(
                condy::detail::make_op_awaiter(io_uring_prep_nop));
        }
        auto [order, results] =
            co_await (condy::parallel<condy::RangedParallelAllSender>(
                std::move(senders)));
        for (size_t i = 0; i < order.size(); ++i) {
            REQUIRE(order[i] == i);
            REQUIRE(results[i] == 0);
        }
    };
    condy::sync_wait(f());
}

TEST_CASE("test senders - ranged empty parallel all") {
    auto f = []() -> condy::Coro<void> {
        using Op = decltype(condy::detail::make_op_awaiter(io_uring_prep_nop));
        std::vector<Op> senders;
        auto [order, r] =
            co_await (condy::parallel<condy::RangedParallelAllSender>(
                std::move(senders)));
        REQUIRE(order.empty());
        REQUIRE(r.empty());
    };
    condy::sync_wait(f());
}

TEST_CASE("test senders - when_any") {
    auto f = []() -> condy::Coro<void> {
        __kernel_timespec ts{
            .tv_sec = 60,
            .tv_nsec = 0,
        };
        auto r = co_await (condy::when_any(
            condy::detail::make_op_awaiter(io_uring_prep_nop),
            condy::detail::make_op_awaiter(io_uring_prep_timeout, &ts, 0, 0)));
        REQUIRE(r.index() == 0);
        REQUIRE(std::get<0>(r) == 0);
    };
    condy::sync_wait(f());
}

TEST_CASE("test senders - ranged when_any") {
    auto f = []() -> condy::Coro<void> {
        using Op = decltype(condy::detail::make_op_awaiter(io_uring_prep_nop));
        std::vector<Op> senders;
        senders.reserve(5);
        for (int i = 0; i < 5; ++i) {
            senders.push_back(
                condy::detail::make_op_awaiter(io_uring_prep_nop));
        }
        auto [index, results] = co_await (condy::when_any(std::move(senders)));
        REQUIRE(index == 0);
        REQUIRE(results == 0);
    };
    condy::sync_wait(f());
}

TEST_CASE("test senders - ranged empty when_any") {
    auto f = []() -> condy::Coro<void> {
        using Op = decltype(condy::detail::make_op_awaiter(io_uring_prep_nop));
        std::vector<Op> senders;
        REQUIRE_THROWS_AS(co_await (condy::when_any(std::move(senders))),
                          std::invalid_argument);
    };
    condy::sync_wait(f());
}

TEST_CASE("test senders - &&") {
    using condy::operators::operator&&;
    auto f = []() -> condy::Coro<void> {
        auto [r1, r2, r3] =
            co_await (condy::detail::make_op_awaiter(io_uring_prep_nop) &&
                      condy::detail::make_op_awaiter(io_uring_prep_nop) &&
                      condy::detail::make_op_awaiter(io_uring_prep_nop));
        REQUIRE(r1 == 0);
        REQUIRE(r2 == 0);
        REQUIRE(r3 == 0);
    };
    condy::sync_wait(f());
}

TEST_CASE("test senders - parallel any") {
    auto f = []() -> condy::Coro<void> {
        __kernel_timespec ts{
            .tv_sec = 60,
            .tv_nsec = 0,
        };
        auto [order, r] = co_await (condy::parallel<condy::ParallelAnySender>(
            condy::detail::make_op_awaiter(io_uring_prep_nop),
            condy::detail::make_op_awaiter(io_uring_prep_timeout, &ts, 0, 0)));
        REQUIRE(order[0] == 0);
        REQUIRE(std::get<0>(r) == 0);
        REQUIRE(std::get<1>(r) == -ECANCELED);
    };
    condy::sync_wait(f());
}

TEST_CASE("test senders - ranged parallel any") {
    auto f = []() -> condy::Coro<void> {
        using Op = decltype(condy::detail::make_op_awaiter(io_uring_prep_nop));
        std::vector<Op> senders;
        senders.reserve(5);
        for (int i = 0; i < 5; ++i) {
            senders.push_back(
                condy::detail::make_op_awaiter(io_uring_prep_nop));
        }
        auto [order, results] =
            co_await (condy::parallel<condy::RangedParallelAnySender>(
                std::move(senders)));
        REQUIRE(order[0] == 0);
        for (size_t i = 0; i < order.size(); ++i) {
            REQUIRE(results[i] == 0);
        }
    };
    condy::sync_wait(f());
}

TEST_CASE("test senders - ranged empty parallel any") {
    auto f = []() -> condy::Coro<void> {
        using Op = decltype(condy::detail::make_op_awaiter(io_uring_prep_nop));
        std::vector<Op> senders;
        auto [order, r] =
            co_await (condy::parallel<condy::RangedParallelAnySender>(
                std::move(senders)));
        REQUIRE(order.empty());
        REQUIRE(r.empty());
    };
    condy::sync_wait(f());
}

TEST_CASE("test senders - empty parallel any") {
    auto f = []() -> condy::Coro<void> {
        [[maybe_unused]] auto [order, r] =
            co_await (condy::parallel<condy::ParallelAnySender>());
    };
    condy::sync_wait(f());
}

TEST_CASE("test senders - ||") {
    using condy::operators::operator||;
    auto f = []() -> condy::Coro<void> {
        __kernel_timespec ts{
            .tv_sec = 60,
            .tv_nsec = 0,
        };
        auto r = co_await (
            condy::detail::make_op_awaiter(io_uring_prep_nop) ||
            condy::detail::make_op_awaiter(io_uring_prep_timeout, &ts, 0, 0) ||
            condy::detail::make_op_awaiter(io_uring_prep_nop));
        REQUIRE(r.index() == 0);
        REQUIRE(std::get<0>(r) == 0);
    };
    condy::sync_wait(f());
}

TEST_CASE("test senders - link") {
    auto f = []() -> condy::Coro<void> {
        auto [r1, r2] = co_await (
            condy::link(condy::detail::make_op_awaiter(io_uring_prep_nop),
                        condy::detail::make_op_awaiter(io_uring_prep_nop)));
        REQUIRE(r1 == 0);
        REQUIRE(r2 == 0);
    };
    condy::sync_wait(f());
}

TEST_CASE("test senders - ranged link") {
    auto f = []() -> condy::Coro<void> {
        using Op = decltype(condy::detail::make_op_awaiter(io_uring_prep_nop));
        std::vector<Op> senders;
        senders.reserve(5);
        for (int i = 0; i < 5; ++i) {
            senders.push_back(
                condy::detail::make_op_awaiter(io_uring_prep_nop));
        }
        auto results = co_await (condy::link(std::move(senders)));
        for (auto r : results) {
            REQUIRE(r == 0);
        }
    };
    condy::sync_wait(f());
}

TEST_CASE("test senders - >>") {
    using condy::operators::operator>>;
    auto f = []() -> condy::Coro<void> {
        auto [r1, r2, r3] =
            co_await (condy::detail::make_op_awaiter(io_uring_prep_nop) >>
                      condy::detail::make_op_awaiter(io_uring_prep_nop) >>
                      condy::detail::make_op_awaiter(io_uring_prep_nop));
        REQUIRE(r1 == 0);
        REQUIRE(r2 == 0);
        REQUIRE(r3 == 0);
    };
    condy::sync_wait(f());
}

TEST_CASE("test senders - hard_link") {
    auto f = []() -> condy::Coro<void> {
        auto [r1, r2] = co_await (condy::hard_link(
            condy::detail::make_op_awaiter(io_uring_prep_nop),
            condy::detail::make_op_awaiter(io_uring_prep_nop)));
        REQUIRE(r1 == 0);
        REQUIRE(r2 == 0);
    };
    condy::sync_wait(f());
}

TEST_CASE("test senders - ranged hard_link") {
    auto f = []() -> condy::Coro<void> {
        using Op = decltype(condy::detail::make_op_awaiter(io_uring_prep_nop));
        std::vector<Op> senders;
        senders.reserve(5);
        for (int i = 0; i < 5; ++i) {
            senders.push_back(
                condy::detail::make_op_awaiter(io_uring_prep_nop));
        }
        auto results = co_await (condy::hard_link(std::move(senders)));
        for (auto r : results) {
            REQUIRE(r == 0);
        }
    };
    condy::sync_wait(f());
}

TEST_CASE("test senders - flags") {
    auto f = []() -> condy::Coro<void> {
        auto r = co_await (condy::always_async(
            condy::detail::make_op_awaiter(io_uring_prep_nop)));
        REQUIRE(r == 0);
    };
    condy::sync_wait(f());
}

TEST_CASE("test senders - channel basic") {
    condy::Runtime runtime;
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
            auto [r, item] = co_await channel.pop();
            REQUIRE(r == 0);
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

    runtime.allow_exit();
    runtime.run();

    for (auto &task : producer_tasks) {
        task.wait();
    }
    for (auto &task : consumer_tasks) {
        task.wait();
    }

    REQUIRE(finished == (num_producers + num_consumers));
}

TEST_CASE("test channel - channel cancel pop") {
    using condy::operators::operator||;

    condy::Runtime runtime;
    condy::Channel<int> ch1(1), ch2(1);

    std::atomic_bool finished = false;

    auto func = [&]() -> condy::Coro<void> {
        __kernel_timespec ts{
            .tv_sec = 60ll * 60ll,
            .tv_nsec = 0,
        };
        auto r = co_await (
            ch1.pop() || ch2.pop() ||
            condy::detail::make_op_awaiter(io_uring_prep_timeout, &ts, 0, 0));
        REQUIRE(r.index() == 1);
        REQUIRE(std::get<1>(r).first == 0);
        REQUIRE(std::get<1>(r).second == 42);
        finished = true;
    };

    condy::co_spawn(runtime, func()).detach();

    std::thread t([&]() {
        runtime.allow_exit();
        runtime.run();
    });

    REQUIRE(!finished);
    REQUIRE(ch2.try_push(42) == 0);

    t.join();
    REQUIRE(finished);
}
