/**
 * @file ring.hpp
 * @brief Wrapper classes for liburing interfaces.
 * @details This file defines wrapper classes around liburing, providing support
 * for most synchronous operations.
 */

#pragma once

#include "condy/condy_uring.hpp"
#include "condy/utils.hpp"
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstring>

namespace condy {

/**
 * @brief File descriptor table for io_uring.
 * @details This class makes an abstraction over the io_uring file registration
 * interface.
 */
class FdTable {
public:
    FdTable(io_uring &ring) : ring_(ring) {}

    FdTable(const FdTable &) = delete;
    FdTable &operator=(const FdTable &) = delete;
    FdTable(FdTable &&) = delete;
    FdTable &operator=(FdTable &&) = delete;

public:
    /**
     * @brief Initialize the file descriptor table with the given capacity
     * @param capacity The number of file descriptors to allocate in the table
     * @return int Returns 0 on success or a negative error code on failure
     */
    int init(size_t capacity) {
        return io_uring_register_files_sparse(&ring_, capacity);
    }

    /**
     * @brief Destroy the file descriptor table
     * @return int Returns 0 on success or a negative error code on failure
     */
    int destroy() { return io_uring_unregister_files(&ring_); }

    /**
     * @brief Update the file descriptor table starting from the given index
     * @param index_base The starting index to update
     * @param fds Pointer to the array of file descriptors
     * @param nr_fds Number of file descriptors to update
     * @return int Returns 0 on success or a negative error code on failure
     */
    int update(unsigned index_base, const int *fds, unsigned nr_fds) {
        return io_uring_register_files_update(&ring_, index_base, fds, nr_fds);
    }

    /**
     * @brief Set the accepter function for incoming file descriptors
     * @details User can use @ref async_fixed_fd_send() to send a fixed fd to
     * the fd table of another Runtime. This function sets the accepter function
     * that will be called when such an operation is performed.
     * @tparam Func The type of the accepter function
     * @param accepter The accepter function to set, which accepts an int32_t
     * parameter representing the fixed file descriptor index being received.
     */
    template <typename Func> void set_fd_accepter(Func &&accepter) {
        fd_accepter_ = std::forward<Func>(accepter);
    }

    /**
     * @brief Set the file allocation range for the fd table
     * @param offset The starting offset of the file allocation range
     * @param size The size of the file allocation range
     * @return int Returns 0 on success or a negative error code on failure
     */
    int set_file_alloc_range(unsigned offset, unsigned size) {
        return io_uring_register_file_alloc_range(&ring_, offset, size);
    }

private:
    std::function<void(int32_t)> fd_accepter_ = nullptr;
    io_uring &ring_;

    friend class Runtime;
    friend auto async_fixed_fd_send(FdTable &dst, int source_fd, int target_fd,
                                    unsigned int flags);
};

/**
 * @brief Buffer table for io_uring.
 * @details This class makes an abstraction over the io_uring buffer
 * registration interface.
 */
class BufferTable {
public:
    BufferTable(io_uring &ring) : ring_(ring) {}

    BufferTable(const BufferTable &) = delete;
    BufferTable &operator=(const BufferTable &) = delete;
    BufferTable(BufferTable &&) = delete;
    BufferTable &operator=(BufferTable &&) = delete;

public:
    /**
     * @brief Initialize the buffer table with the given capacity
     * @param capacity The number of buffers to allocate in the table
     * @return int Returns 0 on success or a negative error code on failure
     */
    int init(size_t capacity) {
        int r = io_uring_register_buffers_sparse(&ring_, capacity);
        if (r < 0) {
            return r;
        }
        initialized_ = true;
        return r;
    }

    /**
     * @brief Destroy the buffer table
     * @return int Returns 0 on success or a negative error code on failure
     */
    int destroy() {
        initialized_ = false;
        return io_uring_unregister_buffers(&ring_);
    }

    /**
     * @brief Update the buffer table starting from the given index
     * @param index_base The starting index to update
     * @param vecs Pointer to the array of iovec structures representing buffers
     * @param nr_vecs Number of buffers to update
     * @return int Returns 0 on success or a negative error code on failure
     */
    int update(unsigned index_base, const iovec *vecs, unsigned nr_vecs) {
        return io_uring_register_buffers_update_tag(&ring_, index_base, vecs,
                                                    nullptr, nr_vecs);
    }

#if !IO_URING_CHECK_VERSION(2, 10) // >= 2.10

