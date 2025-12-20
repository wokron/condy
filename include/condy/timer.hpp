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
        auto prep_func = [ts, count, flags](auto sqe) {
            io_uring_prep_timeout(sqe, ts, count, flags);
        };
        return TimerOpAwaiter(&finish_handle_, prep_func);
    }

    auto async_wait_linked(struct __kernel_timespec *ts, unsigned flags) {
        auto prep_func = [ts, flags](auto sqe) {
            io_uring_prep_link_timeout(sqe, ts, flags);
        };
        return TimerOpAwaiter(&finish_handle_, prep_func);
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