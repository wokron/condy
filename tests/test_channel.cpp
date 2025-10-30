// #include "condy/channel.hpp"
// #include "condy/event_loop.hpp"
// #include "condy/strategies.hpp"
// #include "condy/task.hpp"
// #include <doctest/doctest.h>
// #include <thread>

// TEST_CASE("test channel - single thread") {
//     condy::EventLoop<condy::SimpleStrategy> loop(8);

//     condy::Channel<int> channel(2);

//     const size_t times = 1000;

//     auto producer = [&channel, times]() -> condy::Coro<void> {
//         auto size = co_await channel.size();
//         CHECK(size == 0);
//         for (size_t i = 0; i < times; ++i) {
//             co_await channel.send(static_cast<int>(i));
//         }
//     };

//     auto consumer = [&channel, times]() -> condy::Coro<void> {
//         for (size_t i = 0; i < times; ++i) {
//             int value = co_await channel.receive();
//             CHECK(value == static_cast<int>(i));
//         }
//         auto size = co_await channel.size();
//         CHECK(size == 0);
//     };

//     loop.run(consumer(), producer());
// }

// TEST_CASE("test channel - single thread multiple channels") {
//     condy::EventLoop<condy::SimpleStrategy> loop(8);

//     std::vector<std::unique_ptr<condy::Channel<int>>> channels;
//     const size_t channel_count = 5;
//     for (size_t i = 0; i < channel_count; ++i) {
//         channels.push_back(std::make_unique<condy::Channel<int>>(2));
//     }

//     const size_t times = 200;

//     auto producer = [&channels, times](int no) -> condy::Coro<void> {
//         auto size = co_await channels[no]->size();
//         CHECK(size == 0);
//         for (size_t i = 0; i < times; ++i) {
//             co_await channels[no]->send(static_cast<int>(i));
//         }
//     };

//     auto consumer = [&channels, times](int no) -> condy::Coro<void> {
//         for (size_t i = 0; i < times; ++i) {
//             int value = co_await channels[no]->receive();
//             CHECK(value == static_cast<int>(i));
//         }
//         auto size = co_await channels[no]->size();
//         CHECK(size == 0);
//     };

//     auto launch_all = [&]() -> condy::Coro<void> {
//         for (size_t i = 0; i < channel_count; ++i) {
//             condy::co_spawn(consumer(static_cast<int>(i))).detach();
//             condy::co_spawn(producer(static_cast<int>(i))).detach();
//         }
//         co_return;
//     };

//     loop.run(launch_all());
// }

// TEST_CASE("test channel - multi thread") {
//     condy::EventLoop<condy::SimpleStrategy> loop1(4), loop2(4);

//     condy::Channel<int> channel(5);

//     const size_t times = 1000;

//     auto producer = [&channel, times]() -> condy::Coro<void> {
//         for (size_t i = 0; i < times; ++i) {
//             co_await channel.send(static_cast<int>(i));
//         }
//     };

//     auto consumer = [&channel, times]() -> condy::Coro<void> {
//         for (size_t i = 0; i < times; ++i) {
//             int value = co_await channel.receive();
//             CHECK(value == static_cast<int>(i));
//         }
//     };

//     std::thread t1([&]() { loop1.run(consumer()); });

//     loop2.run(producer());

//     t1.join();
// }

// TEST_CASE("test channel - multi thread multiple channels") {
//     condy::EventLoop<condy::SimpleStrategy> loop1(4), loop2(4);

//     std::vector<std::unique_ptr<condy::Channel<int>>> channels;
//     const size_t channel_count = 5;
//     for (size_t i = 0; i < channel_count; ++i) {
//         channels.push_back(std::make_unique<condy::Channel<int>>(5));
//     }

//     const size_t times = 200;

//     auto producer = [&channels, times](int no) -> condy::Coro<void> {
//         for (size_t i = 0; i < times; ++i) {
//             co_await channels[no]->send(static_cast<int>(i));
//         }
//     };

//     auto consumer = [&channels, times](int no) -> condy::Coro<void> {
//         for (size_t i = 0; i < times; ++i) {
//             int value = co_await channels[no]->receive();
//             CHECK(value == static_cast<int>(i));
//         }
//     };

//     auto launch_producer = [&]() -> condy::Coro<void> {
//         for (size_t i = 0; i < channel_count; ++i) {
//             condy::co_spawn(producer(static_cast<int>(i))).detach();
//         }
//         co_return;
//     };

//     auto launch_consumer = [&]() -> condy::Coro<void> {
//         for (size_t i = 0; i < channel_count; ++i) {
//             condy::co_spawn(consumer(static_cast<int>(i))).detach();
//         }
//         co_return;
//     };

//     std::thread t1([&]() { loop1.run(launch_consumer()); });

//     loop2.run(launch_producer());

//     t1.join();
// }