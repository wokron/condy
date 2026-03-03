#pragma once

#include "condy/buffers.hpp"
#include "condy/condy_uring.hpp"
#include "condy/context.hpp"
#include "condy/ring.hpp"
#include <utility>

namespace condy {

#if !IO_URING_CHECK_VERSION(2, 10) // >= 2.10

class ZeroCopyRxBufferPool;

struct ZeroCopyRxBuffer : public BufferBase {
public:
    ZeroCopyRxBuffer() = default;
    ZeroCopyRxBuffer(void *data, size_t size, ZeroCopyRxBufferPool *pool)
        : data_(data), size_(size), pool_(pool) {}
    ZeroCopyRxBuffer(ZeroCopyRxBuffer &&other) noexcept
        : data_(std::exchange(other.data_, nullptr)),
          size_(std::exchange(other.size_, 0)),
          pool_(std::exchange(other.pool_, nullptr)) {}
    ZeroCopyRxBuffer &operator=(ZeroCopyRxBuffer &&other) noexcept {
        if (this != &other) {
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
            pool_ = std::exchange(other.pool_, nullptr);
        }
        return *this;
    }

    ~ZeroCopyRxBuffer() { reset(); }

    ZeroCopyRxBuffer(const ZeroCopyRxBuffer &) = delete;
    ZeroCopyRxBuffer &operator=(const ZeroCopyRxBuffer &) = delete;

public:
    /**
     * @brief Get the data pointer of the buffer
     */
    void *data() const { return data_; }

    /** *
     * @brief Get the size of the buffer
     */
    size_t size() const { return size_; }

    /**
     * @brief Reset the buffer, returning it to the pool if owned
     */
    void reset();

    /**
     * @brief Check if the buffer owns a buffer from a pool.
     */
    bool owns_buffer() const { return pool_ != nullptr; }

private:
    void *data_ = nullptr;
    size_t size_ = 0;
    ZeroCopyRxBufferPool *pool_ = nullptr;
};

class ZeroCopyRxBufferPool {
public:
    // TODO: support different area types
    ZeroCopyRxBufferPool(uint32_t if_idx, uint32_t if_rxq, uint32_t rq_entries,
                         size_t area_size) {
        area_size_ = area_size;
        ring_size_ = get_refill_ring_size_(rq_entries);

        area_ptr_ = mmap(nullptr, area_size_, PROT_READ | PROT_WRITE,
                         MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
        if (area_ptr_ == MAP_FAILED) {
            throw std::bad_alloc();
        }

        io_uring_region_desc region_reg = {};
        region_reg.user_addr = 0;
        region_reg.size = ring_size_;
        region_reg.flags = 0;
        io_uring_zcrx_area_reg area_reg = {};
        area_reg.addr = reinterpret_cast<uint64_t>(area_ptr_);
        area_reg.len = area_size_;
        area_reg.flags = 0;

        io_uring_zcrx_ifq_reg reg = {};
        reg.if_idx = if_idx;
        reg.if_rxq = if_rxq;
        reg.rq_entries = rq_entries;
        reg.area_ptr = reinterpret_cast<uint64_t>(&area_reg);
        reg.region_ptr = reinterpret_cast<uint64_t>(&region_reg);

        register_ifq_(&reg);
    }

    ~ZeroCopyRxBufferPool() {
        [[maybe_unused]] int r;
        assert(area_ptr_ != nullptr);
        r = munmap(area_ptr_, area_size_);
        assert(r == 0);
        assert(ring_ptr_ != nullptr);
        r = munmap(ring_ptr_, ring_size_);
        assert(r == 0);
        // TODO: Unregister ifq
    }

    ZeroCopyRxBufferPool(const ZeroCopyRxBufferPool &) = delete;
    ZeroCopyRxBufferPool &operator=(const ZeroCopyRxBufferPool &) = delete;
    ZeroCopyRxBufferPool(ZeroCopyRxBufferPool &&) = delete;
    ZeroCopyRxBufferPool &operator=(ZeroCopyRxBufferPool &&) = delete;

public:
    uint32_t zcrx_id() const { return zcrx_id_; }

    ZeroCopyRxBuffer handle_finish(io_uring_cqe *cqe) {
        if (cqe->res < 0) {
            return ZeroCopyRxBuffer();
        }
        io_uring_zcrx_cqe *rcqe =
            reinterpret_cast<io_uring_zcrx_cqe *>(cqe + 1);
        void *data = static_cast<char *>(area_ptr_) +
                     (rcqe->off & ~~IORING_ZCRX_AREA_MASK);
        size_t size = static_cast<size_t>(cqe->res);
        return ZeroCopyRxBuffer(data, size, this);
    }

    void add_buffer_back(void *ptr, size_t size) {
        // TODO: check refill queue full
        io_uring_zcrx_rqe *rqe;
        unsigned rq_mask = rq_ring_.ring_entries - 1;
        rqe = &rq_ring_.rqes[rq_ring_.rq_tail & rq_mask];
        rqe->off = (static_cast<char *>(ptr) - static_cast<char *>(area_ptr_)) |
                   area_token_;
        rqe->len = static_cast<uint32_t>(size);
        io_uring_smp_store_release(rq_ring_.ktail, ++rq_ring_.rq_tail);
    }

private:
    void register_ifq_(io_uring_zcrx_ifq_reg *reg) {
        // NOLINTBEGIN(performance-no-int-to-ptr)
        auto *region_reg =
            reinterpret_cast<io_uring_region_desc *>(reg->region_ptr);
        auto *area_reg =
            reinterpret_cast<io_uring_zcrx_area_reg *>(reg->area_ptr);
        // NOLINTEND(performance-no-int-to-ptr)

        auto *ring = detail::Context::current().ring();
        int r = io_uring_register_ifq(ring->ring(), reg);
        if (r != 0) {
            throw make_system_error("io_uring_register_ifq", -r);
        }

        ring_ptr_ = mmap(nullptr, ring_size_, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_POPULATE, ring->ring()->ring_fd,
                         static_cast<off_t>(region_reg->mmap_offset));
        if (ring_ptr_ == MAP_FAILED) {
            throw std::bad_alloc();
        }
        rq_ring_.khead =
            (unsigned int *)((char *)ring_ptr_ + reg->offsets.head);
        rq_ring_.ktail =
            (unsigned int *)((char *)ring_ptr_ + reg->offsets.tail);
        rq_ring_.rqes =
            (struct io_uring_zcrx_rqe *)((char *)ring_ptr_ + reg->offsets.rqes);
        rq_ring_.rq_tail = 0;
        rq_ring_.ring_entries = reg->rq_entries;

        zcrx_id_ = reg->zcrx_id;
        area_token_ = area_reg->rq_area_token;
    }

    static size_t get_refill_ring_size_(uint32_t rq_entries) {
        constexpr size_t page_size = 4096; // TODO: get system page size
        size_t ring_size = rq_entries * sizeof(io_uring_zcrx_rqe);
        ring_size += page_size;
        ring_size = std::bit_ceil(ring_size);
        return ring_size;
    }

private:
    void *area_ptr_;
    size_t area_size_;
    void *ring_ptr_;
    size_t ring_size_;
    io_uring_zcrx_rq rq_ring_;
    uint32_t zcrx_id_;
    uint64_t area_token_;
};

inline void ZeroCopyRxBuffer::reset() {
    if (pool_ != nullptr) {
        pool_->add_buffer_back(data_, size_);
    }
    data_ = nullptr;
    size_ = 0;
    pool_ = nullptr;
}

#endif

} // namespace condy