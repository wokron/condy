/**
 * @file buffers.hpp
 * @brief Basic buffer types and conversion utilities.
 * @details This file defines basic buffer types and conversion functions.
 * Buffer types are primarily used in asynchronous operations.
 */

#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <sys/mman.h>
#include <sys/uio.h>
#include <utility>
#include <vector>

namespace condy {

// Just a tag
class BufferBase {};

/**
 * @brief Mutable buffer.
 */
class MutableBuffer : public BufferBase {
public:
    MutableBuffer() = default;
    MutableBuffer(void *data, size_t size) : data_(data), size_(size) {}

    /**
     * @brief Get the data of the buffer
     */
    void *data() const noexcept { return data_; }

    /**
     * @brief Get the byte size of the buffer
     */
    size_t size() const noexcept { return size_; }

private:
    void *data_ = nullptr;
    size_t size_ = 0;
};

/**
 * @brief Constant buffer.
 */
class ConstBuffer : public BufferBase {
public:
    ConstBuffer() = default;
    ConstBuffer(const void *data, size_t size) : data_(data), size_(size) {}

    ConstBuffer(MutableBuffer buf) : data_(buf.data()), size_(buf.size()) {}
    ConstBuffer &operator=(MutableBuffer buf) {
        data_ = buf.data();
        size_ = buf.size();
        return *this;
    }

    /**
     * @brief Get the data of the buffer
     */
    const void *data() const noexcept { return data_; }

    /**
     * @brief Get the byte size of the buffer
     */
    size_t size() const noexcept { return size_; }

private:
    const void *data_ = nullptr;
    size_t size_ = 0;
};

/**
 * @brief Create a buffer object from various data sources.
 * @details These overloaded functions construct either @ref MutableBuffer or
 * @ref ConstBuffer from raw pointers, arrays, std::array, std::vector,
 * std::string, std::string_view, or iovec. They provide a unified way to
 * convert different types of memory regions into buffer objects for use in
 * asynchronous operations.
 */
inline MutableBuffer buffer(void *data, size_t size) noexcept {
    return MutableBuffer(data, size);
}

/**
 * @copydoc buffer(void*, size_t)
 */
inline ConstBuffer buffer(const void *data, size_t size) noexcept {
    return ConstBuffer(data, size);
}

/**
 * @copydoc buffer(void*, size_t)
 */
template <typename PodType, size_t N>
inline MutableBuffer buffer(PodType (&arr)[N]) noexcept {
    return MutableBuffer(static_cast<void *>(arr), sizeof(PodType) * N);
}

/**
 * @copydoc buffer(void*, size_t)
 */
template <typename PodType, size_t N>
inline ConstBuffer buffer(const PodType (&arr)[N]) noexcept {
    return ConstBuffer(static_cast<const void *>(arr), sizeof(PodType) * N);
}

/**
 * @copydoc buffer(void*, size_t)
 */
template <typename PodType, size_t N>
inline MutableBuffer buffer(std::array<PodType, N> &arr) noexcept {
    return MutableBuffer(static_cast<void *>(arr.data()), sizeof(PodType) * N);
}

/**
 * @copydoc buffer(void*, size_t)
 */
template <typename PodType, size_t N>
inline ConstBuffer buffer(const std::array<PodType, N> &arr) noexcept {
    return ConstBuffer(static_cast<const void *>(arr.data()),
                       sizeof(PodType) * N);
}

/**
 * @copydoc buffer(void*, size_t)
 */
template <typename PodType>
inline MutableBuffer buffer(std::vector<PodType> &vec) noexcept {
    return MutableBuffer(static_cast<void *>(vec.data()),
                         sizeof(PodType) * vec.size());
}

/**
 * @copydoc buffer(void*, size_t)
 */
template <typename PodType>
inline ConstBuffer buffer(const std::vector<PodType> &vec) noexcept {
    return ConstBuffer(static_cast<const void *>(vec.data()),
                       sizeof(PodType) * vec.size());
}

/**
 * @copydoc buffer(void*, size_t)
 */
inline MutableBuffer buffer(std::string &str) noexcept {
    return MutableBuffer(static_cast<void *>(str.data()), str.size());
}

/**
 * @copydoc buffer(void*, size_t)
 */
inline ConstBuffer buffer(const std::string &str) noexcept {
    return ConstBuffer(static_cast<const void *>(str.data()), str.size());
}

/**
 * @copydoc buffer(void*, size_t)
 */
inline ConstBuffer buffer(std::string_view strv) noexcept {
    return ConstBuffer(static_cast<const void *>(strv.data()), strv.size());
}

/**
 * @copydoc buffer(void*, size_t)
 */
inline MutableBuffer buffer(iovec &iov) noexcept {
    return MutableBuffer(iov.iov_base, iov.iov_len);
}

/**
 * @copydoc buffer(void*, size_t)
 */
template <typename PodType, size_t N>
inline ConstBuffer buffer(std::span<const PodType, N> sp) noexcept {
    return ConstBuffer(static_cast<const void *>(sp.data()),
                       sp.size() * sizeof(PodType));
}

/**
 * @copydoc buffer(void*, size_t)
 */
template <typename PodType, size_t N>
inline MutableBuffer buffer(std::span<PodType, N> sp) noexcept {
    return MutableBuffer(static_cast<void *>(sp.data()),
                         sp.size() * sizeof(PodType));
}

namespace detail {

template <typename BufferPool> struct ManagedBuffer : public BufferBase {
public:
    ManagedBuffer() = default;
    ManagedBuffer(void *data, size_t size, BufferPool *pool)
        : data_(data), size_(size), pool_(pool) {}
    ManagedBuffer(ManagedBuffer &&other) noexcept
        : data_(std::exchange(other.data_, nullptr)),
          size_(std::exchange(other.size_, 0)),
          pool_(std::exchange(other.pool_, nullptr)) {}
    ManagedBuffer &operator=(ManagedBuffer &&other) noexcept {
        if (this != &other) {
            reset();
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
            pool_ = std::exchange(other.pool_, nullptr);
        }
        return *this;
    }

    ~ManagedBuffer() { reset(); }

    ManagedBuffer(const ManagedBuffer &) = delete;
    ManagedBuffer &operator=(const ManagedBuffer &) = delete;

public:
    /**
     * @brief Get the data pointer of the buffer
     */
    void *data() const noexcept { return data_; }

    /** *
     * @brief Get the size of the buffer
     */
    size_t size() const noexcept { return size_; }

    /**
     * @brief Reset the buffer, returning it to the pool if owned
     */
    void reset() noexcept {
        if (pool_ != nullptr) {
            pool_->add_buffer_back(data_, size_);
        }
        data_ = nullptr;
        size_ = 0;
        pool_ = nullptr;
    }

    /**
     * @brief Check if the buffer owns a buffer from a pool.
     */
    bool owns_buffer() const noexcept { return pool_ != nullptr; }

private:
    void *data_ = nullptr;
    size_t size_ = 0;
    BufferPool *pool_ = nullptr;
};

} // namespace detail

} // namespace condy