#pragma once

#include "condy/condy_uring.hpp"
#include "condy/context.hpp"
#include "condy/ring.hpp"
#include "condy/runtime.hpp"
#include <cstddef>
#include <cstdint>
#include <liburing.h>
#include <stdexcept>
#include <sys/types.h>

namespace condy {

struct BufferInfo {
    uint16_t bid;
    uint16_t num_buffers;
};

class ProvidedBufferQueue {
public:
    using ReturnType = BufferInfo;

    ProvidedBufferQueue(size_t log_capacity, unsigned int flags = 0)
        : capacity_(1ll << log_capacity) {
        assert(log_capacity <= 15);
        auto &context = Context::current();
        auto bgid = context.runtime()->next_bgid();

        int r;
        br_ = io_uring_setup_buf_ring(context.ring()->ring(), capacity_, bgid,
                                      flags, &r);
        if (br_ == nullptr) {
            throw_exception("io_uring_setup_buf_ring failed", -r);
        }
        bgid_ = static_cast<uint16_t>(bgid);
    }

    ~ProvidedBufferQueue() {
        assert(br_ != nullptr);
        io_uring_free_buf_ring(Context::current().ring()->ring(), br_,
                               capacity_, bgid_);
    }

    ProvidedBufferQueue(const ProvidedBufferQueue &) = delete;
    ProvidedBufferQueue &operator=(const ProvidedBufferQueue &) = delete;
    ProvidedBufferQueue(ProvidedBufferQueue &&) = delete;
    ProvidedBufferQueue &operator=(ProvidedBufferQueue &&) = delete;

public:
    size_t size() const { return size_; }

    size_t capacity() const { return capacity_; }

