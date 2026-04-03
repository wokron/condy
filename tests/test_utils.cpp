#include <condy/utils.hpp>
#include <doctest/doctest.h>
#include <limits>
#include <memory>
#include <string>
#include <thread>

namespace {

struct int_deleter {
    void operator()(int *p) const {
        called = true;
        delete p;
    }
    inline static bool called = false;
};

} // namespace

TEST_CASE("test raw_storage - int") {
    condy::RawStorage<int> storage;
    storage.construct(77);
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

TEST_CASE("test raw_storage - guaranteed return value optimization") {
    struct Fixed {
        Fixed(int v) : value(v) {}
        Fixed(const Fixed &) = delete;
        Fixed &operator=(const Fixed &) = delete;
        Fixed(Fixed &&) = delete;
        Fixed &operator=(Fixed &&) = delete;
        int value;
    };

    auto f1 = [](int v) { return Fixed(v); };

    auto f2 = [&]() { return f1(42); };

    condy::RawStorage<Fixed> storage;
    storage.accept(f2);
    REQUIRE(storage.get().value == 42);
    storage.destroy();
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

TEST_CASE("test id_pool - basic") {
    condy::IdPool<uint32_t, 0, 8> pool;
    auto id1 = pool.allocate();
    auto id2 = pool.allocate();
    REQUIRE(id1 != id2);
    pool.recycle(id1);
    auto id3 = pool.allocate();
    REQUIRE(id3 == id1);
    pool.recycle(id2);
    pool.recycle(id3);
    pool.reset();
    auto id4 = pool.allocate();
    REQUIRE(id4 == 0);
}

TEST_CASE("test id_pool - exhaustion") {
    constexpr uint32_t max_ids = 2;
    condy::IdPool<uint32_t, 0, max_ids> pool;
    for (uint32_t i = 0; i < max_ids; ++i) {
        pool.allocate();
    }
    REQUIRE_THROWS_AS(pool.allocate(), std::runtime_error);
    pool.recycle(0);
    auto id = pool.allocate();
    REQUIRE(id == 0);
}

TEST_CASE("test atomic_mutex - concurrent") {
    condy::AtomicMutex mutex;
    size_t counter = 0;
    constexpr size_t times = 10000;

    auto worker = [&]() {
        for (size_t i = 0; i < times; ++i) {
            std::lock_guard lock(mutex);
            ++counter;
        }
    };

    std::thread t1(worker);
    std::thread t2(worker);
    t1.join();
    t2.join();
    REQUIRE(counter == times * 2);
}