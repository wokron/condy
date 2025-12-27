#include <condy/finish_handles.hpp>
#include <doctest/doctest.h>

namespace {

struct SetFinishInvoker : public condy::InvokerAdapter<SetFinishInvoker> {
    void invoke() { finished = true; }
    bool finished = false;
};

struct SimpleFinishHandle {
    using ReturnType = int;

    void cancel() { cancelled_++; }

    void invoke(int res) {
        res_ = res;
        (*invoker_)(); // NOLINT(clang-analyzer-core.CallAndMessage)
    }

    int extract_result() { return res_; }

    void set_invoker(condy::Invoker *invoker) { invoker_ = invoker; }

    int res_;
    int cancelled_ = 0;
    condy::Invoker *invoker_ = nullptr;
};

} // namespace

TEST_CASE("test parallel_finish_handle - RangedWaitAllFinishHandle finish") {
    SimpleFinishHandle h1, h2, h3;
    condy::RangedWaitAllFinishHandle<SimpleFinishHandle> handle;
    handle.init(std::vector<SimpleFinishHandle *>{&h1, &h2, &h3});

    SetFinishInvoker invoker;
    handle.set_invoker(&invoker);

    h1.invoke(1);
    REQUIRE(!invoker.finished);

    h2.invoke(2);
    REQUIRE(!invoker.finished);

    h3.invoke(3);
    REQUIRE(invoker.finished);

    auto r = handle.extract_result();
    REQUIRE(r.size() == 3);
    REQUIRE(r[0] == 1);
    REQUIRE(r[1] == 2);
    REQUIRE(r[2] == 3);
}

TEST_CASE("test parallel_finish_handle - RangedWaitAllFinishHandle cancel") {
    SimpleFinishHandle h1, h2, h3;
    condy::RangedWaitAllFinishHandle<SimpleFinishHandle> handle;
    handle.init(std::vector<SimpleFinishHandle *>{&h1, &h2, &h3});
    SetFinishInvoker invoker;
    handle.set_invoker(&invoker);

    h1.invoke(1);
    REQUIRE(!invoker.finished);

    h2.invoke(2);
    REQUIRE(!invoker.finished);

    handle.cancel();
    REQUIRE(!invoker.finished);
    REQUIRE(h1.cancelled_ == 1);
    REQUIRE(h2.cancelled_ == 1);
    REQUIRE(h3.cancelled_ == 1);

    h3.invoke(-1);
    REQUIRE(invoker.finished);

    auto r = handle.extract_result();
    REQUIRE(r.size() == 3);
    REQUIRE(r[0] == 1);
    REQUIRE(r[1] == 2);
    REQUIRE(r[2] == -1);
}

TEST_CASE("test parallel_finish_handle - RangedWaitOneFinishHandle finish") {
    SimpleFinishHandle h1, h2, h3;
    condy::RangedWaitOneFinishHandle<SimpleFinishHandle> handle;
    handle.init(std::vector<SimpleFinishHandle *>{&h1, &h2, &h3});

    SetFinishInvoker invoker;
    handle.set_invoker(&invoker);

    SUBCASE("h1 finish first") {
        h1.invoke(2);
        REQUIRE(!invoker.finished);
        REQUIRE(h2.cancelled_ == 1);
        REQUIRE(h3.cancelled_ == 1);
        h2.invoke(3);
        h3.invoke(1);
        REQUIRE(invoker.finished);

        auto r = handle.extract_result();
        REQUIRE(r.first == 0);
        REQUIRE(r.second == 2);
    }

    SUBCASE("h2 finish first") {
        h2.invoke(3);
        REQUIRE(!invoker.finished);
        REQUIRE(h1.cancelled_ == 1);
        REQUIRE(h3.cancelled_ == 1);
        h3.invoke(1);
        h1.invoke(2);
        REQUIRE(invoker.finished);

        auto r = handle.extract_result();
        REQUIRE(r.first == 1);
        REQUIRE(r.second == 3);
    }

    SUBCASE("h3 finish first") {
        h3.invoke(1);
        REQUIRE(!invoker.finished);
        REQUIRE(h1.cancelled_ == 1);
        REQUIRE(h2.cancelled_ == 1);
        h1.invoke(2);
        h2.invoke(3);
        REQUIRE(invoker.finished);

        auto r = handle.extract_result();
        REQUIRE(r.first == 2);
        REQUIRE(r.second == 1);
    }
}

