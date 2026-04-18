#pragma once

#include "condy/buffers.hpp"
#include "condy/condy_uring.hpp"
#include "condy/context.hpp"
#include "condy/ring.hpp"
#include "condy/utils.hpp"

namespace condy {

// TODO: Maybe we need greater version requirements here
#if !IO_URING_CHECK_VERSION(2, 10) // >= 2.10

class ZeroCopyRxBufferPool;

using ZeroCopyRxBuffer = detail::ManagedBuffer<ZeroCopyRxBufferPool>;

struct ZeroCopyRxArea {
    void *addr = nullptr;
    size_t size;
};

struct ZeroCopyRxDMABufArea {
    int dmabuf_fd;
    size_t offset;
    size_t size;
};

class ZeroCopyRxBufferPool {
public:
    ZeroCopyRxBufferPool(uint32_t if_idx, uint32_t if_rxq, uint32_t rq_entries,
                         const ZeroCopyRxArea &area)
        : ZeroCopyRxBufferPool(if_idx, if_rxq, rq_entries, area, 0) {}

    // Device-less constructor, DO NOT use this in production code if you don't
    // know what you are doing.
    ZeroCopyRxBufferPool(uint32_t rq_entries, const ZeroCopyRxArea &area)
        : ZeroCopyRxBufferPool(0, 0, rq_entries, area, 2) {
        // TODO: flags should be an enum class
        device_less_ = true;
    }

    ZeroCopyRxBufferPool(uint32_t if_idx, uint32_t if_rxq, uint32_t rq_entries,
                         const ZeroCopyRxDMABufArea &area) {
        area_size_ = 0;
        area_ptr_ = nullptr;

        io_uring_zcrx_area_reg area_reg = {};
        area_reg.addr = area.offset;
        area_reg.len = area.size;
        area_reg.flags = IORING_ZCRX_AREA_DMABUF;

        register_ifq_(if_idx, if_rxq, rq_entries, area_reg,
                      sysconf(_SC_PAGESIZE), 0);
    }

    ~ZeroCopyRxBufferPool() {
        [[maybe_unused]] int r;
        if (area_size_ > 0) {
            assert(area_ptr_ != nullptr);
            r = munmap(area_ptr_, area_size_);
            assert(r == 0);
        }
        assert(rq_ring_.ring_ptr != nullptr);
        r = munmap(rq_ring_.ring_ptr, ring_size_);
        assert(r == 0);
        // TODO: Unregister ifq
    }

