#pragma once

namespace condy {

class Invoker {
public:
    using Func = void (*)(void *);
    Invoker(Func func) : func_(func) {}

    void operator()() { func_(this); }

private:
    Func func_;
};

template <typename T> class InvokerAdapter : public Invoker {
public:
    InvokerAdapter() : Invoker(&InvokerAdapter::invoke_) {}

private:
    static void invoke_(void *self) { static_cast<T *>(self)->operator()(); }
};

} // namespace condy