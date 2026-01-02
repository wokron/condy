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
        return io_uring_register_files_sparse(&ring_, capacity);
    }

    int destroy() { return io_uring_unregister_files(&ring_); }

    int update_files(unsigned index_base, const int *fds, unsigned nr_fds) {
        return io_uring_register_files_update(&ring_, index_base, fds, nr_fds);
    }

    template <typename Func> void set_fd_accepter(Func &&accepter) {
        fd_accepter_ = std::forward<Func>(accepter);
    }

    int set_file_alloc_range(unsigned offset, unsigned size) {
        return io_uring_register_file_alloc_range(&ring_, offset, size);
    }

private:
    std::function<void(int32_t)> fd_accepter_ = nullptr;
    io_uring &ring_;

    friend class Runtime;
    friend auto async_send_fd_to(FdTable &dst, int source_fd, int target_fd,
                                 unsigned int flags);
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
    int clone_buffers_from(BufferTable &src, unsigned int dst_off = 0,
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
        initialized_ = true;
        return r;
    }
#endif

private:
    io_uring &ring_;
    bool initialized_ = false;
};

class RingSettings {
public:
    RingSettings(io_uring &ring) : ring_(ring) {}

    ~RingSettings() {
        if (probe_) {
            io_uring_free_probe(probe_);
            probe_ = nullptr;
        }
    }

    RingSettings(const RingSettings &) = delete;
    RingSettings &operator=(const RingSettings &) = delete;
    RingSettings(RingSettings &&) = delete;
    RingSettings &operator=(RingSettings &&) = delete;

public:
    int apply_personality() { return io_uring_register_personality(&ring_); }
    int remove_personality(int id) {
        return io_uring_unregister_personality(&ring_, id);
    }

    int set_restrictions(io_uring_restriction *res, unsigned int nr_res) {
        return io_uring_register_restrictions(&ring_, res, nr_res);
    }

    int apply_iowq_aff(size_t cpusz, const cpu_set_t *mask) {
        return io_uring_register_iowq_aff(&ring_, cpusz, mask);
    }
    int remove_iowq_aff() { return io_uring_unregister_iowq_aff(&ring_); }

    int set_iowq_max_workers(unsigned int *values) {
        return io_uring_register_iowq_max_workers(&ring_, values);
    }

    io_uring_probe *get_probe() {
        if (probe_) {
            return probe_;
        }
        probe_ = io_uring_get_probe_ring(&ring_);
        return probe_;
    }

    uint32_t get_features() const { return features_; }

#if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6
    int apply_napi(io_uring_napi *napi) {
        return io_uring_register_napi(&ring_, napi);
    }
    int remove_napi(io_uring_napi *napi = nullptr) {
        return io_uring_unregister_napi(&ring_, napi);
    }
#endif

#if !IO_URING_CHECK_VERSION(2, 8) // >= 2.8
    int set_clock(io_uring_clock_register *clock_reg) {
        return io_uring_register_clock(&ring_, clock_reg);
    }
#endif

#if !IO_URING_CHECK_VERSION(2, 9) // >= 2.9
    int set_rings_size(io_uring_params *params) {
        return io_uring_resize_rings(&ring_, params);
    }
#endif

#if !IO_URING_CHECK_VERSION(2, 10) // >= 2.10
    int set_iowait(bool enable_iowait) {
        return io_uring_set_iowait(&ring_, enable_iowait);
    }
#endif

private:
    io_uring &ring_;
    io_uring_probe *probe_ = nullptr;
    uint32_t features_ = 0;

    friend class Ring;
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
        settings_.features_ = params->features;
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

    RingSettings &settings() { return settings_; }

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

private:
    bool initialized_ = false;
    io_uring ring_;
    bool sqpoll_mode_ = false;

    FdTable fd_table_{ring_};
    BufferTable buffer_table_{ring_};
    RingSettings settings_{ring_};
};

} // namespace condy