#include <condy.hpp>
#include <iostream>
#include <memory_resource>
#include <vector>

condy::pmr::Coro<void> task_func(auto &) { co_return; }

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
    double tasks_per_second =
        static_cast<double>(task_count) / duration.count();
    std::cout << "Spawned and completed " << task_count << " tasks in "
              << duration.count() << " seconds (" << tasks_per_second
              << " tasks/second)\n";
}

int main() noexcept(false) {
    std::printf("This example is faster than benchmark spawn_tasks.cpp!!!\n");

    const size_t task_count = 50'000'000;

    std::pmr::monotonic_buffer_resource pool(1024l * 1024 * 100); // 100 MB pool
    std::pmr::polymorphic_allocator<std::byte> allocator(&pool);

    condy::sync_wait(spawn_tasks(allocator, task_count));

    return 0;
}