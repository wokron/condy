#pragma once

#include "condy/condy_uring.hpp"
#include "condy/queue.hpp"
#include "condy/strategies.hpp"
#include <cstring>
#include <stdexcept>
#include <string>

namespace condy {

struct IStrategy;
struct OpFinishHandle;

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

    void init(IStrategy *strategy,
              SingleThreadRingQueue<OpFinishHandle *> *ready_queue) {
        int r = strategy->init_io_uring(&ring_);
        if (r < 0) {
            throw std::runtime_error("io_uring_queue_init failed: " +
                                     std::string(std::strerror(-r)));
        }
        strategy_ = strategy;
        ready_queue_ = ready_queue;
    }

    void destroy() {
        io_uring_queue_exit(&ring_);
        strategy_ = nullptr;
        ready_queue_ = nullptr;
    }

    io_uring *get_ring() { return &ring_; }

    IStrategy *get_strategy() { return strategy_; }

    SingleThreadRingQueue<OpFinishHandle *> *get_ready_queue() {
        return ready_queue_;
    }

private:
    Context() = default;

private:
    io_uring ring_{};
    IStrategy *strategy_ = nullptr;

    SingleThreadRingQueue<OpFinishHandle *> *ready_queue_ = nullptr;
};

} // namespace condy