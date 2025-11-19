#include "condy/async_operations.hpp"
#include "condy/awaiter_operations.hpp"
#include "condy/coro.hpp"
#include "condy/runtime.hpp"
#include "condy/task.hpp"
#include <doctest/doctest.h>

TEST_CASE("test runtime_options - single thread sqpoll") {
    condy::RuntimeOptions options;
    options.enable_sqpoll().sq_size(8).cq_size(16);
    condy::Runtime runtime(options);

    bool finished = false;

    auto func = [&]() -> condy::Coro<void> {
        // submit many nops
        using Nop = decltype(condy::async_nop());
        std::vector<Nop> nops;
        for (int i = 0; i < 100; i++) {
            nops.push_back(condy::async_nop());
        }
        auto awaiter = condy::make_ranged_all_awaiter(std::move(nops));
        co_await awaiter;

        finished = true;
    };

    condy::co_spawn(runtime, func()).detach();

    runtime.done();
    runtime.run();

    REQUIRE(finished);
}