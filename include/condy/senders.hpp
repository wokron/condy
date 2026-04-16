#pragma once

#include "condy/concepts.hpp"
#include "condy/op_states.hpp"
#include <array>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace condy {

template <PrepFuncLike PrepFunc, CQEHandlerLike CQEHandler,
          typename... HandleArgs>
class OpSender {
public:
    using ReturnType = typename CQEHandler::ReturnType;

    OpSender(PrepFunc func, HandleArgs... args)
        : prep_func_(std::move(func)), handle_args_(std::move(args)...) {}

    template <typename Receiver> auto connect(Receiver receiver) noexcept {
        return std::apply(
            [&](auto &&...args) {
                return detail::OpSenderOperationState<
                    OpFinishHandle<CQEHandler, Receiver>, PrepFunc, Receiver>(
                    prep_func_, std::move(receiver),
                    std::forward<decltype(args)>(args)...);
            },
            std::move(handle_args_));
    }

private:
    PrepFunc prep_func_;
    std::tuple<HandleArgs...> handle_args_;
};

template <PrepFuncLike PrepFunc, CQEHandlerLike CQEHandler,
          typename MultiShotFunc, typename... HandleArgs>
class MultiShotOpSender {
public:
    using ReturnType = typename CQEHandler::ReturnType;

    MultiShotOpSender(PrepFunc func, MultiShotFunc multi_shot_func,
                      HandleArgs... args)
        : prep_func_(std::move(func)),
          multi_shot_func_(std::move(multi_shot_func)),
          handle_args_(std::move(args)...) {}

    template <typename Receiver> auto connect(Receiver receiver) noexcept {
        return std::apply(
            [&](auto &&...args) {
                return detail::OpSenderOperationState<
                    MultiShotOpFinishHandle<CQEHandler, MultiShotFunc,
                                            Receiver>,
                    PrepFunc, Receiver>(std::move(prep_func_),
                                        std::move(receiver),
                                        std::move(multi_shot_func_),
                                        std::forward<decltype(args)>(args)...);
            },
            std::move(handle_args_));
    }

private:
    PrepFunc prep_func_;
    MultiShotFunc multi_shot_func_;
    std::tuple<HandleArgs...> handle_args_;
};

template <PrepFuncLike PrepFunc, CQEHandlerLike CQEHandler, typename FreeFunc,
          typename... HandleArgs>
class ZeroCopyOpSender {
public:
    using ReturnType = typename CQEHandler::ReturnType;

    ZeroCopyOpSender(PrepFunc func, FreeFunc free_func, HandleArgs... args)
        : prep_func_(std::move(func)), free_func_(std::move(free_func)),
          handle_args_(std::move(args)...) {}

    template <typename Receiver> auto connect(Receiver receiver) noexcept {
        return std::apply(
            [&](auto &&...args) {
                return detail::OpSenderOperationState<
                    ZeroCopyOpFinishHandle<CQEHandler, FreeFunc, Receiver>,
                    PrepFunc, Receiver>(std::move(prep_func_),
                                        std::move(receiver),
                                        std::move(free_func_),
                                        std::forward<decltype(args)>(args)...);
            },
            std::move(handle_args_));
    }

private:
    PrepFunc prep_func_;
    FreeFunc free_func_;
    std::tuple<HandleArgs...> handle_args_;
};

template <unsigned int Flags, typename Sender> class FlaggedOpSender {
public:
    using ReturnType = typename Sender::ReturnType;

    FlaggedOpSender(Sender sender) : sender_(std::move(sender)) {}

    template <typename Receiver> auto connect(Receiver receiver) noexcept {
        return detail::FlaggedOpState<Flags, Sender, Receiver>(
            std::move(sender_), std::move(receiver));
    }

private:
    Sender sender_;
};

template <typename... Senders> class ParallelAllSender {
public:
    using ReturnType = std::pair<std::array<size_t, sizeof...(Senders)>,
                                 std::tuple<typename Senders::ReturnType...>>;

    ParallelAllSender(Senders... senders) : senders_(std::move(senders)...) {}

    template <typename Receiver> auto connect(Receiver receiver) noexcept {
        return detail::ParallelAllOperationState<Receiver, Senders...>(
            std::move(senders_), std::move(receiver));
    }

private:
    std::tuple<Senders...> senders_;
};

template <typename... Senders> class ParallelAnySender {
public:
    using ReturnType = std::pair<std::array<size_t, sizeof...(Senders)>,
                                 std::tuple<typename Senders::ReturnType...>>;

    ParallelAnySender(Senders... senders) : senders_(std::move(senders)...) {}

    template <typename Receiver> auto connect(Receiver receiver) noexcept {
        return detail::ParallelAnyOperationState<Receiver, Senders...>(
            std::move(senders_), std::move(receiver));
    }

private:
    std::tuple<Senders...> senders_;
};

