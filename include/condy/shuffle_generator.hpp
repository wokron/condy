#pragma once

#include <cassert>
#include <cstdint>
#include <numeric>
#include <vector>

namespace condy {

// Adapted from Eigen's NonBlockingThreadPool
// https://gitlab.com/libeigen/eigen/-/blob/master/Eigen/src/ThreadPool/NonBlockingThreadPool.h

class PCG32 {
public:
    PCG32() = default;
    PCG32(uint64_t seed) : state_(seed) {}

    uint32_t next() {
        uint64_t current = state_;
        state_ = current * 6364136223846793005ULL + 0xda3e39cb94b95bdbULL;
        return static_cast<uint32_t>((current ^ (current >> 22)) >>
                                     (22 + (current >> 61)));
    }

private:
    uint64_t state_;
};

class ShuffleGenerator {
public:
    ShuffleGenerator(uint32_t num) : num_(num) { compute_coprimes_(); }

    template <typename Func>
    void generate(uint32_t r32, uint32_t from, uint32_t to, Func &&func) {
        assert(0 <= from && from < to && to <= num_);
        uint32_t size = to - from;
        // uint32_t victim = r32 % size
        uint32_t victim =
            (static_cast<uint64_t>(r32) * static_cast<uint64_t>(size)) >> 32;
        // uint32_t index = r32 % all_coprimes_[size - 1].size()
        uint32_t index =
            (static_cast<uint64_t>(r32) *
             static_cast<uint64_t>(all_coprimes_[size - 1].size())) >>
            32;
        uint32_t step = all_coprimes_[size - 1][index];
        for (uint32_t i = 0; i < size; ++i) {
            uint32_t pick = (victim + i * step) % size + from;
            // Do something with pick
            if (!func(pick)) {
                break;
            }
        }
    }

private:
    void compute_coprimes_() {
        all_coprimes_.reserve(num_);
        for (uint32_t n = 1; n <= num_; ++n) {
            auto &coprimes = all_coprimes_.emplace_back();
            for (uint32_t i = 1; i <= n; ++i) {
                if (std::gcd(i, n) == 1) {
                    coprimes.push_back(i);
                }
            }
        }
    }

private:
    uint32_t num_;
    std::vector<std::vector<uint32_t>> all_coprimes_;
};

} // namespace condy