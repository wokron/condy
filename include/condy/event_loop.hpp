#pragma once

#include "condy/condy_uring.hpp"
#include "condy/context.hpp"
#include "condy/coro.hpp"
#include "condy/finish_handles.hpp"
#include "condy/queue.hpp"
#include "condy/strategies.hpp"
#include "condy/utils.hpp"
#include <atomic>
#include <cerrno>
#include <memory>

namespace condy {

class EventLoop {
public:
    EventLoop(std::unique_ptr<IStrategy> strategy)
        : strategy_(std::move(strategy)),
          inner_ready_queue_(strategy_->get_ready_queue_capacity()) {}

    EventLoop(const EventLoop &) = delete;
    EventLoop &operator=(const EventLoop &) = delete;
    EventLoop(EventLoop &&) = delete;
    EventLoop &operator=(EventLoop &&) = delete;

public:
    template <typename T> void run(Coro<T> entry_point);

    void stop() { state_.store(State::STOPPED, std::memory_order_release); }

public:
    enum class State { IDLE, RUNNING, STOPPED };

    State current_state() const {
        return state_.load(std::memory_order_acquire);
    }
    bool check_running() const { return current_state() == State::RUNNING; }
    bool check_stopped() const { return current_state() == State::STOPPED; }
    bool check_idle() const { return current_state() == State::IDLE; }

private:
    bool should_stop_() const {
        return check_stopped() || strategy_->should_stop();
    }

private:
    std::unique_ptr<IStrategy> strategy_;
    std::atomic<State> state_ = State::IDLE;
    SingleThreadRingQueue<OpFinishHandle *> inner_ready_queue_;
};

template <typename T> void EventLoop::run(Coro<T> entry_point) {
    State expected = State::IDLE;
    if (!state_.compare_exchange_strong(expected, State::RUNNING,
                                        std::memory_order_acq_rel)) {
        throw std::runtime_error("EventLoop is already running or stopped");
    }

    auto d1 = defer(
        [&]() { state_.store(State::STOPPED, std::memory_order_release); });

    Context::current().init(strategy_.get(), &inner_ready_queue_);
    auto d2 = defer([]() { Context::current().destroy(); });

    auto *ring = Context::current().get_ring();

    auto handle = entry_point.release();
    handle.promise().set_task_id(strategy_->generate_task_id());
    handle.resume();

    while (!should_stop_()) {
        std::optional<OpFinishHandle *> ready_handle;
        while ((ready_handle = inner_ready_queue_.try_dequeue())) {
            (*ready_handle)->finish(0);
        }

        int r = strategy_->submit_and_wait(ring);
        if (r == -EINTR) {
            continue;
        } else if (r < 0 && r != -ETIME) {
            throw std::runtime_error("io_uring_submit_and_wait failed: " +
                                     std::string(std::strerror(-r)));
        }
        int submitted = r;
        strategy_->record_submitted(submitted);

        if (*ring->cq.koverflow) {
            throw std::runtime_error("CQ overflow detected");
        }

        unsigned int head;
        int finished = 0;
        io_uring_cqe *cqe;
        io_uring_for_each_cqe(ring, head, cqe) {
            auto handle_ptr =
                static_cast<OpFinishHandle *>(io_uring_cqe_get_data(cqe));
            if (handle_ptr) {
                handle_ptr->finish(cqe->res);
            }
            ++finished;
        }

        io_uring_cq_advance(ring, finished);
        strategy_->record_finished(finished);
    }
};

} // namespace condy