template <typename... Senders> class WhenAllSender {
public:
    using ReturnType = std::tuple<typename Senders::ReturnType...>;

    WhenAllSender(Senders... senders) : senders_(std::move(senders)...) {}

    template <typename S, typename... Ss>
    WhenAllSender(WhenAllSender<Ss...> other, S sender)
        : senders_(std::tuple_cat(std::move(other.senders_),
                                  std::make_tuple(std::move(sender)))) {}

    template <typename Receiver> auto connect(Receiver receiver) noexcept {
        return detail::WhenAllOperationState<Receiver, Senders...>(
            std::move(senders_), std::move(receiver));
    }

private:
    std::tuple<Senders...> senders_;

    template <typename...> friend class WhenAllSender;
};

template <typename... Senders> class WhenAnySender {
public:
    using ReturnType = std::variant<typename Senders::ReturnType...>;

    WhenAnySender(Senders... senders) : senders_(std::move(senders)...) {}

    template <typename S, typename... Ss>
    WhenAnySender(WhenAnySender<Ss...> other, S sender)
        : senders_(std::tuple_cat(std::move(other.senders_),
                                  std::make_tuple(std::move(sender)))) {}

    template <typename Receiver> auto connect(Receiver receiver) noexcept {
        return detail::WhenAnyOperationState<Receiver, Senders...>(
            std::move(senders_), std::move(receiver));
    }

private:
    std::tuple<Senders...> senders_;

    template <typename...> friend class WhenAnySender;
};

template <unsigned int Flags, typename... Senders> class LinkSenderBase {
public:
    using ReturnType = std::tuple<typename Senders::ReturnType...>;

    LinkSenderBase(Senders... senders) : senders_(std::move(senders)...) {}

    template <typename S, typename... Ss>
    LinkSenderBase(LinkSenderBase<Flags, Ss...> other, S sender)
        : senders_(std::tuple_cat(std::move(other.senders_),
                                  std::make_tuple(std::move(sender)))) {}

    template <typename Receiver> auto connect(Receiver receiver) noexcept {
        return detail::LinkOperationState<Receiver, Flags, Senders...>(
            std::move(senders_), std::move(receiver));
    }

private:
    std::tuple<Senders...> senders_;

    template <unsigned int, typename...> friend class LinkSenderBase;
};

template <typename... Senders>
using LinkSender = LinkSenderBase<IOSQE_IO_LINK, Senders...>;

template <typename... Senders>
using HardLinkSender = LinkSenderBase<IOSQE_IO_HARDLINK, Senders...>;

template <typename Sender> class RangedParallelAllSender {
public:
    using ReturnType = std::pair<std::vector<size_t>,
                                 std::vector<typename Sender::ReturnType>>;

    RangedParallelAllSender(std::vector<Sender> senders)
        : senders_(std::move(senders)) {}

    template <typename Receiver> auto connect(Receiver receiver) noexcept {
        return detail::RangedParallelAllOperationState<Receiver, Sender>(
            std::move(senders_), std::move(receiver));
    }

private:
    std::vector<Sender> senders_;
};

template <typename Sender> class RangedParallelAnySender {
public:
    using ReturnType = std::pair<std::vector<size_t>,
                                 std::vector<typename Sender::ReturnType>>;

    RangedParallelAnySender(std::vector<Sender> senders)
        : senders_(std::move(senders)) {}

    template <typename Receiver> auto connect(Receiver receiver) noexcept {
        return detail::RangedParallelAnyOperationState<Receiver, Sender>(
            std::move(senders_), std::move(receiver));
    }

private:
    std::vector<Sender> senders_;
};

template <typename Sender> class RangedWhenAllSender {
public:
    using ReturnType = std::vector<typename Sender::ReturnType>;

    RangedWhenAllSender(std::vector<Sender> senders)
        : senders_(std::move(senders)) {}

    template <typename Receiver> auto connect(Receiver receiver) noexcept {
        return detail::WhenAllRangeOperationState<Receiver, Sender>(
            std::move(senders_), std::move(receiver));
    }

private:
    std::vector<Sender> senders_;
};

template <typename Sender> class RangedWhenAnySender {
public:
    using ReturnType = std::pair<size_t, typename Sender::ReturnType>;

    RangedWhenAnySender(std::vector<Sender> senders)
        : senders_(std::move(senders)) {
        if (senders_.empty()) {
            throw std::invalid_argument(
                "when_any requires at least one sender");
        }
    }

    template <typename Receiver> auto connect(Receiver receiver) noexcept {
        return detail::WhenAnyRangeOperationState<Receiver, Sender>(
            std::move(senders_), std::move(receiver));
    }

private:
    std::vector<Sender> senders_;
};

template <unsigned int Flags, typename Sender> class RangedLinkSenderBase {
public:
    using ReturnType = std::vector<typename Sender::ReturnType>;

    RangedLinkSenderBase(std::vector<Sender> senders)
        : senders_(std::move(senders)) {}

    template <typename Receiver> auto connect(Receiver receiver) noexcept {
        return detail::RangedLinkOperationState<Receiver, Flags, Sender>(
            std::move(senders_), std::move(receiver));
    }

private:
    std::vector<Sender> senders_;
};

template <typename Sender>
using RangedLinkSender = RangedLinkSenderBase<IOSQE_IO_LINK, Sender>;

template <typename Sender>
using RangedHardLinkSender = RangedLinkSenderBase<IOSQE_IO_HARDLINK, Sender>;

} // namespace condy