    ZeroCopyRxBufferPool(const ZeroCopyRxBufferPool &) = delete;
    ZeroCopyRxBufferPool &operator=(const ZeroCopyRxBufferPool &) = delete;
    ZeroCopyRxBufferPool(ZeroCopyRxBufferPool &&) = delete;
    ZeroCopyRxBufferPool &operator=(ZeroCopyRxBufferPool &&) = delete;

private:
    ZeroCopyRxBufferPool(uint32_t if_idx, uint32_t if_rxq, uint32_t rq_entries,
                         const ZeroCopyRxArea &area, uint32_t flags) {
        const size_t page_size = sysconf(_SC_PAGESIZE);

        if (area.addr == nullptr) {
            area_size_ = align_up(area.size, page_size);
            area_ptr_ = mmap(nullptr, area_size_, PROT_READ | PROT_WRITE,
                             MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
            if (area_ptr_ == MAP_FAILED) {
                throw make_system_error("mmap");
            }
            auto d = defer([&]() { munmap(area_ptr_, area_size_); });

            io_uring_zcrx_area_reg area_reg = {};
            area_reg.addr = reinterpret_cast<uint64_t>(area_ptr_);
            area_reg.len = area_size_;
            area_reg.flags = 0;

            register_ifq_(if_idx, if_rxq, rq_entries, area_reg, page_size,
                          flags);

            d.dismiss();
        } else {
            // Not owned, so we don't track the size for unmapping
            area_size_ = 0;
            area_ptr_ = area.addr;

            io_uring_zcrx_area_reg area_reg = {};
            area_reg.addr = reinterpret_cast<uint64_t>(area_ptr_);
            area_reg.len = area.size;
            area_reg.flags = 0;

            register_ifq_(if_idx, if_rxq, rq_entries, area_reg, page_size,
                          flags);
        }
    }

public:
    uint32_t zcrx_id() const noexcept { return zcrx_id_; }

    ZeroCopyRxBuffer handle_finish(io_uring_cqe *cqe) noexcept {
        if (cqe->res < 0) {
            return ZeroCopyRxBuffer();
        }
        io_uring_zcrx_cqe *rcqe =
            reinterpret_cast<io_uring_zcrx_cqe *>(cqe + 1);
        void *data = static_cast<char *>(area_ptr_) +
                     (rcqe->off & ~IORING_ZCRX_AREA_MASK);
        size_t size = static_cast<size_t>(cqe->res);
        return ZeroCopyRxBuffer(data, size, this);
    }

    void add_buffer_back(void *ptr, size_t size) noexcept {
        rq_enqueue_(ptr, size);
        maybe_flush_rq_();
    }

private:
    void register_ifq_(uint32_t if_idx, uint32_t if_rxq, uint32_t rq_entries,
                       io_uring_zcrx_area_reg &area_reg, size_t page_size,
                       uint32_t flags) {
        rq_entries = std::bit_ceil(rq_entries);
        io_uring_region_desc region_reg = {};
        ring_size_ = get_refill_ring_size_(rq_entries, page_size);
        region_reg.user_addr = 0;
        region_reg.size = ring_size_;
        region_reg.flags = 0;

        io_uring_zcrx_ifq_reg reg = {};
        reg.if_idx = if_idx;
        reg.if_rxq = if_rxq;
        reg.rq_entries = rq_entries;
        reg.area_ptr = reinterpret_cast<uint64_t>(&area_reg);
        reg.region_ptr = reinterpret_cast<uint64_t>(&region_reg);
        reg.flags = flags;

        auto *ring = detail::Context::current().ring();
        int r = io_uring_register_ifq(ring->ring(), &reg);
        if (r != 0) {
            throw make_system_error("io_uring_register_ifq", -r);
        }
        // TODO: unregister ifq if any exception

        void *ring_ptr = mmap(nullptr, ring_size_, PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_POPULATE, ring->ring()->ring_fd,
                              static_cast<off_t>(region_reg.mmap_offset));
        if (ring_ptr == MAP_FAILED) {
            throw make_system_error("mmap");
        }
        rq_ring_.khead = (unsigned int *)((char *)ring_ptr + reg.offsets.head);
        rq_ring_.ktail = (unsigned int *)((char *)ring_ptr + reg.offsets.tail);
        rq_ring_.rqes =
            (struct io_uring_zcrx_rqe *)((char *)ring_ptr + reg.offsets.rqes);
        rq_ring_.rq_tail = 0;
        rq_ring_.ring_entries = reg.rq_entries;
        rq_ring_.ring_ptr = ring_ptr;

        zcrx_id_ = reg.zcrx_id;
        area_token_ = area_reg.rq_area_token;
    }

    static size_t get_refill_ring_size_(uint32_t rq_entries,
                                        size_t page_size) noexcept {
        size_t ring_size = rq_entries * sizeof(io_uring_zcrx_rqe);
        ring_size += page_size;
        ring_size = align_up(ring_size, page_size);
        return ring_size;
    }

    size_t rq_nr_queued_() const noexcept {
        return rq_ring_.rq_tail - io_uring_smp_load_acquire(rq_ring_.khead);
    }

    static int io_uring_register_zcrx_ctrl_(struct io_uring *ring,
                                            struct zcrx_ctrl *ctrl) noexcept {
        unsigned int opcode = IORING_REGISTER_ZCRX_CTRL;
        int fd;
        if (ring->int_flags & 1) {
            opcode |= IORING_REGISTER_USE_REGISTERED_RING;
            fd = ring->enter_ring_fd;
        } else {
            fd = ring->ring_fd;
        }
        return io_uring_register(fd, opcode, ctrl, 0);
    }

    void rq_enqueue_(void *ptr, size_t size) noexcept {
        assert(rq_nr_queued_() < rq_ring_.ring_entries);
        io_uring_zcrx_rqe *rqe;
        unsigned rq_mask = rq_ring_.ring_entries - 1;
        rqe = &rq_ring_.rqes[rq_ring_.rq_tail & rq_mask];
        rqe->off = (static_cast<char *>(ptr) - static_cast<char *>(area_ptr_)) |
                   area_token_;
        rqe->len = static_cast<uint32_t>(size);
        io_uring_smp_store_release(rq_ring_.ktail, ++rq_ring_.rq_tail);
    }

    void flush_rq_() noexcept {
        auto *ring = detail::Context::current().ring();
        zcrx_ctrl ctrl = {};
        ctrl.zcrx_id = zcrx_id_;
        ctrl.op = ZCRX_CTRL_FLUSH_RQ;
        [[maybe_unused]] int r =
            io_uring_register_zcrx_ctrl_(ring->ring(), &ctrl);
        assert(r == 0);
    }

    void maybe_flush_rq_() noexcept {
        if (rq_nr_queued_() >= rq_ring_.ring_entries || device_less_) {
            flush_rq_();
        }
    }

private:
    void *area_ptr_;
    size_t area_size_;
    size_t ring_size_;
    io_uring_zcrx_rq rq_ring_;
    uint32_t zcrx_id_;
    uint64_t area_token_;
    bool device_less_ = false;
};

#endif

} // namespace condy