#include "condy/intrusive.hpp"
#include <doctest/doctest.h>
#include <utility>

TEST_CASE("test intrusive - single list") {
    using namespace condy;

    struct Item {
        int value;
        SingleLinkEntry link = {};
    };

    IntrusiveSingleList<Item, &Item::link> list;

    REQUIRE(list.empty());

    Item item1{1};
    Item item2{2};
    Item item3{3};

    list.push_back(&item1);
    list.push_back(&item2);
    list.push_back(&item3);

    REQUIRE(!list.empty());

    Item *popped = list.pop_front();
    REQUIRE(popped->value == 1);

    popped = list.pop_front();
    REQUIRE(popped->value == 2);

    popped = list.pop_front();
    REQUIRE(popped->value == 3);
    REQUIRE(list.empty());

    list.push_back(&item1);
    list.push_back(&item2);
    list.push_back(&item3);

    list.pop_front();
    list.pop_front();
    list.pop_front();

    IntrusiveSingleList<Item, &Item::link> list2;
    list2.push_back(&item1);
    list2.push_back(&item2);

    list.push_back(&item3);
    list.push_back(std::move(list2));
    REQUIRE(list2.empty()); // NOLINT(bugprone-use-after-move)
    REQUIRE(!list.empty());

    REQUIRE(list.pop_front()->value == 3);
    REQUIRE(list.pop_front()->value == 1);
    REQUIRE(list.pop_front()->value == 2);
    REQUIRE(list.pop_front() == nullptr);
    REQUIRE(list.empty());
}

TEST_CASE("test intrusive - double list") {
    using namespace condy;

    struct Item {
        int value;
        DoubleLinkEntry link = {};
    };

    IntrusiveDoubleList<Item, &Item::link> list;

    REQUIRE(list.empty());

    Item item1{1};
    Item item2{2};
    Item item3{3};
    Item item4{4};

    list.push_back(&item1);
    list.push_back(&item2);
    list.push_back(&item3);
    list.push_back(&item4);

    REQUIRE(!list.empty());

    Item *popped = list.pop_front();
    REQUIRE(popped->value == 1);

    list.remove(&item2);

    popped = list.pop_front();
    REQUIRE(popped->value == 3);

    list.remove(&item4);

    REQUIRE(list.empty());
}