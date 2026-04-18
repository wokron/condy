#include "condy/async_operations.hpp"
#include "condy/buffers.hpp"
#include "condy/coro.hpp"
#include "condy/helpers.hpp"
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

TEST_CASE("test buffers - buffer string_view") {
    std::string_view strv = "hello world";
    condy::ConstBuffer cbuf = condy::buffer(strv);
    REQUIRE(cbuf.data() == reinterpret_cast<const void *>(strv.data()));
    REQUIRE(cbuf.size() == strv.size());
}

TEST_CASE("test buffers - buffer iovec") {
    char data[32] = {};
    struct iovec iov;
    iov.iov_base = data;
    iov.iov_len = sizeof(data);

    condy::MutableBuffer mbuf = condy::buffer(iov);
    REQUIRE(mbuf.data() == data);
    REQUIRE(mbuf.size() == sizeof(data));
}

TEST_CASE("test buffers - buffer span") {
    int arr[4] = {1, 2, 3, 4};
    std::span<int> sp(arr);
    condy::MutableBuffer mbuf = condy::buffer(sp.subspan(1));
    REQUIRE(mbuf.data() == reinterpret_cast<void *>(arr + 1));
    REQUIRE(mbuf.size() == sizeof(int) * 3);

    std::span<const int> csp(arr);
    condy::ConstBuffer cbuf = condy::buffer(csp.subspan(1));
    REQUIRE(cbuf.data() == reinterpret_cast<const void *>(arr + 1));
    REQUIRE(cbuf.size() == sizeof(int) * 3);

    std::span<int, 4> sp2(arr);
    condy::MutableBuffer mbuf2 = condy::buffer(sp2);
    REQUIRE(mbuf2.data() == reinterpret_cast<void *>(arr));
    REQUIRE(mbuf2.size() == sizeof(int) * 4);

    std::span<const int, 4> csp2(arr);
    condy::ConstBuffer cbuf2 = condy::buffer(csp2);
    REQUIRE(cbuf2.data() == reinterpret_cast<const void *>(arr));
    REQUIRE(cbuf2.size() == sizeof(int) * 4);
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

    condy::detail::Context::current().init(&ring, &runtime);
    auto d = condy::defer([]() { condy::detail::Context::current().reset(); });

    condy::ProvidedBufferQueue queue(4);
    REQUIRE(queue.capacity() == (1 << 2));

    char buf1[32], buf2[32];
    REQUIRE(queue.push(condy::buffer(buf1)) == 0);
    REQUIRE(queue.size() == 1);
    REQUIRE(queue.push(condy::buffer(buf2)) == 1);
    REQUIRE(queue.size() == 2);

    int pipefd[2];
    REQUIRE(pipe(pipefd) == 0);

    ssize_t r;

    r = ::write(pipefd[1], "test", 4);
    REQUIRE(r == 4);

    auto *sqe = ring.get_sqe();
    io_uring_prep_read(sqe, pipefd[0], nullptr, 0, 0);
    io_uring_sqe_set_flags(sqe, IOSQE_BUFFER_SELECT);
    sqe->buf_group = queue.bgid();
    io_uring_sqe_set_data(sqe, nullptr);

    r = -1;

    condy::BufferInfo ret;

    size_t reaped = 0;
    while (reaped < 1) {
        ring.submit();
        reaped += ring.reap_completions([&](io_uring_cqe *cqe) {
            auto *data = io_uring_cqe_get_data(cqe);
            REQUIRE(data == nullptr);
            r = cqe->res;
            ret = queue.handle_finish(cqe);
        });
    }

    REQUIRE(r > 0);
    REQUIRE(ret.num_buffers == 1);
    REQUIRE(ret.bid == 0);
    REQUIRE(queue.size() == 1);

    REQUIRE(std::memcmp(buf1, "test", 4) == 0);
}

