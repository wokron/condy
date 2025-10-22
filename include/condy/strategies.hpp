#pragma once

#include "condy/condy_uring.hpp"
#include <cstddef>

namespace condy {

class IStrategy {
public:
    virtual ~IStrategy() = default;

    virtual int init_io_uring(io_uring *ring) = 0;

    virtual int generate_task_id() = 0;

    virtual void recycle_task_id(int id) = 0;

    virtual bool should_stop() = 0;

    virtual int submit_and_wait(io_uring *ring) = 0;

    virtual void record_submitted(int submitted) = 0;

    virtual void record_finished(int finished) = 0;
};

class SimpleStrategy : public IStrategy {
public:
    SimpleStrategy(unsigned int io_uring_entries)
        : io_uring_entries_(io_uring_entries) {};

public:
    int init_io_uring(io_uring *ring) override {
        return io_uring_queue_init(io_uring_entries_, ring, 0);
    }

    int generate_task_id() override {
        running_tasks_++;
        return next_task_id_++;
    }

    void recycle_task_id(int id) override {
        (void)id;
        running_tasks_--;
    }

    bool should_stop() override { return running_tasks_ == 0; }

    int submit_and_wait(io_uring *ring) override {
        return io_uring_submit_and_wait(ring, 1);
    }

    void record_submitted(int submitted) override {}

    void record_finished(int finished) override {}

private:
    unsigned int io_uring_entries_;
    int next_task_id_ = 0;
    size_t running_tasks_ = 0;
};

} // namespace condy