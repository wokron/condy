#include <condy/awaiter.hpp>
#include <condy/coro.hpp>
#include <doctest/doctest.h>
#include <memory>

namespace {

struct SimpleFinishHandle {
    using ReturnType = int;

    void set_on_finish(std::function<void(int)> on_finish) {
        on_finish_ = std::move(on_finish);
    }

    void finish(int r) {
        if (on_finish_) {
            std::move(on_finish_)(r);
        }
    }

    void cancel() { cancelled_ = true; }

    std::function<void(int)> on_finish_ = nullptr;
    bool cancelled_ = false;
};

class SimpleAwaiter {
public:
    using HandleType = SimpleFinishHandle;

    HandleType *get_handle() { return handle_ptr_.get(); }

    bool await_ready() const noexcept { return false; }

    template <typename PromiseType>
    void await_suspend(std::coroutine_handle<PromiseType> h) {
        // Do nothing, since it will not be called in this test
    }

    int await_resume() {
        return 0; // Do nothing
    }

    void init_finish_handle() {
        // Do nothing
    }

    // Use ptr to make handle accessible outside of ParallelAwaiter
    std::shared_ptr<HandleType> handle_ptr_ = std::make_shared<HandleType>();
};

} // namespace

TEST_CASE("test parallel_awaiter - RangedWaitAllAwaiter") {
    SimpleAwaiter a1, a2, a3;
    auto h1 = a1.handle_ptr_;
    auto h2 = a2.handle_ptr_;
    auto h3 = a3.handle_ptr_;
    bool finished = false;

    auto func = [&]() -> condy::Coro {
        condy::RangedWaitAllAwaiter<SimpleAwaiter> awaiter(
            std::vector<SimpleAwaiter>{a1, a2, a3});
        std::vector<int> results = co_await awaiter;
        CHECK(results.size() == 3);
        CHECK(results[0] == 1);
        CHECK(results[1] == 2);
        CHECK(results[2] == 3);
        finished = true;
    };

    auto coro = func();
    REQUIRE(!finished);

    coro.release().resume();
    REQUIRE(!finished);
    // Now all awaiters should be registered
    REQUIRE(h1->on_finish_ != nullptr);
    REQUIRE(h2->on_finish_ != nullptr);
    REQUIRE(h3->on_finish_ != nullptr);

    h1->finish(1);
    REQUIRE(!finished);

    h2->finish(2);
    REQUIRE(!finished);

    h3->finish(3);
    REQUIRE(finished);
}

TEST_CASE("test parallel_awaiter - RangedWaitOneAwaiter") {
    SimpleAwaiter a1, a2, a3;
    auto h1 = a1.handle_ptr_;
    auto h2 = a2.handle_ptr_;
    auto h3 = a3.handle_ptr_;
    bool finished = false;

    auto func = [&](size_t expected_idx, int expected_result) -> condy::Coro {
        condy::RangedWaitOneAwaiter<SimpleAwaiter> awaiter(
            std::vector<SimpleAwaiter>{a1, a2, a3});
        auto [idx, result] = co_await awaiter;
        CHECK(idx == expected_idx);
        CHECK(result == expected_result);
        finished = true;
    };

    SUBCASE("a1 finish first") {
        auto coro = func(0, 2);
        REQUIRE(!finished);

        coro.release().resume();
        REQUIRE(!finished);
        // Now all awaiters should be registered
        REQUIRE(h1->on_finish_ != nullptr);
        REQUIRE(h2->on_finish_ != nullptr);
        REQUIRE(h3->on_finish_ != nullptr);

        h1->finish(2);
        REQUIRE(h2->cancelled_);
        REQUIRE(h3->cancelled_);

        h2->finish(-1);
        h3->finish(-1);
        REQUIRE(finished);
    }

    SUBCASE("a2 finish first") {
        auto coro = func(1, 3);
        REQUIRE(!finished);

        coro.release().resume();
        REQUIRE(!finished);
        // Now all awaiters should be registered
        REQUIRE(h1->on_finish_ != nullptr);
        REQUIRE(h2->on_finish_ != nullptr);
        REQUIRE(h3->on_finish_ != nullptr);

        h2->finish(3);
        REQUIRE(h1->cancelled_);
        REQUIRE(h3->cancelled_);

        h1->finish(-1);
        h3->finish(-1);
        REQUIRE(finished);
    }

    SUBCASE("a3 finish first") {
        auto coro = func(2, 1);
        REQUIRE(!finished);

        coro.release().resume();
        REQUIRE(!finished);
        // Now all awaiters should be registered
        REQUIRE(h1->on_finish_ != nullptr);
        REQUIRE(h2->on_finish_ != nullptr);
        REQUIRE(h3->on_finish_ != nullptr);

        h3->finish(1);
        REQUIRE(h1->cancelled_);
        REQUIRE(h2->cancelled_);

        h1->finish(-1);
        h2->finish(-1);
        REQUIRE(finished);
    }
}

TEST_CASE("test parallel_awaiter - Ranged (a && b) || (c && d)") {
    SimpleAwaiter a1, a2, a3, a4;
    auto h1 = a1.handle_ptr_;
    auto h2 = a2.handle_ptr_;
    auto h3 = a3.handle_ptr_;
    auto h4 = a4.handle_ptr_;
    bool finished = false;

    auto func = [&](size_t expected_idx,
                    std::vector<int> expected_results) -> condy::Coro {
        using WaitAll = condy::RangedWaitAllAwaiter<SimpleAwaiter>;
        using WaitOne = condy::RangedWaitOneAwaiter<WaitAll>;
        WaitAll awaiter_ab(std::vector<SimpleAwaiter>{a1, a2});
        WaitAll awaiter_cd(std::vector<SimpleAwaiter>{a3, a4});
        WaitOne awaiter(std::vector<WaitAll>{awaiter_ab, awaiter_cd});
        auto [idx, results] = co_await awaiter;
        CHECK(idx == expected_idx);
        CHECK(results == expected_results);
        finished = true;
    };

    SUBCASE("a1 -> a3 -> a2 -> a4") {
        auto coro = func(0, {2, 3});
        REQUIRE(!finished);

        coro.release().resume();
        REQUIRE(!finished);

        h1->finish(2);
        REQUIRE(!finished);

        h3->finish(4);
        REQUIRE(!finished);

        h2->finish(3);
        REQUIRE(h3->cancelled_);
        REQUIRE(h4->cancelled_);

        h4->finish(-1); // finish due to cancellation
        REQUIRE(finished);
    }
}