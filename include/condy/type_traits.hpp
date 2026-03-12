#pragma once

#include <utility>

namespace condy {

template <typename T>
constexpr bool is_nothrow_extract_result_v =
    noexcept(std::declval<T>().extract_result());

} // namespace condy