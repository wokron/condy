#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <sys/uio.h>
#include <vector>

namespace condy {

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

inline ConstBuffer buffer(std::string_view strv) {
    return ConstBuffer(static_cast<const void *>(strv.data()), strv.size());
}

inline MutableBuffer buffer(iovec &iov) {
    return MutableBuffer(iov.iov_base, iov.iov_len);
}

} // namespace condy