#include "condy/context.hpp"
#include "condy/finish_handles.hpp"
#include "condy/invoker.hpp"
#include <cstddef>
#include <cstring>
#include <doctest/doctest.h>
#include <liburing.h>
#include <liburing/io_uring.h>

namespace {

struct SetFinishInvoker : public condy::InvokerAdapter<SetFinishInvoker> {
    void operator()() { finished = true; }
    bool finished = false;
};

struct SetUnfinishedInvoker
    : public condy::InvokerAdapter<SetUnfinishedInvoker> {
    SetUnfinishedInvoker(size_t &unfinished_ref) : unfinished(unfinished_ref) {}
    void operator()() { unfinished--; }
    size_t &unfinished;
};

void event_loop(size_t &unfinished) {
    auto *ring = condy::Context::current().ring();
    while (unfinished > 0) {
        ring->submit();
        ring->reap_completions([&](io_uring_cqe *cqe) {
            auto handle_ptr = static_cast<condy::OpFinishHandle *>(
                io_uring_cqe_get_data(cqe));
            handle_ptr->set_result(cqe->res);
            (*handle_ptr)();
        });
    }
}

} // namespace

TEST_CASE("test op_finish_handle - basic usage") {
    condy::Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);
    auto &context = condy::Context::current();
    context.init(&ring);

    SetFinishInvoker invoker;
    condy::OpFinishHandle handle;
    handle.set_invoker(&invoker);
    handle.set_ring(&ring);

    ring.register_op(io_uring_prep_nop, &handle);
    ring.submit();

    ring.wait_all_completions([](io_uring_cqe *cqe) {
        auto handle_ptr =
            static_cast<condy::OpFinishHandle *>(io_uring_cqe_get_data(cqe));
        handle_ptr->set_result(42);
        (*handle_ptr)();
    });

    REQUIRE(invoker.finished);
    REQUIRE(handle.extract_result() == 42);

    context.reset();
}

TEST_CASE("test op_finish_handle - concurrent ops") {
    size_t unfinished = 2;
    SetUnfinishedInvoker invoker{unfinished};

    condy::Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);
    auto &context = condy::Context::current();
    context.init(&ring);

    condy::OpFinishHandle handle1, handle2;
    auto on_finish = [](void *self, size_t no) {
        auto *unfinished_ptr = static_cast<size_t *>(self);
        (*unfinished_ptr)--;
    };
    handle1.set_invoker(&invoker);
    handle2.set_invoker(&invoker);
    handle1.set_ring(&ring);
    handle2.set_ring(&ring);

    ring.register_op(io_uring_prep_nop, &handle1);
    ring.register_op(io_uring_prep_nop, &handle2);

    event_loop(invoker.unfinished);

    REQUIRE(!ring.has_outstanding_ops());
    REQUIRE(unfinished == 0);

    context.reset();
}

TEST_CASE("test op_finish_handle - cancel op") {
    size_t unfinished = 1;
    SetUnfinishedInvoker invoker{unfinished};

    condy::Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);
    auto &context = condy::Context::current();
    context.init(&ring);

    condy::OpFinishHandle handle1, handle2;
    condy::ParallelFinishHandle<condy::WaitOneCancelCondition,
                                condy::OpFinishHandle, condy::OpFinishHandle>
        finish_handle;
    finish_handle.init(&handle1, &handle2);
    finish_handle.set_invoker(&invoker);
    handle1.set_ring(&ring);
    handle2.set_ring(&ring);

    ring.register_op(
        [&](io_uring_sqe *sqe) {
            __kernel_timespec ts{
                .tv_sec = 60 * 60,
                .tv_nsec = 0,
            };
            io_uring_prep_timeout(sqe, &ts, 0, 0);
        },
        &handle1);

    ring.register_op(io_uring_prep_nop, &handle2);

    event_loop(unfinished);

    REQUIRE(!ring.has_outstanding_ops());
    REQUIRE(unfinished == 0);

    auto r = finish_handle.extract_result();
    auto &[order, results] = r;
    REQUIRE(order[0] == 1);
    REQUIRE(std::get<0>(results) == -ECANCELED);
    REQUIRE(std::get<1>(results) == 0);

    context.reset();
}

namespace {

struct SetFinishWorkInvoker
    : public condy::InvokerAdapter<SetFinishWorkInvoker, condy::WorkInvoker> {
    void operator()() { finished = true; }
    bool finished = false;
    int result = -1;
};

} // namespace

TEST_CASE("test op_finish_handle - multishot op") {
    SetFinishWorkInvoker invoker;

    auto func = [&](int res) {
        invoker.result = res;
        invoker();
    };

    condy::MultiShotOpFinishHandle<decltype(func)> handle(func);
    REQUIRE(!invoker.finished);
    handle.multishot(1);
    REQUIRE(invoker.finished);
    REQUIRE(invoker.result == 1);
}