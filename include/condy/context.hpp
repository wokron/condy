#pragma once

#include "condy/singleton.hpp"
#include <cassert>
#include <cstddef>

namespace condy {

struct Ring;
struct IRuntime;
struct WorkInvoker;
using ScheduleLocalFunc = void (*)(IRuntime *, WorkInvoker *);
using NextBgidFunc = size_t (*)(IRuntime *);

class Context : public ThreadLocalSingleton<Context> {
public:
    void init(Ring *ring, IRuntime *runtime = nullptr,
              ScheduleLocalFunc schedule_local = nullptr,
              NextBgidFunc next_bgid = nullptr) {
        ring_ = ring;
        runtime_ = runtime;
        schedule_local_ = schedule_local;
        next_bgid_ = next_bgid;
    }
    void reset() {
        ring_ = nullptr;
        runtime_ = nullptr;
        schedule_local_ = nullptr;
        next_bgid_ = nullptr;
    }

    Ring *ring() { return ring_; }

    IRuntime *runtime() { return runtime_; }

    void schedule_local(WorkInvoker *work) {
        assert(runtime_ != nullptr);
        assert(schedule_local_ != nullptr);
        schedule_local_(runtime_, work);
    }

    size_t next_bgid() {
        assert(runtime_ != nullptr);
        assert(next_bgid_ != nullptr);
        return next_bgid_(runtime_);
    }

private:
    Ring *ring_ = nullptr;
    IRuntime *runtime_ = nullptr;
    ScheduleLocalFunc schedule_local_ = nullptr;
    NextBgidFunc next_bgid_ = nullptr;
};

} // namespace condy