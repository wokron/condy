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
    REQUIRE(list.size() == 0);

    Item item1{1};
    Item item2{2};
    Item item3{3};

    list.push_back(&item1);
    list.push_back(&item2);
    list.push_back(&item3);

    REQUIRE(!list.empty());
    REQUIRE(list.size() == 3);

    Item *popped = list.pop_front();
    REQUIRE(popped->value == 1);
    REQUIRE(list.size() == 2);

    popped = list.pop_front();
    REQUIRE(popped->value == 2);
    REQUIRE(list.size() == 1);

    popped = list.pop_front();
    REQUIRE(popped->value == 3);
    REQUIRE(list.size() == 0);
    REQUIRE(list.empty());

    list.push_back(&item1);
    list.push_back(&item2);
    list.push_back(&item3);
    REQUIRE(list.size() == 3);

    auto batch1 = list.pop_front(0);
    REQUIRE(batch1.empty());
    REQUIRE(batch1.size() == 0);

    auto batch2 = list.pop_front(2);
    REQUIRE(!batch2.empty());
    REQUIRE(batch2.size() == 2);
    REQUIRE(batch2.pop_front()->value == 1);
    REQUIRE(batch2.pop_front()->value == 2);
    REQUIRE(batch2.pop_front() == nullptr);

    auto batch3 = list.pop_front(2);
    REQUIRE(!batch3.empty());
    REQUIRE(batch3.size() == 1);
    REQUIRE(batch3.pop_front()->value == 3);
    REQUIRE(batch3.pop_front() == nullptr);

    auto batch4 = list.pop_front(2);
    REQUIRE(batch4.empty());
    REQUIRE(batch4.size() == 0);

    IntrusiveSingleList<Item, &Item::link> list2;
    list2.push_back(&item1);
    list2.push_back(&item2);

    list.push_back(&item3);
    list.push_back(std::move(list2));
    REQUIRE(list2.empty());
    REQUIRE(list2.size() == 0);
    REQUIRE(!list.empty());
    REQUIRE(list.size() == 3);

    REQUIRE(list.pop_front()->value == 3);
    REQUIRE(list.pop_front()->value == 1);
    REQUIRE(list.pop_front()->value == 2);
    REQUIRE(list.pop_front() == nullptr);
    REQUIRE(list.empty());
    REQUIRE(list.size() == 0);
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

    int count = 0;
    list.for_each([&count](Item *item) {
        count++;
        REQUIRE(item->value == count);
    });

    Item *popped = list.pop_front();
    REQUIRE(popped->value == 1);

    list.remove(&item2);

    popped = list.pop_front();
    REQUIRE(popped->value == 3);

    list.remove(&item4);

    REQUIRE(list.empty());
}