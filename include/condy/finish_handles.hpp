/**
 * @file finish_handles.hpp
 * @brief Definitions of finish handle types for asynchronous operations.
 * @details This file defines various FinishHandle types for managing the
 * completion of asynchronous operations. Typically, the address of a
 * FinishHandle is set as the user_data of an async operation.
 */

#pragma once

#include "condy/concepts.hpp"
#include "condy/invoker.hpp"
#include "condy/runtime.hpp"
#include <array>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <memory>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace condy {

template <CQEHandlerLike CQEHandler>
class OpFinishHandle : public OpFinishHandleBase {
public:
    using ReturnType = typename CQEHandler::ReturnType;

    template <typename... Args>
    OpFinishHandle(Args &&...args) : cqe_handler_(std::forward<Args>(args)...) {
        this->handle_func_ = handle_static_;
    }

    ReturnType extract_result() noexcept {
        return cqe_handler_.extract_result();
    }

    void cancel(Runtime *runtime) noexcept {
        assert(runtime != nullptr);
        runtime->cancel(this);
    }

    void set_invoker(Invoker *invoker) noexcept { invoker_ = invoker; }

private:
    static bool handle_static_(void *data, io_uring_cqe *cqe) noexcept {
        auto *self = static_cast<OpFinishHandle *>(data);
        return self->handle_impl_(cqe);
    }

    bool handle_impl_(io_uring_cqe *cqe) noexcept {
        cqe_handler_.handle_cqe(cqe);
        assert(invoker_ != nullptr);
        (*invoker_)();
        return true;
    }

protected:
    Invoker *invoker_ = nullptr;
    CQEHandler cqe_handler_;
};

template <CQEHandlerLike CQEHandler, typename Func>
class MultiShotOpFinishHandle : public OpFinishHandle<CQEHandler> {
public:
    template <typename... Args>
    MultiShotOpFinishHandle(Func func, Args &&...args)
        : OpFinishHandle<CQEHandler>(std::forward<Args>(args)...),
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
            this->cqe_handler_.handle_cqe(cqe);
            assert(this->invoker_ != nullptr);
            (*this->invoker_)();
            return true;
        }
    }

protected:
    Func func_;
};

template <CQEHandlerLike CQEHandler, typename Func>
class ZeroCopyOpFinishHandle : public OpFinishHandle<CQEHandler> {
public:
    template <typename... Args>
    ZeroCopyOpFinishHandle(Func func, Args &&...args)
        : OpFinishHandle<CQEHandler>(std::forward<Args>(args)...),
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
            this->cqe_handler_.handle_cqe(cqe);
            assert(this->invoker_ != nullptr);
            (*this->invoker_)();
            return false;
        } else {
            if (cqe->flags & IORING_CQE_F_NOTIF) {
                notify_(cqe->res);
                return true;
            } else {
                // Only one cqe means the operation is finished without
                // notification. This is rare but possible.
                // https://github.com/axboe/liburing/issues/1462
                this->cqe_handler_.handle_cqe(cqe);
                assert(this->invoker_ != nullptr);
                (*this->invoker_)();
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

template <OpFinishHandleLike Handle> class HandleBox {
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

template <CQEHandlerLike CQEHandler, typename Func>
class HandleBox<ZeroCopyOpFinishHandle<CQEHandler, Func>> {
public:
    using Handle = ZeroCopyOpFinishHandle<CQEHandler, Func>;

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