#if !IO_URING_CHECK_VERSION(2, 8) // >= 2.8
TEST_CASE("test buffers - provided buffer queue usage incr") {
    condy::Runtime runtime;
    condy::Ring ring;
    io_uring_params params = {};
    ring.init(8, &params);

    condy::detail::Context::current().init(&ring, &runtime);
    auto d = condy::defer([]() { condy::detail::Context::current().reset(); });

    condy::ProvidedBufferQueue queue(4, IOU_PBUF_RING_INC);
    char buf[4][16];
    for (int i = 0; i < 4; ++i) {
        REQUIRE(queue.push(condy::buffer(buf[i])) == i);
        REQUIRE(queue.size() == static_cast<size_t>(i + 1));
    }

    io_uring_cqe cqe;

    // 1. n = 9
    cqe = {};
    cqe.res = 9;
    cqe.flags |= IORING_CQE_F_BUFFER;
    cqe.flags |= 0 << IORING_CQE_BUFFER_SHIFT; // bid = 0
    cqe.flags |= IORING_CQE_F_BUF_MORE;
    auto ret = queue.handle_finish(&cqe);
    REQUIRE(ret.bid == 0);
    REQUIRE(ret.num_buffers == 0);
    REQUIRE(queue.size() == 4);

    // 2. n = 16 (7, first half)
    cqe = {};
    cqe.res = 7;
    cqe.flags |= IORING_CQE_F_BUFFER;
    cqe.flags |= 0 << IORING_CQE_BUFFER_SHIFT; // bid = 0
    ret = queue.handle_finish(&cqe);
    REQUIRE(ret.bid == 0);
    REQUIRE(ret.num_buffers == 1);
    REQUIRE(queue.size() == 3);

    // 3. n = 16 (9, second half)
    cqe = {};
    cqe.res = 9;
    cqe.flags |= IORING_CQE_F_BUFFER;
    cqe.flags |= 1 << IORING_CQE_BUFFER_SHIFT; // bid = 1
    cqe.flags |= IORING_CQE_F_BUF_MORE;
    ret = queue.handle_finish(&cqe);
    REQUIRE(ret.bid == 1);
    REQUIRE(ret.num_buffers == 0);
    REQUIRE(queue.size() == 3);

    // 4. n = 1
    cqe = {};
    cqe.res = 1;
    cqe.flags |= IORING_CQE_F_BUFFER;
    cqe.flags |= 1 << IORING_CQE_BUFFER_SHIFT; // bid = 1
    cqe.flags |= IORING_CQE_F_BUF_MORE;
    ret = queue.handle_finish(&cqe);
    REQUIRE(ret.bid == 1);
    REQUIRE(ret.num_buffers == 0);
    REQUIRE(queue.size() == 3);

    REQUIRE(queue.push(condy::buffer(buf[1])) == 0);
}
#endif

TEST_CASE("test buffers - provided buffer queue usage bundle") {
    condy::Runtime runtime;
    condy::Ring ring;
    io_uring_params params = {};
    ring.init(8, &params);

    condy::detail::Context::current().init(&ring, &runtime);
    auto d = condy::defer([]() { condy::detail::Context::current().reset(); });

    condy::detail::BundledProvidedBufferQueue queue(4, 0);
    char buf[4][16];
    for (int i = 0; i < 4; ++i) {
        REQUIRE(queue.push(condy::buffer(buf[i])) == i);
        REQUIRE(queue.size() == static_cast<size_t>(i + 1));
    }

    io_uring_cqe cqe;

    // n = 40 (32, bundle)
    cqe = {};
    cqe.res = 32;
    cqe.flags |= IORING_CQE_F_BUFFER;
    cqe.flags |= 0 << IORING_CQE_BUFFER_SHIFT; // bid = 0
    auto ret = queue.handle_finish(&cqe);
    REQUIRE(ret.bid == 0);
    REQUIRE(ret.num_buffers == 2);
    REQUIRE(queue.size() == 2);

    REQUIRE(queue.push(condy::buffer(buf[0])) == 0);
    REQUIRE(queue.push(condy::buffer(buf[1])) == 1);

    // n = 8 + 17
    cqe = {};
    cqe.res = 25;
    cqe.flags |= IORING_CQE_F_BUFFER;
    cqe.flags |= 2 << IORING_CQE_BUFFER_SHIFT; // bid = 2
    ret = queue.handle_finish(&cqe);
    REQUIRE(ret.bid == 2);
    REQUIRE(ret.num_buffers == 2);
    REQUIRE(queue.size() == 2);
}

