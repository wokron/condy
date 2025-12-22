#include <condy/utils.hpp>
#include <doctest/doctest.h>
#include <limits>
#include <memory>
#include <string>

TEST_CASE("test uninitialized - int") {
    condy::Uninitialized<int> uninit;
    uninit.emplace(42);
    REQUIRE(uninit.get() == 42);
}

TEST_CASE("test uninitialized - std::string") {
    condy::Uninitialized<std::string> uninit;
    uninit.emplace("Hello, World!");
    REQUIRE(uninit.get() == "Hello, World!");
}

namespace {

struct int_deleter {
    void operator()(int *p) const {
        called = true;
        delete p;
    }
    inline static bool called = false;
};

} // namespace

TEST_CASE("test uninitialized - std::unique_ptr") {
    int_deleter::called = false;
    auto ptr = std::unique_ptr<int, int_deleter>(new int(99));

    {
        condy::Uninitialized<std::unique_ptr<int, int_deleter>> uninit;
        uninit.emplace(std::move(ptr));
        REQUIRE(!int_deleter::called);
        REQUIRE(*(uninit.get()) == 99);
    }

    REQUIRE(int_deleter::called);
}

TEST_CASE("test uninitialized - reset") {
    int_deleter::called = false;
    {
        condy::Uninitialized<std::unique_ptr<int, int_deleter>> uninit;
        uninit.emplace(std::unique_ptr<int, int_deleter>(new int(123)));
        REQUIRE(!int_deleter::called);
        REQUIRE(*(uninit.get()) == 123);
        uninit.reset();
        REQUIRE(int_deleter::called);

        int_deleter::called = false;
        uninit.emplace(std::unique_ptr<int, int_deleter>(new int(456)));
        REQUIRE(!int_deleter::called);
        REQUIRE(*(uninit.get()) == 456);
    }
    REQUIRE(int_deleter::called);
}

TEST_CASE("test is_power_of_two") {
    uint16_t next_power_of_two = 1;
    uint16_t max = std::numeric_limits<uint16_t>::max();
    for (uint16_t i = 1; i < max; ++i) {
        if (i == next_power_of_two) {
            REQUIRE_MESSAGE(condy::is_power_of_two(i), "i=", i);
            next_power_of_two <<= 1;
        } else {
            REQUIRE_MESSAGE(!condy::is_power_of_two(i), "i=", i);
        }
    }
}