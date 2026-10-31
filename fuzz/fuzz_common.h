/* Shared helpers of the libFuzzer harnesses (see README.md). Not part of the library. */
#ifndef FUZZ_COMMON_H
#define FUZZ_COMMON_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* An oracle check failed: print what and abort, so that libFuzzer saves the input as a crash. */
#define FUZZ_CHECK(cond, ...)                                                        \
    do {                                                                             \
        if (!(cond)) {                                                               \
            fprintf(stderr, "ORACLE FAILED %s:%d: %s\n  ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                                            \
            fputc('\n', stderr);                                                     \
            abort();                                                                 \
        }                                                                            \
    } while (0)

/* A heap copy of exactly n bytes, so that AddressSanitizer sees any read or write past the end. */
static uint8_t* fuzz_exact_copy(const uint8_t* d, size_t n) {
    uint8_t* p = (uint8_t*)malloc(n ? n : 1);
    if (!p) abort();
    if (n) memcpy(p, d, n);
    return p;
}

/* Independent little-endian readers (the library's own are static; the oracle must not share them). */
static uint16_t fz_u16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t fz_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t fz_u64(const uint8_t* p) { return (uint64_t)fz_u32(p) | ((uint64_t)fz_u32(p + 4) << 32); }

/* Valid UTF-8 as RFC 3629 defines it (no overlong forms, no surrogates, nothing above U+10FFFF). */
static int fz_utf8_valid(const uint8_t* s, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint8_t a = s[i];
        if (a < 0x80) { i++; continue; }
        if (a >= 0xC2 && a <= 0xDF) {
            if (i + 1 >= n || (s[i + 1] & 0xC0) != 0x80) return 0;
            i += 2;
        } else if (a >= 0xE0 && a <= 0xEF) {
            if (i + 2 >= n || (s[i + 1] & 0xC0) != 0x80 || (s[i + 2] & 0xC0) != 0x80) return 0;
            if (a == 0xE0 && s[i + 1] < 0xA0) return 0;
            if (a == 0xED && s[i + 1] > 0x9F) return 0;
            i += 3;
        } else if (a >= 0xF0 && a <= 0xF4) {
            if (i + 3 >= n || (s[i + 1] & 0xC0) != 0x80 || (s[i + 2] & 0xC0) != 0x80 || (s[i + 3] & 0xC0) != 0x80) return 0;
            if (a == 0xF0 && s[i + 1] < 0x90) return 0;
            if (a == 0xF4 && s[i + 1] > 0x8F) return 0;
            i += 4;
        } else {
            return 0;
        }
    }
    return 1;
}

#endif
