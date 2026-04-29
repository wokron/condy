/**
 * @file senders.hpp
 * @brief Sender types for composing asynchronous operations.
 */

#pragma once

#include "condy/concepts.hpp"
#include "condy/execution.hpp"
#include "condy/op_states.hpp"
#include <array>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace condy {

namespace detail {

template <PrepFuncLike PrepFunc, CQEHandlerLike CQEHandler> class OpSenderImpl {
public:
    using ReturnType = std::invoke_result_t<CQEHandler &, io_uring_cqe *>;

    OpSenderImpl(PrepFunc func, CQEHandler cqe_handler)
        : prep_func_(std::move(func)), cqe_handler_(std::move(cqe_handler)) {}

    template <typename Receiver> auto connect_impl(Receiver receiver) noexcept {
        return detail::OpSenderOperationState<
            OpFinishHandle<CQEHandler, Receiver>, PrepFunc>(
            std::move(prep_func_), std::move(cqe_handler_),
            std::move(receiver));
    }

private:
    PrepFunc prep_func_;
    CQEHandler cqe_handler_;
};

template <PrepFuncLike PrepFunc, CQEHandlerLike CQEHandler,
          typename MultiShotFunc>
class MultiShotOpSenderImpl {
public:
    using ReturnType = std::invoke_result_t<CQEHandler &, io_uring_cqe *>;

    MultiShotOpSenderImpl(PrepFunc func, CQEHandler cqe_handler,
                          MultiShotFunc multi_shot_func)
        : prep_func_(std::move(func)), cqe_handler_(std::move(cqe_handler)),
          multi_shot_func_(std::move(multi_shot_func)) {}

    template <typename Receiver> auto connect_impl(Receiver receiver) noexcept {
        return detail::OpSenderOperationState<
            MultiShotOpFinishHandle<CQEHandler, MultiShotFunc, Receiver>,
            PrepFunc>(std::move(prep_func_), std::move(cqe_handler_),
                      std::move(receiver), std::move(multi_shot_func_));
    }

private:
    PrepFunc prep_func_;
    CQEHandler cqe_handler_;
    MultiShotFunc multi_shot_func_;
};

template <PrepFuncLike PrepFunc, CQEHandlerLike CQEHandler, typename FreeFunc>
class ZeroCopyOpSenderImpl {
public:
    using ReturnType = std::invoke_result_t<CQEHandler &, io_uring_cqe *>;

    ZeroCopyOpSenderImpl(PrepFunc func, CQEHandler cqe_handler,
                         FreeFunc free_func)
        : prep_func_(std::move(func)), cqe_handler_(std::move(cqe_handler)),
          free_func_(std::move(free_func)) {}

