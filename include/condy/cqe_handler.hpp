#pragma once

#include "condy/concepts.hpp"
#include <cerrno>
#include <cstdint>
#include <utility>

namespace condy {

class DefaultCQEHandler {
public:
    using ReturnType = int32_t;

    void handle_cqe(io_uring_cqe *cqe) { res_ = cqe->res; }

    ReturnType extract_result() { return res_; }

private:
    int32_t res_ = -ENOTRECOVERABLE; // Internal error if not set
};

template <BufferRingLike Br> class SelectBufferCQEHandler {
public:
    using ReturnType = std::pair<int, typename Br::ReturnType>;

    SelectBufferCQEHandler(Br *buffers) : buffers_(buffers) {}

    void handle_cqe(io_uring_cqe *cqe) {
        res_ = cqe->res;
        flags_ = cqe->flags;
    }

    ReturnType extract_result() {
        return std::make_pair(res_, buffers_->handle_finish(res_, flags_));
    }

private:
    int32_t res_ = -ENOTRECOVERABLE; // Internal error if not set
    uint32_t flags_ = 0;
    Br *buffers_;
};

} // namespace condy