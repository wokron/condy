/**
 * @file context.hpp
 */

#pragma once

#include "condy/singleton.hpp"
#include "condy/utils.hpp"
#include <cassert>
#include <cstdint>

namespace condy {

class Ring;
class Runtime;
class WorkInvoker;

namespace detail {

class Context : public ThreadLocalSingleton<Context> {
public:
    void init(Ring *ring, Runtime *runtime) noexcept {
        ring_ = ring;
        runtime_ = runtime;
        bgid_pool_.reset();
    }
    void reset() noexcept {
        ring_ = nullptr;
        runtime_ = nullptr;
        bgid_pool_.reset();
    }

    Ring *ring() noexcept { return ring_; }

    Runtime *runtime() noexcept { return runtime_; }

    uint16_t next_bgid() { return bgid_pool_.allocate(); }

    void recycle_bgid(uint16_t bgid) noexcept { bgid_pool_.recycle(bgid); }

private:
    Ring *ring_ = nullptr;
    Runtime *runtime_ = nullptr;
    IdPool<uint16_t> bgid_pool_;
};

} // namespace detail

} // namespace condy