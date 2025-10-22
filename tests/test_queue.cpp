#include <algorithm>
#include <condy/queue.hpp>
#include <doctest/doctest.h>
#include <thread>
#include <vector>

TEST_CASE("test queue - test RingQueue single-threaded") {
    condy::RingQueue<int> queue(4);

    REQUIRE(queue.try_enqueue(1));
    REQUIRE(queue.try_enqueue(2));
    REQUIRE(queue.try_enqueue(3));

    // Queue should be full now
    REQUIRE(!queue.try_enqueue(4));

    auto item = queue.try_dequeue();
    REQUIRE(item.has_value());
    REQUIRE(item.value() == 1);

    item = queue.try_dequeue();
    REQUIRE(item.has_value());
    REQUIRE(item.value() == 2);

    // Test wrap-around
    REQUIRE(queue.try_enqueue(4));
    REQUIRE(queue.try_enqueue(5));

    item = queue.try_dequeue();
    REQUIRE(item.has_value());
    REQUIRE(item.value() == 3);

    item = queue.try_dequeue();
    REQUIRE(item.has_value());
    REQUIRE(item.value() == 4);

    item = queue.try_dequeue();
    REQUIRE(item.has_value());
    REQUIRE(item.value() == 5);

    // Queue should be empty now
    item = queue.try_dequeue();
    REQUIRE(!item.has_value());

    REQUIRE(queue.size_unreliable() == 0);
}

TEST_CASE("test queue - test RingQueue double-threaded") {
    condy::RingQueue<int> queue(512);

    const size_t num_items = 100'000;

    auto producer = [&queue, num_items]() {
        for (size_t i = 0; i < num_items; ++i) {
            while (!queue.try_enqueue(static_cast<int>(i)))
                ; // Busy wait
        }
    };

    auto consumer = [&queue, num_items]() {
        bool pass_check = true;
        size_t count = 0;
        while (count < num_items) {
            auto item = queue.try_dequeue();
            if (item.has_value()) {
                pass_check &= (item.value() == static_cast<int>(count));
                count++;
            }
        }
        REQUIRE(pass_check); // Avoid too many assertions in the loop
    };

    std::thread prod_thread(producer);
    std::thread cons_thread(consumer);

    prod_thread.join();
    cons_thread.join();

    REQUIRE(queue.size_unreliable() == 0);
}

TEST_CASE("test queue - test MultiWriterRingQueue") {
    condy::MultiWriterRingQueue<int> queue(512);

    const size_t num_items = 10'000;
    const size_t num_producers = 4;

    auto producer = [&queue, num_items](size_t id) {
        for (size_t i = 0; i < num_items; ++i) {
            while (!queue.try_enqueue(static_cast<int>(id * num_items + i)))
                ; // Busy wait
        }
    };

    std::vector<std::thread> producers;
    for (size_t i = 0; i < num_producers; ++i) {
        producers.emplace_back(producer, i);
    }

    auto consumer = [&queue, num_items, num_producers]() {
        std::vector<bool> seen(num_items * num_producers, false);
        size_t count = 0;
        while (count < num_items * num_producers) {
            auto item = queue.try_dequeue();
            if (item.has_value()) {
                seen[item.value()] = true;
                count++;
            }
        }

        REQUIRE(
            std::all_of(seen.begin(), seen.end(), [](bool v) { return v; }));
    };

    std::thread cons_thread(consumer);

    for (auto &t : producers) {
        t.join();
    }
    cons_thread.join();

    REQUIRE(queue.size_unreliable() == 0);
}