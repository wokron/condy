#include <condy/utils.hpp>
#include <doctest/doctest.h>
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
    auto ptr = std::unique_ptr<int, int_deleter>(new int(99));
    REQUIRE(!int_deleter::called);

    {
        condy::Uninitialized<std::unique_ptr<int, int_deleter>> uninit;
        uninit.emplace(std::move(ptr));
        REQUIRE(!int_deleter::called);
        REQUIRE(*(uninit.get()) == 99);
    }

    REQUIRE(int_deleter::called);
}