#include <condy/finish_handle.hpp>
#include <doctest/doctest.h>

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

} // namespace

TEST_CASE("test parallel_finish_handle - RangedWaitAllFinishHandle finish") {
    SimpleFinishHandle h1, h2, h3;
    condy::RangedWaitAllFinishHandle<SimpleFinishHandle> finish_handle;
    finish_handle.init(std::vector<SimpleFinishHandle *>{&h1, &h2, &h3});

    bool finished = false;
    finish_handle.set_on_finish([&finished](std::vector<int> r) {
        finished = true;
        CHECK(r.size() == 3);
        CHECK(r[0] == 1);
        CHECK(r[1] == 2);
        CHECK(r[2] == 3);
    });

    h1.finish(1);
    CHECK(!finished);

    h2.finish(2);
    CHECK(!finished);

    h3.finish(3);
    CHECK(finished);
}

TEST_CASE("test parallel_finish_handle - RangedWaitAllFinishHandle cancel") {
    SimpleFinishHandle h1, h2, h3;
    condy::RangedWaitAllFinishHandle<SimpleFinishHandle> finish_handle;
    finish_handle.init(std::vector<SimpleFinishHandle *>{&h1, &h2, &h3});

    bool finished = false;
    finish_handle.set_on_finish(
        [&finished](std::vector<int> r) { finished = true; });

    h1.finish(1);
    CHECK(!finished);

    h2.finish(2);
    CHECK(!finished);

    finish_handle.cancel();
    CHECK(!finished);
    CHECK(h1.cancelled_);
    CHECK(h2.cancelled_);
    CHECK(h3.cancelled_);

    h3.finish(3);
    CHECK(finished);
}

TEST_CASE("test parallel_finish_handle - RangedWaitOneFinishHandle finish") {
    SimpleFinishHandle h1, h2, h3;
    condy::RangedWaitOneFinishHandle<SimpleFinishHandle> finish_handle;
    finish_handle.init(std::vector<SimpleFinishHandle *>{&h1, &h2, &h3});
    bool finished = false;

    SUBCASE("h1 finish first") {
        finish_handle.set_on_finish([&finished](size_t idx, int r) {
            finished = true;
            CHECK(idx == 0);
            CHECK(r == 2);
        });

        h1.finish(2);
        h2.finish(3);
        h3.finish(1);
        CHECK(finished);
    }

    SUBCASE("h2 finish first") {
        finish_handle.set_on_finish([&finished](size_t idx, int r) {
            finished = true;
            CHECK(idx == 1);
            CHECK(r == 3);
        });

        h2.finish(3);
        h3.finish(1);
        h1.finish(2);
        CHECK(finished);
    }

    SUBCASE("h3 finish first") {
        finish_handle.set_on_finish([&finished](size_t idx, int r) {
            finished = true;
            CHECK(idx == 2);
            CHECK(r == 1);
        });

        h3.finish(1);
        h1.finish(2);
        h2.finish(3);
        CHECK(finished);
    }
}

TEST_CASE("test parallel_finish_handle - RangedWaitOneFinishHandle cancel") {
    SimpleFinishHandle h1, h2, h3;
    condy::RangedWaitOneFinishHandle<SimpleFinishHandle> finish_handle;
    finish_handle.init(std::vector<SimpleFinishHandle *>{&h1, &h2, &h3});
    bool finished = false;

    finish_handle.set_on_finish(
        [&finished](size_t idx, int r) { finished = true; });

    finish_handle.cancel();
    CHECK(!finished);
    CHECK(h1.cancelled_);
    CHECK(h2.cancelled_);
    CHECK(h3.cancelled_);

    h1.finish(1);
    CHECK(!finished);

    h2.finish(2);
    CHECK(!finished);

    h3.finish(3);
    CHECK(finished);
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

    bool finished = false;

    SUBCASE("h1 -> h3 -> h2 -> h4") {
        finish_handle.set_on_finish(
            [&finished](size_t idx,
                        std::vector<int> r) { // idx: 0 for ab, 1 for cd
                finished = true;
                CHECK(idx == 0);
                CHECK(r.size() == 2);
                CHECK(r[0] == 2);
                CHECK(r[1] == 3);
            });

        h1.finish(2);
        CHECK(!finished);

        h3.finish(4);
        CHECK(!finished);

        h2.finish(3);
        CHECK(!finished);
        CHECK(h3.cancelled_);
        CHECK(h4.cancelled_);

        h4.finish(1);
        CHECK(finished);
    }

    SUBCASE("h3 -> h2 -> h4 -> h1") {
        finish_handle.set_on_finish(
            [&finished](size_t idx,
                        std::vector<int> r) { // idx: 0 for ab, 1 for cd
                finished = true;
                CHECK(idx == 1);
                CHECK(r.size() == 2);
                CHECK(r[0] == 4);
                CHECK(r[1] == 1);
            });

        h3.finish(4);
        CHECK(!finished);

        h2.finish(3);
        CHECK(!finished);

        h4.finish(1);
        CHECK(!finished);
        CHECK(h1.cancelled_);
        CHECK(h2.cancelled_);

        h1.finish(2);
        CHECK(finished);
    }
}

