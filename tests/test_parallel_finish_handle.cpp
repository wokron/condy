#include <condy/finish_handles.hpp>
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

    void cancel() { cancelled_++; }

    std::function<void(int)> on_finish_ = nullptr;
    int cancelled_ = 0;
};

} // namespace

TEST_CASE("test parallel_finish_handle - RangedWaitAllFinishHandle finish") {
    SimpleFinishHandle h1, h2, h3;
    condy::RangedWaitAllFinishHandle<SimpleFinishHandle> finish_handle;
    finish_handle.init(std::vector<SimpleFinishHandle *>{&h1, &h2, &h3});

    bool finished = false;
    finish_handle.set_on_finish([&finished](std::vector<int> r) {
        finished = true;
        REQUIRE(r.size() == 3);
        REQUIRE(r[0] == 1);
        REQUIRE(r[1] == 2);
        REQUIRE(r[2] == 3);
    });

    h1.finish(1);
    REQUIRE(!finished);

    h2.finish(2);
    REQUIRE(!finished);

    h3.finish(3);
    REQUIRE(finished);
}

TEST_CASE("test parallel_finish_handle - RangedWaitAllFinishHandle cancel") {
    SimpleFinishHandle h1, h2, h3;
    condy::RangedWaitAllFinishHandle<SimpleFinishHandle> finish_handle;
    finish_handle.init(std::vector<SimpleFinishHandle *>{&h1, &h2, &h3});

    bool finished = false;
    finish_handle.set_on_finish(
        [&finished](std::vector<int> r) { finished = true; });

    h1.finish(1);
    REQUIRE(!finished);

    h2.finish(2);
    REQUIRE(!finished);

    finish_handle.cancel();
    REQUIRE(!finished);
    REQUIRE(h1.cancelled_);
    REQUIRE(h2.cancelled_);
    REQUIRE(h3.cancelled_);

    h3.finish(3);
    REQUIRE(finished);
}

TEST_CASE("test parallel_finish_handle - RangedWaitOneFinishHandle finish") {
    SimpleFinishHandle h1, h2, h3;
    condy::RangedWaitOneFinishHandle<SimpleFinishHandle> finish_handle;
    finish_handle.init(std::vector<SimpleFinishHandle *>{&h1, &h2, &h3});
    bool finished = false;

    SUBCASE("h1 finish first") {
        finish_handle.set_on_finish([&finished](std::pair<size_t, int> r) {
            finished = true;
            REQUIRE(r.first == 0);
            REQUIRE(r.second == 2);
        });

        h1.finish(2);
        h2.finish(3);
        h3.finish(1);
        REQUIRE(finished);
    }

    SUBCASE("h2 finish first") {
        finish_handle.set_on_finish([&finished](std::pair<size_t, int> r) {
            finished = true;
            REQUIRE(r.first == 1);
            REQUIRE(r.second == 3);
        });

        h2.finish(3);
        h3.finish(1);
        h1.finish(2);
        REQUIRE(finished);
    }

    SUBCASE("h3 finish first") {
        finish_handle.set_on_finish([&finished](std::pair<size_t, int> r) {
            finished = true;
            REQUIRE(r.first == 2);
            REQUIRE(r.second == 1);
        });

        h3.finish(1);
        h1.finish(2);
        h2.finish(3);
        REQUIRE(finished);
    }
}

TEST_CASE("test parallel_finish_handle - RangedWaitOneFinishHandle cancel") {
    SimpleFinishHandle h1, h2, h3;
    condy::RangedWaitOneFinishHandle<SimpleFinishHandle> finish_handle;
    finish_handle.init(std::vector<SimpleFinishHandle *>{&h1, &h2, &h3});
    bool finished = false;

    finish_handle.set_on_finish([&finished](std::pair<size_t, int> r) {
        finished = true;
        REQUIRE(r.first == 0);
        REQUIRE(r.second == 1);
    });

    finish_handle.cancel();
    REQUIRE(!finished);
    REQUIRE(h1.cancelled_);
    REQUIRE(h2.cancelled_);
    REQUIRE(h3.cancelled_);

    h1.finish(1);
    REQUIRE(!finished);

    h2.finish(2);
    REQUIRE(!finished);

    h3.finish(3);
    REQUIRE(finished);
}

