#include "condy/finish_handles.hpp"
#include "condy/ring.hpp"
#include <cerrno>
#include <cstring>
#include <doctest/doctest.h>
#include <sys/eventfd.h>

using namespace condy;

TEST_CASE("test ring - init and destroy") {
    Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);
    ring.destroy();
}

TEST_CASE("test ring - register and complete ops") {
    Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);

    constexpr size_t num_ops = 4;
    OpFinishHandle handles[num_ops];
    for (size_t i = 0; i < num_ops; i++) {
        handles[i].set_result(-1, 0);
        ring.register_op([](io_uring_sqe *sqe) { io_uring_prep_nop(sqe); },
                         &handles[i]);
    }

    int count = 0;

    ring.wait_all_completions([&](io_uring_cqe *cqe) {
        OpFinishHandle *handle =
            reinterpret_cast<OpFinishHandle *>(io_uring_cqe_get_data(cqe));
        REQUIRE(handle != nullptr);
        handle->set_result(cqe->res, 0);
        count++;
    });

    REQUIRE(count == num_ops);

    for (size_t i = 0; i < num_ops; i++) {
        int res = handles[i].extract_result();
        REQUIRE(res == 0); // NOP should complete with result 0
    }

    ring.destroy();
}

TEST_CASE("test ring - cancel ops") {
    Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);

    __kernel_timespec ts{
        .tv_sec = 60 * 60, // 1 hour
        .tv_nsec = 0,
    };
    constexpr size_t num_ops = 8;
    OpFinishHandle handles[num_ops];
    for (size_t i = 0; i < num_ops; i++) {
        handles[i].set_result(-1, 0);
        ring.register_op(
            [&](io_uring_sqe *sqe) {
                if (i % 2 == 0) {
                    io_uring_prep_nop(sqe);
                    return;
                }
                io_uring_prep_timeout(sqe, &ts, 0, 0);
            },
            &handles[i]);
    }

    for (size_t i = 0; i < num_ops; i++) {
        if (i % 2 == 1) {
            ring.cancel_op(&handles[i]);
        }
    }

    int canceled_count = 0;
    int total_count = 0;

    ring.wait_all_completions([&](io_uring_cqe *cqe) {
        OpFinishHandle *handle =
            reinterpret_cast<OpFinishHandle *>(io_uring_cqe_get_data(cqe));
        REQUIRE(handle != nullptr);
        handle->set_result(cqe->res, 0);
        if (cqe->res == -ECANCELED) {
            canceled_count++;
        }
        total_count++;
    });

    REQUIRE(total_count == num_ops);
    REQUIRE(canceled_count == num_ops / 2);

    ring.destroy();
}

TEST_CASE("test ring - reap with timeout") {
    Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);

    __kernel_timespec ts{
        .tv_sec = 0,
        .tv_nsec = 1000 * 1000, // 1 ms
    };
    OpFinishHandle handle;
    handle.set_result(-1, 0);
    ring.register_op(
        [&](io_uring_sqe *sqe) { io_uring_prep_timeout(sqe, &ts, 0, 0); },
        &handle);
    ring.submit();

    size_t reaped = ring.reap_completions(
        [&](io_uring_cqe *cqe) {
            OpFinishHandle *handle =
                reinterpret_cast<OpFinishHandle *>(io_uring_cqe_get_data(cqe));
            REQUIRE(handle != nullptr);
            handle->set_result(cqe->res, 0);
        },
        500); // 500 us

    REQUIRE(reaped == 0); // No completions within 500 us

    reaped = ring.reap_completions(
        [&](io_uring_cqe *cqe) {
            OpFinishHandle *handle =
                reinterpret_cast<OpFinishHandle *>(io_uring_cqe_get_data(cqe));
            REQUIRE(handle != nullptr);
            handle->set_result(cqe->res, 0);
        },
        10 * 1000); // 10 ms

    REQUIRE(reaped == 1); // Completion should be available now

    int res = handle.extract_result();
    REQUIRE(res == -ETIME); // Timeout op should complete with result -ETIME

    ring.destroy();
}