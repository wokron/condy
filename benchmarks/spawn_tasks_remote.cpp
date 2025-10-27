#include "condy/coro.hpp"
#include "condy/event_loop.hpp"
#include "condy/strategies.hpp"
#include <condy.hpp>
#include <iostream>
#include <thread>
#include <vector>

static int counter = 0;
static int counter2 = 0;

condy::Coro<void> task_func() {
    counter++;
    co_return;
}

condy::Coro<void> spawn_tasks(condy::IEventLoop &loop, size_t task_count) {
    std::vector<condy::Task<void>> tasks;
    tasks.reserve(task_count);

    auto start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < task_count; ++i) {
        tasks.emplace_back(co_await condy::co_spawn(loop, task_func()));
    }

    for (auto &task : tasks) {
        co_await std::move(task);
        counter2++;
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = end - start;
    double tasks_per_second = task_count / duration.count();
    std::cout << "Spawned and completed " << task_count << " tasks in "
              << duration.count() << " seconds (" << tasks_per_second
              << " tasks/second)" << std::endl;
}

class NoStopStrategy : public condy::SimpleStrategy {
public:
    using SimpleStrategy::SimpleStrategy;

    bool should_stop() const override {
        return false; // Never stop
    }
};

int main() {
    const size_t task_count = 1'000'000;

    condy::EventLoop<condy::SimpleStrategy> loop(512);
    condy::EventLoop<NoStopStrategy> loop_remote(8);
    std::thread remote_thread([&]() { loop_remote.run(); });

    loop.run(spawn_tasks(loop_remote, task_count));

    loop_remote.stop();
    remote_thread.join();

    return 0;
}