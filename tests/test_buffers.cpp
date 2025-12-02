#include "condy/buffers.hpp"
#include "condy/ring.hpp"
#include <cerrno>
#include <cstddef>
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

    int r;

    r = ::write(pipefd[1], "test", 4);
    REQUIRE(r == 4);

    auto *sqe = ring.get_sqe();
    io_uring_prep_read(sqe, pipefd[0], nullptr, 0, 0);
    io_uring_sqe_set_flags(sqe, IOSQE_BUFFER_SELECT);
    sqe->buf_group = 0;
    io_uring_sqe_set_data(sqe, nullptr);

    r = -1;
    int bid = -1;

    size_t reaped = 0;
    while (reaped < 1) {
        ring.submit();
        reaped += ring.reap_completions([&](io_uring_cqe *cqe) {
            auto *data = io_uring_cqe_get_data(cqe);
            REQUIRE(data == nullptr);
            r = cqe->res;
            REQUIRE(r > 0);
            REQUIRE((cqe->flags & IORING_CQE_F_BUFFER));
            bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
        });
    }

    REQUIRE(r != -1);

    char *buf = reinterpret_cast<char *>(impl.get_buffer(bid));
    REQUIRE(std::memcmp(buf, "test", 4) == 0);
}

TEST_CASE("test buffers - SubmittedBufferQueueImpl construct") {
    condy::Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);
    condy::detail::ProvidedBufferQueueImpl impl(ring.ring(), 0, 4, 0);
}

TEST_CASE("test buffers - SubmittedBufferQueueImpl buffer destruct") {
    condy::Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);

    auto impl = std::make_shared<condy::detail::ProvidedBufferPoolImpl>(
        ring.ring(), 0, 1, 4, 0);

    int pipefd[2];
    REQUIRE(pipe(pipefd) == 0);

    int r;

    r = ::write(pipefd[1], "test", 4);
    REQUIRE(r == 4);

    r = ::write(pipefd[1], "test", 4);
    REQUIRE(r == 4);

    auto prep_read = [&] {
        auto *sqe = ring.get_sqe();
        io_uring_prep_read(sqe, pipefd[0], nullptr, 0, 0);
        io_uring_sqe_set_flags(sqe, IOSQE_BUFFER_SELECT);
        sqe->buf_group = 0;
        io_uring_sqe_set_data(sqe, nullptr);
    };

    condy::detail::ProvidedBufferQueueImpl queue_impl(ring.ring(), 1, 1, 0);
    prep_read();
    prep_read();

    size_t reaped = 0;
    while (reaped < 2) {
        reaped += ring.reap_completions(
            [&](io_uring_cqe *cqe) {
                auto *data = io_uring_cqe_get_data(cqe);
                REQUIRE(data == nullptr);
                int r = cqe->res;
                REQUIRE(r > 0);
                REQUIRE((cqe->flags & IORING_CQE_F_BUFFER));
                size_t bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
                condy::ProvidedBuffer buffer(impl, impl->get_buffer(bid),
                                             impl->buffer_size());
                queue_impl.submit_buffer(std::move(buffer));
            },
            true);
    }

    r = ::write(pipefd[1], "test", 4);
    REQUIRE(r == 4);

    prep_read();
    reaped = 0;
    while (reaped < 1) {
        ring.submit();
        reaped += ring.reap_completions([&](io_uring_cqe *cqe) {
            auto *data = io_uring_cqe_get_data(cqe);
            REQUIRE(data == nullptr);
            int r = cqe->res;
            REQUIRE(r == -ENOBUFS);
        });
    }

    queue_impl.remove_buffer(0);

    prep_read();
    reaped = 0;
    while (reaped < 1) {
        ring.submit();
        reaped += ring.reap_completions([&](io_uring_cqe *cqe) {
            auto *data = io_uring_cqe_get_data(cqe);
            REQUIRE(data == nullptr);
            int r = cqe->res;
            REQUIRE(r > 0);
            REQUIRE((cqe->flags & IORING_CQE_F_BUFFER));
            size_t bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
            REQUIRE(bid == 0);
        });
    }
}

TEST_CASE("test buffers - buffer mutable/const") {
    char data[16] = {};
    condy::MutableBuffer mbuf = condy::buffer(data, sizeof(data));
    REQUIRE(mbuf.data() == data);
    REQUIRE(mbuf.size() == sizeof(data));

    condy::ConstBuffer cb1 = condy::buffer(data, sizeof(data));
    REQUIRE(cb1.data() == data);
    REQUIRE(cb1.size() == sizeof(data));

    condy::ConstBuffer cb2 = mbuf;
    REQUIRE(cb2.data() == data);
    REQUIRE(cb2.size() == sizeof(data));
}

TEST_CASE("test buffers - buffer pod array") {
    int arr[4] = {1, 2, 3, 4};
    condy::MutableBuffer mbuf = condy::buffer(arr);
    REQUIRE(mbuf.data() == reinterpret_cast<void *>(arr));
    REQUIRE(mbuf.size() == sizeof(arr));

    condy::ConstBuffer cbuf = condy::buffer(arr);
    REQUIRE(cbuf.data() == reinterpret_cast<const void *>(arr));
    REQUIRE(cbuf.size() == sizeof(arr));
}

TEST_CASE("test buffers - buffer string") {
    std::string str = "hello";
    condy::ConstBuffer cbuf = condy::buffer(str);
    REQUIRE(cbuf.data() == reinterpret_cast<const void *>(str.data()));
    REQUIRE(cbuf.size() == str.size());
}

TEST_CASE("test buffers - buffer vector") {
    std::vector<int> vec = {1, 2, 3, 4};
    condy::MutableBuffer mbuf = condy::buffer(vec);
    REQUIRE(mbuf.data() == reinterpret_cast<void *>(vec.data()));
    REQUIRE(mbuf.size() == sizeof(int) * vec.size());

    condy::ConstBuffer cbuf = condy::buffer(vec);
    REQUIRE(cbuf.data() == reinterpret_cast<const void *>(vec.data()));
    REQUIRE(cbuf.size() == sizeof(int) * vec.size());
}