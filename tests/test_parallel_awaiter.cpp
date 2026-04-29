#include "condy/invoker.hpp"
#include "condy/runtime.hpp"
#include "condy/type_traits.hpp"
#include <condy/awaiters.hpp>
#include <condy/coro.hpp>
#include <cstddef>
#include <doctest/doctest.h>
#include <memory>
#include <optional>

namespace {

struct SimpleFinishHandle {
    using ReturnType = int;

    void cancel(condy::Runtime *) { cancelled_++; }

    void invoke(int res) {
        res_ = res;
        (*invoker_)();
    }

    int extract_result() { return res_; }

    void set_invoker(condy::Invoker *invoker) {
        invoker_ = invoker;
        registered_ = true;
    }

    int res_;
    int cancelled_ = 0;
    bool registered_ = false;
    condy::Invoker *invoker_ = nullptr;
};

struct SimpleSender {
    using ReturnType = int;

    template <typename Receiver> auto connect_impl(Receiver receiver) noexcept {
        return OperationState<Receiver>{handle_ptr_, std::move(receiver)};
    }

    template <typename Receiver>
    struct OperationState
        : public condy::InvokerAdapter<OperationState<Receiver>> {

        OperationState(std::shared_ptr<SimpleFinishHandle> handle_ptr,
                       Receiver receiver)
            : handle_ptr_(std::move(handle_ptr)),
              receiver_(std::move(receiver)) {}

        void start(unsigned int) {
            handle_ptr_->set_invoker(this);
            auto stop_token = receiver_.get_stop_token();
            if (stop_token.stop_possible()) {
                stop_callback_.emplace(std::move(stop_token),
                                       Cancellation{this});
            }
        }

        void invoke() {
            int res = handle_ptr_->extract_result();
            std::move(receiver_)(res);
        }

        struct Cancellation {
            OperationState *self;
            void operator()() { self->cancel_(); }
        };
        void cancel_() { handle_ptr_->cancel(nullptr); }

        using TokenType = condy::stop_token_t<Receiver>;
        using StopCallbackType =
            condy::stop_callback_t<TokenType, Cancellation>;

        std::shared_ptr<SimpleFinishHandle> handle_ptr_;
        Receiver receiver_;
        std::optional<StopCallbackType> stop_callback_;
    };

    // Use ptr to make handle accessible outside of ParallelAwaiter
    std::shared_ptr<SimpleFinishHandle> handle_ptr_ =
        std::make_shared<SimpleFinishHandle>();
};

using SimpleAwaiter = SimpleSender;

} // namespace

TEST_CASE("test parallel_awaiter - RangedWhenAllAwaiter") {
    SimpleAwaiter a1, a2, a3;
    auto h1 = a1.handle_ptr_;
    auto h2 = a2.handle_ptr_;
    auto h3 = a3.handle_ptr_;
    bool finished = false;

    auto func = [&]() -> condy::Coro<void> {
        condy::RangedWhenAllAwaiter<SimpleAwaiter> awaiter(
            std::vector<SimpleAwaiter>{a1, a2, a3});
        std::vector<int> results = co_await awaiter;
        REQUIRE(results.size() == 3);
        REQUIRE(results[0] == 1);
        REQUIRE(results[1] == 2);
        REQUIRE(results[2] == 3);
        finished = true;
    };

    auto coro = func();
    REQUIRE(!finished);

    auto handle = coro.release();
    handle.resume();
    REQUIRE(!finished);
    // Now all awaiters should be registered
    REQUIRE(h1->registered_);
    REQUIRE(h2->registered_);
    REQUIRE(h3->registered_);

    h1->invoke(1);
    REQUIRE(!finished);

    h2->invoke(2);
    REQUIRE(!finished);

    h3->invoke(3);
    REQUIRE(finished);
    handle.destroy();
}

