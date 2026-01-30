/**
 * @file context.hpp
 */

#pragma once

#include "condy/singleton.hpp"
#include <cassert>
#include <cstdint>

namespace condy {

class Ring;
class Runtime;
class WorkInvoker;

namespace detail {

class Context : public ThreadLocalSingleton<Context> {
public:
    void init(Ring *ring, Runtime *runtime) {
        ring_ = ring;
        runtime_ = runtime;
        next_bgid_ = 0;
        cred_id_ = 0;
    }
    void reset() {
        ring_ = nullptr;
        runtime_ = nullptr;
        next_bgid_ = 0;
        cred_id_ = 0;
    }

    Ring *ring() { return ring_; }

    Runtime *runtime() { return runtime_; }

    uint16_t next_bgid() { return next_bgid_++; }

    void set_cred_id(uint16_t id) { cred_id_ = id; }
    uint16_t cred_id() { return cred_id_; }

private:
    Ring *ring_ = nullptr;
    Runtime *runtime_ = nullptr;
    uint16_t next_bgid_ = 0;
    uint16_t cred_id_ = 0;
};

} // namespace detail

} // namespace condy