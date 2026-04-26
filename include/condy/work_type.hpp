/**
 * @file work_type.hpp
 */

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>
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

namespace detail {

inline uintptr_t encode_work_ptr(uintptr_t ptr, WorkType type) noexcept {
    assert((ptr % 8) == 0);
    return ptr | static_cast<uintptr_t>(type);
}

} // namespace detail

inline std::pair<void *, WorkType> decode_work(uintptr_t ptr) noexcept {
    uintptr_t mask = (1 << 3) - 1;
    WorkType type = static_cast<WorkType>(ptr & mask);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    void *addr = reinterpret_cast<void *>(ptr & (~mask));
    return std::make_pair(addr, type);
}

template <typename T>
inline uintptr_t encode_work(T *ptr, WorkType type) noexcept {
    static_assert(std::alignment_of_v<T> >= 8,
                  "Pointer must be at least 8-byte aligned");
    return detail::encode_work_ptr(reinterpret_cast<uintptr_t>(ptr), type);
}

inline uintptr_t encode_work(std::nullptr_t, WorkType type) noexcept {
    return detail::encode_work_ptr(0, type);
}

} // namespace condy