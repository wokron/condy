/**
 * @file provided_buffers.hpp
 * @brief Support for io_uring provided buffers.
 * @details This file provides support for io_uring provided buffers, which can
 * be used as an alternative to regular buffers in asynchronous operations.
 */

#pragma once

#include "condy/buffers.hpp"
#include "condy/concepts.hpp"
#include "condy/condy_uring.hpp"
#include "condy/context.hpp"
#include "condy/ring.hpp"
#include "condy/utils.hpp"
#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/types.h>

namespace condy {

/**
 * @brief Information about buffers consumed from a provided buffer queue.
 * @details This structure contains information about the buffers that have been
 * consumed from a provided buffer queue, including the buffer ID and the number
 * of buffers consumed. If the buffer is partial comsumption, num_buffers will
 * be zero. If multiple buffers are consumed, num_buffers will indicate how many
 * buffers were used, and the buffer ID will correspond to the first buffer
 * used.
 */
struct BufferInfo {
    /**
     * @brief Buffer ID of the first buffer consumed.
     */
    uint16_t bid;
    /**
     * @brief Number of buffers consumed.
     */
    uint16_t num_buffers;
};

namespace detail {

class BundledProvidedBufferQueue {
public:
    using ReturnType = BufferInfo;

    BundledProvidedBufferQueue(uint32_t capacity, unsigned int flags)
        : capacity_(std::bit_ceil(capacity)), buf_lens_(capacity_, 0) {
        auto &context = detail::Context::current();

        size_t data_size = capacity_ * sizeof(io_uring_buf);
        void *data = mmap(nullptr, data_size, PROT_READ | PROT_WRITE,
                          MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
        if (data == MAP_FAILED) [[unlikely]] {
            throw make_system_error("mmap");
        }
        auto d1 = defer([&]() { munmap(data, data_size); });

        bgid_ = context.next_bgid();
        auto d2 = defer([&]() { context.recycle_bgid(bgid_); });

        br_ = reinterpret_cast<io_uring_buf_ring *>(data);
        io_uring_buf_ring_init(br_);

        io_uring_buf_reg reg = {};
        reg.ring_addr = reinterpret_cast<uint64_t>(br_);
        reg.ring_entries = capacity_;
        reg.bgid = bgid_;
        int r = io_uring_register_buf_ring(context.ring()->ring(), &reg, flags);
        if (r != 0) [[unlikely]] {
            throw make_system_error("io_uring_register_buf_ring", -r);
        }

        d1.dismiss();
        d2.dismiss();
    }

    ~BundledProvidedBufferQueue() {
        assert(br_ != nullptr);
        size_t data_size = capacity_ * sizeof(io_uring_buf);
        munmap(br_, data_size);
        [[maybe_unused]] int r = io_uring_unregister_buf_ring(
            detail::Context::current().ring()->ring(), bgid_);
        assert(r == 0);
        if (r == 0) {
            detail::Context::current().recycle_bgid(bgid_);
        }
    }

    BundledProvidedBufferQueue(const BundledProvidedBufferQueue &) = delete;
    BundledProvidedBufferQueue &
    operator=(const BundledProvidedBufferQueue &) = delete;
    BundledProvidedBufferQueue(BundledProvidedBufferQueue &&) = delete;
    BundledProvidedBufferQueue &
    operator=(BundledProvidedBufferQueue &&) = delete;

public:
    /**
     * @brief Get the current size of the buffer queue
     */
    size_t size() const noexcept { return size_; }

    /**
     * @brief Get the capacity of the buffer queue
     */
    size_t capacity() const noexcept { return capacity_; }

    /**
     * @brief Push a buffer into the provided buffer queue
     * @tparam Buffer Type of the buffer
     * @param buffer The buffer to be pushed
     * @return uint16_t The buffer ID assigned to the pushed buffer.
     * @throws std::logic_error if the capacity of the queue is exceeded
     * @note The returned buffer ID is always sequentially ordered, starting
     * from 0 and incrementing by 1 for each buffer pushed into the queue,
     * wrapping around when reaching the queue's capacity.
     */
    template <BufferLike Buffer> uint16_t push(const Buffer &buffer) {
        if (size_ >= capacity_) [[unlikely]] {
            throw std::logic_error("Capacity exceeded");
        }

        auto mask = io_uring_buf_ring_mask(capacity_);
        uint16_t bid = br_->tail & mask;
        io_uring_buf_ring_add(br_, buffer.data(), buffer.size(), bid, mask, 0);
        buf_lens_[bid] = buffer.size();
        io_uring_buf_ring_advance(br_, 1);
        size_++;

        return bid;
    }

public:
    uint16_t bgid() const noexcept { return bgid_; }

    ReturnType handle_finish(int32_t res, uint32_t flags) noexcept {
        if (!(flags & IORING_CQE_F_BUFFER)) {
            return ReturnType{0, 0};
        }

        assert(res > 0);

        ReturnType result = {
            .bid = static_cast<uint16_t>(flags >> IORING_CQE_BUFFER_SHIFT),
            .num_buffers = 0,
        };

#if !IO_URING_CHECK_VERSION(2, 8) // >= 2.8
        if (flags & IORING_CQE_F_BUF_MORE) {
            assert(buf_lens_[result.bid] > static_cast<uint32_t>(res));
            buf_lens_[result.bid] -= res;
            return result;
        }
#endif

        auto mask = io_uring_buf_ring_mask(capacity_);
        uint16_t curr_bid = result.bid;
        int64_t bytes = res;
        while (bytes > 0) {
            uint32_t buf_len = std::exchange(buf_lens_[curr_bid], 0);
            assert(buf_len > 0);
            bytes -= buf_len;
            result.num_buffers++;
            curr_bid = (curr_bid + 1) & mask;
        }
        assert(size_ >= result.num_buffers);
        size_ -= result.num_buffers;

        return result;
    }

private:
    io_uring_buf_ring *br_ = nullptr;
    uint32_t size_ = 0;
    uint32_t capacity_;
    uint16_t bgid_;
    std::vector<uint32_t> buf_lens_;
};

} // namespace detail

/**
 * @brief Provided buffer queue.
 * @details A provided buffer queue manages a queue of buffers that can be used
 * in asynchronous operations. User is responsible for pushing buffers into the
 * queue.
 * @returns std::pair<int, BufferInfo> When pass to async operations, the return
 * type will be a pair of the operation result and the @ref BufferInfo.
 * @note The lifetime of this queue must not exceed the running period of the
 * associated Runtime. The buffers pushed into the queue must remain valid until
 * they are consumed from the queue.
 */
class ProvidedBufferQueue : public detail::BundledProvidedBufferQueue {
public:
    /**
     * @brief Construct a new ProvidedBufferQueue object in current Runtime.
     * @param capacity Number of buffers the queue can hold.
     * @param flags Optional flags for io_uring buffer ring registration
     * (default: 0).
     */
    ProvidedBufferQueue(uint32_t capacity, unsigned int flags = 0)
        : BundledProvidedBufferQueue(capacity, flags) {}

