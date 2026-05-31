// Host shadow of SdFat's FsApiConstants — just the open-flag type + constants
// the storage interface references.
#pragma once

typedef int oflag_t;

#ifndef O_RDONLY
#define O_RDONLY 0x00
#endif
#ifndef O_WRONLY
#define O_WRONLY 0x01
#endif
#ifndef O_RDWR
#define O_RDWR 0x02
#endif
#ifndef O_CREAT
#define O_CREAT 0x0100
#endif
#ifndef O_TRUNC
#define O_TRUNC 0x0200
#endif
#ifndef O_APPEND
#define O_APPEND 0x0008
#endif
