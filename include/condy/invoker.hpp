/**
 * @file invoker.hpp
 * @brief Polymorphic invocation utilities.
 * @details This file provides utilities for simulating polymorphic invocation.
 */

#pragma once

#include "condy/intrusive.hpp"

namespace condy {

class Invoker {
public:
    using Func = void (*)(void *) noexcept;

    void operator()() noexcept { func_(this); }

protected:
    Func func_ = nullptr;
};

template <typename T, typename Invoker = Invoker>
class InvokerAdapter : public Invoker {
public:
    template <typename... Args>
    InvokerAdapter(Args &&...args) : Invoker(std::forward<Args>(args)...) {
        this->func_ = &invoke_static_;
    }

private:
    static void invoke_static_(void *self) noexcept {
        static_cast<T *>(self)->invoke();
    }
};

class WorkInvoker : public Invoker {
public:
    SingleLinkEntry work_queue_entry_;
};

} // namespace condy