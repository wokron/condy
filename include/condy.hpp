/**
 * @file condy.hpp
 * @brief Main include file for the Condy library.
 * @details This file includes all the necessary headers for using the Condy
 * library. Users should include this file to access the full functionality of
 * Condy.
 */

#pragma once

#include "condy/async_operations.hpp"   // IWYU pragma: export
#include "condy/awaiter_operations.hpp" // IWYU pragma: export
#include "condy/buffers.hpp"            // IWYU pragma: export
#include "condy/channel.hpp"            // IWYU pragma: export
#include "condy/coro.hpp"               // IWYU pragma: export
#include "condy/runtime.hpp"            // IWYU pragma: export
#include "condy/runtime_options.hpp"    // IWYU pragma: export
#include "condy/sync_wait.hpp"          // IWYU pragma: export
#include "condy/task.hpp"               // IWYU pragma: export
#include "condy/version.hpp"            // IWYU pragma: export

/**
 * @brief The main namespace for the Condy library.
 * @details All classes, functions, and types provided by the Condy library
 * are encapsulated within this namespace.
 */
namespace condy {}