TEST_CASE("test parallel_awaiter - RangedWhenAnyAwaiter") {
    SimpleAwaiter a1, a2, a3;
    auto h1 = a1.handle_ptr_;
    auto h2 = a2.handle_ptr_;
    auto h3 = a3.handle_ptr_;
    bool finished = false;

    auto func = [&](size_t expected_idx,
                    int expected_result) -> condy::Coro<void> {
        condy::RangedWhenAnyAwaiter<SimpleAwaiter> awaiter(
            std::vector<SimpleAwaiter>{a1, a2, a3});
        auto [idx, result] = co_await awaiter;
        REQUIRE(idx == expected_idx);
        REQUIRE(result == expected_result);
        finished = true;
    };

    SUBCASE("a1 finish first") {
        auto coro = func(0, 2);
        REQUIRE(!finished);

        auto handle = coro.release();
        handle.resume();
        REQUIRE(!finished);
        // Now all awaiters should be registered
        REQUIRE(h1->registered_);
        REQUIRE(h2->registered_);
        REQUIRE(h3->registered_);

        h1->invoke(2);
        REQUIRE(h2->cancelled_);
        REQUIRE(h3->cancelled_);

        h2->invoke(-1);
        h3->invoke(-1);
        REQUIRE(finished);
        handle.destroy();
    }

    SUBCASE("a2 finish first") {
        auto coro = func(1, 3);
        REQUIRE(!finished);

        auto handle = coro.release();
        handle.resume();
        REQUIRE(!finished);
        // Now all awaiters should be registered
        REQUIRE(h1->registered_);
        REQUIRE(h2->registered_);
        REQUIRE(h3->registered_);

        h2->invoke(3);
        REQUIRE(h1->cancelled_);
        REQUIRE(h3->cancelled_);

        h1->invoke(-1);
        h3->invoke(-1);
        REQUIRE(finished);
        handle.destroy();
    }

    SUBCASE("a3 finish first") {
        auto coro = func(2, 1);
        REQUIRE(!finished);

        auto handle = coro.release();
        handle.resume();
        REQUIRE(!finished);
        // Now all awaiters should be registered
        REQUIRE(h1->registered_);
        REQUIRE(h2->registered_);
        REQUIRE(h3->registered_);

        h3->invoke(1);
        REQUIRE(h1->cancelled_);
        REQUIRE(h2->cancelled_);

        h1->invoke(-1);
        h2->invoke(-1);
        REQUIRE(finished);
        handle.destroy();
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
                    std::vector<int> expected_results) -> condy::Coro<void> {
        using WaitAll = condy::RangedWhenAllAwaiter<SimpleAwaiter>;
        using WaitOne = condy::RangedWhenAnyAwaiter<WaitAll>;
        WaitAll awaiter_ab(std::vector<SimpleAwaiter>{a1, a2});
        WaitAll awaiter_cd(std::vector<SimpleAwaiter>{a3, a4});
        std::vector<WaitAll> awaiters;
        awaiters.push_back(std::move(awaiter_ab));
        awaiters.push_back(std::move(awaiter_cd));
        WaitOne awaiter(std::move(awaiters));
        auto [idx, results] = co_await awaiter;
        REQUIRE(idx == expected_idx);
        REQUIRE(results == expected_results);
        finished = true;
    };

    SUBCASE("a1 -> a3 -> a2 -> a4") {
        auto coro = func(0, {2, 3});
        REQUIRE(!finished);

        auto handle = coro.release();
        handle.resume();
        REQUIRE(!finished);

        h1->invoke(2);
        REQUIRE(!finished);

        h3->invoke(4);
        REQUIRE(!finished);

        h2->invoke(3);
        REQUIRE(h3->cancelled_);
        REQUIRE(h4->cancelled_);

        h4->invoke(-1); // finish due to cancellation
        REQUIRE(finished);
        handle.destroy();
    }
}

TEST_CASE("test parallel_awaiter - WhenAllAwaiter") {
    SimpleAwaiter a1, a2, a3;
    auto h1 = a1.handle_ptr_;
    auto h2 = a2.handle_ptr_;
    auto h3 = a3.handle_ptr_;
    bool finished = false;

    auto func = [&]() -> condy::Coro<void> {
        condy::WhenAllAwaiter<SimpleAwaiter, SimpleAwaiter, SimpleAwaiter>
            awaiter(a1, a2, a3);
        std::tuple<int, int, int> results = co_await awaiter;
        REQUIRE(std::get<0>(results) == 1);
        REQUIRE(std::get<1>(results) == 2);
        REQUIRE(std::get<2>(results) == 3);
        finished = true;
    };

    auto coro = func();
    REQUIRE(!finished);

    auto handle = coro.release();
    handle.resume();
    REQUIRE(!finished);
    // Now all awaiters should be registered
    REQUIRE(h1->registered_);
    REQUIRE(h2->registered_);
    REQUIRE(h3->registered_);

    h1->invoke(1);
    REQUIRE(!finished);

    h2->invoke(2);
    REQUIRE(!finished);

    h3->invoke(3);
    REQUIRE(finished);
    handle.destroy();
}

