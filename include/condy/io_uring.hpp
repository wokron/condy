#pragma once

#include <cstring>
#include <liburing.h>
#include <memory>

namespace condy {

struct io_uring_deleter {
    void operator()(io_uring *ring) const {
        if (ring) {
            io_uring_queue_exit(ring);
            delete ring;
        }
    }
};

using io_uring_ptr = std::unique_ptr<io_uring, io_uring_deleter>;

inline io_uring_ptr make_io_uring(unsigned entries, unsigned flags) {
    int r;
    auto ring = new io_uring;
    r = io_uring_queue_init(entries, ring, flags);
    if (r < 0) {
        delete ring;
        throw std::runtime_error("io_uring_queue_init failed: " +
                                 std::string(std::strerror(-r)));
    }

    return io_uring_ptr{ring};
}

} // namespace condy