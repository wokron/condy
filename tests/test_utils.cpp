#include "condy/utils.hpp"
#include <doctest/doctest.h>

TEST_CASE("test utils - ErasedDestructor") {
    struct Test {
        Test() = default;
        Test(int *c) : counter(c) {}
        Test(const Test &) = delete;
        Test(Test &&obj) : counter(std::exchange(obj.counter, nullptr)) {}
        ~Test() {
            if (counter != nullptr) {
                (*counter)++;
            }
        }

        int *counter = nullptr;
    };

    int counter = 0;
    { Test a(&counter); }
    REQUIRE(counter == 1);

    {
        Test b(&counter);
        { condy::ErasedDestructor erased_b(std::move(b)); }
        REQUIRE(counter == 2);
    }
    REQUIRE(counter == 2);

    {
        Test c(&counter);
        condy::ErasedDestructor may_empty;
        {
            condy::ErasedDestructor erased_c(std::move(c));
            may_empty = std::move(erased_c);
        }
        REQUIRE(counter == 2);
    }
    REQUIRE(counter == 3);
}