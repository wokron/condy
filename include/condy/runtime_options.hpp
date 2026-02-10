/**
 * @file runtime_options.hpp
 */

#pragma once

#include "condy/condy_uring.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace condy {

class Runtime;

/**
 * @brief Runtime options
 * @details Options for configuring the behavior of a Runtime instance. Most of
 * them are io_uring setup options. These options should be set before creating
 * the Runtime instance.
 */
struct RuntimeOptions {
public:
    using Self = RuntimeOptions;

    /**
     * @brief Set event interval
     * @details The event interval determines how often the runtime checks for
     * completed events.
     * @param v The event interval value
     */
    Self &event_interval(size_t v) {
        event_interval_ = v;
        return *this;
    }

    /**
     * @brief Disable register ring fd
     * @details By default, the runtime registers the ring file descriptor with
     * the kernel for performance optimization. This option disables that
     * behavior.
     */
    Self &disable_register_ring_fd() {
        disable_register_ring_fd_ = true;
        return *this;
    }

    /**
     * @brief See IORING_SETUP_IOPOLL
     * @param hybrid See IORING_SETUP_HYBRID_IOPOLL
     */
    Self &enable_iopoll(bool hybrid = false) {
        enable_iopoll_ = true;
        enable_hybrid_iopoll_ = hybrid;
        return *this;
    }

    /**
     * @brief See IORING_SETUP_SQPOLL
     * @param idle_time_ms Idle time in milliseconds for the sqpoll thread
     * @param cpu CPU affinity for the sqpoll thread
     */
    Self &enable_sqpoll(size_t idle_time_ms = 1000,
                        std::optional<uint32_t> cpu = std::nullopt) {
        if (enable_defer_taskrun_ || enable_coop_taskrun_) {
            throw std::logic_error(
                "sqpoll cannot be enabled with defer_taskrun or coop_taskrun");
        }
        enable_sqpoll_ = true;
        sqpoll_idle_time_ms_ = idle_time_ms;
        sqpoll_thread_cpu_ = cpu;
        return *this;
    }

    /**
     * @brief See IORING_SETUP_DEFER_TASKRUN and IORING_SETUP_TASKRUN_FLAG
     * @return Self&
     */
    Self &enable_defer_taskrun() {
        if (enable_sqpoll_ || enable_coop_taskrun_) {
            throw std::logic_error(
                "defer_taskrun cannot be enabled with sqpoll or coop_taskrun");
        }
        enable_defer_taskrun_ = true;
        return *this;
    }

    /**
     * @brief Set SQ size
     * @param v SQ size
     */
    Self &sq_size(size_t v) {
        sq_size_ = v;
        return *this;
    }

    /**
     * @brief Set CQ size
     * @param v CQ size
     */
    Self &cq_size(size_t v) {
        cq_size_ = v;
        return *this;
    }

    /**
     * @brief See IORING_SETUP_ATTACH_WQ
     * @details This option allows the current runtime to share the async
     * worker thread backend with another runtime.
     * @param other The other runtime to attach to.
     */
    Self &enable_attach_wq(Runtime &other) {
        attach_wq_target_ = &other;
        return *this;
    }

    /**
     * @brief See IORING_SETUP_COOP_TASKRUN and IORING_SETUP_TASKRUN_FLAG
     * @return Self&
     */
    Self &enable_coop_taskrun() {
        if (enable_sqpoll_ || enable_defer_taskrun_) {
            throw std::logic_error(
                "coop_taskrun cannot be enabled with sqpoll or defer_taskrun");
        }
        enable_coop_taskrun_ = true;
        return *this;
    }

    /**
     * @brief See IORING_SETUP_COOP_TASKRUN
     * @param taskrun_flag See IORING_SETUP_TASKRUN_FLAG
     * @return Self&
     * @deprecated Use enable_coop_taskrun() without parameters instead
     */
    [[deprecated("Use enable_coop_taskrun() without parameters instead")]]
    Self &enable_coop_taskrun([[maybe_unused]] bool taskrun_flag) {
        enable_coop_taskrun();
        return *this;
    }

    /**
     * @brief See IORING_SETUP_SQE128
     */
    Self &enable_sqe128() {
        if (enable_sqe_mixed_) {
            throw std::logic_error("sqe128 cannot be enabled with sqe_mixed");
        }
        enable_sqe128_ = true;
        return *this;
    }

    /**
     * @brief See IORING_SETUP_CQE32
     */
    Self &enable_cqe32() {
        if (enable_cqe_mixed_) {
            throw std::logic_error("cqe32 cannot be enabled with cqe_mixed");
        }
        enable_cqe32_ = true;
        return *this;
    }

#if !IO_URING_CHECK_VERSION(2, 13) // >= 2.13
    /**
     * @brief See IORING_SETUP_SQE_MIXED
     */
    Self &enable_sqe_mixed() {
        if (enable_sqe128_) {
            throw std::logic_error("sqe_mixed cannot be enabled with sqe128");
        }
        enable_sqe_mixed_ = true;
        return *this;
    }
#endif

#if !IO_URING_CHECK_VERSION(2, 13) // >= 2.13
    /**
     * @brief See IORING_SETUP_CQE_MIXED
     */
    Self &enable_cqe_mixed() {
        if (enable_cqe32_) {
            throw std::logic_error("cqe_mixed cannot be enabled with cqe32");
        }
        enable_cqe_mixed_ = true;
        return *this;
    }
#endif

#if !IO_URING_CHECK_VERSION(2, 5) // >= 2.5
    /**
     * @brief See IORING_SETUP_NO_MMAP
     * @param buf Buffer pointer
     * @param buf_size Buffer size
     */
    Self &enable_no_mmap(void *buf = nullptr, size_t buf_size = 0) {
        enable_no_mmap_ = true;
        no_mmap_buf_ = buf;
        no_mmap_buf_size_ = buf_size;
        return *this;
    }
#endif

protected:
    size_t event_interval_ = 61;
    bool disable_register_ring_fd_ = false;
    bool enable_iopoll_ = false;
    bool enable_hybrid_iopoll_ = false;
    bool enable_sqpoll_ = false;
    size_t sqpoll_idle_time_ms_ = 1000;
    std::optional<uint32_t> sqpoll_thread_cpu_ = std::nullopt;
    bool enable_defer_taskrun_ = false;
    size_t sq_size_ = 128;
    size_t cq_size_ = 0; // 0 means default
    Runtime *attach_wq_target_ = nullptr;
    bool enable_coop_taskrun_ = false;
    bool enable_sqe128_ = false;
    bool enable_cqe32_ = false;
    bool enable_sqe_mixed_ = false;
    bool enable_cqe_mixed_ = false;
    bool enable_no_mmap_ = false;
    void *no_mmap_buf_ = nullptr;
    size_t no_mmap_buf_size_ = 0;

    friend class Runtime;
};

} // namespace condy