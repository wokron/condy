#include "condy/finish_handles.hpp"
#include "condy/strategies.hpp"
#include <cstddef>
#include <doctest/doctest.h>
#include <liburing.h>

TEST_CASE("test op_finish_handle - basic usage") {
    condy::SimpleStrategy strategy(8);
    auto &context = condy::Context::current();
    context.init(&strategy, nullptr, nullptr);
    auto ring = context.get_ring();

    bool finished = false;
    condy::OpFinishHandle handle;
    handle.set_on_finish(
        [](void *self, size_t no) {
            auto *finished_ptr = static_cast<bool *>(self);
            *finished_ptr = true;
        },
        &finished, 0);

    io_uring_sqe *sqe = io_uring_get_sqe(ring);
    REQUIRE(sqe != nullptr);
    io_uring_prep_nop(sqe);
    io_uring_sqe_set_data(sqe, &handle);

    io_uring_submit(ring);

    io_uring_cqe *cqe;
    io_uring_wait_cqe(ring, &cqe);

    auto handle_ptr =
        static_cast<condy::OpFinishHandle *>(io_uring_cqe_get_data(cqe));
    handle_ptr->invoke(cqe->res);

    io_uring_cqe_seen(ring, cqe);

    REQUIRE(finished);

    context.destroy();
}

namespace {

void event_loop(size_t &unfinished) {
    auto &context = condy::Context::current();
    auto ring = context.get_ring();
    while (unfinished > 0) {
        io_uring_submit_and_wait(ring, 1);

        io_uring_cqe *cqe;
        io_uring_peek_cqe(ring, &cqe);
        if (cqe == nullptr) {
            continue;
        }

        auto handle_ptr =
            static_cast<condy::OpFinishHandle *>(io_uring_cqe_get_data(cqe));
        if (handle_ptr) {
            handle_ptr->invoke(cqe->res);
        }

        io_uring_cqe_seen(ring, cqe);
    }
}

} // namespace

TEST_CASE("test op_finish_handle - concurrent ops") {
    size_t unfinished = 2;

    condy::SimpleStrategy strategy(8);
    auto &context = condy::Context::current();
    context.init(&strategy, nullptr, nullptr);
    auto ring = context.get_ring();

    condy::OpFinishHandle handle1, handle2;
    auto on_finish = [](void *self, size_t no) {
        auto *unfinished_ptr = static_cast<size_t *>(self);
        (*unfinished_ptr)--;
    };
    handle1.set_on_finish(on_finish, &unfinished, 0);
    handle2.set_on_finish(on_finish, &unfinished, 0);

    io_uring_sqe *sqe1 = io_uring_get_sqe(ring);
    REQUIRE(sqe1 != nullptr);
    io_uring_prep_nop(sqe1);
    io_uring_sqe_set_data(sqe1, &handle1);

    io_uring_sqe *sqe2 = io_uring_get_sqe(ring);
    REQUIRE(sqe2 != nullptr);
    io_uring_prep_nop(sqe2);
    io_uring_sqe_set_data(sqe2, &handle2);

    event_loop(unfinished);

    REQUIRE(unfinished == 0);

    context.destroy();
}

TEST_CASE("test op_finish_handle - cancel op") {
    size_t unfinished = 1;

    condy::SimpleStrategy strategy(8);
    auto &context = condy::Context::current();
    context.init(&strategy, nullptr, nullptr);
    auto ring = context.get_ring();

    condy::OpFinishHandle handle1, handle2;
    condy::ParallelFinishHandle<condy::WaitOneCancelCondition,
                                condy::OpFinishHandle, condy::OpFinishHandle>
        finish_handle;
    finish_handle.init(&handle1, &handle2);
    auto on_finish = [](void *self, size_t no) {
        auto *unfinished_ptr = static_cast<size_t *>(self);
        (*unfinished_ptr)--;
    };
    finish_handle.set_on_finish(on_finish, &unfinished, 0);

    io_uring_sqe *sqe1 = io_uring_get_sqe(ring);
    REQUIRE(sqe1 != nullptr);
    __kernel_timespec ts{
        .tv_sec = 60 * 60,
        .tv_nsec = 0,
    };
    io_uring_prep_timeout(sqe1, &ts, 0, 0);
    io_uring_sqe_set_data(sqe1, &handle1);

    io_uring_sqe *sqe2 = io_uring_get_sqe(ring);
    REQUIRE(sqe2 != nullptr);
    io_uring_prep_nop(sqe2);
    io_uring_sqe_set_data(sqe2, &handle2);

    event_loop(unfinished);

    REQUIRE(unfinished == 0);

    auto r = finish_handle.extract_result();
    auto &[order, results] = r;
    REQUIRE(order[0] == 1);
    REQUIRE(std::get<0>(results) == -ECANCELED);
    REQUIRE(std::get<1>(results) == 0);

    context.destroy();
}