#include <condy.hpp>
#include <cstddef>
#include <cstdio>
#include <variant>

using CancelChannel = condy::Channel<std::monostate>;

condy::Coro<int> request(CancelChannel &ch) {
    using condy::operators::operator||;
    static size_t counter = 0;

    std::variant<int, std::monostate> result;
    if (counter++ == 1) {
        result = co_await (condy::async_nop() || ch.pop());
    } else {
        __kernel_timespec ts{.tv_sec = 60 * 60, .tv_nsec = 0};
        result = co_await (condy::async_timeout(&ts, 0, 0) || ch.pop());
    }

    if (result.index() == 0) {
        co_return 0;
    } else {
        // Cancelled
        co_return -1;
    }
}

condy::Coro<bool> request_task(size_t num_tasks, CancelChannel &ch) {
    int r = co_await request(ch);
    if (r < 0) {
        co_return false;
    }
    // Cancel other tasks
    for (size_t i = 1; i < num_tasks; i++) {
        co_await ch.push(std::monostate{});
    }
    co_return true;
}

condy::Coro<int> co_main() {
    const size_t num_tasks = 5;
    CancelChannel ch(num_tasks);

    std::vector<condy::Task<bool>> tasks;
    for (size_t i = 0; i < num_tasks; i++) {
        tasks.emplace_back(condy::co_spawn(request_task(num_tasks, ch)));
    }

    // Wait for all tasks to complete
    for (size_t i = 0; i < num_tasks; i++) {
        auto &task = tasks[i];
        bool success = co_await std::move(task);
        if (success) {
            std::printf("Task %zu completed successfully.\n", i);
        } else {
            std::printf("Task %zu cancelled.\n", i);
        }
    }

    co_return 0;
}

int main() { return condy::sync_wait(co_main()); }