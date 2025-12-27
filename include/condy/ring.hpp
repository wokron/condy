#pragma once

#include "condy/condy_uring.hpp"
#include "condy/utils.hpp"
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstring>

namespace condy {

class FdTable {
public:
    FdTable(io_uring &ring) : ring_(ring) {}

    FdTable(const FdTable &) = delete;
    FdTable &operator=(const FdTable &) = delete;
    FdTable(FdTable &&) = delete;
    FdTable &operator=(FdTable &&) = delete;

public:
    int init(size_t capacity) {
        int r = io_uring_register_files_sparse(&ring_, capacity);
        if (r < 0) {
            return r;
        }
        capacity_ = capacity;
        alloc_range_offset_ = 0;
        alloc_range_size_ = capacity;
        return r;
    }

    int destroy() { return io_uring_unregister_files(&ring_); }

    int update_files(unsigned index_base, const int *fds, unsigned nr_fds) {
        return io_uring_register_files_update(&ring_, index_base, fds, nr_fds);
    }

    auto async_update_files(int *fds, unsigned nr_fds, int offset);

    auto async_fixed_fd_install(int fixed_fd, unsigned int flags);

    int set_alloc_range(unsigned offset, unsigned size) {
        alloc_range_offset_ = offset;
        alloc_range_size_ = size;
        return io_uring_register_file_alloc_range(&ring_, offset, size);
    }

    std::pair<unsigned, unsigned> get_alloc_range() const {
        return {alloc_range_offset_, alloc_range_size_};
    }

    size_t capacity() const { return capacity_; }

private:
    size_t capacity_ = 0;
    unsigned alloc_range_offset_ = 0;
    unsigned alloc_range_size_ = 0;
    io_uring &ring_;
};

class BufferTable {
public:
    BufferTable(io_uring &ring) : ring_(ring) {}

    BufferTable(const BufferTable &) = delete;
    BufferTable &operator=(const BufferTable &) = delete;
    BufferTable(BufferTable &&) = delete;
    BufferTable &operator=(BufferTable &&) = delete;

public:
    int init(size_t capacity) {
        int r = io_uring_register_buffers_sparse(&ring_, capacity);
        if (r < 0) {
            return r;
        }
        capacity_ = capacity;
        initialized_ = true;
        return r;
    }

    int destroy() {
        initialized_ = false;
        return io_uring_unregister_buffers(&ring_);
    }

    int update_buffers(unsigned index_base, const iovec *vecs,
                       unsigned nr_vecs) {
        return io_uring_register_buffers_update_tag(&ring_, index_base, vecs,
                                                    nullptr, nr_vecs);
    }

#if !IO_URING_CHECK_VERSION(2, 10) // >= 2.10
    int clone_from(BufferTable &src, unsigned int dst_off = 0,
                   unsigned int src_off = 0, unsigned int nr = 0) {
        auto *src_ring = &src.ring_;
        auto *dst_ring = &ring_;
        unsigned int flags = 0;
        if (initialized_) {
            flags |= IORING_REGISTER_DST_REPLACE;
        }
        int r = __io_uring_clone_buffers_offset(dst_ring, src_ring, dst_off,
                                                src_off, nr, flags);
        if (r < 0) {
            return r;
        }
        bool is_full_clone = dst_off == 0 && src_off == 0 && nr == 0;
        size_t clone_cap = is_full_clone ? src.capacity_ : dst_off + nr;
        capacity_ = std::max(capacity_, clone_cap);
        initialized_ = true;
        return r;
    }
#endif

    size_t capacity() const { return capacity_; }

private:
    bool initialized_ = false;
    size_t capacity_ = 0;
    io_uring &ring_;
};

#if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6
class NAPI {
public:
    NAPI(io_uring &ring) : ring_(ring) {}

    NAPI(const NAPI &) = delete;
    NAPI &operator=(const NAPI &) = delete;
    NAPI(NAPI &&) = delete;
    NAPI &operator=(NAPI &&) = delete;

public:
    int init(io_uring_napi *params) {
        return io_uring_register_napi(&ring_, params);
    }

