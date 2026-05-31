// Shadow Logging.h for the host reader-sim. The real lib/Logging pulls
// Arduino/ESP serial machinery; here LOG_ERR/WARN go to stderr (so OOM
// diagnostics are visible during a sim run) and the chattier levels are
// compiled out.
#pragma once

#include <cstdio>

#define LOG_ERR(tag, fmt, ...) fprintf(stderr, "[ERR][" tag "] " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(tag, fmt, ...) fprintf(stderr, "[WRN][" tag "] " fmt "\n", ##__VA_ARGS__)
#define LOG_INF(tag, fmt, ...) ((void)0)
#define LOG_DBG(tag, fmt, ...) ((void)0)
#define LOG_DIAG(tag, fmt, ...) ((void)0)
#define LOG_VERBOSE(tag, fmt, ...) ((void)0)
