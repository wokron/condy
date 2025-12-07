#include "condy/context.hpp"
#include "condy/finish_handles.hpp"
#include "condy/invoker.hpp"
#include "condy/runtime.hpp"
#include <cstddef>
#include <cstring>
#include <doctest/doctest.h>
#include <liburing.h>
#include <liburing/io_uring.h>

namespace {

struct SetFinishInvoker : public condy::InvokerAdapter<SetFinishInvoker> {
    void invoke() { finished = true; }
    bool finished = false;
};

struct SetUnfinishedInvoker
    : public condy::InvokerAdapter<SetUnfinishedInvoker> {
    SetUnfinishedInvoker(size_t &unfinished_ref) : unfinished(unfinished_ref) {}
    void invoke() { unfinished--; }
    size_t &unfinished;
};

void event_loop(size_t &unfinished) {
    auto *ring = condy::Context::current().ring();
    while (unfinished > 0) {
        ring->submit();
        ring->reap_completions([&](io_uring_cqe *cqe) {
            auto [data, type] = condy::decode_work(io_uring_cqe_get_data(cqe));
            if (type == condy::WorkType::Ignore) {
                return;
            }
            auto handle_ptr = static_cast<condy::OpFinishHandle *>(data);
            handle_ptr->set_result(cqe->res, cqe->flags);
            (*handle_ptr)();
        });
    }
}

// Just placeholder
condy::Runtime runtime;

} // namespace

TEST_CASE("test op_finish_handle - basic usage") {
    condy::Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);
    auto &context = condy::Context::current();

    context.init(&ring, &runtime);

    SetFinishInvoker invoker;
    condy::OpFinishHandle handle;
    handle.set_invoker(&invoker);

    auto *sqe = ring.get_sqe();
    io_uring_prep_nop(sqe);
    io_uring_sqe_set_data(sqe, &handle);
    ring.submit();

    ring.reap_completions([](io_uring_cqe *cqe) {
        auto handle_ptr =
            static_cast<condy::OpFinishHandle *>(io_uring_cqe_get_data(cqe));
        handle_ptr->set_result(42, 0);
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
    context.init(&ring, &runtime);

    condy::OpFinishHandle handle1, handle2;
    handle1.set_invoker(&invoker);
    handle2.set_invoker(&invoker);

    auto *sqe1 = ring.get_sqe();
    io_uring_prep_nop(sqe1);
    io_uring_sqe_set_data(sqe1, &handle1);

    auto *sqe2 = ring.get_sqe();
    io_uring_prep_nop(sqe2);
    io_uring_sqe_set_data(sqe2, &handle2);

    event_loop(invoker.unfinished);

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
    context.init(&ring, &runtime);

    condy::OpFinishHandle handle1, handle2;
    condy::ParallelFinishHandle<condy::WaitOne, condy::OpFinishHandle,
                                condy::OpFinishHandle>
        finish_handle;
    finish_handle.init(&handle1, &handle2);
    finish_handle.set_invoker(&invoker);

    auto *sqe = ring.get_sqe();
    __kernel_timespec ts{
        .tv_sec = 60 * 60,
        .tv_nsec = 0,
    };
    io_uring_prep_timeout(sqe, &ts, 0, 0);
    io_uring_sqe_set_data(sqe, &handle1);

    sqe = ring.get_sqe();
    io_uring_prep_nop(sqe);
    io_uring_sqe_set_data(sqe, &handle2);

    event_loop(unfinished);

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
    void invoke() { finished = true; }
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

    condy::MultiShotMixin<decltype(func), condy::ExtendOpFinishHandle> handle(
        func);
    REQUIRE(!invoker.finished);
    handle.set_result(1, 0);
    handle.invoke_extend(); // Multishot
    REQUIRE(invoker.finished);
    REQUIRE(invoker.result == 1);
}

TEST_CASE("test op_finish_handle - zero copy op") {
    SetFinishWorkInvoker invoker;

    int res = -1;
    auto func = [&](int r) { res = r; };

    auto *handle =
        new condy::ZeroCopyMixin<decltype(func), condy::ExtendOpFinishHandle>(
            func);
    handle->set_invoker(&invoker);
    REQUIRE(!invoker.finished);
    handle->set_result(1, 0);
    (*handle)();
    REQUIRE(invoker.finished);
    REQUIRE(handle->extract_result() == 1);
    REQUIRE(res == -1);
    handle->set_result(2, 0);
    handle->invoke_extend(); // Notify
    REQUIRE(res == 2);
}