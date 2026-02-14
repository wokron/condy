/**
 * @file cqe_handler.hpp
 * @brief Definitions of CQE handlers
 * @details This file defines a series of CQE handlers, which are responsible
 * for processing the completion of asynchronous operations. Each handler
 * defines a `handle_cqe` method to process the CQE and an `extract_result`
 * method to retrieve the result of the operation.
 */

#pragma once

#include "condy/concepts.hpp"
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <utility>

namespace condy {

namespace detail {

// Just for debugging, check if the CQE is big as expected
inline bool check_cqe32([[maybe_unused]] io_uring_cqe *cqe) {
    auto *ring = detail::Context::current().ring();
    assert(ring != nullptr);
    auto ring_flags = ring->ring()->flags;
    if (ring_flags & IORING_SETUP_CQE32) {
        return true;
    }
#if !IO_URING_CHECK_VERSION(2, 13) // >= 2.13
    if (ring_flags & IORING_SETUP_CQE_MIXED) {
        return cqe->flags & IORING_CQE_F_32;
    }
#endif
    return false;
}

} // namespace detail

/**
 * @brief A simple CQE handler that extracts the result from the CQE without any
 * additional processing.
 */
class SimpleCQEHandler {
public:
    using ReturnType = int32_t;

    void handle_cqe(io_uring_cqe *cqe) { res_ = cqe->res; }

    ReturnType extract_result() { return res_; }

private:
    int32_t res_ = -ENOTRECOVERABLE; // Internal error if not set
};

/**
 * @brief A CQE handler that returns the selected buffers based on the result of
 * the CQE.
 * @tparam Br The buffer ring type
 */
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

/**
 * @brief Result for NVMe passthrough commands, containing the status and result
 * of the command
 */
struct NVMeResult {
    int status;      // cqe->res
    uint64_t result; // cqe->big_cqe[0]
};

/**
 * @brief A CQE handler for NVMe passthrough commands that extracts the status
 * and result from the CQE.
 */
class NVMePassthruCQEHandler {
public:
    using ReturnType = NVMeResult;

    void handle_cqe(io_uring_cqe *cqe) {
        assert(detail::check_cqe32(cqe) &&
               "Expected big CQE for NVMe passthrough");
        result_.status = cqe->res;
        result_.result = cqe->big_cqe[0];
    }

    ReturnType extract_result() { return result_; }

private:
    NVMeResult result_;
};

#if !IO_URING_CHECK_VERSION(2, 12) // >= 2.12
/**
 * @brief Result for TX timestamp operations, containing timestamp information
 * from the socket error queue
 */
struct TxTimestampResult {
    int tskey;
    int tstype;
    io_timespec ts;
    bool hwts;
};

/**
 * @brief A CQE handler for TX timestamp operations that extracts timestamp
 * information from the CQE.
 */
class TxTimestampCQEHandler {
public:
    using ReturnType = TxTimestampResult;

    void handle_cqe(io_uring_cqe *cqe) {
        assert(detail::check_cqe32(cqe) &&
               "Expected big CQE for TX timestamp operations");
        result_.tskey = cqe->res;
        result_.tstype =
            static_cast<int>(cqe->flags >> IORING_TIMESTAMP_TYPE_SHIFT);
        result_.hwts = cqe->flags & IORING_CQE_F_TSTAMP_HW;
        result_.ts = *reinterpret_cast<io_timespec *>(cqe + 1);
    }

    ReturnType extract_result() { return result_; }

private:
    TxTimestampResult result_;
};
#endif

} // namespace condy