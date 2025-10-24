#include "condy/event_loop.hpp"
#include "condy/strategies.hpp"
#include <condy/retry.hpp>
#include <doctest/doctest.h>
#include <thread>

namespace {} // namespace

TEST_CASE("test retry") {
    condy::EventLoop<condy::SimpleStrategy> loop1(8), loop2(8);

    std::atomic_int counter = 0;
    auto func = [&](int start) -> condy::Coro<void> {
        for (int i = start; i < 10; i += 2) {
            co_await condy::retry([&] { return counter.load() == i * 100; });
            for (int j = 0; j < 100; j++) {
                counter.fetch_add(1);
            }
        }
    };

    std::thread loop1_thread([&]() { loop1.run(func(0)); });

    std::thread loop2_thread([&]() { loop2.run(func(1)); });

    loop1_thread.join();
    loop2_thread.join();

    REQUIRE(counter.load() == 1000);
}