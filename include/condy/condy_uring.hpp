#pragma once

#include <liburing.h> // IWYU pragma: export

// Earlier liburing has no version macros, define our own
#ifndef CONDY_IO_URING_VERSION_MAJOR
#define CONDY_IO_URING_VERSION_MAJOR 2
#endif

#ifndef CONDY_IO_URING_VERSION_MINOR
#define CONDY_IO_URING_VERSION_MINOR 2
#endif

#ifndef IO_URING_VERSION_MAJOR
#define IO_URING_VERSION_MAJOR CONDY_IO_URING_VERSION_MAJOR
#endif

#ifndef IO_URING_VERSION_MINOR
#define IO_URING_VERSION_MINOR CONDY_IO_URING_VERSION_MINOR
#endif

#ifndef IO_URING_CHECK_VERSION
#define IO_URING_CHECK_VERSION(major, minor)                                   \
    (major > IO_URING_VERSION_MAJOR ||                                         \
     (major == IO_URING_VERSION_MAJOR && minor > IO_URING_VERSION_MINOR))
#endif