    int destroy(io_uring_napi *params = nullptr) {
        return io_uring_unregister_napi(&ring_, params);
    }

private:
    io_uring &ring_;
};
#else
class NAPI {
public:
    NAPI(io_uring &) {}

    NAPI(const NAPI &) = delete;
    NAPI &operator=(const NAPI &) = delete;
    NAPI(NAPI &&) = delete;
    NAPI &operator=(NAPI &&) = delete;
};
#endif

class Ring {
public:
    Ring() = default;
    ~Ring() { destroy(); }

    Ring(const Ring &) = delete;
    Ring &operator=(const Ring &) = delete;
    Ring(Ring &&) = delete;
    Ring &operator=(Ring &&) = delete;

public:
    int init(unsigned int entries, io_uring_params *params,
             [[maybe_unused]] void *buf = nullptr,
             [[maybe_unused]] size_t buf_size = 0) {
        int r;
        assert(!initialized_);
#if !IO_URING_CHECK_VERSION(2, 5) // >= 2.5
        if (params->flags & IORING_SETUP_NO_MMAP) {
            r = io_uring_queue_init_mem(entries, &ring_, params, buf, buf_size);
        } else
#endif
            r = io_uring_queue_init_params(entries, &ring_, params);
        if (r < 0) {
            return r;
        }
        features_ = params->features;
        sqpoll_mode_ = (params->flags & IORING_SETUP_SQPOLL) != 0;
        initialized_ = true;
        return r;
    }

    void destroy() {
        if (initialized_) {
            io_uring_queue_exit(&ring_);
            initialized_ = false;
        }
    }

    void submit() { io_uring_submit(&ring_); }

    template <typename Func>
    size_t reap_completions(Func &&process_func, bool submit_and_wait = false) {
        int r;
        unsigned head;
        io_uring_cqe *cqe;
        size_t reaped = 0;
        if (submit_and_wait) {
            do {
                r = io_uring_submit_and_wait(&ring_, 1);
                if (r >= 0) [[likely]] {
                    break;
                } else if (r == -EINTR) {
                    continue;
                } else {
                    throw make_system_error("io_uring_submit_and_wait", -r);
                }
            } while (true);

            io_uring_for_each_cqe(&ring_, head, cqe) {
                process_func(cqe);
                reaped++;
            }
            io_uring_cq_advance(&ring_, reaped);
            return reaped;
        }

        if (io_uring_peek_cqe(&ring_, &cqe) == 0) {
            io_uring_for_each_cqe(&ring_, head, cqe) {
                process_func(cqe);
                reaped++;
            }
            io_uring_cq_advance(&ring_, reaped);
        }

        return reaped;
    }

    void reserve_space(size_t n) {
        size_t space_left;
        do {
            space_left = io_uring_sq_space_left(&ring_);
            if (space_left >= n) {
                return;
            }
            submit();
        } while (true);
    }

    io_uring *ring() { return &ring_; }

    FdTable &fd_table() { return fd_table_; }

    BufferTable &buffer_table() { return buffer_table_; }

    NAPI &napi() { return napi_; }

    io_uring_sqe *get_sqe() {
        [[maybe_unused]] int r;
        io_uring_sqe *sqe;
        do {
            sqe = io_uring_get_sqe(&ring_);
            if (sqe) {
                break;
            }
            r = io_uring_submit(&ring_);
            assert(r >= 0);
            if (sqpoll_mode_) {
                r = io_uring_sqring_wait(&ring_);
                assert(r >= 0);
            }
        } while (true);
        return sqe;
    }

    uint32_t features() const { return features_; }

private:
    bool initialized_ = false;
    io_uring ring_;
    bool sqpoll_mode_ = false;

    FdTable fd_table_{ring_};
    BufferTable buffer_table_{ring_};
    NAPI napi_{ring_};

    uint32_t features_ = 0;
};

} // namespace condy