#include <algorithm>
#include <condy/wsqueue.hpp>
#include <cstddef>
#include <doctest/doctest.h>
#include <memory>
#include <thread>

TEST_CASE("test wsqueue - basic push/pop/steal") {
    condy::UnboundedTaskQueue<int *> queue{4};

    int a = 0xa;
    int b = 0xb;
    int c = 0xc;

    REQUIRE(queue.capacity() == 16);

    queue.push(&a);
    queue.push(&b);
    queue.push(&c);

    int *item1 = queue.pop();
    REQUIRE(item1 != nullptr);
    REQUIRE(*item1 == 0xc);

    int *item2 = queue.steal();
    REQUIRE(item2 != nullptr);
    REQUIRE(*item2 == 0xa);

    int *item3 = queue.pop();
    REQUIRE(item3 != nullptr);
    REQUIRE(*item3 == 0xb);

    int *item4 = queue.pop();
    REQUIRE(item4 == nullptr);
}

TEST_CASE("test wsqueue - multi thread stealing") {
    const size_t threads = 4;

    std::vector<std::unique_ptr<condy::UnboundedTaskQueue<int *>>> queues;
    for (size_t i = 0; i < threads; ++i) {
        queues.push_back(std::make_unique<condy::UnboundedTaskQueue<int *>>(4));
    }

    std::vector<size_t> counter(threads, 0);

    auto func = [&](size_t thread_no) {
        while (true) {
            int *item;
            item = queues[thread_no]->pop();
            if (item == nullptr) {
                for (size_t j = 0; j < threads; ++j) {
                    if (j != thread_no) {
                        item = queues[j]->steal();
                        if (item != nullptr) {
                            break;
                        }
                    }
                }
            }
            if (item == nullptr) {
                break; // No more works in all queues
            }
            counter[thread_no]++;
            size_t actual_data = reinterpret_cast<size_t>(item);
            if (actual_data > 0) {
                item = reinterpret_cast<int *>(actual_data - 1);
                queues[thread_no]->push(item);
                queues[thread_no]->push(item);
            }
        }
    };

    queues[0]->push(reinterpret_cast<int *>(10));
    std::vector<std::thread> thread_vec;
    for (size_t i = 0; i < threads; ++i) {
        thread_vec.emplace_back(func, i);
    }

    for (auto &t : thread_vec) {
        t.join();
    }

    REQUIRE(std::all_of(counter.begin(), counter.end(),
                        [](size_t c) { return c > 0; }));
}

TEST_CASE("test wsqueue - bounded basic push/pop/steal") {
    condy::BoundedTaskQueue<int *, 4> queue;

    int a = 0xa;
    int b = 0xb;
    int c = 0xc;

    REQUIRE(queue.capacity() == 16);

    queue.try_push(&a);
    queue.try_push(&b);
    queue.try_push(&c);

    int *item1 = queue.pop();
    REQUIRE(item1 != nullptr);
    REQUIRE(*item1 == 0xc);

    int *item2 = queue.steal();
    REQUIRE(item2 != nullptr);
    REQUIRE(*item2 == 0xa);

    int *item3 = queue.pop();
    REQUIRE(item3 != nullptr);
    REQUIRE(*item3 == 0xb);

    int *item4 = queue.pop();
    REQUIRE(item4 == nullptr);
}

TEST_CASE("test wsqueue - bounded multi thread stealing") {
    const size_t threads = 4;

    std::vector<std::unique_ptr<condy::BoundedTaskQueue<int*, 12>>> queues;
    for (size_t i = 0; i < threads; ++i) {
        queues.push_back(std::make_unique<condy::BoundedTaskQueue<int*, 12>>());
    }

    std::vector<size_t> counter(threads, 0);

    auto func = [&](size_t thread_no) {
        while (true) {
            int *item;
            item = queues[thread_no]->pop();
            if (item == nullptr) {
                for (size_t j = 0; j < threads; ++j) {
                    if (j != thread_no) {
                        item = queues[j]->steal();
                        if (item != nullptr) {
                            break;
                        }
                    }
                }
            }
            if (item == nullptr) {
                break; // No more works in all queues
            }
            counter[thread_no]++;
            size_t actual_data = reinterpret_cast<size_t>(item);
            if (actual_data > 0) {
                item = reinterpret_cast<int *>(actual_data - 1);
                REQUIRE(queues[thread_no]->try_push(item));
                REQUIRE(queues[thread_no]->try_push(item));
            }
        }
    };

    REQUIRE(queues[0]->try_push(reinterpret_cast<int *>(10)));
    std::vector<std::thread> thread_vec;
    for (size_t i = 0; i < threads; ++i) {
        thread_vec.emplace_back(func, i);
    }

    for (auto &t : thread_vec) {
        t.join();
    }

    REQUIRE(std::all_of(counter.begin(), counter.end(),
                        [](size_t c) { return c > 0; }));
}