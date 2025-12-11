#pragma once

#include "condy/condy_uring.hpp"
#include "condy/utils.hpp"
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <stdexcept>

namespace condy {

class FdTable {
public:
    FdTable(io_uring &ring) : ring_(ring) {}

    FdTable(const FdTable &) = delete;
    FdTable &operator=(const FdTable &) = delete;
    FdTable(FdTable &&) = delete;
    FdTable &operator=(FdTable &&) = delete;

public:
    void init(size_t capacity) {
        int r = io_uring_register_files_sparse(&ring_, capacity);
        if (r < 0) {
            throw_exception("io_uring_register_files_sparse failed", -r);
        }
        capacity_ = capacity;
        alloc_range_offset_ = 0;
        alloc_range_size_ = capacity;
        initialized_ = true;
    }

    void destroy() {
        if (initialized_) {
            int r = io_uring_unregister_files(&ring_);
            if (r < 0) {
                throw_exception("io_uring_unregister_files failed", -r);
            }
            initialized_ = false;
        }
    }

    int update_files(unsigned index_base, const int *fds, unsigned nr_fds) {
        check_initialized_();
        int r = io_uring_register_files_update(&ring_, index_base, fds, nr_fds);
        if (r < 0) {
            throw_exception("io_uring_register_files_update failed", -r);
        }
        return r;
    }

    auto async_update_files(int *fds, unsigned nr_fds, int offset);

    auto async_fixed_fd_install(int fixed_fd, unsigned int flags);

    void set_alloc_range(unsigned offset, unsigned size) {
        check_initialized_();
        alloc_range_offset_ = offset;
        alloc_range_size_ = size;
        int r = io_uring_register_file_alloc_range(&ring_, offset, size);
        if (r < 0) {
            throw_exception("io_uring_register_file_alloc_range failed", -r);
        }
    }

    std::pair<unsigned, unsigned> get_alloc_range() const {
        return {alloc_range_offset_, alloc_range_size_};
    }

    size_t capacity() const { return capacity_; }

private:
    void check_initialized_() {
        if (!initialized_) {
            throw std::logic_error("FdTable not initialized");
        }
    }

private:
    bool initialized_ = false;
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
    void init(size_t capacity) {
        int r = io_uring_register_buffers_sparse(&ring_, capacity);
        if (r < 0) {
            throw_exception("io_uring_register_buffers_sparse failed", -r);
        }
        capacity_ = capacity;
        initialized_ = true;
    }

    void destroy() {
        if (initialized_) {
            int r = io_uring_unregister_buffers(&ring_);
            if (r < 0) {
                throw_exception("io_uring_unregister_buffers failed", -r);
            }
            initialized_ = false;
        }
    }

    int update_buffers(unsigned index_base, const iovec *vecs,
                       unsigned nr_vecs) {
        check_initialized_();
        int r = io_uring_register_buffers_update_tag(&ring_, index_base, vecs,
                                                     nullptr, nr_vecs);
        if (r < 0) {
            throw_exception("io_uring_register_buffers_update failed", -r);
        }
        return r;
    }

#if !IO_URING_CHECK_VERSION(2, 10) // >= 2.10
    void clone_from(BufferTable &src, unsigned int dst_off = 0,
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
            throw_exception("io_uring_clone_buffers_offset failed", -r);
        }
        bool is_full_clone = dst_off == 0 && src_off == 0 && nr == 0;
        size_t clone_cap = is_full_clone ? src.capacity_ : dst_off + nr;
        capacity_ = std::max(capacity_, clone_cap);
        initialized_ = true;
    }
#endif

    size_t capacity() const { return capacity_; }

private:
    void check_initialized_() {
        if (!initialized_) {
            throw std::logic_error("BufferTable not initialized");
        }
    }

private:
    bool initialized_ = false;
    size_t capacity_ = 0;
    io_uring &ring_;
};

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

class Ring {
public:
    Ring() = default;
    ~Ring() { destroy(); }

    Ring(const Ring &) = delete;
    Ring &operator=(const Ring &) = delete;
    Ring(Ring &&) = delete;
    Ring &operator=(Ring &&) = delete;

public:
    void init(unsigned int entries, io_uring_params *params) {
        int r;
        assert(!initialized_);
        r = io_uring_queue_init_params(entries, &ring_, params);
        if (r < 0) {
            throw_exception("io_uring_queue_init_params failed", -r);
        }
        features_ = params->features;
        sqpoll_mode_ = (params->flags & IORING_SETUP_SQPOLL) != 0;
        initialized_ = true;
    }

    void destroy() {
        if (initialized_) {
            io_uring_queue_exit(&ring_);
            initialized_ = false;
        }
    }

    void submit() {
        unsubmitted_count_ = 0;
        io_uring_submit(&ring_);
    }

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
                    throw_exception("io_uring_submit_and_wait failed", -r);
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
        int r;
        io_uring_sqe *sqe;
        do {
            sqe = io_uring_get_sqe(&ring_);
            if (sqe) {
                break;
            }
            r = io_uring_submit(&ring_);
            if (r < 0) {
                throw_exception("io_uring_submit failed", -r);
            }
            unsubmitted_count_ = 0;
            if (sqpoll_mode_) {
                r = io_uring_sqring_wait(&ring_);
                if (r < 0) {
                    throw_exception("io_uring_sqring_wait failed", -r);
                }
            }
        } while (true);
        return sqe;
    }

    uint32_t features() const { return features_; }

private:
    bool initialized_ = false;
    io_uring ring_;
    bool sqpoll_mode_ = false;
    size_t unsubmitted_count_ = 0;

    FdTable fd_table_{ring_};
    BufferTable buffer_table_{ring_};
    NAPI napi_{ring_};

    uint32_t features_ = 0;
};

} // namespace condy