#pragma once

#include <cassert>
#include <cstddef>

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
    IntrusiveSingleList() : head_(nullptr) {}

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
        return reinterpret_cast<T *>(container_of_(entry));
    }

    T *front() noexcept {
        if (empty()) {
            return nullptr;
        }
        return reinterpret_cast<T *>(container_of_(head_));
    }

    template <typename Func> void for_each(Func &&func) noexcept {
        SingleLinkEntry *current = head_;
        while (current) {
            T *item = container_of_(current);
            func(item);
            current = current->next;
        }
    }

private:
    static T *container_of_(SingleLinkEntry *entry) noexcept {
        constexpr T *dummy = 0;
        constexpr auto *offset = &(dummy->*Member);
        return reinterpret_cast<T *>(reinterpret_cast<size_t>(entry) -
                                     reinterpret_cast<size_t>(offset));
    }

private:
    SingleLinkEntry *head_ = nullptr;
    SingleLinkEntry *tail_ = nullptr;
};

template <typename T, DoubleLinkEntry T::*Member> class IntrusiveDoubleList {
public:
    IntrusiveDoubleList() : head_(nullptr), tail_(nullptr) {}

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

    void remove(T *item) noexcept {
        assert(item != nullptr);
        DoubleLinkEntry *entry = &(item->*Member);
#ifndef NDEBUG
        assert(entry->owner == this);
        entry->owner = nullptr;
#endif
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
        constexpr auto *offset = &(dummy->*Member);
        return reinterpret_cast<T *>(reinterpret_cast<size_t>(entry) -
                                     reinterpret_cast<size_t>(offset));
    }

private:
    DoubleLinkEntry *head_ = nullptr;
    DoubleLinkEntry *tail_ = nullptr;
};

} // namespace condy