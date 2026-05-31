// Shadow Logging.h for the host ZIP sim (see test_reader_sim/sim_shadows).
#pragma once

#include <cstdio>

#define LOG_ERR(tag, fmt, ...) fprintf(stderr, "[ERR][" tag "] " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(tag, fmt, ...) fprintf(stderr, "[WRN][" tag "] " fmt "\n", ##__VA_ARGS__)
#define LOG_INF(tag, fmt, ...) ((void)0)
#define LOG_DBG(tag, fmt, ...) ((void)0)
#define LOG_DIAG(tag, fmt, ...) ((void)0)
#define LOG_VERBOSE(tag, fmt, ...) ((void)0)
