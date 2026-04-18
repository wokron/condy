/**
 * @file awaiters.hpp
 * @brief Definitions of awaiter types for asynchronous operations.
 * @details This file defines a set of awaiter types used to represent and
 * manage asynchronous operations. These awaiters encapsulate the logic for
 * preparing, submitting, and resuming asynchronous tasks, and provide the
 * building blocks for composing complex asynchronous workflows.
 */

#pragma once

#include "condy/senders.hpp"

namespace condy {

/**
 * @brief Awaiter to wait for all operations in a range to complete.
 * @details An awaiter that waits for all operations in a range to complete.
 * Unlike @ref RangedWhenAllAwaiter, this awaiter will also return the order of
 * completion.
 * @tparam Awaiter The type of the awaiters in the range.
 * @return std::pair<std::vector<size_t>, std::vector<...>> A pair containing a
 * vector of completion orders and a vector of results from each awaiter.
 */
template <typename Awaiter>
using RangedParallelAllAwaiter = RangedParallelAllSender<Awaiter>;

/**
 * @brief Awaiter to wait for any operation in a range to complete.
 * @details An awaiter that waits for any operation in a range to complete.
 * Unlike @ref RangedWhenAnyAwaiter, this awaiter will return the order of
 * completion of all operations and the result of each awaiter.
 * @tparam Awaiter The type of the awaiters in the range.
 * @return std::pair<std::vector<size_t>, std::vector<...>> A pair containing a
 * vector of completion orders and a vector of results from each awaiter.
 */
template <typename Awaiter>
using RangedParallelAnyAwaiter = RangedParallelAnySender<Awaiter>;

/**
 * @brief Awaiter to wait for all operations in a range to complete.
 * @details An awaiter that waits for all operations in a range to complete.
 * @tparam Awaiter The type of the awaiters in the range.
 * @return std::vector<...> A vector of results from each awaiter.
 */
template <typename Awaiter>
using RangedWhenAllAwaiter = RangedWhenAllSender<Awaiter>;

/**
 * @brief Awaiter to wait for any operation in a range to complete.
 * @details An awaiter that waits for any operation in a range to complete.
 * @tparam Awaiter The type of the awaiters in the range.
 * @return std::pair<size_t, ...> A pair containing the index of the completed
 * awaiter and its result.
 */
template <typename Awaiter>
using RangedWhenAnyAwaiter = RangedWhenAnySender<Awaiter>;

/**
 * @brief Awaiter that links multiple operations in a range using IO_LINK.
 * @details An awaiter that links multiple operations in a range using IO_LINK.
 * @tparam Awaiter The type of the awaiters in the range.
 * @return std::vector<...> A vector of results from each awaiter.
 */
template <typename Awaiter> using RangedLinkAwaiter = RangedLinkSender<Awaiter>;

/**
 * @brief Awaiter that links multiple operations in a range using IO_HARDLINK.
 * @details An awaiter that links multiple operations in a range using
 * IO_HARDLINK.
 * @tparam Awaiter The type of the awaiters in the range.
 * @return std::vector<...> A vector of results from each awaiter.
 */
template <typename Awaiter>
using RangedHardLinkAwaiter = RangedHardLinkSender<Awaiter>;

/**
 * @brief Awaiter to wait for all operations to complete in parallel.
 * @details An awaiter that waits for all operations to complete in parallel.
 * Unlike @ref WhenAllAwaiter, this awaiter will also return the order of
 * completion.
 * @tparam Awaiter The types of the awaiters.
 * @return std::pair<std::array<size_t, N>, std::tuple<...>> A pair containing
 * an array of completion orders and a tuple of results from each awaiter.
 */
template <typename... Awaiter>
using ParallelAllAwaiter = ParallelAllSender<Awaiter...>;

/**
 * @brief Awaiter to wait for any operation to complete in parallel.
 * @details An awaiter that waits for any operation to complete in parallel.
 * Unlike @ref WhenAnyAwaiter, this awaiter will return the order of completion
 * of all operations and the result of each awaiter.
 * @tparam Awaiter The types of the awaiters.
 * @return std::pair<std::array<size_t, N>, std::tuple<...>> A pair containing
 * an array of completion orders and a tuple of results from each awaiter.
 */
template <typename... Awaiter>
using ParallelAnyAwaiter = ParallelAnySender<Awaiter...>;

/**
 * @brief Awaiter that waits for all operations to complete in parallel.
 * @details An awaiter that waits for all operations to complete in parallel.
 * @tparam Awaiter The types of the awaiters.
 * @return std::tuple<...> A tuple of results from each awaiter.
 */
template <typename... Awaiter> using WhenAllAwaiter = WhenAllSender<Awaiter...>;

/**
 * @brief Awaiter that waits for any operation to complete in parallel.
 * @details An awaiter that waits for any operation to complete in parallel.
 * @tparam Awaiter The types of the awaiters.
 * @return std::variant<...> A variant containing the result of the completed
 * awaiter.
 */
template <typename... Awaiter> using WhenAnyAwaiter = WhenAnySender<Awaiter...>;

/**
 * @brief Awaiter that links multiple operations using IO_LINK.
 * @details An awaiter that links multiple operations using IO_LINK.
 * @tparam Awaiter The types of the awaiters.
 * @return std::tuple<...> A tuple of results from each awaiter.
 */
template <typename... Awaiter> using LinkAwaiter = LinkSender<Awaiter...>;

/**
 * @brief Awaiter that links multiple operations using IO_HARDLINK.
 * @details An awaiter that links multiple operations using IO_HARDLINK.
 * @tparam Awaiter The types of the awaiters.
 * @return std::tuple<...> A tuple of results from each awaiter.
 */
template <typename... Awaiter>
using HardLinkAwaiter = HardLinkSender<Awaiter...>;

} // namespace condy