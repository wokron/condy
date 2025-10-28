#pragma once

#include <atomic>
#include <cassert>

namespace condy {

struct IntrusiveNode {
    IntrusiveNode *next_ = nullptr;
};

template <typename T> class LinkList {
public:
    // LinkList does not take ownership of the nodes, so use default
    // constructor & destructor
    LinkList() = default;
    ~LinkList() = default;

public:
    void push(T *node) noexcept {
        // Push the node to the front of the local list
        assert(node != nullptr && node->next_ == nullptr);
        auto *old_head = head_.load(std::memory_order_relaxed);
        do {
            node->next_ = old_head;
        } while (!head_.compare_exchange_weak(old_head, node,
                                              std::memory_order_release,
                                              std::memory_order_relaxed));
    }

    T *try_pop() noexcept {
        if (local_head_ == nullptr) {
            // Now fetch the global list to local list
            local_head_ = head_.exchange(nullptr, std::memory_order_acquire);
            local_head_ = reverse_list_(local_head_);
        }
        return static_cast<T *>(fetch_head_(&local_head_));
    }

    template <typename Func> void pop_all(Func &&func) noexcept {
        auto *local_head2 = head_.exchange(nullptr, std::memory_order_acquire);
        local_head2 = reverse_list_(local_head2);

        while (local_head_ != nullptr) {
            func(static_cast<T *>(fetch_head_(&local_head_)));
        }

        while (local_head2 != nullptr) {
            func(static_cast<T *>(fetch_head_(&local_head2)));
        }
    }

private:
    static IntrusiveNode *reverse_list_(IntrusiveNode *head) noexcept {
        IntrusiveNode *prev = nullptr;
        while (head != nullptr) {
            IntrusiveNode *next = head->next_;
            head->next_ = prev;
            prev = head;
            head = next;
        }
        return prev;
    }

    static IntrusiveNode *fetch_head_(IntrusiveNode **head) noexcept {
        IntrusiveNode *old_head = *head;
        if (old_head == nullptr) {
            return nullptr;
        }
        IntrusiveNode *new_head = old_head->next_;
        old_head->next_ = nullptr;
        *head = new_head;
        return old_head;
    }

private:
    std::atomic<IntrusiveNode *> head_{nullptr};
    IntrusiveNode *local_head_ = nullptr;
};

} // namespace condy