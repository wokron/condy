/**
 * @file futex-semaphore.cpp
 * @brief Implementation of semaphore and mutex using futexes with Condy
 */

#include <atomic>
#include <cassert>
#include <condy.hpp>
#include <cstdio>
#include <linux/futex.h>
#include <queue>
#include <thread>
#include <unistd.h>

#if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6

class FutexSemaphore {
public:
    FutexSemaphore(uint32_t initial_count = 0) : count(initial_count) {}

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
                c = count.load(std::memory_order_relaxed);
                if (c > 0 && count.compare_exchange_weak(
                                 c, c - 1, std::memory_order_acquire,
                                 std::memory_order_relaxed)) {
                    co_return;
                }
            }
            [[maybe_unused]] int r = co_await condy::async_futex_wait(
                raw_count_ptr_(), c, FUTEX_BITSET_MATCH_ANY, FUTEX2_SIZE_U32,
                0);
            assert(r == 0 || r == -EAGAIN);
        }
    }

    condy::Coro<void> release(uint32_t n = 1) {
        count.fetch_add(n, std::memory_order_release);
        [[maybe_unused]] int r = co_await condy::async_futex_wake(
            raw_count_ptr_(), n, FUTEX_BITSET_MATCH_ANY, FUTEX2_SIZE_U32, 0);
        assert(r >= 0);
    }

private:
    uint32_t *raw_count_ptr_() { return reinterpret_cast<uint32_t *>(&count); }

private:
    static constexpr size_t MAX_RETRIES = 32;

    std::atomic<uint32_t> count;
};

class FutexMutex {
public:
    FutexMutex() = default;

    FutexMutex(const FutexMutex &) = delete;
    FutexMutex &operator=(const FutexMutex &) = delete;
    FutexMutex(FutexMutex &&) = delete;
    FutexMutex &operator=(FutexMutex &&) = delete;

public:
    auto lock() { return sem.acquire(); }

    auto unlock() { return sem.release(); }

private:
    FutexSemaphore sem{1};
};

struct State {
    State(size_t queue_size) : empty(queue_size), full(0) {}

    std::queue<int> queue;
    FutexMutex queue_mutex;
    FutexSemaphore empty, full;
};

condy::Coro<void> producer(State &share, [[maybe_unused]] int id,
                           size_t produce_count) {
    for (size_t i = 0; i < produce_count; ++i) {
        co_await share.empty.acquire();
        {
            co_await share.queue_mutex.lock();
            share.queue.push(static_cast<int>(i));
            co_await share.queue_mutex.unlock();
        }
        co_await share.full.release();
    }
}

condy::Coro<void> consumer(State &share, int id, size_t consume_count) {
    int item;
    for (size_t i = 0; i < consume_count; ++i) {
        co_await share.full.acquire();
        {
            co_await share.queue_mutex.lock();
            item = share.queue.front();
            share.queue.pop();
            co_await share.queue_mutex.unlock();
        }
        co_await share.empty.release();

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
    State share(queue_size);
    std::thread producer_thread([&]() {
        for (size_t i = 0; i < num_producers; ++i) {
            condy::co_spawn(
                rt1, producer(share, static_cast<int>(i), items_per_producer))
                .detach();
        }
        rt1.allow_exit();
        rt1.run();
    });

    std::thread consumer_thread([&]() {
        for (size_t i = 0; i < num_consumers; ++i) {
            condy::co_spawn(
                rt2, consumer(share, static_cast<int>(i), items_per_consumer))
                .detach();
        }
        rt2.allow_exit();
        rt2.run();
    });

    producer_thread.join();
    consumer_thread.join();
}

#else

int main() {
    std::fprintf(stderr,
                 "Futex-based semaphore and mutex require io_uring version "
                 "2.6 or higher.\n");
    return 1;
}

#endif