    template <typename Receiver> auto connect_impl(Receiver receiver) noexcept {
        return detail::OpSenderOperationState<
            ZeroCopyOpFinishHandle<CQEHandler, FreeFunc, Receiver>, PrepFunc>(
            std::move(prep_func_), std::move(cqe_handler_), std::move(receiver),
            std::move(free_func_));
    }

private:
    PrepFunc prep_func_;
    CQEHandler cqe_handler_;
    FreeFunc free_func_;
};

template <unsigned int Flags, typename Sender> class FlaggedOpSenderImpl {
public:
    using ReturnType = typename Sender::ReturnType;

    FlaggedOpSenderImpl(Sender sender) : sender_(std::move(sender)) {}

    template <typename Receiver> auto connect_impl(Receiver receiver) noexcept {
        return detail::FlaggedOpState<Flags, Sender, Receiver>(
            std::move(sender_), std::move(receiver));
    }

private:
    Sender sender_;
};

template <typename... Senders> class ParallelAllSenderImpl {
public:
    using ReturnType = std::pair<std::array<size_t, sizeof...(Senders)>,
                                 std::tuple<typename Senders::ReturnType...>>;

    ParallelAllSenderImpl(Senders... senders)
        : senders_(std::move(senders)...) {}

    template <typename Receiver> auto connect_impl(Receiver receiver) noexcept {
        return detail::ParallelAllOperationState<Receiver, Senders...>(
            std::move(senders_), std::move(receiver));
    }

private:
    std::tuple<Senders...> senders_;
};

template <typename... Senders> class ParallelAnySenderImpl {
public:
    using ReturnType = std::pair<std::array<size_t, sizeof...(Senders)>,
                                 std::tuple<typename Senders::ReturnType...>>;

    ParallelAnySenderImpl(Senders... senders)
        : senders_(std::move(senders)...) {}

    template <typename Receiver> auto connect_impl(Receiver receiver) noexcept {
        return detail::ParallelAnyOperationState<Receiver, Senders...>(
            std::move(senders_), std::move(receiver));
    }

private:
    std::tuple<Senders...> senders_;
};

template <typename... Senders> class WhenAllSenderImpl {
public:
    using ReturnType = std::tuple<typename Senders::ReturnType...>;

    WhenAllSenderImpl(Senders... senders) : senders_(std::move(senders)...) {}

    template <typename S, typename... Ss>
    WhenAllSenderImpl(WhenAllSenderImpl<Ss...> other, S sender)
        : senders_(std::tuple_cat(std::move(other.senders_),
                                  std::make_tuple(std::move(sender)))) {}

    template <typename Receiver> auto connect_impl(Receiver receiver) noexcept {
        return detail::WhenAllOperationState<Receiver, Senders...>(
            std::move(senders_), std::move(receiver));
    }

private:
    std::tuple<Senders...> senders_;

    template <typename...> friend class WhenAllSenderImpl;
};

template <typename... Senders> class WhenAnySenderImpl {
public:
    using ReturnType = std::variant<typename Senders::ReturnType...>;

    WhenAnySenderImpl(Senders... senders) : senders_(std::move(senders)...) {}

    template <typename S, typename... Ss>
    WhenAnySenderImpl(WhenAnySenderImpl<Ss...> other, S sender)
        : senders_(std::tuple_cat(std::move(other.senders_),
                                  std::make_tuple(std::move(sender)))) {}

    template <typename Receiver> auto connect_impl(Receiver receiver) noexcept {
        return detail::WhenAnyOperationState<Receiver, Senders...>(
            std::move(senders_), std::move(receiver));
    }

private:
    std::tuple<Senders...> senders_;

    template <typename...> friend class WhenAnySenderImpl;
};

template <unsigned int Flags, typename... Senders> class LinkSenderImplBase {
public:
    using ReturnType = std::tuple<typename Senders::ReturnType...>;

    LinkSenderImplBase(Senders... senders) : senders_(std::move(senders)...) {}

    template <typename S, typename... Ss>
    LinkSenderImplBase(LinkSenderImplBase<Flags, Ss...> other, S sender)
        : senders_(std::tuple_cat(std::move(other.senders_),
                                  std::make_tuple(std::move(sender)))) {}

    template <typename Receiver> auto connect_impl(Receiver receiver) noexcept {
        return detail::LinkOperationState<Receiver, Flags, Senders...>(
            std::move(senders_), std::move(receiver));
    }

private:
    std::tuple<Senders...> senders_;

    template <unsigned int, typename...> friend class LinkSenderImplBase;
};

template <typename Sender> class RangedParallelAllSenderImpl {
public:
    using ReturnType = std::pair<std::vector<size_t>,
                                 std::vector<typename Sender::ReturnType>>;

    RangedParallelAllSenderImpl(std::vector<Sender> senders)
        : senders_(std::move(senders)) {}

    template <typename Receiver> auto connect_impl(Receiver receiver) noexcept {
        return detail::RangedParallelAllOperationState<Receiver, Sender>(
            std::move(senders_), std::move(receiver));
    }

private:
    std::vector<Sender> senders_;
};

template <typename Sender> class RangedParallelAnySenderImpl {
public:
    using ReturnType = std::pair<std::vector<size_t>,
                                 std::vector<typename Sender::ReturnType>>;

    RangedParallelAnySenderImpl(std::vector<Sender> senders)
        : senders_(std::move(senders)) {}

    template <typename Receiver> auto connect_impl(Receiver receiver) noexcept {
        return detail::RangedParallelAnyOperationState<Receiver, Sender>(
            std::move(senders_), std::move(receiver));
    }

private:
    std::vector<Sender> senders_;
};

template <typename Sender> class RangedWhenAllSenderImpl {
public:
    using ReturnType = std::vector<typename Sender::ReturnType>;

    RangedWhenAllSenderImpl(std::vector<Sender> senders)
        : senders_(std::move(senders)) {}

    template <typename Receiver> auto connect_impl(Receiver receiver) noexcept {
        return detail::WhenAllRangeOperationState<Receiver, Sender>(
            std::move(senders_), std::move(receiver));
    }

private:
    std::vector<Sender> senders_;
};

template <typename Sender> class RangedWhenAnySenderImpl {
public:
    using ReturnType = std::pair<size_t, typename Sender::ReturnType>;

    RangedWhenAnySenderImpl(std::vector<Sender> senders)
        : senders_(std::move(senders)) {
        if (senders_.empty()) {
            throw std::invalid_argument(
                "when_any requires at least one sender");
        }
    }

    template <typename Receiver> auto connect_impl(Receiver receiver) noexcept {
        return detail::WhenAnyRangeOperationState<Receiver, Sender>(
            std::move(senders_), std::move(receiver));
    }

private:
    std::vector<Sender> senders_;
};

template <unsigned int Flags, typename Sender> class RangedLinkSenderImplBase {
public:
    using ReturnType = std::vector<typename Sender::ReturnType>;

    RangedLinkSenderImplBase(std::vector<Sender> senders)
        : senders_(std::move(senders)) {}

    template <typename Receiver> auto connect_impl(Receiver receiver) noexcept {
        return detail::RangedLinkOperationState<Receiver, Flags, Sender>(
            std::move(senders_), std::move(receiver));
    }

private:
    std::vector<Sender> senders_;
};

} // namespace detail

