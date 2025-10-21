#pragma once

#include <cstring>
#include <liburing.h>
#include <stdexcept>
#include <string>

namespace condy {

class Context {
public:
    struct InitParams {
        unsigned int io_uring_entries = 256;
        unsigned int io_uring_flags = 0;
    };

    Context(const Context &) = delete;
    Context &operator=(const Context &) = delete;
    Context(Context &&) = delete;
    Context &operator=(Context &&) = delete;

public:
    static Context &current() {
        static thread_local Context ctx;
        return ctx;
    }

    void init(InitParams params) {
        int r = io_uring_queue_init(params.io_uring_entries, &ring_,
                                    params.io_uring_flags);
        if (r < 0) {
            throw std::runtime_error("io_uring_queue_init failed: " +
                                     std::string(std::strerror(-r)));
        }
    }

    void destroy() { io_uring_queue_exit(&ring_); }

    io_uring *get_ring() { return &ring_; }

private:
    Context() = default;

private:
    io_uring ring_{};
};

} // namespace condy