#pragma once

#include "condy/runtime.hpp"
#include <stdexec/execution.hpp>

namespace condy {

namespace ex = stdexec;

namespace detail {

template <typename R> struct set_value_traits {
    using type = ex::set_value_t(R);
};
template <typename R1, typename R2> struct set_value_traits<std::pair<R1, R2>> {
    using type = ex::set_value_t(R1, R2);
};
template <typename... R> struct set_value_traits<std::tuple<R...>> {
    using type = ex::set_value_t(R...);
};
template <typename R>
using set_value_traits_t = typename set_value_traits<R>::type;

template <typename SenderImpl> class StandardSender : public SenderImpl {
public:
    using SenderImpl::SenderImpl;

    using sender_concept = ex::sender_t;
    using completion_signatures = ex::completion_signatures<
        set_value_traits_t<typename SenderImpl::ReturnType>,
        ex::set_error_t(std::error_code), ex::set_stopped_t()>;

    template <typename Receiver> auto connect(Receiver receiver) noexcept {
        using OpState = decltype(this->connect_impl(
            ReceiverWrapper<Receiver>{std::move(receiver)}));
        return OperationStateWrapper<OpState>{
            this->connect_impl(ReceiverWrapper<Receiver>{std::move(receiver)})};
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

        template <typename T> void operator()(T &&res) noexcept {
            ex::set_value(std::move(receiver), std::forward<T>(res));
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
};

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