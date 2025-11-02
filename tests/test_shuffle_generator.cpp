#include "condy/shuffle_generator.hpp"
#include <cstddef>
#include <cstdint>
#include <doctest/doctest.h>
#include <unordered_set>

TEST_CASE("test shuffle_generator - PCG32") {
    condy::PCG32 pcg(42);
    std::vector<uint32_t> results;
    const size_t N = 1000;
    for (size_t i = 0; i < N; ++i) {
        results.push_back(pcg.next());
    }

    std::unordered_set<uint32_t> unique_results(results.begin(), results.end());
    REQUIRE(unique_results.size() == N); // Check that all values are unique
}

TEST_CASE("test shuffle_generator - ShuffleGenerator") {
    const uint32_t num = 64;
    condy::ShuffleGenerator shuffler(num);

    std::vector<int> seen(num, 0);
    condy::PCG32 pcg(42);

    uint32_t r32 = pcg.next();
    shuffler.generate(r32, 0, num, [&](uint32_t pick) {
        seen[pick]++;
        return true; // Continue generating
    });

    for (uint32_t i = 0; i < num; ++i) {
        REQUIRE(seen[i] == 1); // Check that all indices were seen
    }
}

TEST_CASE("test shuffle_generator - ShuffleGenerator partial range") {
    const uint32_t num = 100;
    condy::ShuffleGenerator shuffler(num);

    std::vector<int> seen(num, 0);
    condy::PCG32 pcg(123);

    uint32_t r32 = pcg.next();
    uint32_t from = 20;
    uint32_t to = 50;
    shuffler.generate(r32, from, to, [&](uint32_t pick) {
        seen[pick]++;
        return true; // Continue generating
    });

    for (uint32_t i = 0; i < num; ++i) {
        if (i >= from && i < to) {
            REQUIRE(seen[i] == 1); // Check that indices in range were seen
        } else {
            REQUIRE(seen[i] == 0); // Check that indices out of range were not seen
        }
    }
}