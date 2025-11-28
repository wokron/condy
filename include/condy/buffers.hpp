#pragma once

#include "condy/condy_uring.hpp"
#include "condy/utils.hpp"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <utility>
#include <vector>

namespace condy {

namespace detail {

class ProvidedBufferPoolImpl {
public:
    ProvidedBufferPoolImpl(io_uring *ring, uint16_t bgid,
                           size_t log_num_buffers, size_t buffer_size,
                           unsigned int flags)
        : ring_(ring), num_buffers_(1 << log_num_buffers),
          buffer_size_(buffer_size), bgid_(bgid),
          buf_ring_mask_(num_buffers_ - 1) {
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
        int r = io_uring_register_buf_ring(ring_, &reg, flags);
        if (r != 0) {
            munmap(data_, data_size_);
            throw_exception("io_uring_register_buf_ring failed", -r);
        }
        buffers_base_ = reinterpret_cast<char *>(data_) +
                        sizeof(io_uring_buf) * num_buffers_;
        for (size_t i = 0; i < num_buffers_; i++) {
            char *ptr = buffers_base_ + i * buffer_size_;
            io_uring_buf_ring_add(buf_ring_, ptr, buffer_size_, i,
                                  buf_ring_mask_, i);
        }
        io_uring_buf_ring_advance(buf_ring_, num_buffers_);
    }

    ProvidedBufferPoolImpl(const ProvidedBufferPoolImpl &) = delete;
    ProvidedBufferPoolImpl &operator=(const ProvidedBufferPoolImpl &) = delete;
    ProvidedBufferPoolImpl(ProvidedBufferPoolImpl &&) = delete;
    ProvidedBufferPoolImpl &operator=(ProvidedBufferPoolImpl &&) = delete;

    ~ProvidedBufferPoolImpl() { munmap(data_, data_size_); }

public:
    uint16_t bgid() const { return bgid_; }

    size_t buffer_size() const { return buffer_size_; }

    void *get_buffer(size_t bid) const {
        assert(bid < num_buffers_);
        auto *addr = reinterpret_cast<void *>(buf_ring_->bufs[bid].addr);
        return addr;
    }

    void add_buffer(void *ptr) {
        assert(is_valid_buffer_(ptr));
        io_uring_buf_ring_add(buf_ring_, ptr, buffer_size_, next_bid_,
                              buf_ring_mask_, 0);
        io_uring_buf_ring_advance(buf_ring_, 1);
        next_bid_ = (next_bid_ + 1) & buf_ring_mask_;
    }

private:
    bool is_valid_buffer_(void *ptr) const {
        char *cptr = static_cast<char *>(ptr);
        auto offset = cptr - buffers_base_;
        return offset % buffer_size_ == 0 &&
               offset / buffer_size_ < num_buffers_;
    }

private:
    io_uring *ring_;
    size_t num_buffers_;
    size_t buffer_size_;
    uint16_t bgid_;
    size_t buf_ring_mask_;

    size_t next_bid_ = 0;

    io_uring_buf_ring *buf_ring_;
    char *buffers_base_;

    void *data_;
    size_t data_size_;
};

using ProvidedBufferPoolImplPtr = std::shared_ptr<ProvidedBufferPoolImpl>;

} // namespace detail

class ProvidedBuffer {
public:
    ProvidedBuffer() = default;
    ProvidedBuffer(detail::ProvidedBufferPoolImplPtr impl, void *data,
                   size_t size)
        : impl_(std::move(impl)), data_(data), size_(size) {}
    ProvidedBuffer(ProvidedBuffer &&other)
        : impl_(std::move(other.impl_)),
          data_(std::exchange(other.data_, nullptr)),
          size_(std::exchange(other.size_, 0)) {}
    ProvidedBuffer &operator=(ProvidedBuffer &&other) {
        if (this != &other) {
            if (impl_ != nullptr) {
                impl_->add_buffer(data_);
            }
            impl_ = std::move(other.impl_);
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }

    ~ProvidedBuffer() {
        if (impl_ != nullptr) {
            impl_->add_buffer(data_);
        }
    }

    ProvidedBuffer(const ProvidedBuffer &) = delete;
    ProvidedBuffer &operator=(const ProvidedBuffer &) = delete;

public:
    void *data() const { return data_; }

    size_t size() const { return size_; }

    void reset() {
        if (impl_ != nullptr) {
            impl_->add_buffer(data_);
            impl_.reset();
            data_ = nullptr;
            size_ = 0;
        }
    }

    bool owns_buffer() const { return impl_ != nullptr; }

private:
    detail::ProvidedBufferPoolImplPtr impl_ = nullptr;
    void *data_ = nullptr;
    size_t size_ = 0;
};

class MutableBuffer {
public:
    MutableBuffer() = default;
    MutableBuffer(void *data, size_t size) : data_(data), size_(size) {}

    void *data() const { return data_; }

    size_t size() const { return size_; }

private:
    void *data_ = nullptr;
    size_t size_ = 0;
};

class ConstBuffer {
public:
    ConstBuffer() = default;
    ConstBuffer(const void *data, size_t size) : data_(data), size_(size) {}

    ConstBuffer(MutableBuffer buf) : data_(buf.data()), size_(buf.size()) {}
    ConstBuffer &operator=(MutableBuffer buf) {
        data_ = buf.data();
        size_ = buf.size();
        return *this;
    }

    const void *data() const { return data_; }

    size_t size() const { return size_; }

private:
    const void *data_ = nullptr;
    size_t size_ = 0;
};

inline MutableBuffer buffer(void *data, size_t size) {
    return MutableBuffer(data, size);
}

inline ConstBuffer buffer(const void *data, size_t size) {
    return ConstBuffer(data, size);
}

template <typename PodType, size_t N>
inline MutableBuffer buffer(PodType (&arr)[N]) {
    return MutableBuffer(static_cast<void *>(arr), sizeof(PodType) * N);
}

template <typename PodType, size_t N>
inline ConstBuffer buffer(const PodType (&arr)[N]) {
    return ConstBuffer(static_cast<const void *>(arr), sizeof(PodType) * N);
}

template <typename PodType, size_t N>
inline MutableBuffer buffer(std::array<PodType, N> &arr) {
    return MutableBuffer(static_cast<void *>(arr.data()), sizeof(PodType) * N);
}

template <typename PodType, size_t N>
inline ConstBuffer buffer(const std::array<PodType, N> &arr) {
    return ConstBuffer(static_cast<const void *>(arr.data()),
                       sizeof(PodType) * N);
}

template <typename PodType>
inline MutableBuffer buffer(std::vector<PodType> &vec) {
    return MutableBuffer(static_cast<void *>(vec.data()),
                         sizeof(PodType) * vec.size());
}

template <typename PodType>
inline ConstBuffer buffer(const std::vector<PodType> &vec) {
    return ConstBuffer(static_cast<const void *>(vec.data()),
                       sizeof(PodType) * vec.size());
}

inline MutableBuffer buffer(std::string &str) {
    return MutableBuffer(static_cast<void *>(str.data()), str.size());
}

inline ConstBuffer buffer(const std::string &str) {
    return ConstBuffer(static_cast<const void *>(str.data()), str.size());
}

} // namespace condy