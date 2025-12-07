#include "condy/work_type.hpp"
#include <cstdint>
#include <doctest/doctest.h>

namespace {

struct A {
    void *data;
};

void test_type(condy::WorkType type) {
    A a;
    void *addr = &a;
    REQUIRE(reinterpret_cast<intptr_t>(addr) % 8 == 0);
    void *work_addr = condy::encode_work(addr, type);
    // REQUIRE(addr != work_addr);

    auto [origin_addr, decoded_type] = condy::decode_work(work_addr);
    REQUIRE(origin_addr == addr);
    REQUIRE(decoded_type == type);
}

} // namespace

TEST_CASE("test work_type - encode and decode") {
    test_type(condy::WorkType::Common);
    test_type(condy::WorkType::Ignore);
    test_type(condy::WorkType::Notify);
    test_type(condy::WorkType::Schedule);
    test_type(condy::WorkType::MultiShot);
    test_type(condy::WorkType::ZeroCopy);
}