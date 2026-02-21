#pragma once

#include "condy/runtime.hpp"
#include <stdexec/execution.hpp>

namespace condy {

namespace detail {

class ScheduleSender {
public:
    using sender_concept = stdexec::sender_t;

    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t()>;

    ScheduleSender(Runtime &runtime) : runtime_(runtime) {}

    template <typename Receiver> auto connect(Receiver &&receiver) {
        return OpState<std::decay_t<Receiver>>(
            runtime_, std::forward<Receiver>(receiver));
    }

private:
    template <typename Receiver>
    class OpState : public InvokerAdapter<OpState<Receiver>, WorkInvoker> {
    public:
        OpState(Runtime &runtime, Receiver receiver)
            : runtime_(runtime), receiver_(std::move(receiver)) {}

        void start() noexcept { runtime_.schedule(this); }

        void invoke() noexcept { stdexec::set_value(std::move(receiver_)); }

    private:
        Runtime &runtime_;
        Receiver receiver_;
    };

    Runtime &runtime_;
};

} // namespace detail

class RuntimeScheduler {
public:
    RuntimeScheduler(Runtime &runtime) : runtime_(runtime) {}

    bool operator==(const RuntimeScheduler &other) const noexcept {
        return &runtime_ == &other.runtime_;
    }

    auto schedule() const noexcept { return detail::ScheduleSender{runtime_}; }

private:
    Runtime &runtime_;
};

} // namespace condy