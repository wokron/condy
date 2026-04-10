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
#include "condy/context.hpp"
#include "condy/ring.hpp"
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
 * @return int32_t The result of the operation, which is the value of `cqe->res`
 * for the corresponding CQE.
 */
class SimpleCQEHandler {
public:
    using ReturnType = int32_t;

    void handle_cqe(io_uring_cqe *cqe) noexcept { res_ = cqe->res; }

    ReturnType extract_result() noexcept { return res_; }

private:
    int32_t res_ = -ENOTRECOVERABLE; // Internal error if not set
};

/**
 * @brief A CQE handler that returns the selected buffers based on the result of
 * the CQE.
 * @tparam Br The buffer ring type
 * @return std::pair<int32_t, typename Br::ReturnType> A pair containing the
 * result of the operation (the value of `cqe->res`) and the selected buffers.
 */
template <BufferRingLike Br> class SelectBufferCQEHandler {
public:
    using ReturnType = std::pair<int32_t, typename Br::ReturnType>;

    SelectBufferCQEHandler(Br *buffers) : buffers_(buffers) {}

    void handle_cqe(io_uring_cqe *cqe) noexcept {
        res_ = cqe->res;
        buffer_ = buffers_->handle_finish(cqe);
    }

    ReturnType extract_result() noexcept { return {res_, std::move(buffer_)}; }

private:
    using BufferType = typename Br::ReturnType;

    int32_t res_ = -ENOTRECOVERABLE; // Internal error if not set
    BufferType buffer_ = {};
    Br *buffers_;
};

/**
 * @brief A CQE handler for NVMe passthrough commands that extracts the status
 * and result from the CQE.
 * @return std::pair<int32_t, uint64_t> A pair containing the status and result
 * of the NVMe command.
 */
class NVMePassthruCQEHandler {
public:
    using ReturnType = std::pair<int32_t, uint64_t>;

    void handle_cqe(io_uring_cqe *cqe) noexcept {
        assert(detail::check_cqe32(cqe) &&
               "Expected big CQE for NVMe passthrough");
        res_ = cqe->res;
        nvme_result_ = cqe->big_cqe[0];
    }

    ReturnType extract_result() noexcept { return {res_, nvme_result_}; }

private:
    int32_t res_ = -ENOTRECOVERABLE; // Internal error if not set
    uint64_t nvme_result_ = 0;
};

#if !IO_URING_CHECK_VERSION(2, 12) // >= 2.12
/**
 * @brief Result for TX timestamp operations, containing timestamp information
 * from the socket error queue
 */
struct TxTimestampResult {
    /**
     * @brief The timestamp type, could be SCM_TSTAMP_SND, SCM_TSTAMP_SCHED,
     * SCM_TSTAMP_ACK, etc.
     */
    int tstype; // cqe->flags >> IORING_TIMESTAMP_TYPE_SHIFT

    /**
     * @brief Whether this timestamp is a hardware timestamp.
     */
    bool hwts; // cqe->flags & IORING_CQE_F_TSTAMP_HW

    /**
     * @brief The timestamp value.
     */
    io_timespec ts; // *(io_timespec *)(cqe + 1)
};

/**
 * @brief A CQE handler for TX timestamp operations that extracts timestamp
 * information from the CQE.
 * @return std::pair<int32_t, TxTimestampResult> Result of the TX timestamp
 * operation.
 */
class TxTimestampCQEHandler {
public:
    using ReturnType = std::pair<int32_t, TxTimestampResult>;

    void handle_cqe(io_uring_cqe *cqe) noexcept {
        assert(detail::check_cqe32(cqe) &&
               "Expected big CQE for TX timestamp operations");
        res_ = cqe->res;
        result_.tstype =
            static_cast<int>(cqe->flags >> IORING_TIMESTAMP_TYPE_SHIFT);
        result_.hwts = cqe->flags & IORING_CQE_F_TSTAMP_HW;
        result_.ts = *reinterpret_cast<io_timespec *>(cqe + 1);
    }

    ReturnType extract_result() noexcept { return {res_, result_}; }

private:
    int32_t res_ = -ENOTRECOVERABLE; // Internal error if not set
    TxTimestampResult result_;
};
#endif

} // namespace condy