TEST_CASE("test parallel_finish_handle - RangedWaitOneFinishHandle "
          "multiple cancel") {
    SimpleFinishHandle h1, h2, h3;
    condy::RangedWaitOneFinishHandle<SimpleFinishHandle> handle;
    handle.init(std::vector<SimpleFinishHandle *>{&h1, &h2, &h3});

    SetFinishInvoker invoker;

    handle.set_invoker(&invoker);

    h1.invoke(1);
    REQUIRE(!invoker.finished);
    REQUIRE(h2.cancelled_ == 1);
    REQUIRE(h3.cancelled_ == 1);

    h3.invoke(-1);
    REQUIRE(!invoker.finished);
    REQUIRE(h2.cancelled_ == 1); // Should not increase

    handle.cancel();
    REQUIRE(!invoker.finished);
    REQUIRE(h2.cancelled_ == 1); // Should not increase

    h2.invoke(-1);
    REQUIRE(invoker.finished);
}

TEST_CASE("test parallel_finish_handle - Ranged (a && b) || (c && d)") {
    SimpleFinishHandle h1, h2, h3, h4;
    condy::RangedWaitAllFinishHandle<SimpleFinishHandle> finish_handle_ab;
    finish_handle_ab.init(std::vector<SimpleFinishHandle *>{&h1, &h2});
    condy::RangedWaitAllFinishHandle<SimpleFinishHandle> finish_handle_cd;
    finish_handle_cd.init(std::vector<SimpleFinishHandle *>{&h3, &h4});
    condy::RangedWaitOneFinishHandle<
        condy::RangedWaitAllFinishHandle<SimpleFinishHandle>>
        finish_handle;
    finish_handle.init(
        std::vector<condy::RangedWaitAllFinishHandle<SimpleFinishHandle> *>{
            &finish_handle_ab, &finish_handle_cd});

    SetFinishInvoker invoker;
    finish_handle.set_invoker(&invoker);

    SUBCASE("h1 -> h3 -> h2 -> h4") {
        h1.invoke(2);
        REQUIRE(!invoker.finished);

        h3.invoke(4);
        REQUIRE(!invoker.finished);

        h2.invoke(3);
        REQUIRE(!invoker.finished);
        REQUIRE(h3.cancelled_ == 1);
        REQUIRE(h4.cancelled_ == 1);

        h4.invoke(1);
        REQUIRE(invoker.finished);

        auto [idx, results] = finish_handle.extract_result();
        REQUIRE(idx == 0); // from ab
        REQUIRE(results.size() == 2);
        REQUIRE(results[0] == 2);
        REQUIRE(results[1] == 3);
    }

    SUBCASE("h3 -> h2 -> h4 -> h1") {
        h3.invoke(4);
        REQUIRE(!invoker.finished);

        h2.invoke(3);
        REQUIRE(!invoker.finished);

        h4.invoke(1);
        REQUIRE(!invoker.finished);
        REQUIRE(h1.cancelled_ == 1);
        REQUIRE(h2.cancelled_ == 1);

        h1.invoke(2);
        REQUIRE(invoker.finished);

        auto [idx, results] = finish_handle.extract_result();
        REQUIRE(idx == 1); // from cd
        REQUIRE(results.size() == 2);
        REQUIRE(results[0] == 4);
        REQUIRE(results[1] == 1);
    }
}

TEST_CASE("test parallel_finish_handle - WaitAllFinishHandle finish") {
    SimpleFinishHandle h1, h2, h3;
    condy::WaitAllFinishHandle<SimpleFinishHandle, SimpleFinishHandle,
                               SimpleFinishHandle>
        finish_handle;
    finish_handle.init(&h1, &h2, &h3);
    SetFinishInvoker invoker;
    finish_handle.set_invoker(&invoker);

    h1.invoke(1);
    REQUIRE(!invoker.finished);

    h2.invoke(2);
    REQUIRE(!invoker.finished);

    h3.invoke(3);
    REQUIRE(invoker.finished);

    auto r = finish_handle.extract_result();
    REQUIRE(std::get<0>(r) == 1);
    REQUIRE(std::get<1>(r) == 2);
    REQUIRE(std::get<2>(r) == 3);
}

