#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace condy {

struct SingleLinkEntry {
    SingleLinkEntry *next = nullptr;
};

struct DoubleLinkEntry {
    DoubleLinkEntry *next = nullptr;
    DoubleLinkEntry *prev = nullptr;
#ifndef NDEBUG
    void *owner = nullptr;
#endif
};

template <typename T, SingleLinkEntry T::*Member> class IntrusiveSingleList {
public:
    IntrusiveSingleList() = default;
    IntrusiveSingleList(IntrusiveSingleList &&other) noexcept
        : head_(std::exchange(other.head_, nullptr)),
          tail_(std::exchange(other.tail_, nullptr)),
          size_(std::exchange(other.size_, 0)) {}
    IntrusiveSingleList &operator=(IntrusiveSingleList &&other) noexcept {
        if (this != &other) {
            head_ = std::exchange(other.head_, nullptr);
            tail_ = std::exchange(other.tail_, nullptr);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }

    IntrusiveSingleList(const IntrusiveSingleList &) = delete;
    IntrusiveSingleList &operator=(const IntrusiveSingleList &) = delete;

    void push_back(T *item) noexcept {
        assert(item != nullptr);
        SingleLinkEntry *entry = &(item->*Member);
        assert(entry->next == nullptr);
        entry->next = nullptr;
        if (!head_) {
            head_ = entry;
            tail_ = entry;
        } else {
            tail_->next = entry;
            tail_ = entry;
        }
        size_++;
    }

    void push_back(IntrusiveSingleList other) noexcept {
        if (other.empty()) {
            return;
        }
        if (empty()) {
            head_ = other.head_;
            tail_ = other.tail_;
        } else {
            tail_->next = other.head_;
            tail_ = other.tail_;
        }
        size_ += other.size_;
    }

    bool empty() const noexcept { return head_ == nullptr; }

    T *pop_front() noexcept {
        if (empty()) {
            return nullptr;
        }
        SingleLinkEntry *entry = head_;
        head_ = head_->next;
        if (!head_) {
            tail_ = nullptr;
        }
        entry->next = nullptr;
        size_--;
        return reinterpret_cast<T *>(container_of_(entry));
    }

    IntrusiveSingleList pop_front(size_t max_count) noexcept {
        if (empty() || max_count == 0) {
            return IntrusiveSingleList();
        }
        IntrusiveSingleList batch;
        SingleLinkEntry *current = head_->next;
        SingleLinkEntry *prev = head_;

        size_t batch_size = 1;
        while (current && max_count > 1) {
            prev = current;
            current = current->next;
            max_count--;
            batch_size++;
        }

        batch.head_ = head_;
        batch.tail_ = prev;
        batch.size_ = batch_size;
        head_ = current;
        size_ -= batch_size;
        if (!head_) {
            tail_ = nullptr;
        }
        batch.tail_->next = nullptr;
        return batch;
    }

    size_t size() const noexcept { return size_; }

private:
    static T *container_of_(SingleLinkEntry *entry) noexcept {
        constexpr T *dummy = 0;
#ifdef __clang__
        constexpr auto *offset = &(dummy->*Member);
#else
        auto *offset = &(dummy->*Member);
#endif
        return reinterpret_cast<T *>(reinterpret_cast<intptr_t>(entry) -
                                     reinterpret_cast<intptr_t>(offset));
    }

private:
    SingleLinkEntry *head_ = nullptr;
    SingleLinkEntry *tail_ = nullptr;
    size_t size_ = 0;
};

template <typename T, DoubleLinkEntry T::*Member> class IntrusiveDoubleList {
public:
    IntrusiveDoubleList() = default;

    void push_back(T *item) noexcept {
        assert(item != nullptr);
        DoubleLinkEntry *entry = &(item->*Member);
        assert(entry->next == nullptr && entry->prev == nullptr);
        entry->next = nullptr;
        entry->prev = tail_;
        if (!head_) {
            head_ = entry;
            tail_ = entry;
        } else {
            tail_->next = entry;
            tail_ = entry;
        }
#ifndef NDEBUG
        assert(entry->owner == nullptr);
        entry->owner = this;
#endif
    }

    bool empty() const noexcept { return head_ == nullptr; }

    T *pop_front() noexcept {
        if (empty()) {
            return nullptr;
        }
        DoubleLinkEntry *entry = head_;
        head_ = head_->next;
        if (head_) {
            head_->prev = nullptr;
        } else {
            tail_ = nullptr;
        }
        entry->next = nullptr;
        entry->prev = nullptr;
#ifndef NDEBUG
        assert(entry->owner == this);
        entry->owner = nullptr;
#endif
        return reinterpret_cast<T *>(container_of_(entry));
    }

    bool remove(T *item) noexcept {
        assert(item != nullptr);
        DoubleLinkEntry *entry = &(item->*Member);
#ifndef NDEBUG
        assert(entry->owner == this);
        entry->owner = nullptr;
#endif
        if (entry->prev == nullptr && entry->next == nullptr &&
            head_ != entry) {
            return false;
        }
        if (entry->prev) {
            entry->prev->next = entry->next;
        } else {
            assert(head_ == entry);
            head_ = entry->next;
        }
        if (entry->next) {
            entry->next->prev = entry->prev;
        } else {
            assert(tail_ == entry);
            tail_ = entry->prev;
        }
        entry->next = nullptr;
        entry->prev = nullptr;
        return true;
    }

    template <typename Func> void for_each(Func &&func) noexcept {
        DoubleLinkEntry *current = head_;
        while (current) {
            T *item = container_of_(current);
            func(item);
            current = current->next;
        }
    }

private:
    static T *container_of_(DoubleLinkEntry *entry) noexcept {
        constexpr T *dummy = 0;
#ifdef __clang__
        constexpr auto *offset = &(dummy->*Member);
#else
        auto *offset = &(dummy->*Member);
#endif
        return reinterpret_cast<T *>(reinterpret_cast<intptr_t>(entry) -
                                     reinterpret_cast<intptr_t>(offset));
    }

private:
    DoubleLinkEntry *head_ = nullptr;
    DoubleLinkEntry *tail_ = nullptr;
};

} // namespace condy