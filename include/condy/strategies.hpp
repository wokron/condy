#pragma once

#include "condy/condy_uring.hpp"
#include <cstddef>

namespace condy {

class IStrategy {
public:
    virtual ~IStrategy() = default;

    virtual size_t get_ready_queue_capacity() const = 0;

    virtual int init_io_uring(io_uring *ring) = 0;

    virtual int generate_task_id() = 0;

    virtual void recycle_task_id(int id) = 0;

    virtual bool should_stop() = 0;

    virtual int submit_and_wait(io_uring *ring) = 0;

    virtual void record_submitted(int submitted) = 0;

    virtual void record_finished(int finished) = 0;

    virtual io_uring_sqe *get_sqe(io_uring *ring) = 0;
};

class SimpleStrategy : public IStrategy {
public:
    SimpleStrategy(unsigned int io_uring_entries)
        : io_uring_entries_(io_uring_entries) {};

public:
    size_t get_ready_queue_capacity() const override { return 1024; }

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
        __kernel_timespec ts = {
            .tv_sec = 0,
            .tv_nsec = 1000000,
        };
        io_uring_cqe *cqe;
        return condy_submit_and_wait_timeout(ring, &cqe, 1, &ts, nullptr);
    }

    void record_submitted(int submitted) override {}

    void record_finished(int finished) override {}

    io_uring_sqe *get_sqe(io_uring *ring) override {
        io_uring_sqe *sqe = io_uring_get_sqe(ring);
        if (!sqe) {
            io_uring_submit(ring);
            sqe = io_uring_get_sqe(ring);
        }
        return sqe;
    }

private:
    unsigned int io_uring_entries_;
    int next_task_id_ = 0;
    size_t running_tasks_ = 0;
};

} // namespace condy