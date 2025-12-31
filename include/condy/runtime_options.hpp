#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace condy {

class Runtime;

// TODO: Check versions for each option
struct RuntimeOptions {
public:
    using Self = RuntimeOptions;

    Self &event_interval(size_t v) {
        event_interval_ = v;
        return *this;
    }

    Self &disable_register_ring_fd() {
        disable_register_ring_fd_ = true;
        return *this;
    }

    Self &enable_iopoll(bool hybrid = false) {
        enable_iopoll_ = true;
        enable_hybrid_iopoll_ = hybrid;
        return *this;
    }

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

    Self &enable_defer_taskrun() {
        if (enable_sqpoll_ || enable_coop_taskrun_) {
            throw std::logic_error(
                "defer_taskrun cannot be enabled with sqpoll or coop_taskrun");
        }
        enable_defer_taskrun_ = true;
        return *this;
    }

    Self &sq_size(size_t v) {
        sq_size_ = v;
        return *this;
    }

    Self &cq_size(size_t v) {
        cq_size_ = v;
        return *this;
    }

    Self &enable_attach_wq(Runtime &other) {
        attach_wq_target_ = &other;
        return *this;
    }

    Self &enable_coop_taskrun(bool taskrun_flag = false) {
        if (enable_sqpoll_ || enable_defer_taskrun_) {
            throw std::logic_error(
                "coop_taskrun cannot be enabled with sqpoll or defer_taskrun");
        }
        enable_coop_taskrun_ = true;
        enable_coop_taskrun_flag_ = taskrun_flag;
        return *this;
    }

    Self &enable_sqe128() {
        if (enable_sqe_mixed_) {
            throw std::logic_error("sqe128 cannot be enabled with sqe_mixed");
        }
        enable_sqe128_ = true;
        return *this;
    }

    Self &enable_cqe32() {
        if (enable_cqe_mixed_) {
            throw std::logic_error("cqe32 cannot be enabled with cqe_mixed");
        }
        enable_cqe32_ = true;
        return *this;
    }

    Self &enable_sqe_mixed() {
        if (enable_sqe128_) {
            throw std::logic_error("sqe_mixed cannot be enabled with sqe128");
        }
        enable_sqe_mixed_ = true;
        return *this;
    }

    Self &enable_cqe_mixed() {
        if (enable_cqe32_) {
            throw std::logic_error("cqe_mixed cannot be enabled with cqe32");
        }
        enable_cqe_mixed_ = true;
        return *this;
    }

    Self &enable_no_mmap(void *buf = nullptr, size_t buf_size = 0) {
        enable_no_mmap_ = true;
        no_mmap_buf_ = buf;
        no_mmap_buf_size_ = buf_size;
        return *this;
    }

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
    bool enable_coop_taskrun_flag_ = false;
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