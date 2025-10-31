#include "condy/singleton.hpp"
#include <doctest/doctest.h>
#include <thread>

TEST_CASE("test singleton - ThreadLocalSingleton") {
    struct MySingleton : public condy::ThreadLocalSingleton<MySingleton> {
        int value = 0;
    };

    MySingleton::current().value = 42;
    REQUIRE(MySingleton::current().value == 42);

    auto *ptr = &MySingleton::current();

    std::thread t1([&]() {
        REQUIRE(MySingleton::current().value == 0);
        MySingleton::current().value = 100;
        REQUIRE(MySingleton::current().value == 100);
        REQUIRE(ptr != &MySingleton::current());
    });

    t1.join();
}
