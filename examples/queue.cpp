/**
 * @file queue.cpp
 * @brief Similar to futex-semaphore.cpp but implements through
 * condy::AsyncFutex
 */

#include <atomic>
#include <cassert>
#include <condy.hpp>
#include <cstdio>
#include <queue>
#include <thread>
#include <unistd.h>

class FutexSemaphore {
public:
    FutexSemaphore(uint32_t initial_count = 0) : count_(initial_count) {}

    FutexSemaphore(const FutexSemaphore &) = delete;
    FutexSemaphore &operator=(const FutexSemaphore &) = delete;
    FutexSemaphore(FutexSemaphore &&) = delete;
    FutexSemaphore &operator=(FutexSemaphore &&) = delete;

public:
    condy::Coro<void> acquire() {
        uint32_t c;
        while (true) {
            size_t retries = 0;
            while (retries++ < MAX_RETRIES) {
                c = count_.load(std::memory_order_relaxed);
                if (c > 0 && count_.compare_exchange_weak(
                                 c, c - 1, std::memory_order_acquire,
                                 std::memory_order_relaxed)) {
                    co_return;
                }
            }
            co_await futex_.wait(c);
        }
    }

    void release(uint32_t n = 1) {
        count_.fetch_add(n, std::memory_order_release);
        for (uint32_t i = 0; i < n; ++i) {
            futex_.notify_one();
        }
    }

private:
    static constexpr size_t MAX_RETRIES = 32;

    std::atomic<uint32_t> count_;
    condy::AsyncFutex<uint32_t> futex_{count_};
};

class FutexMutex {
public:
    FutexMutex() = default;

    FutexMutex(const FutexMutex &) = delete;
    FutexMutex &operator=(const FutexMutex &) = delete;
    FutexMutex(FutexMutex &&) = delete;
    FutexMutex &operator=(FutexMutex &&) = delete;

public:
    auto lock() { return sem_.acquire(); }

    void unlock() { sem_.release(); }

private:
    FutexSemaphore sem_{1};
};

template <typename T> class Queue {
public:
    Queue(size_t queue_size) : empty_(queue_size), full_(0) {}

    condy::Coro<void> enqueue(const T &item) {
        co_await empty_.acquire();
        {
            co_await queue_mutex_.lock();
            queue_.push(item);
            queue_mutex_.unlock();
        }
        full_.release();
    }

    condy::Coro<T> dequeue() {
        co_await full_.acquire();
        T item;
        {
            co_await queue_mutex_.lock();
            item = queue_.front();
            queue_.pop();
            queue_mutex_.unlock();
        }
        empty_.release();
        co_return item;
    }

private:
    std::queue<T> queue_;
    FutexMutex queue_mutex_;
    FutexSemaphore empty_, full_;
};

condy::Coro<void> producer(Queue<int> &q, size_t produce_count) {
    for (size_t i = 0; i < produce_count; ++i) {
        co_await q.enqueue(static_cast<int>(i));
    }
}

condy::Coro<void> consumer(Queue<int> &q, int id, size_t consume_count) {
    int item;
    for (size_t i = 0; i < consume_count; ++i) {
        item = co_await q.dequeue();
        std::printf("Consumer %d consumed item %d\n", id, item);
    }
}

void usage(const char *prog_name) {
    std::printf("Usage: %s [-h] [-q queue_size] [-p num_producers] [-c "
                "num_consumers] [-n items_per_producer]\n",
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
        std::fprintf(stderr,
                     "Total items (%zu) must be divisible by number of "
                     "consumers (%zu)\n",
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
