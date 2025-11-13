#pragma once

#include "condy/condy_uring.hpp"
#include "condy/context.hpp"
#include "condy/ring.hpp"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <utility>

namespace condy {

namespace detail {

// TODO: Is this thread-safe?
class ProvidedBuffersImpl {
public:
    ProvidedBuffersImpl(io_uring *ring, uint16_t bgid, size_t log_num_buffers,
                        size_t buffer_size)
        : ring_(ring), num_buffers_(1 << log_num_buffers), bgid_(bgid),
          buffer_size_(buffer_size) {
        assert(log_num_buffers <= 15);
        data_size_ = num_buffers_ * (sizeof(io_uring_buf) + buffer_size_);
        data_ = mmap(NULL, data_size_, PROT_READ | PROT_WRITE,
                     MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
        if (data_ == MAP_FAILED) {
            throw std::bad_alloc();
        }
        buf_ring_ = reinterpret_cast<io_uring_buf_ring *>(data_);
        io_uring_buf_ring_init(buf_ring_);

        io_uring_buf_reg reg = {
            .ring_addr = reinterpret_cast<unsigned long>(buf_ring_),
            .ring_entries = static_cast<unsigned>(num_buffers_),
            .bgid = bgid_,
        };
        // TODO: Support IOU_PBUF_RING_INC
        int r = io_uring_register_buf_ring(ring_, &reg, 0);
        if (r != 0) {
            munmap(data_, data_size_);
            throw std::runtime_error("io_uring_register_buf_ring failed: " +
                                     std::string(strerror(-r)));
        }
        buffers_base_ = reinterpret_cast<char *>(data_) +
                        sizeof(io_uring_buf) * num_buffers_;
        for (size_t i = 0; i < num_buffers_; i++) {
            char *ptr = buffers_base_ + i * buffer_size_;
            io_uring_buf_ring_add(buf_ring_, ptr, buffer_size_, i,
                                  io_uring_buf_ring_mask(num_buffers_), i);
        }
        io_uring_buf_ring_advance(buf_ring_, num_buffers_);
    }

    ProvidedBuffersImpl(const ProvidedBuffersImpl &) = delete;
    ProvidedBuffersImpl &operator=(const ProvidedBuffersImpl &) = delete;
    ProvidedBuffersImpl(ProvidedBuffersImpl &&) = delete;
    ProvidedBuffersImpl &operator=(ProvidedBuffersImpl &&) = delete;

    ~ProvidedBuffersImpl() { munmap(data_, data_size_); }

public:
    uint16_t bgid() const { return bgid_; }

    size_t buffer_size() const { return buffer_size_; }

    void *get_buffer(size_t bid) const {
        assert(bid < num_buffers_);
        return buffers_base_ + bid * buffer_size_;
    }

    void add_buffer(size_t bid) {
        void *ptr = get_buffer(bid);
        io_uring_buf_ring_add(buf_ring_, ptr, buffer_size_, bid,
                              io_uring_buf_ring_mask(num_buffers_), 0);
        io_uring_buf_ring_advance(buf_ring_, 1);
    }

private:
    io_uring *ring_;
    size_t num_buffers_;
    size_t buffer_size_;
    uint16_t bgid_;

    io_uring_buf_ring *buf_ring_;
    char *buffers_base_;

    void *data_;
    size_t data_size_;
};

using ProvidedBuffersImplPtr = std::shared_ptr<ProvidedBuffersImpl>;

} // namespace detail

class ProvidedBuffers {
public:
    ProvidedBuffers(size_t log_num_buffers, size_t buffer_size)
        : impl_(std::make_shared<detail::ProvidedBuffersImpl>(
              Context::current().ring()->ring(), Context::current().next_bgid(),
              log_num_buffers, buffer_size)) {}
    ProvidedBuffers(ProvidedBuffers &&) = default;

    ProvidedBuffers(const ProvidedBuffers &) = delete;
    ProvidedBuffers &operator=(const ProvidedBuffers &) = delete;
    ProvidedBuffers &operator=(ProvidedBuffers &&) = delete;

public:
    detail::ProvidedBuffersImplPtr copy_impl() const & { return impl_; }
    detail::ProvidedBuffersImplPtr copy_impl() && { return std::move(impl_); }

private:
    detail::ProvidedBuffersImplPtr impl_;
};

class ProvidedBufferEntry {
public:
    ProvidedBufferEntry() = default;
    ProvidedBufferEntry(detail::ProvidedBuffersImplPtr impl, size_t bid)
        : impl_(std::move(impl)), bid_(bid) {}
    ProvidedBufferEntry(ProvidedBufferEntry &&other)
        : impl_(std::move(other.impl_)), bid_(std::exchange(other.bid_, 0)) {}
    ProvidedBufferEntry &operator=(ProvidedBufferEntry &&other) {
        if (this != &other) {
            if (impl_ != nullptr) {
                impl_->add_buffer(bid_);
            }
            impl_ = std::move(other.impl_);
            bid_ = std::exchange(other.bid_, 0);
        }
        return *this;
    }

    ~ProvidedBufferEntry() {
        if (impl_ != nullptr) {
            impl_->add_buffer(bid_);
        }
    }

    ProvidedBufferEntry(const ProvidedBufferEntry &) = delete;
    ProvidedBufferEntry &operator=(const ProvidedBufferEntry &) = delete;

public:
    void *data() const { return impl_->get_buffer(bid_); }

    size_t size() const { return impl_->buffer_size(); }

    void reset() {
        impl_->add_buffer(bid_);
        impl_.reset();
    }

private:
    detail::ProvidedBuffersImplPtr impl_ = nullptr;
    size_t bid_ = 0;
};

// Just buffer
class Buffer {
public:
    Buffer(void *data, size_t size) : data_(data), size_(size) {}

    void *data() const { return data_; }

    size_t size() const { return size_; }

private:
    void *data_;
    size_t size_;
};

class ConstBuffer {
public:
    ConstBuffer(const void *data, size_t size) : data_(data), size_(size) {}
    ConstBuffer(Buffer buf) : data_(buf.data()), size_(buf.size()) {}

    const void *data() const { return data_; }

    size_t size() const { return size_; }

private:
    const void *data_;
    size_t size_;
};

inline Buffer buffer(void *data, size_t size) { return Buffer(data, size); }

inline ConstBuffer buffer(const void *data, size_t size) {
    return ConstBuffer(data, size);
}

template <size_t N> inline Buffer buffer(char (&arr)[N]) {
    return Buffer(static_cast<void *>(arr), N);
}

template <size_t N> inline ConstBuffer buffer(const char (&arr)[N]) {
    return ConstBuffer(static_cast<const void *>(arr), N);
}

} // namespace condy