#if !IO_URING_CHECK_VERSION(2, 8) // >= 2.8
TEST_CASE("test buffers - provided buffer queue usage bundle incr") {
    condy::Runtime runtime;
    condy::Ring ring;
    io_uring_params params = {};
    ring.init(8, &params);

    condy::detail::Context::current().init(&ring, &runtime);
    auto d = condy::defer([]() { condy::detail::Context::current().reset(); });

    condy::detail::BundledProvidedBufferQueue queue(4, IOU_PBUF_RING_INC);
    char buf[4][16];
    for (int i = 0; i < 4; ++i) {
        REQUIRE(queue.push(condy::buffer(buf[i])) == i);
        REQUIRE(queue.size() == static_cast<size_t>(i + 1));
    }

    io_uring_cqe cqe;

    // n = 9 (incr)
    cqe = {};
    cqe.res = 9;
    cqe.flags |= IORING_CQE_F_BUFFER;
    cqe.flags |= 0 << IORING_CQE_BUFFER_SHIFT; // bid = 0
    cqe.flags |= IORING_CQE_F_BUF_MORE;
    auto ret = queue.handle_finish(&cqe);
    REQUIRE(ret.bid == 0);
    REQUIRE(ret.num_buffers == 0);
    REQUIRE(queue.size() == 4);

    // n = 21 (bundle)
    cqe = {};
    cqe.res = 21;
    cqe.flags |= IORING_CQE_F_BUFFER;
    cqe.flags |= 0 << IORING_CQE_BUFFER_SHIFT; // bid = 0
    auto ret2 = queue.handle_finish(&cqe);
    REQUIRE(ret2.bid == 0);
    REQUIRE(ret2.num_buffers == 2);
    REQUIRE(queue.size() == 2);
}
#endif

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
    auto d = condy::defer([]() { condy::detail::Context::current().reset(); });

    condy::detail::Context::current().init(&ring, &runtime);

    condy::ProvidedBufferPool pool(4, 16);
    REQUIRE(pool.capacity() == (1 << 2));

    int pipefd[2];
    REQUIRE(pipe(pipefd) == 0);

    ssize_t r;

    r = ::write(pipefd[1], "test", 4);
    REQUIRE(r == 4);

    auto *sqe = ring.get_sqe();
    io_uring_prep_read(sqe, pipefd[0], nullptr, 0, 0);
    io_uring_sqe_set_flags(sqe, IOSQE_BUFFER_SELECT);
    sqe->buf_group = pool.bgid();
    io_uring_sqe_set_data(sqe, nullptr);

    r = -1;

    condy::ProvidedBuffer ret;

    size_t reaped = 0;
    while (reaped < 1) {
        ring.submit();
        reaped += ring.reap_completions([&](io_uring_cqe *cqe) {
            auto *data = io_uring_cqe_get_data(cqe);
            REQUIRE(data == nullptr);
            r = cqe->res;
            ret = pool.handle_finish(cqe);
        });
    }

    REQUIRE(r == 4);
    REQUIRE(ret.owns_buffer());
    REQUIRE(ret.size() == 16);

    REQUIRE(std::memcmp(ret.data(), "test", 4) == 0);
}

#if !IO_URING_CHECK_VERSION(2, 8) // >= 2.8
TEST_CASE("test buffers - provided buffer pool usage incr") {
    condy::Runtime runtime;
    condy::Ring ring;
    io_uring_params params = {};
    ring.init(8, &params);

    condy::detail::Context::current().init(&ring, &runtime);
    auto d = condy::defer([]() { condy::detail::Context::current().reset(); });

    condy::ProvidedBufferPool pool(4, 16, IOU_PBUF_RING_INC);
    REQUIRE(pool.capacity() == (1 << 2));
    REQUIRE(pool.buffer_size() == 16);

    io_uring_cqe cqe;

    // 1. n = 9
    cqe = {};
    cqe.res = 9;
    cqe.flags |= IORING_CQE_F_BUFFER;
    cqe.flags |= 0 << IORING_CQE_BUFFER_SHIFT; // bid = 0
    cqe.flags |= IORING_CQE_F_BUF_MORE;
    auto ret = pool.handle_finish(&cqe);
    REQUIRE(ret.owns_buffer() == false);
    REQUIRE(ret.size() == 9);

    // 2. n = 16 (7, first half)
    cqe = {};
    cqe.res = 7;
    cqe.flags |= IORING_CQE_F_BUFFER;
    cqe.flags |= 0 << IORING_CQE_BUFFER_SHIFT; // bid = 0
    ret = pool.handle_finish(&cqe);
    REQUIRE(ret.owns_buffer() == true);
    REQUIRE(ret.size() == 7);

    // 3. n = 16 (9, second half)
    cqe = {};
    cqe.res = 9;
    cqe.flags |= IORING_CQE_F_BUFFER;
    cqe.flags |= 1 << IORING_CQE_BUFFER_SHIFT; // bid = 1
    cqe.flags |= IORING_CQE_F_BUF_MORE;
    ret = pool.handle_finish(&cqe);
    REQUIRE(ret.owns_buffer() == false);
    REQUIRE(ret.size() == 9);

    // 4. n = 1
    cqe = {};
    cqe.res = 1;
    cqe.flags |= IORING_CQE_F_BUFFER;
    cqe.flags |= 1 << IORING_CQE_BUFFER_SHIFT; // bid = 1
    cqe.flags |= IORING_CQE_F_BUF_MORE;
    ret = pool.handle_finish(&cqe);
    REQUIRE(ret.owns_buffer() == false);
    REQUIRE(ret.size() == 1);
}
#endif

