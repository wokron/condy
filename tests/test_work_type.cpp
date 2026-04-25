#include "condy/work_type.hpp"
#include <doctest/doctest.h>

namespace {

struct A {
    void *data;
};

void test_type(condy::WorkType type) {
    A a;
    void *addr = &a;
    auto work_addr = condy::encode_work(&a, type);

    auto [origin_addr, decoded_type] = condy::decode_work(work_addr);
    REQUIRE(origin_addr == addr);
    REQUIRE(decoded_type == type);
}

} // namespace

TEST_CASE("test work_type - encode and decode") {
    test_type(condy::WorkType::Common);
    test_type(condy::WorkType::Ignore);
    test_type(condy::WorkType::Schedule);
    test_type(condy::WorkType::Cancel);
}