    ReturnType handle_finish(int32_t res, uint32_t flags) noexcept {
        auto result = BundledProvidedBufferQueue::handle_finish(res, flags);
        assert(result.num_buffers <= 1);
        return result;
    }
};

namespace detail {
class BundledProvidedBufferPool;
}

/**
 * @brief Provided buffer.
 * @details A provided buffer represents a buffer obtained from a provided
 * buffer pool. It automatically returns the buffer to the pool when it goes
 * out of scope.
 * @note The lifetime of the provided buffer must not exceed the lifetime of the
 * provided buffer pool it is associated with.
 */
struct ProvidedBuffer : public BufferBase {
public:
    ProvidedBuffer() = default;
    ProvidedBuffer(void *data, size_t size,
                   detail::BundledProvidedBufferPool *pool)
        : data_(data), size_(size), pool_(pool) {}
    ProvidedBuffer(ProvidedBuffer &&other) noexcept
        : data_(std::exchange(other.data_, nullptr)),
          size_(std::exchange(other.size_, 0)),
          pool_(std::exchange(other.pool_, nullptr)) {}
    ProvidedBuffer &operator=(ProvidedBuffer &&other) noexcept {
        if (this != &other) {
            reset();
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
            pool_ = std::exchange(other.pool_, nullptr);
        }
        return *this;
    }

    ~ProvidedBuffer() { reset(); }

    ProvidedBuffer(const ProvidedBuffer &) = delete;
    ProvidedBuffer &operator=(const ProvidedBuffer &) = delete;

public:
    /**
     * @brief Get the data pointer of the provided buffer
     */
    void *data() const noexcept { return data_; }

    /** *
     * @brief Get the size of the provided buffer
     */
    size_t size() const noexcept { return size_; }

