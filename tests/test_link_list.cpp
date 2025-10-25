#include <condy/link_list.hpp>
#include <doctest/doctest.h>
#include <thread>
#include <vector>

namespace {

struct TestNode : public condy::IntrusiveNode {
    int value;
    TestNode(int v) : value(v) {}
};

} // namespace

TEST_CASE("test link_list - test push and try_pop") {
    const int nodes_per_thread = 1000;
    const int thread_count = 4;

    condy::LinkList list;

    // Producer threads
    std::vector<std::thread> producers;
    for (int t = 0; t < thread_count; ++t) {
        producers.emplace_back([&list, t, nodes_per_thread]() {
            for (int i = 0; i < nodes_per_thread; ++i) {
                int value = i * thread_count + t;
                list.push(new TestNode(value));
            }
        });
    }

    // Single consumer is required
    std::vector<int> prev_values(thread_count, -1);
    bool all_pass = true;
    for (int i = 0; i < thread_count * nodes_per_thread; ++i) {
        TestNode *node = nullptr;
        while ((node = static_cast<TestNode *>(list.try_pop())) == nullptr) {
            std::this_thread::yield();
        }
        int t = node->value % thread_count;
        all_pass &= (node->value > prev_values[t]);
        prev_values[t] = node->value;
        delete node;
    }

    for (auto &prod : producers) {
        prod.join();
    }

    REQUIRE(all_pass);
}

TEST_CASE("test link_list - test pop_all") {
    const int nodes_per_thread = 1000;
    const int thread_count = 4;

    condy::LinkList list;

    // Producer threads
    std::vector<std::thread> producers;
    for (int t = 0; t < thread_count; ++t) {
        producers.emplace_back([&list, t, nodes_per_thread]() {
            for (int i = 0; i < nodes_per_thread; ++i) {
                int value = i * thread_count + t;
                list.push(new TestNode(value));
            }
        });
    }

    // Single consumer is required
    std::vector<int> prev_values(thread_count, -1);
    bool all_pass = true;
    int count = 0;
    while (count < thread_count * nodes_per_thread) {
        list.pop_all([&](condy::IntrusiveNode *n) {
            TestNode *test_node = static_cast<TestNode *>(n);
            int t = test_node->value % thread_count;
            all_pass &= (test_node->value > prev_values[t]);
            prev_values[t] = test_node->value;
            delete test_node;
            count++;
        });
    }

    for (auto &prod : producers) {
        prod.join();
    }

    REQUIRE(all_pass);
}