TEST_CASE("test buffers - provided buffer pool usage bundle") {
    condy::Runtime runtime;
    condy::Ring ring;
    io_uring_params params = {};
    ring.init(8, &params);

    condy::detail::Context::current().init(&ring, &runtime);
    auto d = condy::defer([]() { condy::detail::Context::current().reset(); });

    condy::detail::BundledProvidedBufferPool pool(4, 16, 0);
    REQUIRE(pool.capacity() == (1 << 2));
    REQUIRE(pool.buffer_size() == 16);

    io_uring_cqe cqe;

    // n = 40 (32, bundle)
    cqe = {};
    cqe.res = 32;
    cqe.flags |= IORING_CQE_F_BUFFER;
    cqe.flags |= 0 << IORING_CQE_BUFFER_SHIFT; // bid = 0
    auto ret = pool.handle_finish(&cqe);
    REQUIRE(ret.size() == 2);
    for (size_t i = 0; i < ret.size(); ++i) {
        REQUIRE(ret[i].owns_buffer() == true);
        REQUIRE(ret[i].size() == 16);
    }

    // n = 8 + 17
    cqe = {};
    cqe.res = 25;
    cqe.flags |= IORING_CQE_F_BUFFER;
    cqe.flags |= 2 << IORING_CQE_BUFFER_SHIFT; // bid = 2
    auto ret2 = pool.handle_finish(&cqe);
    REQUIRE(ret2.size() == 2);
    for (size_t i = 0; i < ret2.size(); ++i) {
        REQUIRE(ret2[i].owns_buffer() == true);
        REQUIRE(ret2[i].size() == 16);
    }
}

#if !IO_URING_CHECK_VERSION(2, 8) // >= 2.8
TEST_CASE("test buffers - provided buffer pool usage bundle incr") {
    condy::Runtime runtime;
    condy::Ring ring;
    io_uring_params params = {};
    ring.init(8, &params);

    condy::detail::Context::current().init(&ring, &runtime);
    auto d = condy::defer([]() { condy::detail::Context::current().reset(); });

    condy::detail::BundledProvidedBufferPool pool(4, 16, IOU_PBUF_RING_INC);
    REQUIRE(pool.capacity() == (1 << 2));
    REQUIRE(pool.buffer_size() == 16);

    io_uring_cqe cqe;

    // n = 9 (incr)
    cqe = {};
    cqe.res = 9;
    cqe.flags |= IORING_CQE_F_BUFFER;
    cqe.flags |= 0 << IORING_CQE_BUFFER_SHIFT; // bid = 0
    cqe.flags |= IORING_CQE_F_BUF_MORE;
    auto ret = pool.handle_finish(&cqe);
    REQUIRE(ret.size() == 1);
    REQUIRE(ret[0].owns_buffer() == false);
    REQUIRE(ret[0].size() == 9);

    // n = 21
    cqe = {};
    cqe.res = 21;
    cqe.flags |= IORING_CQE_F_BUFFER;
    cqe.flags |= 0 << IORING_CQE_BUFFER_SHIFT; // bid = 0
    auto ret2 = pool.handle_finish(&cqe);
    REQUIRE(ret2.size() == 2);
    REQUIRE(ret2[0].owns_buffer() == true);
    REQUIRE(ret2[0].size() == 7);
    REQUIRE(static_cast<char *>(ret[0].data()) + 9 == ret2[0].data());
    REQUIRE(ret2[1].owns_buffer() == true);
    REQUIRE(ret2[1].size() == 16);
}
#endif

TEST_CASE("test buffers - provided buffer is also buffer") {
    condy::ProvidedBuffer buf;
    auto fixed_buf = condy::fixed(2, buf);

    // ok
    [[maybe_unused]] auto aw1 = condy::async_read(0, fixed_buf, 0);

    // also ok
    [[maybe_unused]]
    auto aw2 = condy::async_read(0, buf, 0);
}