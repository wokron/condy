/**
 * @file queue-condy-futex.cpp
 * @brief Producer-consumer queue example using condy::Futex
 * @details Synchronization style: mutex + condition variable
 */

#include <atomic>
#include <cassert>
#include <condy.hpp>
#include <cstdio>
#include <format>
#include <iostream>
#include <queue>
#include <thread>
#include <unistd.h>

class FutexMutex {
public:
    FutexMutex() : state_(false) {}

    FutexMutex(const FutexMutex &) = delete;
    FutexMutex &operator=(const FutexMutex &) = delete;
    FutexMutex(FutexMutex &&) = delete;
    FutexMutex &operator=(FutexMutex &&) = delete;

public:
    condy::Coro<void> lock() {
        bool expected = false;
        while (!state_.compare_exchange_weak(expected, true,
                                             std::memory_order_acquire,
                                             std::memory_order_relaxed)) {
            expected = false;
            co_await futex_.wait(true);
        }
    }

    void unlock() {
        state_.store(false, std::memory_order_release);
        futex_.notify_one();
    }

private:
    std::atomic<bool> state_;
    condy::Futex<bool> futex_{state_};
};

class FutexConditionVariable {
public:
    FutexConditionVariable() : generation_(0) {}

    FutexConditionVariable(const FutexConditionVariable &) = delete;
    FutexConditionVariable &operator=(const FutexConditionVariable &) = delete;
    FutexConditionVariable(FutexConditionVariable &&) = delete;
    FutexConditionVariable &operator=(FutexConditionVariable &&) = delete;

public:
    condy::Coro<void> wait(FutexMutex &mutex) {
        uint32_t gen = generation_.load(std::memory_order_acquire);
        mutex.unlock();
        while (generation_.load(std::memory_order_acquire) == gen) {
            co_await futex_.wait(gen);
        }
        co_await mutex.lock();
    }

    void notify_one() {
        generation_.fetch_add(1, std::memory_order_release);
        futex_.notify_one();
    }

    void notify_all() {
        generation_.fetch_add(1, std::memory_order_release);
        futex_.notify_all();
    }

private:
    std::atomic<uint32_t> generation_;
    condy::Futex<uint32_t> futex_{generation_};
};

template <typename T> class Queue {
public:
    Queue(size_t queue_size) : capacity_(queue_size) {}

    Queue(const Queue &) = delete;
    Queue &operator=(const Queue &) = delete;
    Queue(Queue &&) = delete;
    Queue &operator=(Queue &&) = delete;

public:
    condy::Coro<void> enqueue(const T &item) {
        co_await queue_mutex_.lock();
        while (queue_.size() >= capacity_) {
            co_await not_full_.wait(queue_mutex_);
        }
        queue_.push(item);
        queue_mutex_.unlock();
        not_empty_.notify_one();
    }

    condy::Coro<T> dequeue() {
        co_await queue_mutex_.lock();
        while (queue_.empty()) {
            co_await not_empty_.wait(queue_mutex_);
        }
        T item = queue_.front();
        queue_.pop();
        queue_mutex_.unlock();
        not_full_.notify_one();
        co_return item;
    }

private:
    std::queue<T> queue_;
    size_t capacity_;
    FutexMutex queue_mutex_;
    FutexConditionVariable not_empty_, not_full_;
};

condy::Coro<void> producer(Queue<int> &q, size_t produce_count) {
    for (size_t i = 0; i < produce_count; ++i) {
        co_await q.enqueue(static_cast<int>(i));
        // Yield to improve fairness
        co_await condy::co_switch(condy::current_runtime());
    }
}

condy::Coro<void> consumer(Queue<int> &q, int id, size_t consume_count) {
    int item;
    for (size_t i = 0; i < consume_count; ++i) {
        item = co_await q.dequeue();
        std::cout << std::format("Consumer {} consumed item {}\n", id, item);
        // Yield to improve fairness
        co_await condy::co_switch(condy::current_runtime());
    }
}

void usage(const char *prog_name) {
    std::cerr << std::format(
        "Usage: {} [-h] [-q queue_size] [-p num_producers] [-c num_consumers] "
        "[-n items_per_producer]\n",
        prog_name);
}

static size_t queue_size = 32;
static size_t num_producers = 8;
static size_t num_consumers = 8;
static size_t items_per_producer = 32;

int main(int argc, char *argv[]) noexcept(false) {
    int opt;
    while ((opt = getopt(argc, argv, "hq:p:c:n:")) != -1) {
        switch (opt) {
        case 'q':
            queue_size = std::stoul(optarg);
            break;
        case 'p':
            num_producers = std::stoul(optarg);
            break;
        case 'c':
            num_consumers = std::stoul(optarg);
            break;
        case 'n':
            items_per_producer = std::stoul(optarg);
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    size_t total_items = num_producers * items_per_producer;
    if (total_items % num_consumers != 0) {
        std::cerr << std::format(
            "Total items ({}) must be divisible by number of consumers ({})\n",
            total_items, num_consumers);
        return 1;
    }
    size_t items_per_consumer = total_items / num_consumers;

    condy::Runtime rt1, rt2;
    Queue<int> queue(queue_size);

    std::thread producer_thread([&]() {
        for (size_t i = 0; i < num_producers; ++i) {
            condy::co_spawn(rt1, producer(queue, items_per_producer)).detach();
        }
        rt1.allow_exit();
        rt1.run();
    });

    std::thread consumer_thread([&]() {
        for (size_t i = 0; i < num_consumers; ++i) {
            condy::co_spawn(
                rt2, consumer(queue, static_cast<int>(i), items_per_consumer))
                .detach();
        }
        rt2.allow_exit();
        rt2.run();
    });

    producer_thread.join();
    consumer_thread.join();
}
