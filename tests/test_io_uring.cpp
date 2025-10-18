#include <condy/io_uring.hpp>
#include <doctest/doctest.h>
#include <liburing/io_uring.h>

TEST_CASE("test io_uring - construct and destruct") {
    io_uring_sqe *sqe = nullptr;
    auto ring = condy::make_io_uring(8, 0);
    REQUIRE(ring != nullptr);

    sqe = io_uring_get_sqe(ring.get());
    REQUIRE(sqe != nullptr);
    sqe = nullptr;

    auto prev_ptr = ring.release();
    ring = condy::make_io_uring(8, 0);
    REQUIRE(ring != nullptr);
    REQUIRE_NE(ring.get(), prev_ptr);
    sqe = io_uring_get_sqe(ring.get());
    REQUIRE(sqe != nullptr);
}