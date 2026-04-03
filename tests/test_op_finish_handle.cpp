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
            handle_ptr->handle(cqe);
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
    condy::OpFinishHandle<condy::SimpleCQEHandler> handle;
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
        handle_ptr->handle(&mock_cqe);
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

    condy::OpFinishHandle<condy::SimpleCQEHandler> handle1, handle2;
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

    condy::MultiShotOpFinishHandle<condy::SimpleCQEHandler, decltype(func)>
        handle(func);
    REQUIRE(!invoker.finished);
    io_uring_cqe cqe{};
    cqe.res = 1;
    cqe.flags |= IORING_CQE_F_MORE;       // Indicate more results to come
    auto op_finish = handle.handle(&cqe); // Multishot
    REQUIRE(invoker.finished);
    REQUIRE(invoker.result == 1);
    REQUIRE(!op_finish);
}

TEST_CASE("test op_finish_handle - zero copy op") {
    SetFinishWorkInvoker invoker;

    int res = -1;
    auto func = [&](int r) { res = r; };

    auto *handle = new condy::ZeroCopyOpFinishHandle<condy::SimpleCQEHandler,
                                                     decltype(func)>(func);
    handle->set_invoker(&invoker);
    REQUIRE(!invoker.finished);
    io_uring_cqe cqe{};
    cqe.res = 1;
    cqe.flags |= IORING_CQE_F_MORE; // Indicate more results to come
    auto op_finish1 = handle->handle(&cqe);
    REQUIRE(!op_finish1);
    REQUIRE(invoker.finished);
    REQUIRE(handle->extract_result() == 1);
    REQUIRE(res == -1);
    io_uring_cqe cqe2{};
    cqe2.res = 2;
    cqe2.flags |= IORING_CQE_F_NOTIF;
    auto op_finish2 = handle->handle(&cqe2);
    REQUIRE(op_finish2);
    REQUIRE(res == 2);
}