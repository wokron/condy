#include "condy/cqe_handler.hpp"
#include "condy/finish_handles.hpp"
#include "condy/ring.hpp"
#include <cerrno>
#include <cstddef>
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
    std::vector<OpFinishHandle<SimpleCQEHandler>> handles(num_ops);
    for (size_t i = 0; i < num_ops; i++) {
        auto *sqe = ring.get_sqe();
        io_uring_prep_nop(sqe);
        io_uring_sqe_set_data(sqe, &handles[i]);
    }

    int count = 0;

    size_t reaped = 0;
    while (reaped < num_ops) {
        ring.submit();
        reaped += ring.reap_completions([&](io_uring_cqe *cqe) {
            auto *handle = reinterpret_cast<OpFinishHandleBase *>(
                io_uring_cqe_get_data(cqe));
            REQUIRE(handle != nullptr);
            handle->handle_cqe(cqe);
            count++;
        });
    }

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
        .tv_sec = 60ll * 60ll, // 1 hour
        .tv_nsec = 0,
    };
    constexpr size_t num_ops = 8;
    std::vector<OpFinishHandle<SimpleCQEHandler>> handles(num_ops);
    for (size_t i = 0; i < num_ops; i++) {
        auto *sqe = ring.get_sqe();
        if (i % 2 == 0) {
            io_uring_prep_nop(sqe);
        } else {
            io_uring_prep_timeout(sqe, &ts, 0, 0);
        }
        io_uring_sqe_set_data(sqe, &handles[i]);
    }

    for (size_t i = 0; i < num_ops; i++) {
        if (i % 2 == 1) {
            io_uring_sqe *sqe = ring.get_sqe();
            io_uring_prep_cancel(sqe, &handles[i], 0);
            io_uring_sqe_set_data(
                sqe, condy::encode_work(nullptr, condy::WorkType::Ignore));
        }
    }

    int canceled_count = 0;
    int total_count = 0;

    size_t reaped = 0;
    ring.submit();
    while (reaped < num_ops) {
        reaped += ring.reap_completions([&](io_uring_cqe *cqe) {
            if (io_uring_cqe_get_data(cqe) ==
                condy::encode_work(nullptr, condy::WorkType::Ignore)) {
                return;
            }
            auto *handle = reinterpret_cast<OpFinishHandleBase *>(
                io_uring_cqe_get_data(cqe));
            REQUIRE(handle != nullptr);
            handle->handle_cqe(cqe);
            if (cqe->res == -ECANCELED) {
                canceled_count++;
            }
            total_count++;
        });
    }

    REQUIRE(total_count == num_ops);
    REQUIRE(canceled_count == num_ops / 2);

    ring.destroy();
}
