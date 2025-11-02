#pragma once

#include "condy/singleton.hpp"
#include <cassert>

namespace condy {

struct Ring;
struct IRuntime;
struct WorkInvoker;
using ScheduleLocalFunc = void (*)(IRuntime *, WorkInvoker *);

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

    void schedule_local(WorkInvoker *work) {
        assert(runtime_ != nullptr);
        assert(schedule_local_ != nullptr);
        schedule_local_(runtime_, work);
    }

private:
    Ring *ring_ = nullptr;
    IRuntime *runtime_ = nullptr;
    ScheduleLocalFunc schedule_local_ = nullptr;
};

} // namespace condy