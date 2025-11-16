#pragma once

#include "condy/intrusive.hpp"

namespace condy {

class Invoker {
public:
    using Func = void (*)(void *);
    Invoker(Func func) : func_(func) {}

    void operator()() { func_(this); }

protected:
    Func func_;
};

template <typename T, typename Invoker = Invoker>
class InvokerAdapter : public Invoker {
public:
    InvokerAdapter() : Invoker(&InvokerAdapter::invoke_) {}

private:
    static void invoke_(void *self) { static_cast<T *>(self)->invoke(); }
};

class WorkInvoker : public Invoker {
public:
    using Invoker::Invoker;
    SingleLinkEntry work_queue_entry_;

    bool is_operation() const { return is_operation_; }

protected:
    bool is_operation_ = false;
};

} // namespace condy