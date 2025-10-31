#pragma once

#include "condy/context.hpp"
#include "condy/finish_handles.hpp"
#include "condy/ring.hpp"

namespace condy {

inline void OpFinishHandle::cancel() {
    auto ring = Context::current().ring();
    ring->cancel_op(this);
}

} // namespace condy