TEST_CASE("test parallel_finish_handle - WaitAllFinishHandle cancel") {
    SimpleFinishHandle h1, h2, h3;
    condy::WaitAllFinishHandle<SimpleFinishHandle, SimpleFinishHandle,
                               SimpleFinishHandle>
        finish_handle;
    finish_handle.init(&h1, &h2, &h3);
    SetFinishInvoker invoker;
    finish_handle.set_invoker(&invoker);

    h1.invoke(1);
    REQUIRE(!invoker.finished);

    h2.invoke(2);
    REQUIRE(!invoker.finished);

    finish_handle.cancel();
    REQUIRE(!invoker.finished);
    REQUIRE(h1.cancelled_ == 1);
    REQUIRE(h2.cancelled_ == 1);
    REQUIRE(h3.cancelled_ == 1);

    h3.invoke(-1);
    REQUIRE(invoker.finished);

    auto r = finish_handle.extract_result();
    REQUIRE(std::get<0>(r) == 1);
    REQUIRE(std::get<1>(r) == 2);
    REQUIRE(std::get<2>(r) == -1);
}

TEST_CASE("test parallel_finish_handle - WaitOneFinishHandle finish") {
    SimpleFinishHandle h1, h2, h3;
    condy::WaitOneFinishHandle<SimpleFinishHandle, SimpleFinishHandle,
                               SimpleFinishHandle>
        finish_handle;
    finish_handle.init(&h1, &h2, &h3);
    SetFinishInvoker invoker;
    finish_handle.set_invoker(&invoker);

    SUBCASE("h1 finish first") {
        h1.invoke(2);
        REQUIRE(!invoker.finished);
        REQUIRE(h2.cancelled_ == 1);
        REQUIRE(h3.cancelled_ == 1);
        h2.invoke(3);
        h3.invoke(1);
        REQUIRE(invoker.finished);

        auto r = finish_handle.extract_result();
        REQUIRE(r.index() == 0);
        REQUIRE(std::get<0>(r) == 2);
    }

    SUBCASE("h2 finish first") {
        h2.invoke(3);
        REQUIRE(!invoker.finished);
        REQUIRE(h1.cancelled_ == 1);
        REQUIRE(h3.cancelled_ == 1);
        h3.invoke(1);
        h1.invoke(2);
        REQUIRE(invoker.finished);

        auto r = finish_handle.extract_result();
        REQUIRE(r.index() == 1);
        REQUIRE(std::get<1>(r) == 3);
    }

    SUBCASE("h3 finish first") {
        h3.invoke(1);
        REQUIRE(!invoker.finished);
        REQUIRE(h1.cancelled_ == 1);
        REQUIRE(h2.cancelled_ == 1);
        h1.invoke(2);
        h2.invoke(3);
        REQUIRE(invoker.finished);

        auto r = finish_handle.extract_result();
        REQUIRE(r.index() == 2);
        REQUIRE(std::get<2>(r) == 1);
    }
}

TEST_CASE("test parallel_finish_handle - WaitOneFinishHandle multiple "
          "cancel") {
    SimpleFinishHandle h1, h2, h3;
    condy::WaitOneFinishHandle<SimpleFinishHandle, SimpleFinishHandle,
                               SimpleFinishHandle>
        finish_handle;
    finish_handle.init(&h1, &h2, &h3);

    SetFinishInvoker invoker;
    finish_handle.set_invoker(&invoker);

    h1.invoke(1);
    REQUIRE(!invoker.finished);
    REQUIRE(h2.cancelled_ == 1);
    REQUIRE(h3.cancelled_ == 1);

    h3.invoke(-1);
    REQUIRE(!invoker.finished);
    REQUIRE(h2.cancelled_ == 1); // Should not increase

    finish_handle.cancel();
    REQUIRE(!invoker.finished);
    REQUIRE(h2.cancelled_ == 1); // Should not increase

    h2.invoke(-1);
    REQUIRE(invoker.finished);
}

