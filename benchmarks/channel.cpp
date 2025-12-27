#include <chrono>
#include <condy.hpp>
#include <cstddef>
#include <cstdio>
#include <iostream>

condy::Coro<void> producer_task(condy::Channel<int> &channel,
                                size_t num_messages) {
    for (size_t i = 0; i < num_messages; ++i) {
        co_await channel.push(static_cast<int>(i));
    }
    co_return;
}

condy::Coro<void> consumer_task(condy::Channel<int> &channel,
                                size_t num_messages) {
    for (size_t i = 0; i < num_messages; ++i) {
        auto value = co_await channel.pop();
        if (value != static_cast<int>(i)) {
            std::cerr << "Data corruption detected!\n";
        }
    }
    co_return;
}

condy::Coro<void>
launch_producers(std::vector<std::unique_ptr<condy::Channel<int>>> &channels,
                 size_t num_messages) {
    std::vector<condy::Task<void>> tasks;
    tasks.reserve(channels.size());
    for (auto &channel : channels) {
        tasks.emplace_back(
            condy::co_spawn(producer_task(*channel, num_messages)));
    }
    for (auto &task : tasks) {
        co_await std::move(task);
    }
    co_return;
}

condy::Coro<void>
launch_consumers(std::vector<std::unique_ptr<condy::Channel<int>>> &channels,
                 size_t num_messages) {
    size_t num_pairs = channels.size();
    std::vector<condy::Task<void>> tasks;
    auto start = std::chrono::high_resolution_clock::now();
    tasks.reserve(channels.size());
    for (auto &channel : channels) {
        tasks.emplace_back(
            condy::co_spawn(consumer_task(*channel, num_messages)));
    }
    for (auto &task : tasks) {
        co_await std::move(task);
    }
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = end - start;
    double throughput = num_messages * num_pairs / duration.count(); // ops/s
    std::printf("Total time: %.4f seconds\n", duration.count());
    std::printf("Throughput: %.2f M msg/s\n", throughput / 1'000'000);
    co_return;
}

int main() {
    const size_t num_pairs = 8;
    const size_t num_messages = 1'600'000;
    const size_t buffer_size = 1024;

    std::vector<std::unique_ptr<condy::Channel<int>>> channels;
    channels.reserve(num_pairs);
    for (size_t i = 0; i < num_pairs; ++i) {
        channels.push_back(std::make_unique<condy::Channel<int>>(buffer_size));
    }

    condy::Runtime runtime1, runtime2;
    std::thread rt1([&]() {
        condy::sync_wait(runtime1, launch_producers(channels, num_messages));
    });

    condy::sync_wait(runtime2, launch_consumers(channels, num_messages));

    rt1.join();

    return 0;
}