TEST_CASE("test parallel_awaiter - WhenAnyAwaiter") {
    SimpleAwaiter a1, a2, a3;
    auto h1 = a1.handle_ptr_;
    auto h2 = a2.handle_ptr_;
    auto h3 = a3.handle_ptr_;
    bool finished = false;

    auto func =
        [&](size_t expected_idx,
            std::variant<int, int, int> expected_result) -> condy::Coro<void> {
        condy::WhenAnyAwaiter<SimpleAwaiter, SimpleAwaiter, SimpleAwaiter>
            awaiter(a1, a2, a3);
        auto result = co_await awaiter;
        REQUIRE(result.index() == expected_idx);
        REQUIRE(result == expected_result);
        finished = true;
    };

    SUBCASE("a1 finish first") {
        auto coro =
            func(0, std::variant<int, int, int>{std::in_place_index<0>, 2});
        REQUIRE(!finished);

        auto handle = coro.release();
        handle.resume();
        REQUIRE(!finished);
        // Now all awaiters should be registered
        REQUIRE(h1->registered_);
        REQUIRE(h2->registered_);
        REQUIRE(h3->registered_);

        h1->invoke(2);
        REQUIRE(h2->cancelled_);
        REQUIRE(h3->cancelled_);

        h2->invoke(-1);
        h3->invoke(-1);
        REQUIRE(finished);
        handle.destroy();
    }

    SUBCASE("a2 finish first") {
        auto coro =
            func(1, std::variant<int, int, int>{std::in_place_index<1>, 3});
        REQUIRE(!finished);

        auto handle = coro.release();
        handle.resume();
        REQUIRE(!finished);
        // Now all awaiters should be registered
        REQUIRE(h1->registered_);
        REQUIRE(h2->registered_);
        REQUIRE(h3->registered_);

        h2->invoke(3);
        REQUIRE(h1->cancelled_);
        REQUIRE(h3->cancelled_);

        h1->invoke(-1);
        h3->invoke(-1);
        REQUIRE(finished);
        handle.destroy();
    }

    SUBCASE("a3 finish first") {
        auto coro =
            func(2, std::variant<int, int, int>{std::in_place_index<2>, 1});
        REQUIRE(!finished);

        auto handle = coro.release();
        handle.resume();
        REQUIRE(!finished);
        // Now all awaiters should be registered
        REQUIRE(h1->registered_);
        REQUIRE(h2->registered_);
        REQUIRE(h3->registered_);

        h3->invoke(1);
        REQUIRE(h1->cancelled_);
        REQUIRE(h2->cancelled_);

        h1->invoke(-1);
        h2->invoke(-1);
        REQUIRE(finished);
        handle.destroy();
    }
}

TEST_CASE("test parallel_awaiter - (a && b) || (c && d) with WaitAllAwaiter "
          "and WaitOneAwaiter") {
    SimpleAwaiter a1, a2, a3, a4;
    auto h1 = a1.handle_ptr_;
    auto h2 = a2.handle_ptr_;
    auto h3 = a3.handle_ptr_;
    auto h4 = a4.handle_ptr_;
    bool finished = false;

    auto func = [&](size_t expected_idx,
                    std::variant<std::tuple<int, int>, std::tuple<int, int>>
                        expected_results) -> condy::Coro<void> {
        using WaitAll = condy::WhenAllAwaiter<SimpleAwaiter, SimpleAwaiter>;
        using WaitOne = condy::WhenAnyAwaiter<WaitAll, WaitAll>;
        WaitAll awaiter_ab(a1, a2);
        WaitAll awaiter_cd(a3, a4);
        WaitOne awaiter(std::move(awaiter_ab), std::move(awaiter_cd));
        auto result = co_await awaiter;
        REQUIRE(result.index() == expected_idx);
        REQUIRE(result == expected_results);
        finished = true;
    };

    SUBCASE("a1 -> a3 -> a2 -> a4") {
        auto coro =
            func(0, std::variant<std::tuple<int, int>, std::tuple<int, int>>{
                        std::in_place_index<0>, std::make_tuple(2, 3)});
        REQUIRE(!finished);

        auto handle = coro.release();
        handle.resume();
        REQUIRE(!finished);

        h1->invoke(2);
        REQUIRE(!finished);

        h3->invoke(4);
        REQUIRE(!finished);

        h2->invoke(3);
        REQUIRE(h3->cancelled_);
        REQUIRE(h4->cancelled_);

        h4->invoke(-1); // finish due to cancellation
        REQUIRE(finished);
        handle.destroy();
    }

    SUBCASE("a1 -> a3 -> a2 -> a4") {
        auto coro =
            func(1, std::variant<std::tuple<int, int>, std::tuple<int, int>>{
                        std::in_place_index<1>, std::make_tuple(4, 5)});
        REQUIRE(!finished);

        auto handle = coro.release();
        handle.resume();
        REQUIRE(!finished);

        h3->invoke(4);
        REQUIRE(!finished);

        h1->invoke(2);
        REQUIRE(!finished);

        h4->invoke(5);
        REQUIRE(h1->cancelled_);
        REQUIRE(h2->cancelled_);

        h2->invoke(-1); // finish due to cancellation
        REQUIRE(finished);
        handle.destroy();
    }
}
