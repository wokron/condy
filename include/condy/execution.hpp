#pragma once

#include "condy/runtime.hpp"
#include <stdexec/execution.hpp>

namespace condy {

namespace ex = stdexec;

namespace detail {

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

private:
    Runtime *runtime_;
};

} // namespace detail

inline detail::RuntimeScheduler get_scheduler(Runtime &runtime) {
    return {runtime};
}

} // namespace condy