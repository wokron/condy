#pragma once

#include "condy/concepts.hpp"
#include "condy/invoker.hpp"
#include <cerrno>

namespace condy {

class OpFinishHandleBase {
public:
    using HandleFunc = bool (*)(void *, io_uring_cqe *) noexcept;

    bool handle(io_uring_cqe *cqe) noexcept {
        assert(handle_func_ != nullptr);
        return handle_func_(this, cqe);
    }

    void set_invoker(Invoker *invoker) noexcept { invoker_ = invoker; }

protected:
    OpFinishHandleBase() = default;

protected:
    HandleFunc handle_func_ = nullptr;
    Invoker *invoker_ = nullptr;
};

} // namespace condy