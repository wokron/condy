#pragma once

#include "condy/singleton.hpp"
#include <cassert>

namespace condy {

struct Ring;
struct IRuntime;
struct Invoker;
using ScheduleLocalFunc = void (*)(IRuntime *, Invoker *);

class Context : public ThreadLocalSingleton<Context> {
public:
    void init(Ring *ring, IRuntime *runtime = nullptr,
              ScheduleLocalFunc schedule_local = nullptr) {
        ring_ = ring;
        runtime_ = runtime;
        schedule_local_ = schedule_local;
    }
    void destroy() {
        ring_ = nullptr;
        runtime_ = nullptr;
        schedule_local_ = nullptr;
    }

    Ring *ring() { return ring_; }

    IRuntime *runtime() { return runtime_; }

    void schedule_local(Invoker *invoker) {
        assert(runtime_ != nullptr);
        assert(schedule_local_ != nullptr);
        schedule_local_(runtime_, invoker);
    }

private:
    Ring *ring_ = nullptr;
    IRuntime *runtime_ = nullptr;
    ScheduleLocalFunc schedule_local_ = nullptr;
};

} // namespace condy