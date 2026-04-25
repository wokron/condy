#include "condy/context.hpp"
#include "condy/cqe_handler.hpp"
#include "condy/finish_handles.hpp"
#include "condy/runtime.hpp"
#include <cstddef>
#include <cstring>
#include <doctest/doctest.h>

namespace {

struct MockReceiver {
    void operator()(int res) {
        r = res;
        invoke_count++;
    }
    std::stop_token get_stop_token() { return {}; }
    size_t &invoke_count;
    int &r;
};

void event_loop(size_t &count, size_t expected) {
    auto *ring = condy::detail::Context::current().ring();
    while (count != expected) {
        ring->submit();
        ring->reap_completions([&](io_uring_cqe *cqe) {
            auto [data, type] =
                condy::decode_work(io_uring_cqe_get_data64(cqe));
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

    size_t invoke_count = 0;
    int r = 0;
    MockReceiver receiver{invoke_count, r};
    condy::OpFinishHandle<condy::SimpleCQEHandler, MockReceiver> handle(
        condy::SimpleCQEHandler(), receiver);

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

    REQUIRE(invoke_count == 1);
    REQUIRE(r == 42);

    context.reset();
}

TEST_CASE("test op_finish_handle - concurrent ops") {
    condy::Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);
    auto &context = condy::detail::Context::current();
    context.init(&ring, &runtime);

    size_t invoke_count = 0;
    int r = 0;
    MockReceiver receiver{invoke_count, r};
    condy::OpFinishHandle<condy::SimpleCQEHandler, MockReceiver> handle1(
        condy::SimpleCQEHandler(), receiver),
        handle2(condy::SimpleCQEHandler(), receiver);

    auto *sqe1 = ring.get_sqe();
    io_uring_prep_nop(sqe1);
    io_uring_sqe_set_data(sqe1, &handle1);

    auto *sqe2 = ring.get_sqe();
    io_uring_prep_nop(sqe2);
    io_uring_sqe_set_data(sqe2, &handle2);

    event_loop(invoke_count, 2);

    REQUIRE(invoke_count == 2);

    context.reset();
}

TEST_CASE("test op_finish_handle - multishot op") {
    size_t invoke_count = 0;
    int r = 0;
    MockReceiver receiver{invoke_count, r};

    int res = -1;
    auto func = [&](int r) { res = r; };

    condy::MultiShotOpFinishHandle<condy::SimpleCQEHandler, decltype(func),
                                   MockReceiver>
        handle(condy::SimpleCQEHandler(), receiver, func);
    REQUIRE(invoke_count == 0);
    io_uring_cqe cqe{};
    cqe.res = 42;
    cqe.flags |= IORING_CQE_F_MORE;       // Indicate more results to come
    auto op_finish = handle.handle(&cqe); // Multishot
    REQUIRE(!op_finish);
    REQUIRE(res == 42);
    REQUIRE(invoke_count == 0);

    io_uring_cqe cqe2{};
    cqe2.res = 43;
    res = -1;
    op_finish = handle.handle(&cqe2); // Finish
    REQUIRE(op_finish);
    REQUIRE(res == -1);
    REQUIRE(invoke_count == 1);
}

TEST_CASE("test op_finish_handle - zero copy op") {
    size_t invoke_count = 0;
    int r = 0;
    MockReceiver receiver{invoke_count, r};

    int res = -1;
    auto func = [&](int r) { res = r; };

    auto *handle =
        new condy::ZeroCopyOpFinishHandle<condy::SimpleCQEHandler,
                                          decltype(func), MockReceiver>(
            condy::SimpleCQEHandler(), receiver, func);

    REQUIRE(invoke_count == 0);
    io_uring_cqe cqe{};
    cqe.res = 1;
    cqe.flags |= IORING_CQE_F_MORE; // Indicate more results to come
    auto op_finish1 = handle->handle(&cqe);
    REQUIRE(!op_finish1);
    REQUIRE(invoke_count == 1);
    REQUIRE(res == -1);
    io_uring_cqe cqe2{};
    cqe2.res = 2;
    cqe2.flags |= IORING_CQE_F_NOTIF;
    auto op_finish2 = handle->handle(&cqe2);
    REQUIRE(op_finish2);
    REQUIRE(invoke_count == 1);
    REQUIRE(res == 2);
}