TEST_CASE("test parallel_finish_handle - WaitAllFinishHandle finish") {
    SimpleFinishHandle h1, h2, h3;
    condy::WaitAllFinishHandle<SimpleFinishHandle, SimpleFinishHandle,
                               SimpleFinishHandle>
        finish_handle;
    finish_handle.init(&h1, &h2, &h3);
    bool finished = false;
    finish_handle.set_on_finish([&finished](std::tuple<int, int, int> r) {
        finished = true;
        CHECK(std::get<0>(r) == 1);
        CHECK(std::get<1>(r) == 2);
        CHECK(std::get<2>(r) == 3);
    });

    h1.finish(1);
    CHECK(!finished);

    h2.finish(2);
    CHECK(!finished);

    h3.finish(3);
    CHECK(finished);
}

TEST_CASE("test parallel_finish_handle - WaitAllFinishHandle cancel") {
    SimpleFinishHandle h1, h2, h3;
    condy::WaitAllFinishHandle<SimpleFinishHandle, SimpleFinishHandle,
                               SimpleFinishHandle>
        finish_handle;
    finish_handle.init(&h1, &h2, &h3);
    bool finished = false;
    finish_handle.set_on_finish([&finished](std::tuple<int, int, int> r) {
        finished = true;
        CHECK(std::get<0>(r) == 1);
        CHECK(std::get<1>(r) == 2);
        CHECK(std::get<2>(r) == 3);
    });

    h1.finish(1);
    CHECK(!finished);

    h2.finish(2);
    CHECK(!finished);

    finish_handle.cancel();
    CHECK(!finished);
    CHECK(h1.cancelled_);
    CHECK(h2.cancelled_);
    CHECK(h3.cancelled_);

    h3.finish(3);
    CHECK(finished);
}

TEST_CASE("test parallel_finish_handle - WaitOneFinishHandle finish") {
    SimpleFinishHandle h1, h2, h3;
    condy::WaitOneFinishHandle<SimpleFinishHandle, SimpleFinishHandle,
                               SimpleFinishHandle>
        finish_handle;
    finish_handle.init(&h1, &h2, &h3);
    bool finished = false;

    SUBCASE("h1 finish first") {
        finish_handle.set_on_finish([&finished](std::variant<int, int, int> r) {
            finished = true;
            CHECK(r.index() == 0);
            CHECK(std::get<0>(r) == 2);
        });

        h1.finish(2);
        h2.finish(3);
        h3.finish(1);
        CHECK(finished);
    }

    SUBCASE("h2 finish first") {
        finish_handle.set_on_finish([&finished](std::variant<int, int, int> r) {
            finished = true;
            CHECK(r.index() == 1);
            CHECK(std::get<1>(r) == 3);
        });

        h2.finish(3);
        h3.finish(1);
        h1.finish(2);
        CHECK(finished);
    }

    SUBCASE("h3 finish first") {
        finish_handle.set_on_finish([&finished](std::variant<int, int, int> r) {
            finished = true;
            CHECK(r.index() == 2);
            CHECK(std::get<2>(r) == 1);
        });

        h3.finish(1);
        h1.finish(2);
        h2.finish(3);
        CHECK(finished);
    }
}

TEST_CASE("test parallel_finish_handle - WaitOneFinishHandle cancel") {
    SimpleFinishHandle h1, h2, h3;
    condy::WaitOneFinishHandle<SimpleFinishHandle, SimpleFinishHandle,
                               SimpleFinishHandle>
        finish_handle;
    finish_handle.init(&h1, &h2, &h3);
    bool finished = false;

    finish_handle.set_on_finish(
        [&finished](std::variant<int, int, int> r) { finished = true; });

    finish_handle.cancel();
    CHECK(!finished);
    CHECK(h1.cancelled_);
    CHECK(h2.cancelled_);
    CHECK(h3.cancelled_);

    h1.finish(1);
    CHECK(!finished);

    h2.finish(2);
    CHECK(!finished);

    h3.finish(3);
    CHECK(finished);
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

    bool finished = false;

    SUBCASE("h1 -> h3 -> h2 -> h4") {
        finish_handle.set_on_finish(
            [&finished](
                std::variant<std::tuple<int, int>, std::tuple<int, int>> r) {
                finished = true;
                CHECK(r.index() == 0);
                auto res = std::get<0>(r);
                CHECK(std::get<0>(res) == 2);
                CHECK(std::get<1>(res) == 3);
            });

        h1.finish(2);
        CHECK(!finished);

        h3.finish(4);
        CHECK(!finished);

        h2.finish(3);
        CHECK(!finished);
        CHECK(h3.cancelled_);
        CHECK(h4.cancelled_);

        h4.finish(1);
        CHECK(finished);
    }

    SUBCASE("h3 -> h2 -> h4 -> h1") {
        finish_handle.set_on_finish(
            [&finished](
                std::variant<std::tuple<int, int>, std::tuple<int, int>> r) {
                finished = true;
                CHECK(r.index() == 1);
                auto res = std::get<1>(r);
                CHECK(std::get<0>(res) == 4);
                CHECK(std::get<1>(res) == 1);
            });

        h3.finish(4);
        CHECK(!finished);

        h2.finish(3);
        CHECK(!finished);

        h4.finish(1);
        CHECK(!finished);
        CHECK(h1.cancelled_);
        CHECK(h2.cancelled_);

        h1.finish(2);
        CHECK(finished);
    }
}