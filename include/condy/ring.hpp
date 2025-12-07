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

    void register_fd(unsigned index, int fd) { register_fd(index, &fd, 1); }

    void register_fd(unsigned index_base, const int *fds, unsigned nr_fds) {
        check_initialized_();
        int r = io_uring_register_files_update(&ring_, index_base, fds, nr_fds);
        if (r < 0) {
            throw_exception("io_uring_register_files_update failed", -r);
        }
    }

    auto async_register_fd(int *fds, unsigned nr_fds, int offset);

    auto async_get_raw_fd(int fixed_fd, unsigned int flags);

    void unregister_fd(unsigned fixed_fd) {
        check_initialized_();
        int invalid_fd = -1;
        int r =
            io_uring_register_files_update(&ring_, fixed_fd, &invalid_fd, 1);
        if (r < 0) {
            throw_exception("io_uring_register_files_update failed", -r);
        }
    }

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

    void register_buffer(unsigned index_base, const iovec *vecs,
                         unsigned nr_vecs) {
        check_initialized_();
        int r = io_uring_register_buffers_update_tag(&ring_, index_base, vecs,
                                                     nullptr, nr_vecs);
        if (r < 0) {
            throw_exception("io_uring_register_buffers_update failed", -r);
        }
    }

    template <typename Buffer>
    void register_buffer(unsigned index, Buffer &&buf) {
        iovec vec{const_cast<void *>(buf.data()), buf.size()};
        register_buffer(index, &vec, 1);
    }

    void unregister_buffer(unsigned index) {
        check_initialized_();
        iovec vec{nullptr, 0};
        int r = io_uring_register_buffers_update_tag(&ring_, index, &vec,
                                                     nullptr, 1);
        if (r < 0) {
            throw_exception("io_uring_register_buffers_update failed", -r);
        }
    }

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

    void set_submit_batch_size(size_t size) { submit_batch_size_ = size; }

    void maybe_submit() {
        if (unsubmitted_count_++ >= submit_batch_size_) {
            submit();
        }
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

    // Configuration
    size_t submit_batch_size_ = 128;

    uint32_t features_ = 0;
};

} // namespace condy