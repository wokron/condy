#include <condy/context.hpp>
#include <condy/strategies.hpp>
#include <doctest/doctest.h>
#include <thread>

TEST_CASE("test context - init and destroy") {
    condy::SimpleStrategy strategy(8);
    condy::Context &ctx = condy::Context::current();
    ctx.init(&strategy);

    io_uring *ring = ctx.get_ring();
    REQUIRE(ring != nullptr);
    REQUIRE(ring->sq.ring_sz >= 512);
    REQUIRE(ring->cq.ring_sz >= 512);

    ctx.destroy();
}

TEST_CASE("test context - thread local") {
    auto p1 = &condy::Context::current();
    auto p2 = &condy::Context::current();
    REQUIRE(p1 == p2);

    condy::Context *p3 = nullptr;
    std::thread t([&p3]() { p3 = &condy::Context::current(); });
    t.join();
    REQUIRE(p3 != nullptr);
    REQUIRE(p1 != p3);
}