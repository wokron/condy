#include <condy.hpp>
#include <iostream>
#include <memory_resource>
#include <vector>

condy::pmr::Coro<void> task_func(auto &alloc) { co_return; }

condy::pmr::Coro<void> spawn_tasks(auto &alloc, size_t task_count) {
    std::vector<condy::pmr::Task<void>> tasks;
    tasks.reserve(task_count);

    auto start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < task_count; ++i) {
        tasks.emplace_back(condy::co_spawn(task_func(alloc)));
    }

    for (auto &task : tasks) {
        co_await std::move(task);
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = end - start;
    double tasks_per_second = task_count / duration.count();
    std::cout << "Spawned and completed " << task_count << " tasks in "
              << duration.count() << " seconds (" << tasks_per_second
              << " tasks/second)" << std::endl;
}

int main() {
    const size_t task_count = 1'000'000;

    std::pmr::monotonic_buffer_resource pool;
    std::pmr::polymorphic_allocator<std::byte> allocator(&pool);

    condy::SingleThreadRuntime runtime;
    condy::sync_wait(runtime, spawn_tasks(allocator, task_count));

    return 0;
}