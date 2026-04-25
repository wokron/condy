#include "condy/cqe_handler.hpp"
#include "condy/finish_handles.hpp"
#include "condy/ring.hpp"
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <doctest/doctest.h>
#include <memory>
#include <vector>

using namespace condy;

namespace {

struct MockReceiver {
    void operator()(int r) {
        REQUIRE((r == 0 || r == -ECANCELED));
        invoke_count++;
    }
    std::stop_token get_stop_token() { return {}; }
    size_t &invoke_count;
};

using Handle = OpFinishHandle<SimpleCQEHandler, MockReceiver>;

} // namespace

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

    size_t invoke_count = 0;
    MockReceiver receiver{invoke_count};

    constexpr size_t num_ops = 4;
    std::vector<std::unique_ptr<Handle>> handles;
    for (size_t i = 0; i < num_ops; i++) {
        handles.push_back(
            std::make_unique<Handle>(condy::SimpleCQEHandler(), receiver));
        auto &handle = handles.back();
        auto *sqe = ring.get_sqe();
        io_uring_prep_nop(sqe);
        io_uring_sqe_set_data(sqe, handle.get());
    }

    int count = 0;

    size_t reaped = 0;
    while (reaped < num_ops) {
        ring.submit();
        reaped += ring.reap_completions([&](io_uring_cqe *cqe) {
            auto *handle = reinterpret_cast<OpFinishHandleBase *>(
                io_uring_cqe_get_data(cqe));
            REQUIRE(handle != nullptr);
            handle->handle(cqe);
            count++;
        });
    }

    REQUIRE(count == num_ops);
    REQUIRE(invoke_count == num_ops);

    ring.destroy();
}

TEST_CASE("test ring - cancel ops") {
    Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);

    size_t invoke_count = 0;
    MockReceiver receiver{invoke_count};

    __kernel_timespec ts{
        .tv_sec = 60ll * 60ll, // 1 hour
        .tv_nsec = 0,
    };
    constexpr size_t num_ops = 8;
    std::vector<std::unique_ptr<Handle>> handles;
    for (size_t i = 0; i < num_ops; i++) {
        handles.push_back(
            std::make_unique<Handle>(condy::SimpleCQEHandler(), receiver));
        auto &handle = handles.back();
        auto *sqe = ring.get_sqe();
        if (i % 2 == 0) {
            io_uring_prep_nop(sqe);
        } else {
            io_uring_prep_timeout(sqe, &ts, 0, 0);
        }
        io_uring_sqe_set_data(sqe, handle.get());
    }

    for (size_t i = 0; i < num_ops; i++) {
        if (i % 2 == 1) {
            io_uring_sqe *sqe = ring.get_sqe();
            io_uring_prep_cancel(sqe, handles[i].get(), 0);
            io_uring_sqe_set_data64(
                sqe, condy::encode_work(nullptr, condy::WorkType::Ignore));
        }
    }

    int canceled_count = 0;
    int total_count = 0;

    size_t reaped = 0;
    ring.submit();
    while (reaped < num_ops) {
        reaped += ring.reap_completions([&](io_uring_cqe *cqe) {
            if (io_uring_cqe_get_data64(cqe) ==
                condy::encode_work(nullptr, condy::WorkType::Ignore)) {
                return;
            }
            auto *handle = reinterpret_cast<OpFinishHandleBase *>(
                io_uring_cqe_get_data(cqe));
            REQUIRE(handle != nullptr);
            handle->handle(cqe);
            if (cqe->res == -ECANCELED) {
                canceled_count++;
            }
            total_count++;
        });
    }

    REQUIRE(total_count == num_ops);
    REQUIRE(invoke_count == num_ops);
    REQUIRE(canceled_count == num_ops / 2);

    ring.destroy();
}
