#pragma once
#include <fcntl.h>
typedef int oflag_t;

// SdFat uses O_READ/O_WRITE — POSIX uses O_RDONLY/O_WRONLY. Alias so DX34
// code that mixes both compiles under the simulator.
#ifndef O_READ
#define O_READ O_RDONLY
#endif
#ifndef O_WRITE
#define O_WRITE O_WRONLY
#endif
