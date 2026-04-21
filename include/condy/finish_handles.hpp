/**
 * @file finish_handles.hpp
 * @brief Definitions of finish handle types for asynchronous operations.
 * @details This file defines various FinishHandle types for managing the
 * completion of asynchronous operations. Typically, the address of a
 * FinishHandle is set as the user_data of an async operation.
 */

#pragma once

#include "condy/concepts.hpp"
#include "condy/runtime.hpp"
#include "condy/type_traits.hpp"
#include <cassert>
#include <cerrno>
#include <optional>
#include <utility>

namespace condy {

template <CQEHandlerLike CQEHandler, typename Receiver>
class OpFinishHandle : public OpFinishHandleBase {
public:
    OpFinishHandle(CQEHandler cqe_handler, Receiver receiver)
        : cqe_handler_(std::move(cqe_handler)), receiver_(std::move(receiver)) {
        this->handle_func_ = handle_static_;
    }

    OpFinishHandle(const OpFinishHandle &) = delete;
    OpFinishHandle &operator=(const OpFinishHandle &) = delete;
    OpFinishHandle(OpFinishHandle &&) = delete;
    OpFinishHandle &operator=(OpFinishHandle &&) = delete;

public:
    void maybe_set_cancel(Runtime *runtime) noexcept {
        auto stop_token = receiver_.get_stop_token();
        if (stop_token.stop_possible()) {
            stop_callback_.emplace(std::move(stop_token),
                                   Cancellation{this, runtime});
        }
    }

private:
    static bool handle_static_(void *data, io_uring_cqe *cqe) noexcept {
        auto *self = static_cast<OpFinishHandle *>(data);
        return self->handle_impl_(cqe);
    }

    bool handle_impl_(io_uring_cqe *cqe) noexcept {
        finish_(cqe);
        return true;
    }

    struct Cancellation {
        OpFinishHandle *self;
        Runtime *runtime;
        void operator()() noexcept { runtime->cancel(self); }
    };

    using TokenType = stop_token_t<Receiver>;
    using StopCallbackType = stop_callback_t<TokenType, Cancellation>;

protected:
    void finish_(io_uring_cqe *cqe) noexcept {
        stop_callback_.reset();
        std::move(receiver_)(cqe_handler_(cqe));
    }

    CQEHandler cqe_handler_;
    Receiver receiver_;
    std::optional<StopCallbackType> stop_callback_;
};

template <CQEHandlerLike CQEHandler, typename Func, typename Receiver>
class MultiShotOpFinishHandle : public OpFinishHandle<CQEHandler, Receiver> {
public:
    MultiShotOpFinishHandle(CQEHandler cqe_handler, Receiver receiver,
                            Func func)
        : OpFinishHandle<CQEHandler, Receiver>(std::move(cqe_handler),
                                               std::move(receiver)),
          func_(std::move(func)) {
        this->handle_func_ = handle_static_;
    }

private:
    static bool handle_static_(void *data, io_uring_cqe *cqe) noexcept {
        auto *self = static_cast<MultiShotOpFinishHandle *>(data);
        return self->handle_impl_(cqe);
    }

    bool handle_impl_(io_uring_cqe *cqe) noexcept
    /* fake override */ {
        if (cqe->flags & IORING_CQE_F_MORE) {
            func_(this->cqe_handler_(cqe));
            return false;
        } else {
            this->finish_(cqe);
            return true;
        }
    }

protected:
    Func func_;
};

template <CQEHandlerLike CQEHandler, typename Func, typename Receiver>
class ZeroCopyOpFinishHandle : public OpFinishHandle<CQEHandler, Receiver> {
public:
    ZeroCopyOpFinishHandle(CQEHandler cqe_handler, Receiver receiver, Func func)
        : OpFinishHandle<CQEHandler, Receiver>(std::move(cqe_handler),
                                               std::move(receiver)),
          free_func_(std::move(func)) {
        this->handle_func_ = handle_static_;
    }

private:
    static bool handle_static_(void *data, io_uring_cqe *cqe) noexcept {
        auto *self = static_cast<ZeroCopyOpFinishHandle *>(data);
        return self->handle_impl_(cqe);
    }

    bool handle_impl_(io_uring_cqe *cqe) noexcept
    /* fake override */ {
        if (cqe->flags & IORING_CQE_F_MORE) {
            this->finish_(cqe);
            return false;
        } else {
            if (cqe->flags & IORING_CQE_F_NOTIF) {
                notify_(cqe->res);
                return true;
            } else {
                // Only one cqe means the operation is finished without
                // notification. This is rare but possible.
                // https://github.com/axboe/liburing/issues/1462
                this->finish_(cqe);
                notify_(0);
                return true;
            }
        }
    }

    void notify_(int32_t res) noexcept {
        free_func_(res);
        delete this;
    }

protected:
    Func free_func_;
};

template <typename Handle> class HandleBox {
public:
    template <typename... Args>
    HandleBox(Args &&...args) : handle_(std::forward<Args>(args)...) {}

    HandleBox(const HandleBox &) = delete;
    HandleBox &operator=(const HandleBox &) = delete;
    HandleBox(HandleBox &&) = delete;
    HandleBox &operator=(HandleBox &&) = delete;

public:
    Handle &get() noexcept { return handle_; }

private:
    Handle handle_;
};

template <CQEHandlerLike CQEHandler, typename Func, typename Receiver>
class HandleBox<ZeroCopyOpFinishHandle<CQEHandler, Func, Receiver>> {
public:
    using Handle = ZeroCopyOpFinishHandle<CQEHandler, Func, Receiver>;

    template <typename... Args>
    HandleBox(Args &&...args)
        : handle_ptr_(new Handle(std::forward<Args>(args)...)) {}

    HandleBox(const HandleBox &) = delete;
    HandleBox &operator=(const HandleBox &) = delete;
    HandleBox(HandleBox &&) = delete;
    HandleBox &operator=(HandleBox &&) = delete;

public:
    Handle &get() noexcept { return *handle_ptr_; }

private:
    Handle *handle_ptr_;
};

} // namespace condy
