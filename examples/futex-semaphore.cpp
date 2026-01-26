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

long futex_wait(void *uaddr, unsigned long val, unsigned long mask,
                unsigned int flags, const struct timespec *timeout,
                int clockid) {
    return syscall(SYS_futex_wait, uaddr, val, mask, flags, timeout, clockid);
}

long futex_wake(void *uaddr, unsigned long mask, int nr, unsigned int flags) {
    return syscall(SYS_futex_wake, uaddr, mask, nr, flags);
}

class FutexSemaphore {
public:
    FutexSemaphore(uint32_t initial_count = 0) : count(initial_count) {}

    FutexSemaphore(const FutexSemaphore &) = delete;
    FutexSemaphore &operator=(const FutexSemaphore &) = delete;
    FutexSemaphore(FutexSemaphore &&) = delete;
    FutexSemaphore &operator=(FutexSemaphore &&) = delete;

public:
    condy::Coro<void> async_acquire() {
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

    void acquire() {
        uint32_t c;
        while (true) {
            size_t retries = 0;
            while (retries++ < MAX_RETRIES) {
                c = count.load(std::memory_order_relaxed);
                if (c > 0 && count.compare_exchange_weak(
                                 c, c - 1, std::memory_order_acquire,
                                 std::memory_order_relaxed)) {
                    return;
                }
            }
            futex_wait(raw_count_ptr_(), c, FUTEX_BITSET_MATCH_ANY,
                       FUTEX2_SIZE_U32, nullptr, 0);
        }
    }

    condy::Coro<void> async_release(uint32_t n = 1) {
        count.fetch_add(n, std::memory_order_release);
        [[maybe_unused]] int r = co_await condy::async_futex_wake(
            raw_count_ptr_(), n, FUTEX_BITSET_MATCH_ANY, FUTEX2_SIZE_U32, 0);
        assert(r >= 0);
    }

    void release(uint32_t n = 1) {
        count.fetch_add(n, std::memory_order_release);
        [[maybe_unused]] long r =
            futex_wake(raw_count_ptr_(), FUTEX_BITSET_MATCH_ANY,
                       static_cast<int>(n), FUTEX2_SIZE_U32);
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
    auto async_lock() { return sem.async_acquire(); }

    auto async_unlock() { return sem.async_release(); }

    void lock() { sem.acquire(); }

    void unlock() { sem.release(); }

private:
    FutexSemaphore sem{1};
};

struct State {
    State(size_t queue_size) : empty(queue_size), full(0) {}

    std::queue<int> queue;
    FutexMutex queue_mutex;
    FutexSemaphore empty, full;
};

void producer(State &share, [[maybe_unused]] int id, size_t produce_count) {
    for (size_t i = 0; i < produce_count; ++i) {
        share.empty.acquire();
        {
            share.queue_mutex.lock();
            share.queue.push(static_cast<int>(i));
            share.queue_mutex.unlock();
        }
        share.full.release();
    }
}

condy::Coro<void> async_consumer(State &share, int id, size_t consume_count) {
    int item;
    for (size_t i = 0; i < consume_count; ++i) {
        co_await share.full.async_acquire();
        {
            co_await share.queue_mutex.async_lock();
            item = share.queue.front();
            share.queue.pop();
            co_await share.queue_mutex.async_unlock();
        }
        co_await share.empty.async_release();

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

    std::vector<std::thread> producers;
    producers.reserve(num_producers);
    for (size_t i = 0; i < num_producers; ++i) {
        producers.emplace_back(producer, std::ref(share), static_cast<int>(i),
                               items_per_producer);
    }

    std::thread consumer_thread([&]() {
        for (size_t i = 0; i < num_consumers; ++i) {
            condy::co_spawn(rt2, async_consumer(share, static_cast<int>(i),
                                                items_per_consumer))
                .detach();
        }
        rt2.allow_exit();
        rt2.run();
    });

    for (auto &producer_thread : producers) {
        producer_thread.join();
    }
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