    /**
     * @brief Reset the provided buffer, returning it to the pool if owned
     */
    void reset() noexcept;

    /**
     * @brief Check if the provided buffer owns a buffer from a pool.
     */
    bool owns_buffer() const noexcept { return pool_ != nullptr; }

private:
    void *data_ = nullptr;
    size_t size_ = 0;
    detail::BundledProvidedBufferPool *pool_ = nullptr;
};

namespace detail {

class BundledProvidedBufferPool {
public:
    using ReturnType = std::vector<ProvidedBuffer>;

    BundledProvidedBufferPool(uint32_t num_buffers, size_t buffer_size,
                              unsigned int flags)
        : num_buffers_(std::bit_ceil(num_buffers)), buffer_size_(buffer_size) {
        auto &context = detail::Context::current();

        size_t data_size = num_buffers_ * (sizeof(io_uring_buf) + buffer_size);
        void *data = mmap(nullptr, data_size, PROT_READ | PROT_WRITE,
                          MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
        if (data == MAP_FAILED) [[unlikely]] {
            throw make_system_error("mmap");
        }
        auto d1 = defer([&]() { munmap(data, data_size); });

        bgid_ = context.next_bgid();
        auto d2 = defer([&]() { context.recycle_bgid(bgid_); });

        br_ = reinterpret_cast<io_uring_buf_ring *>(data);
        io_uring_buf_ring_init(br_);

        io_uring_buf_reg reg = {};
        reg.ring_addr = reinterpret_cast<uint64_t>(br_);
        reg.ring_entries = num_buffers_;
        reg.bgid = bgid_;
        int r = io_uring_register_buf_ring(context.ring()->ring(), &reg, flags);
        if (r != 0) [[unlikely]] {
            throw make_system_error("io_uring_register_buf_ring", -r);
        }

        char *buffer_base =
            static_cast<char *>(data) + sizeof(io_uring_buf) * num_buffers_;
        auto mask = io_uring_buf_ring_mask(num_buffers_);
        for (size_t bid = 0; bid < num_buffers_; bid++) {
            char *ptr = buffer_base + bid * buffer_size;
            io_uring_buf_ring_add(br_, ptr, buffer_size, bid, mask,
                                  static_cast<int>(bid));
        }
        io_uring_buf_ring_advance(br_, static_cast<int>(num_buffers_));

        d1.dismiss();
        d2.dismiss();
    }

    ~BundledProvidedBufferPool() {
        assert(br_ != nullptr);
        size_t data_size = num_buffers_ * (sizeof(io_uring_buf) + buffer_size_);
        munmap(br_, data_size);
        [[maybe_unused]] int r = io_uring_unregister_buf_ring(
            detail::Context::current().ring()->ring(), bgid_);
        assert(r == 0);
        if (r == 0) {
            detail::Context::current().recycle_bgid(bgid_);
        }
    }

    BundledProvidedBufferPool(const BundledProvidedBufferPool &) = delete;
    BundledProvidedBufferPool &
    operator=(const BundledProvidedBufferPool &) = delete;
    BundledProvidedBufferPool(BundledProvidedBufferPool &&) = delete;
    BundledProvidedBufferPool &operator=(BundledProvidedBufferPool &&) = delete;

public:
    /**
     * @brief Get the capacity of the buffer pool
     */
    size_t capacity() const noexcept { return num_buffers_; }

    /**
     * @brief Get the size of each buffer in the pool
     */
    size_t buffer_size() const noexcept { return buffer_size_; }

public:
    uint16_t bgid() const noexcept { return bgid_; }

    ReturnType handle_finish(int32_t res, uint32_t flags) noexcept {
        std::vector<ProvidedBuffer> buffers;

        if (!(flags & IORING_CQE_F_BUFFER)) {
            return buffers;
        }

        assert(res > 0);

        uint16_t bid = flags >> IORING_CQE_BUFFER_SHIFT;

#if !IO_URING_CHECK_VERSION(2, 8) // >= 2.8
        if (flags & IORING_CQE_F_BUF_MORE) {
            char *data = get_buffer_(bid) + partial_size_;
            buffers.emplace_back(data, res, nullptr);
            partial_size_ += res;
            assert(partial_size_ < buffer_size_);
            return buffers;
        }
#endif
        assert(bid == curr_io_uring_buf_()->bid);

        int32_t bytes = res;
        while (bytes > 0) {
            auto *buf_ptr = curr_io_uring_buf_();
            bid = buf_ptr->bid;
            uint32_t curr_buffer_size = buffer_size_ - partial_size_;
            char *data = get_buffer_(bid) + partial_size_;
            buffers.emplace_back(data, curr_buffer_size, this);
            bytes -= static_cast<int32_t>(curr_buffer_size);
            partial_size_ = 0;
            advance_io_uring_buf_();
        }

        return buffers;
    }

    void add_buffer_back(void *ptr) noexcept {
        char *base = get_buffers_base_();
        assert(ptr >= base);
        size_t offset = static_cast<char *>(ptr) - base;
        size_t bid = offset / buffer_size_;
        assert(bid < num_buffers_);
        char *buffer_ptr = base + bid * buffer_size_;
        auto mask = io_uring_buf_ring_mask(num_buffers_);
        io_uring_buf_ring_add(br_, buffer_ptr, buffer_size_, bid, mask, 0);
        io_uring_buf_ring_advance(br_, 1);
    }

private:
    char *get_buffer_(uint16_t bid) const noexcept {
        return get_buffers_base_() + static_cast<size_t>(bid) * buffer_size_;
    }

    char *get_buffers_base_() const noexcept {
        return reinterpret_cast<char *>(br_) +
               sizeof(io_uring_buf) * num_buffers_;
    }

    io_uring_buf *curr_io_uring_buf_() noexcept {
        auto mask = io_uring_buf_ring_mask(num_buffers_);
        return &br_->bufs[br_head_ & mask];
    }

    void advance_io_uring_buf_() noexcept { br_head_++; }

private:
    io_uring_buf_ring *br_ = nullptr;
    uint32_t num_buffers_;
    uint32_t buffer_size_;
    uint32_t partial_size_ = 0;
    uint16_t bgid_;
    uint16_t br_head_ = 0;
};

} // namespace detail

inline void ProvidedBuffer::reset() noexcept {
    if (pool_ != nullptr) {
        pool_->add_buffer_back(data_);
    }
    data_ = nullptr;
    size_ = 0;
    pool_ = nullptr;
}

/**
 * @brief Provided buffer pool.
 * @details A provided buffer pool manages a pool of buffers that can be used in
 * asynchronous operations. Only receiving operations can obtain buffers from
 * the pool.
 * @returns std::pair<int, ProvidedBuffer> When pass to async operations, the
 * return type will be a pair of the operation result and the @ref
 * ProvidedBuffer.
 * @note The lifetime of this pool must not exceed the running period of the
 * associated Runtime, and the lifetime of any ProvidedBuffer obtained from
 * this pool must not exceed the lifetime of this pool.
 */
class ProvidedBufferPool : public detail::BundledProvidedBufferPool {
public:
    using ReturnType = ProvidedBuffer;

