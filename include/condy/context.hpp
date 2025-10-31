#pragma once

#include "condy/singleton.hpp"

namespace condy {

struct Ring;

class Context : public ThreadLocalSingleton<Context> {
public:
    void init(Ring *ring) { ring_ = ring; }
    void destroy() { ring_ = nullptr; }

    Ring *ring() { return ring_; }

private:
    Ring *ring_ = nullptr;
};

} // namespace condy