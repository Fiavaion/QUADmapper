#pragma once

// Small platform shims shared by several tests.

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

// The id of this process, for unique temp names.
inline int testProcessId() {
#ifdef _WIN32
    return _getpid();
#else
    return static_cast<int>(getpid());
#endif
}
