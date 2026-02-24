/**
 * @file cqe_handler.hpp
 * @brief Definitions of CQE handlers
 * @details This file defines a series of CQE handlers, which are responsible
 * for processing the completion of asynchronous operations.
 */

#pragma once

#include "condy/concepts.hpp"
#include "condy/context.hpp"
#include "condy/ring.hpp"
#include "condy/zcrx.hpp"
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
struct SimpleCQEHandler {
    int32_t operator()(io_uring_cqe *cqe) noexcept { return cqe->res; }
};

/**
 * @brief A CQE handler that returns the selected buffers based on the result of
 * the CQE.
 * @tparam Br The buffer ring type
 * @return std::pair<int32_t, BufferType> A pair containing the
 * result of the operation (the value of `cqe->res`) and the selected buffer,
 * whose type is determined by the buffer ring.
 */
template <BufferRingLike Br> class SelectBufferCQEHandler {
public:
    SelectBufferCQEHandler(Br *buffers) : buffers_(buffers) {}

    auto operator()(io_uring_cqe *cqe) noexcept {
        return std::make_pair(cqe->res, buffers_->handle_finish(cqe));
    }

private:
    Br *buffers_;
};

/**
 * @brief A CQE handler for NVMe passthrough commands that extracts the status
 * and result from the CQE.
 * @return std::pair<int32_t, uint64_t> A pair containing the status and result
 * of the NVMe command.
 */
struct NVMePassthruCQEHandler {
    std::pair<int32_t, uint64_t> operator()(io_uring_cqe *cqe) noexcept {
        assert(detail::check_cqe32(cqe) &&
               "Expected big CQE for NVMe passthrough");
        return {cqe->res, cqe->big_cqe[0]};
    }
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
struct TxTimestampCQEHandler {
    std::pair<int32_t, TxTimestampResult>
    operator()(io_uring_cqe *cqe) noexcept {
        assert(detail::check_cqe32(cqe) &&
               "Expected big CQE for TX timestamp operations");
        TxTimestampResult result;
        result.tstype =
            static_cast<int>(cqe->flags >> IORING_TIMESTAMP_TYPE_SHIFT);
        result.hwts = cqe->flags & IORING_CQE_F_TSTAMP_HW;
        result.ts = *reinterpret_cast<io_timespec *>(cqe + 1);
        return {cqe->res, result};
    }
};
#endif

#if !IO_URING_CHECK_VERSION(2, 10) // >= 2.10
class ZeroCopyRxCQEHandler {
public:
    using ReturnType = std::pair<int, ZeroCopyRxBuffer>;

    ZeroCopyRxCQEHandler(ZeroCopyRxBufferPool *pool) : pool_(pool) {}

    void handle_cqe(io_uring_cqe *cqe) {
        assert(detail::check_cqe32(cqe) &&
               "Expected big CQE for zero-copy RX operations");
        result_.first = cqe->res;
        result_.second = pool_->handle_finish(cqe);
    }

    ReturnType extract_result() { return std::move(result_); }

private:
    ReturnType result_;
    ZeroCopyRxBufferPool *pool_;
};
#endif

} // namespace condy