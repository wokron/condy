#pragma once

#include <cstddef>
#include <stdexcept>

namespace condy {

struct RuntimeOptions {
public:
    using Self = RuntimeOptions;

    Self &event_interval(size_t v) {
        event_interval_ = v;
        return *this;
    }

    Self &enable_sqpoll(size_t idle_time_ms = 1000) {
        if (enable_defer_taskrun_ || enable_coop_taskrun_) {
            throw std::logic_error(
                "sqpoll cannot be enabled with defer_taskrun or coop_taskrun");
        }
        enable_sqpoll_ = true;
        sqpoll_idle_time_ms_ = idle_time_ms;
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

    Self &enable_coop_taskrun() {
        if (enable_sqpoll_ || enable_defer_taskrun_) {
            throw std::logic_error(
                "coop_taskrun cannot be enabled with sqpoll or defer_taskrun");
        }
        enable_coop_taskrun_ = true;
        return *this;
    }

protected:
    size_t event_interval_ = 61;
    bool enable_sqpoll_ = false;
    size_t sqpoll_idle_time_ms_ = 1000;
    bool enable_defer_taskrun_ = false;
    size_t sq_size_ = 128;
    size_t cq_size_ = 1024;
    bool enable_coop_taskrun_ = false;

    friend class Runtime;
};

} // namespace condy