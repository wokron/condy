#include "condy/buffers.hpp"
#include "condy/coro.hpp"
#include "condy/provided_buffers.hpp"
#include "condy/ring.hpp"
#include "condy/runtime.hpp"
#include "condy/sync_wait.hpp"
#include <cerrno>
#include <cstddef>
#include <doctest/doctest.h>

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

TEST_CASE("test buffers - provided buffer queue init") {
    auto func = []() -> condy::Coro<void> {
        condy::ProvidedBufferQueue queue(16);
        char data1[16], data2[16];
        queue.push(condy::buffer(data1));
        queue.push(condy::buffer(data2));
        co_return;
    };

    condy::sync_wait(func());
}

TEST_CASE("test buffers - provided buffer queue usage") {
    condy::Runtime runtime;
    condy::Ring ring;
    io_uring_params params = {};
    ring.init(8, &params);

    condy::Context::current().init(&ring, &runtime);

    condy::ProvidedBufferQueue queue(4);
    REQUIRE(queue.capacity() == (1 << 2));

    char buf1[32], buf2[32];
    REQUIRE(queue.push(condy::buffer(buf1)) == 0);
    REQUIRE(queue.size() == 1);
    REQUIRE(queue.push(condy::buffer(buf2)) == 1);
    REQUIRE(queue.size() == 2);

    int pipefd[2];
    REQUIRE(pipe(pipefd) == 0);

    int r;

    r = ::write(pipefd[1], "test", 4);
    REQUIRE(r == 4);

    auto *sqe = ring.get_sqe();
    io_uring_prep_read(sqe, pipefd[0], nullptr, 0, 0);
    io_uring_sqe_set_flags(sqe, IOSQE_BUFFER_SELECT);
    sqe->buf_group = queue.bgid();
    io_uring_sqe_set_data(sqe, nullptr);

    r = -1;

    condy::ProvidedBufferQueue::ReturnType ret;

    size_t reaped = 0;
    while (reaped < 1) {
        ring.submit();
        reaped += ring.reap_completions([&](io_uring_cqe *cqe) {
            auto *data = io_uring_cqe_get_data(cqe);
            REQUIRE(data == nullptr);
            r = cqe->res;
            ret = queue.handle_finish(cqe->res, cqe->flags);
        });
    }

    REQUIRE(r > 0);
    REQUIRE(ret.num_buffers == 1);
    REQUIRE(ret.bid == 0);
    REQUIRE(queue.size() == 1);

    REQUIRE(std::memcmp(buf1, "test", 4) == 0);
}

TEST_CASE("test buffers - provided buffer pool init") {
    auto func = []() -> condy::Coro<void> {
        condy::ProvidedBufferPool pool(16, 16);
        co_return;
    };

    condy::sync_wait(func());
}

TEST_CASE("test buffers - provided buffer pool usage") {
    condy::Runtime runtime;
    condy::Ring ring;
    io_uring_params params = {};
    ring.init(8, &params);

    condy::Context::current().init(&ring, &runtime);

    condy::ProvidedBufferPool pool(4, 16);
    REQUIRE(pool.capacity() == (1 << 2));

    int pipefd[2];
    REQUIRE(pipe(pipefd) == 0);

    int r;

    r = ::write(pipefd[1], "test", 4);
    REQUIRE(r == 4);

    auto *sqe = ring.get_sqe();
    io_uring_prep_read(sqe, pipefd[0], nullptr, 0, 0);
    io_uring_sqe_set_flags(sqe, IOSQE_BUFFER_SELECT);
    sqe->buf_group = pool.bgid();
    io_uring_sqe_set_data(sqe, nullptr);

    r = -1;

    condy::ProvidedBufferPool::ReturnType ret;

    size_t reaped = 0;
    while (reaped < 1) {
        ring.submit();
        reaped += ring.reap_completions([&](io_uring_cqe *cqe) {
            auto *data = io_uring_cqe_get_data(cqe);
            REQUIRE(data == nullptr);
            r = cqe->res;
            ret = pool.handle_finish(cqe->res, cqe->flags);
        });
    }

    REQUIRE(r == 4);
    REQUIRE(ret.owns_buffer());
    REQUIRE(ret.size() == 16);

    REQUIRE(std::memcmp(ret.data(), "test", 4) == 0);
}