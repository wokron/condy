#include <chrono>
#include <condy.hpp>
#include <cstddef>
#include <cstdio>
#include <iostream>

condy::Coro<void> producer_task(condy::Channel<int> &channel,
                                size_t message_count) {
    for (size_t i = 0; i < message_count; ++i) {
        co_await channel.push(static_cast<int>(i));
    }
    co_return;
}

condy::Coro<void> consumer_task(condy::Channel<int> &channel,
                                size_t message_count) {
    for (size_t i = 0; i < message_count; ++i) {
        auto value = co_await channel.pop();
        // 简单验证
        if (value != static_cast<int>(i)) {
            std::cerr << "Data corruption detected!" << std::endl;
        }
    }
    co_return;
}

condy::Coro<void> spsc_benchmark(size_t pair_count, size_t message_count,
                                 size_t buffer_size) {
    std::vector<std::unique_ptr<condy::Channel<int>>> channels;
    for (size_t i = 0; i < pair_count; ++i) {
        channels.push_back(std::make_unique<condy::Channel<int>>(buffer_size));
    }

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<condy::Task<void>> tasks;
    for (size_t i = 0; i < pair_count; ++i) {
        tasks.emplace_back(
            condy::co_spawn(producer_task(*channels[i], message_count)));
        tasks.emplace_back(
            condy::co_spawn(consumer_task(*channels[i], message_count)));
    }

    for (auto &task : tasks) {
        co_await std::move(task);
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = end - start;

    double throughput = message_count * pair_count / duration.count(); // ops/s
    double bandwidth = throughput * sizeof(int) / (1024 * 1024);       // MB/s
    std::printf("Total time: %.4f seconds\n", duration.count());
    std::printf("Throughput: %.2f M msg/s, Bandwidth: %.2f MB/s\n",
                throughput / 1'000'000, bandwidth);

    co_return;
}

int main() {
    const size_t pair_count = 512;
    const size_t message_count = 50'000;
    const size_t buffer_size = 1024;

    std::printf(
        "Channel benchmark, num channels: %zu, messages per channel: %zu, "
        "buffer size: %zu\n",
        pair_count, message_count, buffer_size);

    condy::sync_wait(spsc_benchmark(pair_count, message_count, buffer_size));

    return 0;
}