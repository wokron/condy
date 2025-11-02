// TASKFLOW MIT LICENSE
//
// Copyright (c) 2018-2025 Dr. Tsung-Wei Huang
//
// The University of Wisconsin at Madison
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

namespace condy {

template <typename T> class UnboundedTaskQueue {

    static_assert(std::is_pointer_v<T>, "T must be a pointer type");

    struct Array {

        int64_t C;
        int64_t M;
        std::atomic<T> *S;

        explicit Array(int64_t c)
            : C{c}, M{c - 1}, S{new std::atomic<T>[static_cast<size_t>(C)]} {}

        ~Array() { delete[] S; }

        int64_t capacity() const noexcept { return C; }

        void push(int64_t i, T o) noexcept {
            S[i & M].store(o, std::memory_order_relaxed);
        }

        T pop(int64_t i) noexcept {
            return S[i & M].load(std::memory_order_relaxed);
        }

        Array *resize(int64_t b, int64_t t) {
            Array *ptr = new Array{2 * C};
            for (int64_t i = t; i != b; ++i) {
                ptr->push(i, pop(i));
            }
            return ptr;
        }
    };

    alignas(64) std::atomic<int64_t> _top;
    alignas(64) std::atomic<int64_t> _bottom;
    std::atomic<Array *> _array;
    std::vector<Array *> _garbage;

public:
    /**
    @brief constructs the queue with the given size in the base-2 logarithm

    @param LogSize the base-2 logarithm of the queue size
    */
    explicit UnboundedTaskQueue(int64_t LogSize);

    /**
    @brief destructs the queue
    */
    ~UnboundedTaskQueue();

    /**
    @brief queries if the queue is empty at the time of this call
    */
    bool empty() const noexcept;

    /**
    @brief queries the number of items at the time of this call
    */
    size_t size() const noexcept;

    /**
    @brief queries the capacity of the queue
    */
    int64_t capacity() const noexcept;

    /**
    @brief inserts an item to the queue

    @param item the item to push to the queue

    Only the owner thread can insert an item to the queue.
    The operation can trigger the queue to resize its capacity
    if more space is required.
    */
    void push(T item);

    /**
    @brief pops out an item from the queue

    Only the owner thread can pop out an item from the queue.
    The return can be a @c nullptr if this operation failed (empty queue).
    */
    T pop();

    /**
    @brief steals an item from the queue

    Any threads can try to steal an item from the queue.
    The return can be a @c nullptr if this operation failed (not necessary
    empty).
    */
    T steal();

private:
    Array *resize_array(Array *a, int64_t b, int64_t t);
};

// Constructor
template <typename T>
UnboundedTaskQueue<T>::UnboundedTaskQueue(int64_t LogSize) {
    _top.store(0, std::memory_order_relaxed);
    _bottom.store(0, std::memory_order_relaxed);
    _array.store(new Array{(int64_t{1} << LogSize)}, std::memory_order_relaxed);
    _garbage.reserve(32);
}

// Destructor
template <typename T> UnboundedTaskQueue<T>::~UnboundedTaskQueue() {
    for (auto a : _garbage) {
        delete a;
    }
    delete _array.load();
}

// Function: empty
template <typename T> bool UnboundedTaskQueue<T>::empty() const noexcept {
    int64_t t = _top.load(std::memory_order_relaxed);
    int64_t b = _bottom.load(std::memory_order_relaxed);
    return (b <= t);
}

// Function: size
template <typename T> size_t UnboundedTaskQueue<T>::size() const noexcept {
    int64_t t = _top.load(std::memory_order_relaxed);
    int64_t b = _bottom.load(std::memory_order_relaxed);
    return static_cast<size_t>(b >= t ? b - t : 0);
}

// Function: push
template <typename T> void UnboundedTaskQueue<T>::push(T o) {

    int64_t b = _bottom.load(std::memory_order_relaxed);
    int64_t t = _top.load(std::memory_order_acquire);
    Array *a = _array.load(std::memory_order_relaxed);

    // queue is full with one additional item (b-t+1)
    if (a->capacity() - 1 < (b - t)) [[unlikely]] {
        a = resize_array(a, b, t);
    }

    a->push(b, o);
    std::atomic_thread_fence(std::memory_order_release);

    // original paper uses relaxed here but tsa complains
    _bottom.store(b + 1, std::memory_order_release);
}

// Function: pop
template <typename T> T UnboundedTaskQueue<T>::pop() {

    int64_t b = _bottom.load(std::memory_order_relaxed) - 1;
    Array *a = _array.load(std::memory_order_relaxed);
    _bottom.store(b, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    int64_t t = _top.load(std::memory_order_relaxed);

    T item{nullptr};

    if (t <= b) {
        item = a->pop(b);
        if (t == b) {
            // the last item just got stolen
            if (!_top.compare_exchange_strong(t, t + 1,
                                              std::memory_order_seq_cst,
                                              std::memory_order_relaxed)) {
                item = nullptr;
            }
            _bottom.store(b + 1, std::memory_order_relaxed);
        }
    } else {
        _bottom.store(b + 1, std::memory_order_relaxed);
    }

    return item;
}

// Function: steal
template <typename T> T UnboundedTaskQueue<T>::steal() {

    int64_t t = _top.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    int64_t b = _bottom.load(std::memory_order_acquire);

    T item{nullptr};

    if (t < b) {
        Array *a = _array.load(std::memory_order_consume);
        item = a->pop(t);
        if (!_top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                          std::memory_order_relaxed)) {
            return nullptr;
        }
    }

    return item;
}

// Function: capacity
template <typename T> int64_t UnboundedTaskQueue<T>::capacity() const noexcept {
    return _array.load(std::memory_order_relaxed)->capacity();
}

template <typename T>
typename UnboundedTaskQueue<T>::Array *
UnboundedTaskQueue<T>::resize_array(Array *a, int64_t b, int64_t t) {
    Array *tmp = a->resize(b, t);
    _garbage.push_back(a);
    _array.store(tmp, std::memory_order_release);
    // Note: the original paper using relaxed causes t-san to complain
    //_array.store(a, std::memory_order_relaxed);
    return tmp;
}

} // namespace condy