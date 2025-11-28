#pragma once

#include "condy/singleton.hpp"
#include <cassert>

namespace condy {

class Ring;
class Runtime;
class WorkInvoker;

class Context : public ThreadLocalSingleton<Context> {
public:
    void init(Ring *ring, Runtime *runtime) {
        ring_ = ring;
        runtime_ = runtime;
    }
    void reset() {
        ring_ = nullptr;
        runtime_ = nullptr;
    }

    Ring *ring() { return ring_; }

    Runtime *runtime() { return runtime_; }

private:
    Ring *ring_ = nullptr;
    Runtime *runtime_ = nullptr;
};

} // namespace condy