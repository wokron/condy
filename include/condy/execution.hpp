#pragma once

#include "condy/concepts.hpp"
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

template <AwaiterLike Awaiter> class AwaiterSender {
public:
    using sender_concept = stdexec::sender_t;

    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(typename Awaiter::HandleType::ReturnType),
        stdexec::set_error_t(std::exception_ptr)>;

    AwaiterSender(Awaiter awaiter) : awaiter_(std::move(awaiter)) {}

    template <typename Receiver> auto connect(Receiver &&receiver) {
        return OpState<std::decay_t<Receiver>>(
            std::move(awaiter_), std::forward<Receiver>(receiver));
    }

private:
    template <typename Receiver>
    class OpState : public InvokerAdapter<OpState<Receiver>> {
    public:
        OpState(Awaiter awaiter, Receiver receiver)
            : awaiter_(std::move(awaiter)), receiver_(std::move(receiver)) {}

        void start() noexcept {
            awaiter_.init_finish_handle();
            awaiter_.get_handle()->set_invoker(this);
            awaiter_.register_operation(0);
        }

        void invoke() noexcept {
            try {
                auto result = awaiter_.await_resume();
                stdexec::set_value(std::move(receiver_), std::move(result));
            } catch (...) {
                stdexec::set_error(std::move(receiver_),
                                   std::current_exception());
            }
        }

    private:
        Awaiter awaiter_;
        Receiver receiver_;
    };
    ;

    Awaiter awaiter_;
};

template <AwaiterLike Awaiter> auto convert_to_sender(Awaiter &&awaiter) {
    return AwaiterSender<std::decay_t<Awaiter>>(std::forward<Awaiter>(awaiter));
}

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