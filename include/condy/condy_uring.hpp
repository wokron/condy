#pragma once

#include <liburing.h> // IWYU pragma: export

// liburing <= 2.3 has no version macros, define them here

#ifndef IO_URING_VERSION_MAJOR
#define IO_URING_VERSION_MAJOR 2
#endif

#ifndef IO_URING_VERSION_MINOR
#define IO_URING_VERSION_MINOR 3
#endif

#ifndef IO_URING_CHECK_VERSION
#define IO_URING_CHECK_VERSION(major, minor)                                   \
    (major > IO_URING_VERSION_MAJOR ||                                         \
     (major == IO_URING_VERSION_MAJOR && minor > IO_URING_VERSION_MINOR))
#endif
