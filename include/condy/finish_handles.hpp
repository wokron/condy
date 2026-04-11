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
#include <cassert>
#include <cerrno>
#include <stop_token>
#include <utility>

namespace condy {

template <CQEHandlerLike CQEHandler, typename Receiver>
class OpFinishHandle : public OpFinishHandleBase {
public:
    using ReturnType = typename CQEHandler::ReturnType;

    template <typename... Args>
    OpFinishHandle(Receiver receiver, Args &&...args)
        : cqe_handler_(std::forward<Args>(args)...),
          receiver_(std::move(receiver)) {
        this->handle_func_ = handle_static_;
    }

    void maybe_install_cancellation(Runtime *runtime) noexcept {
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
        stop_callback_.reset();
        cqe_handler_.handle_cqe(cqe);
        std::move(receiver_)(cqe_handler_.extract_result());
        return true;
    }

    struct Cancellation {
        OpFinishHandle *self;
        Runtime *runtime;
        void operator()() noexcept { runtime->cancel(self); }
    };

protected:
    CQEHandler cqe_handler_;
    Receiver receiver_;
    std::optional<std::stop_callback<Cancellation>> stop_callback_;
};

template <CQEHandlerLike CQEHandler, typename Func, typename Receiver>
class MultiShotOpFinishHandle : public OpFinishHandle<CQEHandler, Receiver> {
public:
    template <typename... Args>
    MultiShotOpFinishHandle(Receiver receiver, Func func, Args &&...args)
        : OpFinishHandle<CQEHandler, Receiver>(std::move(receiver),
                                               std::forward<Args>(args)...),
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
            this->cqe_handler_.handle_cqe(cqe);
            func_(this->cqe_handler_.extract_result());
            return false;
        } else {
            this->stop_callback_.reset();
            this->cqe_handler_.handle_cqe(cqe);
            std::move(this->receiver_)(this->cqe_handler_.extract_result());
            return true;
        }
    }

protected:
    Func func_;
};

template <CQEHandlerLike CQEHandler, typename Func, typename Receiver>
class ZeroCopyOpFinishHandle : public OpFinishHandle<CQEHandler, Receiver> {
public:
    template <typename... Args>
    ZeroCopyOpFinishHandle(Receiver receiver, Func func, Args &&...args)
        : OpFinishHandle<CQEHandler, Receiver>(std::move(receiver),
                                               std::forward<Args>(args)...),
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
            this->stop_callback_.reset();
            this->cqe_handler_.handle_cqe(cqe);
            std::move(this->receiver_)(this->cqe_handler_.extract_result());
            return false;
        } else {
            if (cqe->flags & IORING_CQE_F_NOTIF) {
                notify_(cqe->res);
                return true;
            } else {
                // Only one cqe means the operation is finished without
                // notification. This is rare but possible.
                // https://github.com/axboe/liburing/issues/1462
                this->stop_callback_.reset();
                this->cqe_handler_.handle_cqe(cqe);
                std::move(this->receiver_)(this->cqe_handler_.extract_result());
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
