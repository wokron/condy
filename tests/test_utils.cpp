#include <condy/utils.hpp>
#include <doctest/doctest.h>
#include <limits>
#include <memory>
#include <string>

namespace {

struct int_deleter {
    void operator()(int *p) const {
        called = true;
        delete p;
    }
    inline static bool called = false;
};

} // namespace

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

TEST_CASE("test raw_storage - int") {
    condy::RawStorage<int> storage;
    new (&storage) int(77);
    REQUIRE(storage.get() == 77);
    storage.destroy();
}

TEST_CASE("test raw_storage - std::string") {
    condy::RawStorage<std::string> storage;
    storage.construct("Raw Storage Test");
    REQUIRE(storage.get() == "Raw Storage Test");
    storage.destroy();
}

TEST_CASE("test raw_storage - std::unique_ptr") {
    int_deleter::called = false;
    auto ptr = std::unique_ptr<int, int_deleter>(new int(99));
    condy::RawStorage<std::unique_ptr<int, int_deleter>> storage;
    storage.construct(std::move(ptr));
    REQUIRE(!int_deleter::called);
    REQUIRE(*(storage.get()) == 99);
    storage.destroy();
    REQUIRE(int_deleter::called);
}

TEST_CASE("test small_array - small") {
    condy::SmallArray<int, 4> arr(3);
    arr[0] = 10;
    arr[1] = 20;
    arr[2] = 30;
    REQUIRE(arr.capacity() == 3);
    REQUIRE(arr[0] == 10);
    REQUIRE(arr[1] == 20);
    REQUIRE(arr[2] == 30);
}

TEST_CASE("test small_array - large") {
    condy::SmallArray<int, 4> arr(10);
    for (size_t i = 0; i < arr.capacity(); ++i) {
        arr[i] = static_cast<int>(i * 5);
    }
    REQUIRE(arr.capacity() == 10);
    for (size_t i = 0; i < arr.capacity(); ++i) {
        REQUIRE(arr[i] == static_cast<int>(i * 5));
    }
}

TEST_CASE("test small_array - small with raw_storage") {
    condy::SmallArray<condy::RawStorage<std::string>, 2> arr(2);
    arr[0].construct("Hello");
    arr[1].construct("World");

    REQUIRE(arr[0].get() == "Hello");
    REQUIRE(arr[1].get() == "World");

    arr[0].destroy();
    arr[1].destroy();
}

TEST_CASE("test small_array - large with raw_storage") {
    condy::SmallArray<condy::RawStorage<std::string>, 2> arr(3);
    arr[0].construct("First");
    arr[1].construct("Second");
    arr[2].construct("Third");

    REQUIRE(arr[0].get() == "First");
    REQUIRE(arr[1].get() == "Second");
    REQUIRE(arr[2].get() == "Third");

    arr[0].destroy();
    arr[1].destroy();
    arr[2].destroy();
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