TEST_CASE(
    "test parallel_finish_handle - RangedWaitOneFinishHandle multiple cancel") {
    SimpleFinishHandle h1, h2, h3;
    condy::RangedWaitOneFinishHandle<SimpleFinishHandle> finish_handle;
    finish_handle.init(std::vector<SimpleFinishHandle *>{&h1, &h2, &h3});
    bool finished = false;

    finish_handle.set_on_finish([&finished](std::pair<size_t, int> r) {
        finished = true;
        REQUIRE(r.first == 0);
        REQUIRE(r.second == 1);
    });

    h1.finish(1);
    REQUIRE(!finished);
    REQUIRE(h2.cancelled_ == 1);
    REQUIRE(h3.cancelled_ == 1);

    h3.finish(-1);
    REQUIRE(!finished);
    REQUIRE(h2.cancelled_ == 1); // should not increase

    finish_handle.cancel();
    REQUIRE(!finished);
    REQUIRE(h2.cancelled_ == 1); // should not increase

    h2.finish(-1);
    REQUIRE(finished);
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
            [&finished](std::pair<size_t, std::vector<int>>
                            r) { // idx: 0 for ab, 1 for cd
                finished = true;
                REQUIRE(r.first == 0);
                REQUIRE(r.second.size() == 2);
                REQUIRE(r.second[0] == 2);
                REQUIRE(r.second[1] == 3);
            });

        h1.finish(2);
        REQUIRE(!finished);

        h3.finish(4);
        REQUIRE(!finished);

        h2.finish(3);
        REQUIRE(!finished);
        REQUIRE(h3.cancelled_);
        REQUIRE(h4.cancelled_);

        h4.finish(1);
        REQUIRE(finished);
    }

    SUBCASE("h3 -> h2 -> h4 -> h1") {
        finish_handle.set_on_finish(
            [&finished](std::pair<size_t, std::vector<int>>
                            r) { // idx: 0 for ab, 1 for cd
                finished = true;
                REQUIRE(r.first == 1);
                REQUIRE(r.second.size() == 2);
                REQUIRE(r.second[0] == 4);
                REQUIRE(r.second[1] == 1);
            });

        h3.finish(4);
        REQUIRE(!finished);

        h2.finish(3);
        REQUIRE(!finished);

        h4.finish(1);
        REQUIRE(!finished);
        REQUIRE(h1.cancelled_);
        REQUIRE(h2.cancelled_);

        h1.finish(2);
        REQUIRE(finished);
    }
}

TEST_CASE("test parallel_finish_handle - Ranged (a || b) && (c || d)") {
    SimpleFinishHandle h1, h2, h3, h4;
    condy::RangedWaitOneFinishHandle<SimpleFinishHandle> finish_handle_ab;
    finish_handle_ab.init(std::vector<SimpleFinishHandle *>{&h1, &h2});
    condy::RangedWaitOneFinishHandle<SimpleFinishHandle> finish_handle_cd;
    finish_handle_cd.init(std::vector<SimpleFinishHandle *>{&h3, &h4});
    condy::RangedWaitAllFinishHandle<
        condy::RangedWaitOneFinishHandle<SimpleFinishHandle>>
        finish_handle;
    finish_handle.init(
        std::vector<condy::RangedWaitOneFinishHandle<SimpleFinishHandle> *>{
            &finish_handle_ab, &finish_handle_cd});

    bool finished = false;

    finish_handle.set_on_finish(
        [&finished](std::vector<std::pair<size_t, int>> r) {
            finished = true;
            REQUIRE(r.size() == 2);
            REQUIRE(r[0].first == 0); // from ab
            REQUIRE(r[0].second == 2);
            REQUIRE(r[1].first == 0); // from cd
            REQUIRE(r[1].second == 4);
        });

    h1.finish(2);
    REQUIRE(!finished);
    REQUIRE(h2.cancelled_);

    h2.finish(-1);
    REQUIRE(!finished);

    h3.finish(4);
    REQUIRE(!finished);
    REQUIRE(h4.cancelled_);

    h4.finish(-1);
    REQUIRE(finished);
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
        REQUIRE(std::get<0>(r) == 1);
        REQUIRE(std::get<1>(r) == 2);
        REQUIRE(std::get<2>(r) == 3);
    });

    h1.finish(1);
    REQUIRE(!finished);

    h2.finish(2);
    REQUIRE(!finished);

    h3.finish(3);
    REQUIRE(finished);
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
        REQUIRE(std::get<0>(r) == 1);
        REQUIRE(std::get<1>(r) == 2);
        REQUIRE(std::get<2>(r) == 3);
    });

    h1.finish(1);
    REQUIRE(!finished);

    h2.finish(2);
    REQUIRE(!finished);

    finish_handle.cancel();
    REQUIRE(!finished);
    REQUIRE(h1.cancelled_);
    REQUIRE(h2.cancelled_);
    REQUIRE(h3.cancelled_);

    h3.finish(3);
    REQUIRE(finished);
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
            REQUIRE(r.index() == 0);
            REQUIRE(std::get<0>(r) == 2);
        });

        h1.finish(2);
        h2.finish(3);
        h3.finish(1);
        REQUIRE(finished);
    }

    SUBCASE("h2 finish first") {
        finish_handle.set_on_finish([&finished](std::variant<int, int, int> r) {
            finished = true;
            REQUIRE(r.index() == 1);
            REQUIRE(std::get<1>(r) == 3);
        });

        h2.finish(3);
        h3.finish(1);
        h1.finish(2);
        REQUIRE(finished);
    }

    SUBCASE("h3 finish first") {
        finish_handle.set_on_finish([&finished](std::variant<int, int, int> r) {
            finished = true;
            REQUIRE(r.index() == 2);
            REQUIRE(std::get<2>(r) == 1);
        });

        h3.finish(1);
        h1.finish(2);
        h2.finish(3);
        REQUIRE(finished);
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
    REQUIRE(!finished);
    REQUIRE(h1.cancelled_);
    REQUIRE(h2.cancelled_);
    REQUIRE(h3.cancelled_);

    h1.finish(1);
    REQUIRE(!finished);

    h2.finish(2);
    REQUIRE(!finished);

    h3.finish(3);
    REQUIRE(finished);
}

