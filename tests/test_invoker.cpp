#include "condy/invoker.hpp"
#include <doctest/doctest.h>

namespace {

class TestA : public condy::InvokerAdapter<TestA> {
public:
    TestA() : called_(false) {}
    void operator()() { called_ = true; }
    bool called() const { return called_; }

private:
    bool called_;
};

class TestB : public condy::InvokerAdapter<TestB> {
public:
    TestB() : value_(0) {}
    void operator()() { value_ += 42; }
    int value() const { return value_; }

private:
    int value_;
};

} // namespace

TEST_CASE("test invoker - functionality") {
    TestA a;
    REQUIRE_FALSE(a.called());
    a();
    REQUIRE(a.called());

    TestB b;
    REQUIRE_EQ(b.value(), 0);
    b();
    REQUIRE_EQ(b.value(), 42);
}

TEST_CASE("test invoker - type erase") {
    condy::Invoker *invoker = nullptr;

    {
        TestA a;
        invoker = &a;
        REQUIRE_FALSE(a.called());
        (*invoker)();
        REQUIRE(a.called());
    }

    {
        TestB b;
        invoker = &b;
        REQUIRE_EQ(b.value(), 0);
        (*invoker)();
        REQUIRE_EQ(b.value(), 42);
    }
}