#include "condy/buffers.hpp"
#include "condy/ring.hpp"
#include <doctest/doctest.h>
#include <liburing.h>
#include <liburing/io_uring.h>

TEST_CASE("test buffers - ProvidedBuffersImpl construct") {
    condy::Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);
    condy::detail::ProvidedBufferPoolImpl impl(ring.ring(), 0, 2, 32, 0);
}

TEST_CASE("test buffers - ProvidedBuffersImpl buffer select") {
    condy::Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);
    condy::detail::ProvidedBufferPoolImpl impl(ring.ring(), 0, 2, 32, 0);

    int pipefd[2];
    REQUIRE(pipe(pipefd) == 0);

    ::write(pipefd[1], "test", 4);

    ring.register_op(
        [&](io_uring_sqe *sqe) {
            io_uring_prep_read(sqe, pipefd[0], nullptr, 0, 0);
            io_uring_sqe_set_flags(sqe, IOSQE_BUFFER_SELECT);
            sqe->buf_group = 0;
        },
        nullptr);

    int r = -1;
    int bid = -1;

    ring.wait_all_completions([&](io_uring_cqe *cqe) {
        auto* data = io_uring_cqe_get_data(cqe);
        REQUIRE(data == nullptr);
        r = cqe->res;
        REQUIRE(r > 0);
        REQUIRE((cqe->flags & IORING_CQE_F_BUFFER));
        bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
    });

    REQUIRE(r != -1);

    char* buf = reinterpret_cast<char*>(impl.get_buffer(bid));
    REQUIRE(std::memcmp(buf, "test", 4) == 0);
}