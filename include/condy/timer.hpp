#pragma once

#include "condy/awaiter_operations.hpp"
#include "condy/awaiters.hpp"
#include "condy/condy_uring.hpp"
#include "condy/finish_handles.hpp"

namespace condy {

class Timer {
public:
    Timer() = default;
    Timer(const Timer &) = delete;
    Timer &operator=(const Timer &) = delete;
    Timer(Timer &&) = delete;
    Timer &operator=(Timer &&) = delete;

    ~Timer() = default;

public:
    auto async_wait(struct __kernel_timespec *ts, unsigned count,
                    unsigned flags) {
        return TimerOpAwaiter(&finish_handle_, io_uring_prep_timeout, ts, count,
                              flags);
    }

    auto async_update(struct __kernel_timespec *ts, unsigned flags) {
        return make_op_awaiter(io_uring_prep_timeout_update, ts,
                               reinterpret_cast<__u64>(&finish_handle_), flags);
    }

    auto async_remove(unsigned flags) {
        return make_op_awaiter(io_uring_prep_timeout_remove,
                               reinterpret_cast<__u64>(&finish_handle_), flags);
    }

private:
    TimerFinishHandle finish_handle_;
};

} // namespace condy