    /**
     * @brief Construct a new ProvidedBufferPool object in current Runtime.
     * @param num_buffers Number of buffers to allocate in the pool.
     * @param buffer_size Size of each buffer in bytes.
     * @param flags Optional flags for io_uring buffer registration (default:
     * 0).
     */
    ProvidedBufferPool(uint32_t num_buffers, size_t buffer_size,
                       unsigned int flags = 0)
        : BundledProvidedBufferPool(num_buffers, buffer_size, flags) {}

public:
    ReturnType handle_finish(int32_t res, uint32_t flags) noexcept {
        auto buffers = BundledProvidedBufferPool::handle_finish(res, flags);
        if (buffers.empty()) {
            return ReturnType();
        }
        assert(buffers.size() == 1);
        return std::move(buffers[0]);
    }
};

/**
 * @brief Get the bundled variant of a provided buffer pool. This will
 * enable buffer bundling feature of io_uring.
 * @param buffer The provided buffer pool.
 * @return auto& The bundled variant of the provided buffer.
 * @note When using bundled provided buffer pool, the return type of async
 * operations will be a vector of @ref ProvidedBuffer instead of a single
 * buffer.
 */
inline auto &bundled(ProvidedBufferPool &buffer) {
    return static_cast<detail::BundledProvidedBufferPool &>(buffer);
}

/**
 * @brief Get the bundled variant of a provided buffer queue. This will
 * enable buffer bundling feature of io_uring.
 * @param buffer The provided buffer queue.
 * @return auto& The bundled variant of the provided buffer.
 */
inline auto &bundled(ProvidedBufferQueue &buffer) {
    return static_cast<detail::BundledProvidedBufferQueue &>(buffer);
}

} // namespace condy