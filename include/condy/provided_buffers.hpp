#pragma once

#include "condy/condy_uring.hpp"
#include "condy/context.hpp"
#include "condy/ring.hpp"
#include "condy/utils.hpp"
#include <bit>
#include <cstddef>
#include <cstdint>
#include <liburing.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/types.h>

namespace condy {

struct BufferInfo {
    uint16_t bid;
    uint16_t num_buffers;
};

class BundledProvidedBufferQueue {
public:
    using ReturnType = BufferInfo;

    BundledProvidedBufferQueue(uint32_t capacity, unsigned int flags = 0)
        : capacity_(std::bit_ceil(capacity)) {
        auto &context = Context::current();
        auto bgid = context.next_bgid();

        size_t data_size = capacity_ * sizeof(io_uring_buf);
        void *data = mmap(nullptr, data_size, PROT_READ | PROT_WRITE,
                          MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
        if (data == MAP_FAILED) [[unlikely]] {
            throw std::bad_alloc();
        }
        br_ = reinterpret_cast<io_uring_buf_ring *>(data);
        io_uring_buf_ring_init(br_);

        io_uring_buf_reg reg = {};
        reg.ring_addr = reinterpret_cast<uint64_t>(br_);
        reg.ring_entries = capacity_;
        reg.bgid = bgid;
        int r = io_uring_register_buf_ring(context.ring()->ring(), &reg, flags);
        if (r != 0) [[unlikely]] {
            munmap(data, data_size);
            throw make_system_error("io_uring_register_buf_ring", -r);
        }

        bgid_ = bgid;
    }

    ~BundledProvidedBufferQueue() {
        assert(br_ != nullptr);
        size_t data_size = capacity_ * sizeof(io_uring_buf);
        munmap(br_, data_size);
        [[maybe_unused]] int r = io_uring_unregister_buf_ring(
            Context::current().ring()->ring(), bgid_);
        assert(r == 0);
    }

    BundledProvidedBufferQueue(const BundledProvidedBufferQueue &) = delete;
    BundledProvidedBufferQueue &
    operator=(const BundledProvidedBufferQueue &) = delete;
    BundledProvidedBufferQueue(BundledProvidedBufferQueue &&) = delete;
    BundledProvidedBufferQueue &
    operator=(BundledProvidedBufferQueue &&) = delete;

public:
    size_t size() const { return size_; }

    size_t capacity() const { return capacity_; }

    template <typename Buffer> uint16_t push(Buffer &&buffer) {
        if (size_ >= capacity_) [[unlikely]] {
            throw std::logic_error("Capacity exceeded");
        }

        auto mask = io_uring_buf_ring_mask(capacity_);
        uint16_t bid = br_->tail & mask;
        io_uring_buf_ring_add(br_, buffer.data(), buffer.size(), bid, mask, 0);
        io_uring_buf_ring_advance(br_, 1);
        size_++;

        return bid;
    }

public:
    uint16_t bgid() const { return bgid_; }

    ReturnType handle_finish(int32_t res, uint32_t flags) {
        if (res < 0) {
            return ReturnType{0, 0};
        }

        assert(flags & IORING_CQE_F_BUFFER);

        ReturnType result = {
            .bid = static_cast<uint16_t>(flags >> IORING_CQE_BUFFER_SHIFT),
            .num_buffers = 0,
        };

#if !IO_URING_CHECK_VERSION(2, 8) // >= 2.8
        if (flags & IORING_CQE_F_BUF_MORE) {
            return result;
        }
#endif

        uint16_t curr_bid = result.bid;
        auto bytes = res;
        while (bytes > 0) {
            auto &buf = br_->bufs[curr_bid];
            assert(buf.bid == curr_bid);
            size_t buf_size = buf.len;
            bytes -= buf_size;
            result.num_buffers++;
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
};

class ProvidedBufferQueue : public BundledProvidedBufferQueue {
public:
    using BundledProvidedBufferQueue::BundledProvidedBufferQueue;
};

class BundledProvidedBufferPool;

struct ProvidedBuffer {
public:
    ProvidedBuffer() = default;
    ProvidedBuffer(void *data, size_t size, BundledProvidedBufferPool *pool)
        : data_(data), size_(size), pool_(pool) {}
    ProvidedBuffer(ProvidedBuffer &&other)
        : data_(std::exchange(other.data_, nullptr)),
          size_(std::exchange(other.size_, 0)),
          pool_(std::exchange(other.pool_, nullptr)) {}
    ProvidedBuffer &operator=(ProvidedBuffer &&other) {
        if (this != &other) {
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
    void *data() const { return data_; }

    size_t size() const { return size_; }

    void reset();

    bool owns_buffer() const { return pool_ != nullptr; }

private:
    void *data_ = nullptr;
    size_t size_ = 0;
    BundledProvidedBufferPool *pool_ = nullptr;
};

class BundledProvidedBufferPool {
public:
    using ReturnType = std::vector<ProvidedBuffer>;

    BundledProvidedBufferPool(uint32_t num_buffers, size_t buffer_size,
                              unsigned int flags = 0)
        : num_buffers_(std::bit_ceil(num_buffers)), buffer_size_(buffer_size) {
        auto &context = Context::current();
        auto bgid = context.next_bgid();

        size_t data_size = num_buffers_ * (sizeof(io_uring_buf) + buffer_size);
        void *data = mmap(nullptr, data_size, PROT_READ | PROT_WRITE,
                          MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
        if (data == MAP_FAILED) [[unlikely]] {
            throw std::bad_alloc();
        }
        br_ = reinterpret_cast<io_uring_buf_ring *>(data);
        io_uring_buf_ring_init(br_);

        io_uring_buf_reg reg = {};
        reg.ring_addr = reinterpret_cast<uint64_t>(br_);
        reg.ring_entries = num_buffers_;
        reg.bgid = bgid;
        int r = io_uring_register_buf_ring(context.ring()->ring(), &reg, flags);
        if (r != 0) [[unlikely]] {
            munmap(data, data_size);
            throw make_system_error("io_uring_register_buf_ring", -r);
        }

        bgid_ = bgid;

        char *buffer_base =
            static_cast<char *>(data) + sizeof(io_uring_buf) * num_buffers_;
        auto mask = io_uring_buf_ring_mask(num_buffers_);
        for (size_t bid = 0; bid < num_buffers_; bid++) {
            char *ptr = buffer_base + bid * buffer_size;
            io_uring_buf_ring_add(br_, ptr, buffer_size, bid, mask, bid);
        }
        io_uring_buf_ring_advance(br_, num_buffers_);
    }

    ~BundledProvidedBufferPool() {
        assert(br_ != nullptr);
        size_t data_size = num_buffers_ * (sizeof(io_uring_buf) + buffer_size_);
        munmap(br_, data_size);
        [[maybe_unused]] int r = io_uring_unregister_buf_ring(
            Context::current().ring()->ring(), bgid_);
        assert(r == 0);
    }

    BundledProvidedBufferPool(const BundledProvidedBufferPool &) = delete;
    BundledProvidedBufferPool &
    operator=(const BundledProvidedBufferPool &) = delete;
    BundledProvidedBufferPool(BundledProvidedBufferPool &&) = delete;
    BundledProvidedBufferPool &operator=(BundledProvidedBufferPool &&) = delete;

public:
    size_t capacity() const { return num_buffers_; }

    size_t buffer_size() const { return buffer_size_; }

public:
    uint16_t bgid() const { return bgid_; }

    ReturnType handle_finish(int32_t res, [[maybe_unused]] uint32_t flags) {
        std::vector<ProvidedBuffer> buffers;
        if (res < 0) {
            return buffers;
        }

        assert(flags & IORING_CQE_F_BUFFER);

#if !IO_URING_CHECK_VERSION(2, 8) // >= 2.8
        if (flags & IORING_CQE_F_BUF_MORE) {
            uint16_t bid = flags >> IORING_CQE_BUFFER_SHIFT;
            char *data = get_buffer_(bid) + partial_size_;
            buffers.emplace_back(data, res, nullptr);
            partial_size_ += res;
            return buffers;
        }
#endif

        auto bytes = res;
        while (bytes > 0) {
            auto *buf_ptr = curr_io_uring_buf_();
            uint16_t bid = buf_ptr->bid;
            size_t curr_buffer_size = buffer_size_ - partial_size_;
            char *data = get_buffer_(bid) + partial_size_;
            buffers.emplace_back(data, curr_buffer_size, this);
            bytes -= curr_buffer_size;
            partial_size_ = 0;
            advance_io_uring_buf_();
        }

        return buffers;
    }

    void add_buffer_back(void *ptr) {
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
    char *get_buffer_(uint16_t bid) const {
        return get_buffers_base_() + bid * buffer_size_;
    }

    char *get_buffers_base_() const {
        return reinterpret_cast<char *>(br_) +
               sizeof(io_uring_buf) * num_buffers_;
    }

    io_uring_buf *curr_io_uring_buf_() {
        auto mask = io_uring_buf_ring_mask(num_buffers_);
        return &br_->bufs[br_head_ & mask];
    }

    void advance_io_uring_buf_() { br_head_++; }

private:
    io_uring_buf_ring *br_ = nullptr;
    uint32_t num_buffers_;
    uint32_t buffer_size_;
    uint32_t partial_size_ = 0;
    uint16_t bgid_;
    uint16_t br_head_ = 0;
};

inline void ProvidedBuffer::reset() {
    if (pool_ != nullptr) {
        pool_->add_buffer_back(data_);
    }
    data_ = nullptr;
    size_ = 0;
    pool_ = nullptr;
}

class ProvidedBufferPool : public BundledProvidedBufferPool {
public:
    using ReturnType = ProvidedBuffer;

    ProvidedBufferPool(uint32_t num_buffers, size_t buffer_size,
                       unsigned int flags = 0)
        : BundledProvidedBufferPool(num_buffers, buffer_size, flags) {}

public:
    ReturnType handle_finish(int32_t res, uint32_t flags) {
        auto buffers = BundledProvidedBufferPool::handle_finish(res, flags);
        if (buffers.empty()) {
            return ReturnType();
        }
        assert(buffers.size() == 1);
        return std::move(buffers[0]);
    }
};

} // namespace condy