    template <typename Buffer> uint16_t push(Buffer &&buffer) {
        if (size_ >= capacity_) {
            throw std::logic_error("ProvidedBufferQueue capacity exceeded");
        }

        auto mask = io_uring_buf_ring_mask(static_cast<uint32_t>(capacity_));
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

        if (!(flags & IORING_CQE_F_BUF_MORE)) {
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
        }

        return result;
    }

private:
    io_uring_buf_ring *br_ = nullptr;
    size_t size_ = 0;
    size_t capacity_;
    uint16_t bgid_;
};

// class NewProvidedBufferPool;

// struct NewProvidedBuffer {
// public:
//     NewProvidedBuffer() = default;
//     NewProvidedBuffer(void *data, size_t size, NewProvidedBufferPool *pool)
//         : data_(data), size_(size), pool_(pool) {}
//     NewProvidedBuffer(NewProvidedBuffer &&other)
//         : data_(std::exchange(other.data_, nullptr)),
//           size_(std::exchange(other.size_, 0)),
//           pool_(std::exchange(other.pool_, nullptr)) {}
//     NewProvidedBuffer &operator=(NewProvidedBuffer &&other) {
//         if (this != &other) {
//             data_ = std::exchange(other.data_, nullptr);
//             size_ = std::exchange(other.size_, 0);
//             pool_ = std::exchange(other.pool_, nullptr);
//         }
//         return *this;
//     }

//     ~NewProvidedBuffer();

//     NewProvidedBuffer(const NewProvidedBuffer &) = delete;
//     NewProvidedBuffer &operator=(const NewProvidedBuffer &) = delete;

// public:
//     void *data() const { return data_; }

//     size_t size() const { return size_; }

// private:
//     void *data_ = nullptr;
//     size_t size_ = 0;
//     NewProvidedBufferPool *pool_ = nullptr;
// };

// class NewProvidedBufferPool {
// public:
//     using ReturnType = std::vector<NewProvidedBuffer>;

//     NewProvidedBufferPool(size_t log_num_buffers, size_t buffer_size,
//                           unsigned int flags = 0)
//         : num_buffers_(1ll << log_num_buffers), buffer_size_(buffer_size) {
//         assert(log_num_buffers <= 15);

//         auto &context = Context::current();
//         auto bgid = context.runtime()->next_bgid();

//         size_t data_size = num_buffers_ * (sizeof(io_uring_buf) +
//         buffer_size); void *data = mmap(nullptr, data_size, PROT_READ |
//         PROT_WRITE,
//                           MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
//         if (data == MAP_FAILED) {
//             throw std::bad_alloc();
//         }
//         br_ = reinterpret_cast<io_uring_buf_ring *>(data);
//         io_uring_buf_ring_init(br_);

//         io_uring_buf_reg reg = {};
//         reg.ring_addr = reinterpret_cast<uint64_t>(br_);
//         reg.ring_entries = static_cast<uint32_t>(num_buffers_);
//         reg.bgid = static_cast<uint16_t>(bgid);
//         int r = io_uring_register_buf_ring(context.ring()->ring(), &reg,
//         flags); if (r != 0) {
//             munmap(data, data_size);
//             throw_exception("io_uring_register_buf_ring failed", -r);
//         }

//         bgid_ = static_cast<uint16_t>(bgid);

//         char *buffer_base = reinterpret_cast<char *>(data) +
//                             sizeof(io_uring_buf) * num_buffers_;
//         auto mask =
//         io_uring_buf_ring_mask(static_cast<uint32_t>(num_buffers_)); for
//         (size_t bid = 0; bid < num_buffers_; bid++) {
//             char *ptr = buffer_base + bid * buffer_size;
//             io_uring_buf_ring_add(br_, ptr, buffer_size, bid, mask, bid);
//         }
//         io_uring_buf_ring_advance(br_, num_buffers_);
//     }

//     ~NewProvidedBufferPool() {
//         assert(br_ != nullptr);
//         size_t data_size = num_buffers_ * (sizeof(io_uring_buf) +
//         buffer_size_); munmap(br_, data_size);
//         [[maybe_unused]] int r = io_uring_unregister_buf_ring(
//             Context::current().ring()->ring(), bgid_);
//         assert(r == 0);
//     }

//     NewProvidedBufferPool(const NewProvidedBufferPool &) = delete;
//     NewProvidedBufferPool &operator=(const NewProvidedBufferPool &) = delete;
//     NewProvidedBufferPool(NewProvidedBufferPool &&) = delete;
//     NewProvidedBufferPool &operator=(NewProvidedBufferPool &&) = delete;

// public:
//     size_t capacity() const { return num_buffers_; }

//     size_t buffer_size() const { return buffer_size_; }

// public:
//     uint16_t bgid() const { return bgid_; }

//     ReturnType handle_finish(int32_t res, uint32_t flags) {
//         std::vector<NewProvidedBuffer> buffers;
//         if (res < 0) {
//             return buffers;
//         }

//         assert(flags & IORING_CQE_F_BUFFER);

//         if (flags & IORING_CQE_F_BUF_MORE) {
//             auto *buf_ptr = curr_uring_buf_();
//             assert(buf_ptr->bid == flags >> IORING_CQE_BUFFER_SHIFT);
//             void *data = reinterpret_cast<char *>(buf_ptr->addr) - res;
//             buffers.emplace_back(data, res, nullptr);
//         } else {
//             auto bytes = res;
//             while (bytes > 0) {
//                 auto *buf_ptr = curr_uring_buf_();
//                 advance_uring_buf_();
//                 size_t buf_size = buf_ptr->len;
//                 void *data = reinterpret_cast<char *>(buf_ptr->addr);
//                 buffers.emplace_back(data, buf_size, this);
//                 bytes -= buf_size;
//             }
//             assert(bytes == 0);
//         }

//         return buffers;
//     }

//     void add_buffer_back(void *ptr) {
//         char *base = buffer_base_();
//         assert(ptr >= base);
//         size_t offset = static_cast<char *>(ptr) - base;
//         size_t bid = offset / buffer_size_;
//         assert(bid < num_buffers_);
//         char *buffer_ptr = base + bid * buffer_size_;
//         auto mask =
//         io_uring_buf_ring_mask(static_cast<uint32_t>(num_buffers_));
//         io_uring_buf_ring_add(br_, buffer_ptr, buffer_size_, bid, mask, 0);
//         io_uring_buf_ring_advance(br_, 1);
//     }

// private:
//     char *buffer_base_() const {
//         return reinterpret_cast<char *>(br_) +
//                sizeof(io_uring_buf) * num_buffers_;
//     }

//     io_uring_buf *curr_uring_buf_() {
//         auto mask =
//         io_uring_buf_ring_mask(static_cast<uint32_t>(num_buffers_)); return
//         &br_->bufs[br_head_ & mask];
//     }

//     void advance_uring_buf_() { br_head_++; }

// private:
//     io_uring_buf_ring *br_ = nullptr;
//     size_t num_buffers_;
//     size_t buffer_size_;
//     uint16_t bgid_;
//     uint16_t br_head_ = 0;

//     friend struct NewProvidedBuffer;
// };

// inline NewProvidedBuffer::~NewProvidedBuffer() {
//     if (pool_ != nullptr) {
//         pool_->add_buffer_back(data_);
//     }
// }

} // namespace condy