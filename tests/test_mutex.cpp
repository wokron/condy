// #include "condy/async_operations.hpp"
// #include "condy/event_loop.hpp"
// #include "condy/mutex.hpp"
// #include "condy/strategies.hpp"
// #include "condy/task.hpp"
// #include <doctest/doctest.h>
// #include <thread>

// namespace {

// class NoStopStrategy : public condy::SimpleStrategy {
// public:
//     using Base = condy::SimpleStrategy;
//     using Base::Base;

//     bool should_stop() const override { return false; }
// };

// } // namespace

// TEST_CASE("test mutex - lock and unlock") {
//     condy::EventLoop<condy::SimpleStrategy> loop1(8);
//     condy::EventLoop<NoStopStrategy> loop2(8);

//     std::thread loop2_thread([&]() { loop2.run(); });

//     const int times = 10000;
//     condy::Mutex mutex;
//     int count = 0;
//     bool all_pass = true;
//     auto func = [&]() -> condy::Coro<void> {
//         for (int i = 0; i < times; i++) {
//             if (i % 10 == 0) {
//                 co_await mutex.lock();
//                 all_pass &= (count % 10 == 0);
//             }

//             count++;

//             if (i % 10 == 9) {
//                 mutex.unlock();
//                 // TODO: need to optimize this
//                 __kernel_timespec ts{
//                     .tv_sec = 0,
//                     .tv_nsec = 1000,
//                 };
//                 co_await condy::async_timeout(&ts, 0, 0);
//             }
//         }
//     };

//     auto main = [&]() -> condy::Coro<void> {
//         auto t = co_await condy::co_spawn(loop2, func());
//         co_await func();
//         co_await std::move(t);
//     };

//     loop1.run(main());

//     loop2.stop();
//     loop2_thread.join();

//     REQUIRE(all_pass);
//     REQUIRE(count == 2 * times);
// }

// TEST_CASE("test mutex - lock_guard") {
//     condy::EventLoop<condy::SimpleStrategy> loop1(8);
//     condy::EventLoop<NoStopStrategy> loop2(8);

//     std::thread loop2_thread([&]() { loop2.run(); });

//     const int times = 1000;
//     condy::Mutex mutex;
//     int count = 0;
//     bool all_pass = true;
//     auto func = [&]() -> condy::Coro<void> {
//         for (int i = 0; i < times; i++) {

//             {
//                 auto guard = co_await mutex.lock_guard();
//                 all_pass &= (count % 10 == 0);
//                 for (int j = 0; j < 10; j++) {
//                     count++;
//                 }
//             }

//             // TODO: need to optimize this
//             __kernel_timespec ts{
//                 .tv_sec = 0,
//                 .tv_nsec = 1000,
//             };
//             co_await condy::async_timeout(&ts, 0, 0);
//         }
//     };

//     auto main = [&]() -> condy::Coro<void> {
//         auto t = co_await condy::co_spawn(loop2, func());
//         co_await func();
//         co_await std::move(t);
//     };

//     loop1.run(main());

//     loop2.stop();
//     loop2_thread.join();

//     REQUIRE(all_pass);
//     REQUIRE(count == 2 * times * 10);
// }