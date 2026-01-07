#include <condy.hpp>
#include <iostream>
#include <vector>

condy::Coro<void> task_func() { co_return; }

condy::Coro<void> spawn_tasks(size_t task_count) {
    std::vector<condy::Task<void>> tasks;
    tasks.reserve(task_count);

    auto start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < task_count; ++i) {
        tasks.emplace_back(condy::co_spawn(task_func()));
    }

    for (auto &task : tasks) {
        co_await std::move(task);
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = end - start;
    double tasks_per_second =
        static_cast<double>(task_count) / duration.count();
    std::cout << "Spawned and completed " << task_count << " tasks in "
              << duration.count() << " seconds (" << tasks_per_second
              << " tasks/second)\n";
}

int main() noexcept(false) {
    const size_t task_count = 50'000'000;

    condy::sync_wait(spawn_tasks(task_count));

    return 0;
}