/**
 * @file work_type.hpp
 */

#pragma once

#include <cassert>
#include <cstdint>
#include <utility>

namespace condy {

enum class WorkType : uint8_t {
    Common,
    Ignore,
    Schedule,
    Cancel,

    // Add new work types above this line
    WorkTypeMax,
};
static_assert(static_cast<uint8_t>(WorkType::WorkTypeMax) <= 8,
              "WorkType must fit in 3 bits");

inline std::pair<void *, WorkType> decode_work(void *ptr) noexcept {
    intptr_t mask = (1 << 3) - 1;
    intptr_t addr = reinterpret_cast<intptr_t>(ptr);
    WorkType type = static_cast<WorkType>(addr & mask);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    void *actual_ptr = reinterpret_cast<void *>(addr & (~mask));
    return std::make_pair(actual_ptr, type);
}

inline void *encode_work(void *ptr, WorkType type) noexcept {
    intptr_t addr = reinterpret_cast<intptr_t>(ptr);
    // Ensure align of 8
    assert(addr % 8 == 0);
    addr |= static_cast<intptr_t>(type);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<void *>(addr);
}

} // namespace condy