template <PrepFuncLike PrepFunc, CQEHandlerLike CQEHandler>
using OpSender =
    detail::StandardSender<detail::OpSenderImpl<PrepFunc, CQEHandler>>;

template <PrepFuncLike PrepFunc, CQEHandlerLike CQEHandler,
          typename MultiShotFunc>
using MultiShotOpSender = detail::StandardSender<
    detail::MultiShotOpSenderImpl<PrepFunc, CQEHandler, MultiShotFunc>>;

template <PrepFuncLike PrepFunc, CQEHandlerLike CQEHandler, typename FreeFunc>
using ZeroCopyOpSender = detail::StandardSender<
    detail::ZeroCopyOpSenderImpl<PrepFunc, CQEHandler, FreeFunc>>;

template <unsigned int Flags, typename Sender>
using FlaggedOpSender =
    detail::StandardSender<detail::FlaggedOpSenderImpl<Flags, Sender>>;

template <typename... Senders>
using ParallelAllSender =
    detail::StandardSender<detail::ParallelAllSenderImpl<Senders...>>;

template <typename... Senders>
using ParallelAnySender =
    detail::StandardSender<detail::ParallelAnySenderImpl<Senders...>>;

template <typename... Senders>
using WhenAllSender =
    detail::StandardSender<detail::WhenAllSenderImpl<Senders...>>;

template <typename... Senders>
using WhenAnySender =
    detail::StandardSender<detail::WhenAnySenderImpl<Senders...>>;

template <typename... Senders>
using LinkSender = detail::StandardSender<
    detail::LinkSenderImplBase<IOSQE_IO_LINK, Senders...>>;

template <typename... Senders>
using HardLinkSender = detail::StandardSender<
    detail::LinkSenderImplBase<IOSQE_IO_HARDLINK, Senders...>>;

template <typename Sender>
using RangedParallelAllSender =
    detail::StandardSender<detail::RangedParallelAllSenderImpl<Sender>>;

template <typename Sender>
using RangedParallelAnySender =
    detail::StandardSender<detail::RangedParallelAnySenderImpl<Sender>>;

template <typename Sender>
using RangedWhenAllSender =
    detail::StandardSender<detail::RangedWhenAllSenderImpl<Sender>>;

template <typename Sender>
using RangedWhenAnySender =
    detail::StandardSender<detail::RangedWhenAnySenderImpl<Sender>>;

template <typename Sender>
using RangedLinkSender = detail::StandardSender<
    detail::RangedLinkSenderImplBase<IOSQE_IO_LINK, Sender>>;

template <typename Sender>
using RangedHardLinkSender = detail::StandardSender<
    detail::RangedLinkSenderImplBase<IOSQE_IO_HARDLINK, Sender>>;

} // namespace condy