TEST_CASE("test parallel_finish_handle - (a && b) || (c && d)") {
    SimpleFinishHandle h1, h2, h3, h4;
    condy::WaitAllFinishHandle<SimpleFinishHandle, SimpleFinishHandle>
        finish_handle_ab;
    finish_handle_ab.init(&h1, &h2);
    condy::WaitAllFinishHandle<SimpleFinishHandle, SimpleFinishHandle>
        finish_handle_cd;
    finish_handle_cd.init(&h3, &h4);
    condy::WaitOneFinishHandle<
        condy::WaitAllFinishHandle<SimpleFinishHandle, SimpleFinishHandle>,
        condy::WaitAllFinishHandle<SimpleFinishHandle, SimpleFinishHandle>>
        finish_handle;
    finish_handle.init(&finish_handle_ab, &finish_handle_cd);

    SetFinishInvoker invoker;
    finish_handle.set_invoker(&invoker);

    SUBCASE("h1 -> h3 -> h2 -> h4") {
        h1.invoke(2);
        REQUIRE(!invoker.finished);

        h3.invoke(4);
        REQUIRE(!invoker.finished);

        h2.invoke(3);
        REQUIRE(!invoker.finished);
        REQUIRE(h3.cancelled_ == 1);
        REQUIRE(h4.cancelled_ == 1);

        h4.invoke(1);
        REQUIRE(invoker.finished);

        auto r = finish_handle.extract_result();
        REQUIRE(r.index() == 0);
        auto res = std::get<0>(r);
        REQUIRE(std::get<0>(res) == 2);
        REQUIRE(std::get<1>(res) == 3);
    }

    SUBCASE("h3 -> h2 -> h4 -> h1") {
        h3.invoke(4);
        REQUIRE(!invoker.finished);

        h2.invoke(3);
        REQUIRE(!invoker.finished);

        h4.invoke(1);
        REQUIRE(!invoker.finished);
        REQUIRE(h1.cancelled_ == 1);
        REQUIRE(h2.cancelled_ == 1);

        h1.invoke(2);
        REQUIRE(invoker.finished);

        auto r = finish_handle.extract_result();
        REQUIRE(r.index() == 1);
        auto res = std::get<1>(r);
        REQUIRE(std::get<0>(res) == 4);
        REQUIRE(std::get<1>(res) == 1);
    }
}

TEST_CASE("test parallel_finish_handle - (a || b) && (c || d)") {
    SimpleFinishHandle h1, h2, h3, h4;
    condy::WaitOneFinishHandle<SimpleFinishHandle, SimpleFinishHandle>
        finish_handle_ab;
    finish_handle_ab.init(&h1, &h2);
    condy::WaitOneFinishHandle<SimpleFinishHandle, SimpleFinishHandle>
        finish_handle_cd;
    finish_handle_cd.init(&h3, &h4);
    condy::WaitAllFinishHandle<
        condy::WaitOneFinishHandle<SimpleFinishHandle, SimpleFinishHandle>,
        condy::WaitOneFinishHandle<SimpleFinishHandle, SimpleFinishHandle>>
        finish_handle;
    finish_handle.init(&finish_handle_ab, &finish_handle_cd);

    SetFinishInvoker invoker;
    finish_handle.set_invoker(&invoker);

    h1.invoke(2);
    REQUIRE(!invoker.finished);
    REQUIRE(h2.cancelled_ == 1);

    h2.invoke(-1);
    REQUIRE(!invoker.finished);

    h3.invoke(4);
    REQUIRE(!invoker.finished);
    REQUIRE(h4.cancelled_ == 1);

    h4.invoke(-1);
    REQUIRE(invoker.finished);

    auto r = finish_handle.extract_result();
    auto res_ab = std::get<0>(r);
    REQUIRE(res_ab.index() == 0);
    REQUIRE(std::get<0>(res_ab) == 2);
    auto res_cd = std::get<1>(r);
    REQUIRE(res_cd.index() == 0);
    REQUIRE(std::get<0>(res_cd) == 4);
}