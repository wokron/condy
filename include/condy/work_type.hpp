#pragma once

#include <cassert>
#include <cstdint>
#include <utility>

namespace condy {

enum class WorkType : intptr_t {
    Common,
    Ignore,
    Notify,
    Schedule,
    MultiShot,
    ZeroCopy,
};

inline std::pair<void *, WorkType> decode_work(void *ptr) {
    intptr_t mask = (1 << 3) - 1;
    intptr_t addr = reinterpret_cast<intptr_t>(ptr);
    WorkType type = static_cast<WorkType>(addr & mask);
    void *actual_ptr = reinterpret_cast<void *>(addr & (~mask));
    return std::make_pair(actual_ptr, type);
}

inline void *encode_work(void *ptr, WorkType type) {
    intptr_t addr = reinterpret_cast<intptr_t>(ptr);
    // Ensure align of 8
    assert(addr % 8 == 0);
    addr |= static_cast<intptr_t>(type);
    return reinterpret_cast<void *>(addr);
}

} // namespace condy