#pragma once

#include "condy/condy_uring.hpp"
#include "condy/strategies.hpp"
#include <cstring>
#include <stdexcept>
#include <string>

namespace condy {

struct IStrategy;

class Context {
public:
    Context(const Context &) = delete;
    Context &operator=(const Context &) = delete;
    Context(Context &&) = delete;
    Context &operator=(Context &&) = delete;

public:
    static Context &current() {
        static thread_local Context ctx;
        return ctx;
    }

    void init(IStrategy *strategy) {
        int r = strategy->init_io_uring(&ring_);
        if (r < 0) {
            throw std::runtime_error("io_uring_queue_init failed: " +
                                     std::string(std::strerror(-r)));
        }
        strategy_ = strategy;
    }

    void destroy() {
        io_uring_queue_exit(&ring_);
        strategy_ = nullptr;
    }

    io_uring *get_ring() { return &ring_; }

    IStrategy *get_strategy() { return strategy_; }

private:
    Context() = default;

private:
    io_uring ring_{};
    IStrategy *strategy_ = nullptr;
};

} // namespace condy