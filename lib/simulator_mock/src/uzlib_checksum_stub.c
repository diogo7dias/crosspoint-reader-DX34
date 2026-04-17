// Minimal Adler32 + CRC32 implementations for the simulator build.
// On ESP32, these come from upstream uzlib's adler32.c/crc32.c, which are
// not compiled into the local lib/uzlib (they're linked separately). For
// the simulator we inline them here.
#include <stdint.h>
#include <stddef.h>

#define A32_BASE 65521
#define A32_NMAX 5552

uint32_t uzlib_adler32(const void *data, unsigned int length, uint32_t prev_sum) {
    const unsigned char *buf = (const unsigned char *)data;
    uint32_t s1 = prev_sum & 0xffff;
    uint32_t s2 = (prev_sum >> 16) & 0xffff;
    while (length > 0) {
        unsigned int k = length < A32_NMAX ? length : A32_NMAX;
        length -= k;
        while (k--) {
            s1 += *buf++;
            s2 += s1;
        }
        s1 %= A32_BASE;
        s2 %= A32_BASE;
    }
    return (s2 << 16) | s1;
}

static uint32_t crc32_table[256];
static int crc32_table_init = 0;
static void init_crc32_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) c = (c & 1) ? (0xedb88320 ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_table_init = 1;
}

uint32_t uzlib_crc32(const void *data, unsigned int length, uint32_t crc) {
    if (!crc32_table_init) init_crc32_table();
    const unsigned char *buf = (const unsigned char *)data;
    crc = ~crc;
    while (length--) crc = crc32_table[(crc ^ *buf++) & 0xff] ^ (crc >> 8);
    return ~crc;
}