TEST_CASE("test parallel_finish_handle - WaitOneFinishHandle multiple cancel") {
    SimpleFinishHandle h1, h2, h3;
    condy::WaitOneFinishHandle<SimpleFinishHandle, SimpleFinishHandle,
                               SimpleFinishHandle>
        finish_handle;
    finish_handle.init(&h1, &h2, &h3);
    bool finished = false;

    finish_handle.set_on_finish(
        [&finished](std::variant<int, int, int> r) { finished = true; });

    h1.finish(1);
    REQUIRE(!finished);
    REQUIRE(h2.cancelled_ == 1);
    REQUIRE(h3.cancelled_ == 1);

    h3.finish(-1);
    REQUIRE(!finished);
    REQUIRE(h2.cancelled_ == 1); // should not increase

    finish_handle.cancel();
    REQUIRE(!finished);
    REQUIRE(h2.cancelled_ == 1); // should not increase

    h2.finish(-1);
    REQUIRE(finished);
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
                REQUIRE(r.index() == 0);
                auto res = std::get<0>(r);
                REQUIRE(std::get<0>(res) == 2);
                REQUIRE(std::get<1>(res) == 3);
            });

        h1.finish(2);
        REQUIRE(!finished);

        h3.finish(4);
        REQUIRE(!finished);

        h2.finish(3);
        REQUIRE(!finished);
        REQUIRE(h3.cancelled_);
        REQUIRE(h4.cancelled_);

        h4.finish(1);
        REQUIRE(finished);
    }

    SUBCASE("h3 -> h2 -> h4 -> h1") {
        finish_handle.set_on_finish(
            [&finished](
                std::variant<std::tuple<int, int>, std::tuple<int, int>> r) {
                finished = true;
                REQUIRE(r.index() == 1);
                auto res = std::get<1>(r);
                REQUIRE(std::get<0>(res) == 4);
                REQUIRE(std::get<1>(res) == 1);
            });

        h3.finish(4);
        REQUIRE(!finished);

        h2.finish(3);
        REQUIRE(!finished);

        h4.finish(1);
        REQUIRE(!finished);
        REQUIRE(h1.cancelled_);
        REQUIRE(h2.cancelled_);

        h1.finish(2);
        REQUIRE(finished);
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

    bool finished = false;

    finish_handle.set_on_finish(
        [&finished](
            std::tuple<std::variant<int, int>, std::variant<int, int>> r) {
            finished = true;
            auto res_ab = std::get<0>(r);
            REQUIRE(res_ab.index() == 0);
            REQUIRE(std::get<0>(res_ab) == 2);
            auto res_cd = std::get<1>(r);
            REQUIRE(res_cd.index() == 0);
            REQUIRE(std::get<0>(res_cd) == 4);
        });

    h1.finish(2);
    REQUIRE(!finished);
    REQUIRE(h2.cancelled_);

    h2.finish(-1);
    REQUIRE(!finished);

    h3.finish(4);
    REQUIRE(!finished);
    REQUIRE(h4.cancelled_);

    h4.finish(-1);
    REQUIRE(finished);
}