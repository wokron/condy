#include "condy/context.hpp"
#include "condy/cqe_handler.hpp"
#include "condy/finish_handles.hpp"
#include "condy/invoker.hpp"
#include "condy/runtime.hpp"
#include <cstddef>
#include <cstring>
#include <doctest/doctest.h>

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
    auto *ring = condy::detail::Context::current().ring();
    while (unfinished > 0) {
        ring->submit();
        ring->reap_completions([&](io_uring_cqe *cqe) {
            auto [data, type] = condy::decode_work(io_uring_cqe_get_data(cqe));
            if (type == condy::WorkType::Ignore) {
                return;
            }
            auto handle_ptr = static_cast<condy::OpFinishHandleBase *>(data);
            handle_ptr->handle_cqe(cqe);
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
    auto &context = condy::detail::Context::current();

    context.init(&ring, &runtime);

    SetFinishInvoker invoker;
    condy::OpFinishHandle<condy::DefaultCQEHandler> handle;
    handle.set_invoker(&invoker);

    auto *sqe = ring.get_sqe();
    io_uring_prep_nop(sqe);
    io_uring_sqe_set_data(sqe, &handle);
    ring.submit();

    ring.reap_completions([](io_uring_cqe *cqe) {
        auto handle_ptr = static_cast<condy::OpFinishHandleBase *>(
            io_uring_cqe_get_data(cqe));
        io_uring_cqe mock_cqe = *cqe;
        mock_cqe.res = 42;
        handle_ptr->handle_cqe(&mock_cqe);
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
    auto &context = condy::detail::Context::current();
    context.init(&ring, &runtime);

    condy::OpFinishHandle<condy::DefaultCQEHandler> handle1, handle2;
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
    auto &context = condy::detail::Context::current();
    context.init(&ring, &runtime);

    condy::OpFinishHandle<condy::DefaultCQEHandler> handle1, handle2;
    condy::ParallelFinishHandle<true,
                                condy::OpFinishHandle<condy::DefaultCQEHandler>,
                                condy::OpFinishHandle<condy::DefaultCQEHandler>>
        finish_handle;
    finish_handle.init(&handle1, &handle2);
    finish_handle.set_invoker(&invoker);

    auto *sqe = ring.get_sqe();
    __kernel_timespec ts{
        .tv_sec = 60ll * 60ll,
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

    condy::MultiShotMixin<decltype(func),
                          condy::OpFinishHandle<condy::DefaultCQEHandler>>
        handle(func);
    REQUIRE(!invoker.finished);
    io_uring_cqe cqe{};
    cqe.res = 1;
    cqe.flags |= IORING_CQE_F_MORE;     // Indicate more results to come
    auto act = handle.handle_cqe(&cqe); // Multishot
    REQUIRE(invoker.finished);
    REQUIRE(invoker.result == 1);
    REQUIRE(!act.op_finish);
    REQUIRE(!act.queue_work);
}

TEST_CASE("test op_finish_handle - zero copy op") {
    SetFinishWorkInvoker invoker;

    int res = -1;
    auto func = [&](int r) { res = r; };

    auto *handle = new condy::ZeroCopyMixin<
        decltype(func), condy::OpFinishHandle<condy::DefaultCQEHandler>>(func);
    handle->set_invoker(&invoker);
    REQUIRE(!invoker.finished);
    io_uring_cqe cqe{};
    cqe.res = 1;
    cqe.flags |= IORING_CQE_F_MORE; // Indicate more results to come
    auto act1 = handle->handle_cqe(&cqe);
    REQUIRE(act1.queue_work);
    REQUIRE(!act1.op_finish);
    (*handle)();
    REQUIRE(invoker.finished);
    REQUIRE(handle->extract_result() == 1);
    REQUIRE(res == -1);
    io_uring_cqe cqe2{};
    cqe2.res = 2;
    cqe2.flags |= IORING_CQE_F_NOTIF;
    auto act2 = handle->handle_cqe(&cqe2);
    REQUIRE(act2.op_finish);
    REQUIRE(!act2.queue_work);
    REQUIRE(res == 2);
}