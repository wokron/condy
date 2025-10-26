#pragma once

#include "condy/condy_uring.hpp"
#include "condy/context.hpp"
#include "condy/coro.hpp"
#include "condy/finish_handles.hpp"
#include "condy/queue.hpp"
#include "condy/utils.hpp"
#include <atomic>
#include <cerrno>
#include <coroutine>
#include <cstddef>

namespace condy {

class IEventLoop {
public:
    virtual bool try_post(std::coroutine_handle<> handle) = 0;
};

template <typename Strategy> class EventLoop : public IEventLoop {
public:
    template <typename... Args>
    EventLoop(Args &&...args)
        : strategy_(std::forward<Args>(args)...),
          inner_ready_queue_(strategy_.get_ready_queue_capacity()),
          outer_ready_queue_(strategy_.get_ready_queue_capacity()) {}

    EventLoop(const EventLoop &) = delete;
    EventLoop &operator=(const EventLoop &) = delete;
    EventLoop(EventLoop &&) = delete;
    EventLoop &operator=(EventLoop &&) = delete;

public:
    template <typename... Ts> void prologue(Coro<Ts>... coros);

    void run_once();

    void epilogue();

    template <typename... Ts> void run(Coro<Ts>... entry_point) {
        prologue(std::move(entry_point)...);
        auto d = defer([&]() { epilogue(); });
        while (!should_stop_()) {
            run_once();
        }
    }

    void stop() { state_.store(State::STOPPED, std::memory_order_release); }

    bool try_post(std::coroutine_handle<> handle) override {
        return outer_ready_queue_.try_enqueue(handle);
    }

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
        return check_stopped() || strategy_.should_stop();
    }

    template <size_t Idx = 0, typename... Ts>
    void foreach_coro_prologue_(std::tuple<Coro<Ts>...> coros) {
        if constexpr (Idx < sizeof...(Ts)) {
            auto &coro = std::get<Idx>(coros);
            auto handle = coro.release();
            handle.promise().set_new_task(true);
            handle.resume();
            foreach_coro_prologue_<Idx + 1, Ts...>(std::move(coros));
        }
    }

private:
    Strategy strategy_;
    std::atomic<State> state_ = State::IDLE;
    SingleThreadRingQueue<std::coroutine_handle<>> inner_ready_queue_;
    MultiWriterRingQueue<std::coroutine_handle<>> outer_ready_queue_;
};

template <typename Strategy>
template <typename... Ts>
void EventLoop<Strategy>::prologue(Coro<Ts>... coros) {
    State expected = State::IDLE;
    if (!state_.compare_exchange_strong(expected, State::RUNNING,
                                        std::memory_order_acq_rel)) {
        throw std::runtime_error("EventLoop is already running or stopped");
    }
    Context::current().init(&strategy_, &inner_ready_queue_, this);

    foreach_coro_prologue_(std::make_tuple(std::move(coros)...));
}

template <typename Strategy> void EventLoop<Strategy>::epilogue() {
    state_.store(State::STOPPED, std::memory_order_release);
    Context::current().destroy();
}

template <typename Strategy> void EventLoop<Strategy>::run_once() {
    auto *ring = Context::current().get_ring();

    std::optional<std::coroutine_handle<>> handle;
    while ((handle = outer_ready_queue_.try_dequeue())) {
        (*handle).resume();
    }
    while ((handle = inner_ready_queue_.try_dequeue())) {
        (*handle).resume();
    }

    int r = strategy_.submit_and_wait(ring);
    if (r == -EINTR) {
        return;
    } else if (r == -ETIME) {
        r = 0;
    } else if (r < 0) {
        throw std::runtime_error("io_uring_submit_and_wait failed: " +
                                 std::string(std::strerror(-r)));
    }
    int submitted = r;
    strategy_.record_submitted(submitted); // TODO: Is this useful?

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
    strategy_.record_finished(finished);
}

} // namespace condy