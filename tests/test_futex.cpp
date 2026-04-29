#include "condy/async_operations.hpp"
#include "condy/coro.hpp"
#include "condy/futex.hpp"
#include "condy/sync_wait.hpp"
#include "condy/task.hpp"
#include <algorithm>
#include <atomic>
#include <doctest/doctest.h>
#include <queue>
#include <thread>
#include <vector>

TEST_CASE("test async_futex - basic wait and notify") {
    std::atomic<int> atomic_counter = 0;
    condy::Futex<int> futex(atomic_counter);

    bool finished = false;

    auto wake_func = [&]() -> condy::Coro<void> {
        REQUIRE(!finished);
        REQUIRE(atomic_counter.load() == 0);
        atomic_counter++;
        futex.notify_one();
        co_return;
    };

    auto wait_func = [&]() -> condy::Coro<void> {
        auto t = condy::co_spawn(wake_func());
        co_await futex.wait(0);
        REQUIRE(atomic_counter.load() == 1);
        finished = true;
        co_await t;
    };

    condy::sync_wait(wait_func());
    REQUIRE(finished);
}

TEST_CASE("test async_futex - notify all") {
    std::atomic<int> atomic_counter = 0;
    condy::Futex<int> futex(atomic_counter);

    std::vector<int> finished(10, false);

    auto wake_func = [&]() -> condy::Coro<void> {
        REQUIRE(atomic_counter.load() == 0);
        REQUIRE(std::all_of(finished.begin(), finished.end(),
                            [](int f) { return !f; }));
        atomic_counter++;
        futex.notify_all();
        co_return;
    };

    auto wait_func = [&](size_t no) -> condy::Coro<void> {
        co_await futex.wait(0);
        REQUIRE(atomic_counter.load() == 1);
        finished[no] = true;
    };

    auto wait_func_all = [&]() -> condy::Coro<void> {
        std::vector<condy::Task<void>> tasks;
        tasks.reserve(finished.size());
        for (size_t i = 0; i < finished.size(); i++) {
            if (i == finished.size() / 2) {
                tasks.push_back(condy::co_spawn(wake_func()));
            }
            tasks.push_back(condy::co_spawn(wait_func(i)));
        }
        for (auto &t : tasks) {
            co_await t;
        }
    };

    condy::sync_wait(wait_func_all());
    REQUIRE(
        std::all_of(finished.begin(), finished.end(), [](int f) { return f; }));
}

TEST_CASE("test async_futex - cross thread") {
    std::atomic<int> atomic_counter = 0;
    condy::Futex<int> futex(atomic_counter);

    bool finished = false;

    std::atomic_bool ready_to_wait = false;

    auto wait_func = [&]() -> condy::Coro<void> {
        ready_to_wait.store(true);
        ready_to_wait.notify_one();

        co_await futex.wait(0);
        REQUIRE(atomic_counter.load() == 1);
        finished = true;
    };

    std::thread t1([&] { condy::sync_wait(wait_func()); });

    ready_to_wait.wait(false);

    atomic_counter++;
    futex.notify_one();

    t1.join();

    REQUIRE(finished);
}

TEST_CASE("test async_futex - cancel") {
    std::atomic<int> atomic_counter = 0;
    condy::Futex<int> futex(atomic_counter);

    auto wait_func = [&]() -> condy::Coro<void> {
        auto r = co_await condy::when_any(futex.wait(0), condy::async_nop());
        REQUIRE(r.index() == 1);
        REQUIRE(atomic_counter.load() == 0);
    };

    condy::sync_wait(wait_func());
}

TEST_CASE("test async_futex - destroy while waiting") {
    std::atomic<int> atomic_counter = 0;
    std::unique_ptr<condy::Futex<int>> futex =
        std::make_unique<condy::Futex<int>>(atomic_counter);

    auto destroy_func = [&]() -> condy::Coro<void> {
        futex.reset();
        co_return;
    };

    auto wait_func = [&]() -> condy::Coro<void> {
        auto t = condy::co_spawn(destroy_func());
        auto r = co_await futex->wait(0);
        REQUIRE(r == -EIDRM);
        co_await t;
    };

    condy::sync_wait(wait_func());
}

namespace {

class FutexSemaphore {
public:
    FutexSemaphore(uint32_t initial_count = 0) : count_(initial_count) {}

    FutexSemaphore(const FutexSemaphore &) = delete;
    FutexSemaphore &operator=(const FutexSemaphore &) = delete;
    FutexSemaphore(FutexSemaphore &&) = delete;
    FutexSemaphore &operator=(FutexSemaphore &&) = delete;

public:
    condy::Coro<void> acquire() {
        while (true) {
            uint32_t c = count_.load(std::memory_order_relaxed);
            if (c > 0) {
                if (count_.compare_exchange_strong(c, c - 1,
                                                   std::memory_order_acquire,
                                                   std::memory_order_relaxed)) {
                    co_return;
                }
                continue;
            }
            co_await futex_.wait(0);
        }
    }

    void release(uint32_t n = 1) {
        count_.fetch_add(n, std::memory_order_release);
        for (uint32_t i = 0; i < n; ++i) {
            futex_.notify_one();
        }
    }

private:
    std::atomic<uint32_t> count_;
    condy::Futex<uint32_t> futex_{count_};
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

} // namespace

TEST_CASE("test async_futex - queue") {
    constexpr size_t queue_size = 32;
    constexpr size_t num_messages = 100;

    Queue<int> queue(queue_size);

    auto producer = [&]() -> condy::Coro<void> {
        for (size_t i = 0; i < num_messages; ++i) {
            co_await queue.enqueue(static_cast<int>(i));
        }
    };

    auto consumer = [&]() -> condy::Coro<void> {
        for (size_t i = 0; i < num_messages; ++i) {
            int item = co_await queue.dequeue();
            REQUIRE(item == static_cast<int>(i));
        }
    };

    std::thread rt1_thread([&] { condy::sync_wait(producer()); });

    condy::sync_wait(consumer());

    rt1_thread.join();
}