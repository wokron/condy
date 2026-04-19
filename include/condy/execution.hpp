#pragma once

#include "condy/runtime.hpp"
#include <stdexec/execution.hpp>

namespace condy {

namespace ex = stdexec;

namespace detail {

class RuntimeScheduler;

template <typename Sender> class StandardSenderWrapper {
public:
    using sender_concept = ex::sender_t;
    using completion_signatures =
        ex::completion_signatures<ex::set_value_t(typename Sender::ReturnType),
                                  ex::set_error_t(std::error_code),
                                  ex::set_stopped_t()>;

    StandardSenderWrapper(Sender sender) : sender_(std::move(sender)) {}

    template <typename Receiver> auto connect(Receiver receiver) noexcept {
        using OpState =
            decltype(sender_.connect(ReceiverWrapper<Receiver>{receiver}));
        return OperationStateWrapper<OpState>{
            sender_.connect(ReceiverWrapper<Receiver>{std::move(receiver)})};
    }

private:
    template <typename Receiver> struct ReceiverWrapper {
        Receiver receiver;

        void operator()(int32_t res) noexcept {
            if (res >= 0) {
                ex::set_value(std::move(receiver), res);
            } else if (res == -ECANCELED) {
                ex::set_stopped(std::move(receiver));
            } else {
                ex::set_error(std::move(receiver),
                              std::error_code(-res, std::generic_category()));
            }
        }

        template <typename T>
        void operator()(std::pair<int32_t, T> res) noexcept {
            auto &[res_code, _] = res;
            if (res_code >= 0) {
                ex::set_value(std::move(receiver), std::move(res));
            } else if (res_code == -ECANCELED) {
                ex::set_stopped(std::move(receiver));
            } else {
                ex::set_error(
                    std::move(receiver),
                    std::error_code(-res_code, std::generic_category()));
            }
        }

        auto get_stop_token() const noexcept {
            auto env = ex::get_env(receiver);
            return ex::get_stop_token(env);
        }
    };

    template <typename OperationState> struct OperationStateWrapper {
        OperationState op_state;
        void start() noexcept { op_state.start(0); }
    };

    Sender sender_;
};

template <typename Sender> auto as_sender(Sender &&sender) {
    return StandardSenderWrapper<std::decay_t<Sender>>(
        std::forward<Sender>(sender));
}

class ScheduleSender {
public:
    using sender_concept = ex::sender_t;

    using completion_signatures = ex::completion_signatures<ex::set_value_t()>;

    ScheduleSender(Runtime &runtime) : runtime_(runtime) {}

    template <typename Receiver> auto connect(Receiver receiver) {
        return OperationState<std::decay_t<Receiver>>(runtime_,
                                                      std::move(receiver));
    }

private:
    template <typename Receiver>
    class OperationState
        : public InvokerAdapter<OperationState<Receiver>, WorkInvoker> {
    public:
        OperationState(Runtime &runtime, Receiver receiver)
            : runtime_(runtime), receiver_(std::move(receiver)) {}

        OperationState(const OperationState &) = delete;
        OperationState &operator=(const OperationState &) = delete;
        OperationState(OperationState &&) = delete;
        OperationState &operator=(OperationState &&) = delete;

    public:
        void start() noexcept { runtime_.schedule(this); }

        void invoke() noexcept { ex::set_value(std::move(receiver_)); }

    private:
        Runtime &runtime_;
        Receiver receiver_;
    };

    Runtime &runtime_;
};

class RuntimeScheduler {
public:
    RuntimeScheduler(Runtime &runtime) : runtime_(&runtime) {}

    bool operator==(const RuntimeScheduler &other) const noexcept {
        return runtime_ == other.runtime_;
    }

    auto schedule() const noexcept { return detail::ScheduleSender{*runtime_}; }

    Runtime &runtime() const noexcept { return *runtime_; }

private:
    Runtime *runtime_;
};

} // namespace detail

inline detail::RuntimeScheduler get_scheduler(Runtime &runtime) {
    return {runtime};
}

} // namespace condy