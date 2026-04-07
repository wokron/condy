#pragma once

#include "condy/concepts.hpp"
#include "condy/op_states.hpp"

namespace condy {

template <OpFinishHandleLike Handle, PrepFuncLike Func, typename... HandleArgs>
class OpSenderBase {
public:
    using ReturnType = typename Handle::ReturnType;

    OpSenderBase(Func func, HandleArgs &&...args)
        : prep_func_(std::move(func)),
          handle_args_(std::forward<HandleArgs>(args)...) {}

    template <typename Receiver> auto connect(Receiver receiver) noexcept {
        return detail::OpSenderOperationState<Handle, Func, Receiver>(
            prep_func_,
            std::apply(
                [](auto &&...args) {
                    return HandleBox(
                        Handle(std::forward<decltype(args)>(args)...));
                },
                std::move(handle_args_)),
            std::move(receiver));
    }

private:
    Func prep_func_;
    std::tuple<HandleArgs &&...> handle_args_;
};

template <PrepFuncLike PrepFunc, CQEHandlerLike CQEHandler,
          typename... HandleArgs>
using OpSender =
    OpSenderBase<OpFinishHandle<CQEHandler>, PrepFunc, HandleArgs...>;

template <PrepFuncLike PrepFunc, CQEHandlerLike CQEHandler,
          typename MultiShotFunc, typename... HandleArgs>
using MultiShotOpSender =
    OpSenderBase<MultiShotOpFinishHandle<CQEHandler, MultiShotFunc>, PrepFunc,
                 HandleArgs...>;

template <PrepFuncLike PrepFunc, CQEHandlerLike CQEHandler, typename FreeFunc,
          typename... HandleArgs>
using ZeroCopyOpSender =
    OpSenderBase<ZeroCopyOpFinishHandle<CQEHandler, FreeFunc>, PrepFunc,
                 HandleArgs...>;

} // namespace condy