    /**
     * @brief Clone buffers from another BufferTable into this one
     * @param src The source BufferTable to clone from
     * @param dst_off The starting offset in the destination buffer table
     * @param src_off The starting offset in the source buffer table
     * @param nr The number of buffers to clone
     * @return int Returns 0 on success or a negative error code on failure
     */
    int clone_buffers(BufferTable &src, unsigned int dst_off = 0,
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

/**
 * @brief Settings manager for io_uring.
 * @details This class provides an interface to manage various runtime settings
 * for an io_uring instance, including personalities, restrictions, and other
 * features.
 */
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
    /**
     * @brief Set restrictions for the io_uring instance.
     * @details See io_uring_register_restrictions for more details.
     * @param res  Pointer to an array of restrictions.
     * @param nr_res Number of restrictions in the array.
     */
    int set_restrictions(io_uring_restriction *res, unsigned int nr_res) {
        return io_uring_register_restrictions(&ring_, res, nr_res);
    }

    /**
     * @brief Apply I/O worker queue affinity settings.
     * @details See io_uring_register_iowq_aff for more details.
     * @param cpusz Number of CPUs in the affinity mask.
     * @param mask Pointer to the CPU affinity mask.
     */
    int apply_iowq_aff(size_t cpusz, const cpu_set_t *mask) {
        return io_uring_register_iowq_aff(&ring_, cpusz, mask);
    }
    /**
     * @brief Remove I/O worker queue affinity settings.
     * @return int Returns 0 on success or a negative error code on failure
     */
    int remove_iowq_aff() { return io_uring_unregister_iowq_aff(&ring_); }

    /**
     * @brief Set the maximum number of I/O workers.
     * @details See io_uring_register_iowq_max_workers for more details.
     * @param values Pointer to an array with 2 elements representing the
     * max_workers
     */
    int set_iowq_max_workers(unsigned int *values) {
        return io_uring_register_iowq_max_workers(&ring_, values);
    }

    /**
     * @brief Get the io_uring probe for the ring.
     * @return io_uring_probe* Pointer to the io_uring probe structure. User
     * shall not free the returned pointer.
     */
    io_uring_probe *get_probe() {
        if (probe_) {
            return probe_;
        }
        probe_ = io_uring_get_probe_ring(&ring_);
        return probe_;
    }

    /**
     * @brief Get the supported features of the ring.
     * @return uint32_t Supported features bitmask.
     */
    uint32_t get_features() const { return features_; }

#if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6
    /**
     * @brief Apply NAPI settings to the io_uring instance.
     * @details See io_uring_register_napi for more details.
     * @param napi Pointer to the io_uring_napi structure.
     */
    int apply_napi(io_uring_napi *napi) {
        return io_uring_register_napi(&ring_, napi);
    }
    /**
     * @brief Remove NAPI settings from the io_uring instance.
     * @param napi Pointer to the io_uring_napi structure. Can be nullptr.
     */
    int remove_napi(io_uring_napi *napi = nullptr) {
        return io_uring_unregister_napi(&ring_, napi);
    }
#endif

#if !IO_URING_CHECK_VERSION(2, 8) // >= 2.8
    /**
     * @brief Set the clock registration for the io_uring instance.
     * @details See io_uring_register_clock for more details.
     * @param clock_reg Pointer to the io_uring_clock_register structure.
     */
    int set_clock(io_uring_clock_register *clock_reg) {
        return io_uring_register_clock(&ring_, clock_reg);
    }
#endif

#if !IO_URING_CHECK_VERSION(2, 9) // >= 2.9
    /**
     * @brief Resize the rings of the io_uring instance.
     * @details See io_uring_resize_rings for more details.
     * @param params Pointer to the io_uring_params structure.
     */
    int set_rings_size(io_uring_params *params) {
        return io_uring_resize_rings(&ring_, params);
    }
#endif

#if !IO_URING_CHECK_VERSION(2, 10) // >= 2.10
    /**
     * @brief Enable or disable iowait for the io_uring instance.
     * @details See io_uring_set_iowait for more details.
     * @param enable_iowait Boolean flag to enable or disable iowait mode.
     */
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

    template <typename Func> size_t reap_completions_wait(Func &&process_func) {
        unsigned head;
        io_uring_cqe *cqe;
        size_t reaped = 0;
        do {
            int r = io_uring_submit_and_wait(&ring_, 1);
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

    template <typename Func> size_t reap_completions(Func &&process_func) {
        unsigned head;
        io_uring_cqe *cqe;
        size_t reaped = 0;

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