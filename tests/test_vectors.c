/* Runs the shared link test vectors (tests/vectors/link, see the README there) against rewrite_link.h:
 * control lines, datagrams, pixel files and scripted sessions over real sockets.
 *
 * The vectors are derived from docs/CONTRACT.md of the rewrite-bridge repository alone and are the same
 * files the Rust, Java and C# implementations run, so a vector this file fails is a place where this
 * implementation and the contract (or another implementation) disagree. Every failing vector is listed
 * with its id, section and basis, and the run exits 1.
 *
 * KNOWN_DISAGREEMENTS names vectors this implementation is known to fail, with the reason. They are
 * expected to fail: a known vector that fails is reported and does not fail the run, a known vector that
 * passes does fail it, so the list cannot go stale.
 *
 *   make test                 (builds and runs it next to test_link)
 *   make asan                 (the same under AddressSanitizer and UndefinedBehaviorSanitizer)
 *
 * The vectors directory is $RBL_VECTORS_DIR, else tests/vectors/link or ../tests/vectors/link relative to
 * the working directory, else the compile-time RBL_VECTORS_DEFAULT_DIR (CMake sets it). With
 * RBL_VECTORS_SKIP_LARGE=1 the cases that need a sparse file of up to 1 GiB are skipped.
 *
 * Not checked here: that no file other than those SHA256SUMS lists sits in the directory (listing a
 * directory differs too much between POSIX and Windows for a test of this size).
 */
/* strict C99 hides POSIX (clock_gettime, nanosleep, mkfifo, ...): ask for it before the first system header */
#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#endif

#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REWRITE_LINK_IMPLEMENTATION
#include "../rewrite_link.h"

#if RBLI_WINDOWS
#include <winioctl.h>
#endif

/* The SHA-256 of tests/vectors/link/SHA256SUMS: the vectors can only change together with a deliberate
 * edit of this constant. To update the copy of the vectors: in the rewrite-bridge repository run
 *     scripts/sync-link-vectors.sh <path of this repository>
 * (it copies the directory and prints "VECTORS_DIGEST = <hex>"), paste the hex here, then run `make test`
 * and re-check KNOWN_DISAGREEMENTS. */
#define VECTORS_DIGEST "2cb6121126bd62ac753041c73a3ddb8d5c762c7adf0b749f7fa5672e22e90603"

/* Vectors this implementation fails: { id, why }, the list ends with { NULL, NULL }. */
static const struct { const char* id; const char* why; } KNOWN_DISAGREEMENTS[] = {
    { NULL, NULL }
};

/* ------------------------------------------------------------------ small utilities */

#if defined(__GNUC__)
#define PRINTF_LIKE(f, a) __attribute__((format(printf, f, a)))
#else
#define PRINTF_LIKE(f, a)
#endif

static void* xrealloc(void* p, size_t n) {
    void* q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "test_vectors: out of memory\n"); exit(2); }
    return q;
}
static char* xstrdup(const char* s) {
    size_t n = strlen(s);
    char* p = (char*)xrealloc(NULL, n + 1);
    memcpy(p, s, n + 1);
    return p;
}

/* a growing, always NUL terminated byte buffer */
typedef struct { char* p; size_t n, cap; } SB;

static void sb_reserve(SB* b, size_t extra) {
    if (b->n + extra + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 64;
        while (c < b->n + extra + 1) c *= 2;
        b->p = (char*)xrealloc(b->p, c);
        b->cap = c;
    }
}
static void sb_add(SB* b, const void* d, size_t n) {
    sb_reserve(b, n);
    if (n) memcpy(b->p + b->n, d, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void sb_puts(SB* b, const char* s) { sb_add(b, s, strlen(s)); }
static void sb_printf(SB* b, const char* fmt, ...) PRINTF_LIKE(2, 3);
static void sb_printf(SB* b, const char* fmt, ...) {
    char tmp[512];
    va_list ap, ap2;
    int n;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n >= 0 && (size_t)n < sizeof tmp) {
        sb_add(b, tmp, (size_t)n);
    } else if (n >= 0) {
        sb_reserve(b, (size_t)n);
        vsnprintf(b->p + b->n, (size_t)n + 1, fmt, ap2);
        b->n += (size_t)n;
    }
    va_end(ap2);
}
static void sb_free(SB* b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

/* The message of the check that failed last. A check returns 1 after failf(), 0 when it passed. */
static char g_err[2048];
static int failf(const char* fmt, ...) PRINTF_LIKE(1, 2);
static int failf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
    return 1;
}
/* failf with the message of an inner check in front: "<prefix>: <g_err>" */
static int failw(const char* prefix) {
    char inner[sizeof g_err];
    memcpy(inner, g_err, sizeof inner);
    return failf("%s: %s", prefix, inner);
}

static int g_format_errors; /* a vector file that does not have the shape the README promises */
static void format_error(const char* what, const char* key) {
    g_format_errors++;
    fprintf(stderr, "vector format error: %s \"%s\"\n", what, key);
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* Decodes n hex digits into a new buffer; returns NULL for odd or bad input. */
static uint8_t* hex_decode(const char* s, size_t n, size_t* out_len) {
    uint8_t* b;
    size_t i;
    if (n % 2) return NULL;
    b = (uint8_t*)xrealloc(NULL, n / 2 + 1);
    for (i = 0; i < n / 2; i++) {
        int hi = hexval(s[2 * i]), lo = hexval(s[2 * i + 1]);
        if (hi < 0 || lo < 0) { free(b); return NULL; }
        b[i] = (uint8_t)(hi * 16 + lo);
    }
    *out_len = n / 2;
    return b;
}
static void hex_encode(const uint8_t* b, size_t n, SB* out) {
    static const char d[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) { char c[2]; c[0] = d[b[i] >> 4]; c[1] = d[b[i] & 15]; sb_add(out, c, 2); }
}

static int f32_same(float a, float b) { return memcmp(&a, &b, sizeof a) == 0; }

/* ------------------------------------------------------------------ SHA-256 */

typedef struct { uint32_t h[8]; uint8_t buf[64]; uint64_t total; size_t fill; } Sha;

static uint32_t ror32(uint32_t x, unsigned n) { return (x >> n) | (x << (32u - n)); }

static void sha_block(Sha* s, const uint8_t* p) {
    static const uint32_t K[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u };
    uint32_t w[64], v[8], t1, t2;
    unsigned i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(v, s->h, sizeof v);
    for (i = 0; i < 64; i++) {
        t1 = v[7] + (ror32(v[4], 6) ^ ror32(v[4], 11) ^ ror32(v[4], 25)) + ((v[4] & v[5]) ^ (~v[4] & v[6])) + K[i] + w[i];
        t2 = (ror32(v[0], 2) ^ ror32(v[0], 13) ^ ror32(v[0], 22)) + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
        v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1; v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
    }
    for (i = 0; i < 8; i++) s->h[i] += v[i];
}
static void sha_init(Sha* s) {
    static const uint32_t H0[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
    memcpy(s->h, H0, sizeof H0);
    s->total = 0;
    s->fill = 0;
}
static void sha_update(Sha* s, const uint8_t* p, size_t n) {
    s->total += n;
    while (n > 0) {
        size_t k = 64 - s->fill < n ? 64 - s->fill : n;
        memcpy(s->buf + s->fill, p, k);
        s->fill += k; p += k; n -= k;
        if (s->fill == 64) { sha_block(s, s->buf); s->fill = 0; }
    }
}
/* hex: 64 digits and a NUL */
static void sha_final(Sha* s, char hex[65]) {
    static const char d[] = "0123456789abcdef";
    uint64_t bits = s->total * 8u;
    uint8_t pad = 0x80, zero = 0, len[8];
    int i;
    sha_update(s, &pad, 1);
    while (s->fill != 56) sha_update(s, &zero, 1);
    for (i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_update(s, len, 8);
    for (i = 0; i < 8; i++) {
        int k;
        for (k = 0; k < 8; k++) hex[8 * i + k] = d[(s->h[i] >> (28 - 4 * k)) & 15u];
    }
    hex[64] = 0;
}
static void sha256_hex(const uint8_t* p, size_t n, char hex[65]) {
    Sha s;
    sha_init(&s);
    sha_update(&s, p, n);
    sha_final(&s, hex);
}

/* ------------------------------------------------------------------ JSON */

typedef enum { J_NULL = 0, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JV JV;
struct JV {
    JType t;
    int b;               /* J_BOOL */
    int is_int;          /* J_NUM: written without fraction or exponent */
    double num;          /* J_NUM */
    char* s;             /* J_STR: NUL terminated, may hold NULs in between */
    size_t len;
    JV* v;               /* J_ARR items, J_OBJ values */
    char** k;            /* J_OBJ keys */
    size_t n, cap;
};

static void jv_free(JV* x) {
    size_t i;
    free(x->s);
    for (i = 0; i < x->n; i++) {
        jv_free(&x->v[i]);
        if (x->k) free(x->k[i]);
    }
    free(x->v);
    free(x->k);
    memset(x, 0, sizeof *x);
}

static JV jv_num(double d, int is_int) { JV x; memset(&x, 0, sizeof x); x.t = J_NUM; x.num = d; x.is_int = is_int; return x; }
static JV jv_bool(int b) { JV x; memset(&x, 0, sizeof x); x.t = J_BOOL; x.b = b != 0; return x; }
static JV jv_str(const char* s, size_t n) {
    JV x;
    memset(&x, 0, sizeof x);
    x.t = J_STR;
    x.s = (char*)xrealloc(NULL, n + 1);
    if (n) memcpy(x.s, s, n);
    x.s[n] = 0;
    x.len = n;
    return x;
}
static JV jv_cstr(const char* s) { return jv_str(s, strlen(s)); }
static JV jv_new(JType t) { JV x; memset(&x, 0, sizeof x); x.t = t; return x; }

static void jv_grow(JV* c) {
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 4;
        c->v = (JV*)xrealloc(c->v, c->cap * sizeof(JV));
        if (c->t == J_OBJ) c->k = (char**)xrealloc(c->k, c->cap * sizeof(char*));
    }
}
static void jv_push(JV* arr, JV item) { jv_grow(arr); arr->v[arr->n++] = item; }
static void jv_set(JV* obj, const char* key, JV val) {
    jv_grow(obj);
    obj->k[obj->n] = xstrdup(key);
    obj->v[obj->n++] = val;
}
static const JV* jv_get(const JV* o, const char* key) {
    size_t i;
    if (!o || o->t != J_OBJ) return NULL;
    for (i = 0; i < o->n; i++) if (strcmp(o->k[i], key) == 0) return &o->v[i];
    return NULL;
}

static int jv_eq(const JV* a, const JV* b) {
    size_t i;
    if (a->t != b->t) return 0;
    switch (a->t) {
    case J_NULL: return 1;
    case J_BOOL: return a->b == b->b;
    case J_NUM: return a->num == b->num && a->is_int == b->is_int;
    case J_STR: return a->len == b->len && memcmp(a->s, b->s, a->len) == 0;
    case J_ARR:
        if (a->n != b->n) return 0;
        for (i = 0; i < a->n; i++) if (!jv_eq(&a->v[i], &b->v[i])) return 0;
        return 1;
    case J_OBJ:
        if (a->n != b->n) return 0;
        for (i = 0; i < a->n; i++) {
            const JV* o = jv_get(b, a->k[i]);
            if (!o || !jv_eq(&a->v[i], o)) return 0;
        }
        return 1;
    }
    return 0;
}

static void jv_dump(const JV* x, SB* out) {
    size_t i, k;
    switch (x->t) {
    case J_NULL: sb_puts(out, "null"); break;
    case J_BOOL: sb_puts(out, x->b ? "true" : "false"); break;
    case J_NUM:
        if (x->is_int || x->num == floor(x->num)) sb_printf(out, "%.0f", x->num); else sb_printf(out, "%.9g", x->num);
        break;
    case J_STR:
        sb_puts(out, "\"");
        for (k = 0; k < x->len && k < 200; k++) {
            unsigned char c = (unsigned char)x->s[k];
            if (c == '"' || c == '\\') { sb_add(out, "\\", 1); sb_add(out, &c, 1); }
            else if (c < 0x20) sb_printf(out, "\\u%04x", (unsigned)c);
            else sb_add(out, &c, 1);
        }
        sb_puts(out, x->len > 200 ? "...\"" : "\"");
        break;
    case J_ARR:
        sb_puts(out, "[");
        for (i = 0; i < x->n && i < 20; i++) { if (i) sb_puts(out, ","); jv_dump(&x->v[i], out); }
        sb_puts(out, x->n > 20 ? ",...]" : "]");
        break;
    case J_OBJ:
        sb_puts(out, "{");
        for (i = 0; i < x->n; i++) {
            if (i) sb_puts(out, ",");
            sb_printf(out, "\"%s\":", x->k[i]);
            jv_dump(&x->v[i], out);
        }
        sb_puts(out, "}");
        break;
    }
}

typedef struct { const char* p; const char* end; const char* err; int depth; } JP;

static void jp_ws(JP* j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++;
}
static void sb_utf8(SB* b, uint32_t cp) {
    char c[4];
    size_t n;
    if (cp < 0x80) { c[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { c[0] = (char)(0xC0 | (cp >> 6)); c[1] = (char)(0x80 | (cp & 63)); n = 2; }
    else if (cp < 0x10000) {
        c[0] = (char)(0xE0 | (cp >> 12)); c[1] = (char)(0x80 | ((cp >> 6) & 63)); c[2] = (char)(0x80 | (cp & 63)); n = 3;
    } else {
        c[0] = (char)(0xF0 | (cp >> 18)); c[1] = (char)(0x80 | ((cp >> 12) & 63));
        c[2] = (char)(0x80 | ((cp >> 6) & 63)); c[3] = (char)(0x80 | (cp & 63)); n = 4;
    }
    sb_add(b, c, n);
}
static int jp_hex4(JP* j, uint32_t* out) {
    uint32_t v = 0;
    int i;
    if (j->end - j->p < 4) return -1;
    for (i = 0; i < 4; i++) {
        int h = hexval(j->p[i]);
        if (h < 0) return -1;
        v = v * 16u + (uint32_t)h;
    }
    j->p += 4;
    *out = v;
    return 0;
}
static int jp_fail(JP* j, const char* why) { if (!j->err) j->err = why; return -1; }

static int jp_string(JP* j, JV* out) {
    SB b;
    memset(&b, 0, sizeof b);
    sb_add(&b, "", 0);
    j->p++; /* the opening quote */
    for (;;) {
        unsigned char c;
        if (j->p >= j->end) { sb_free(&b); return jp_fail(j, "unterminated string"); }
        c = (unsigned char)*j->p++;
        if (c == '"') break;
        if (c < 0x20) { sb_free(&b); return jp_fail(j, "control character in string"); }
        if (c != '\\') { sb_add(&b, &c, 1); continue; }
        if (j->p >= j->end) { sb_free(&b); return jp_fail(j, "unterminated escape"); }
        c = (unsigned char)*j->p++;
        switch (c) {
        case '"': case '\\': case '/': sb_add(&b, &c, 1); break;
        case 'b': sb_add(&b, "\b", 1); break;
        case 'f': sb_add(&b, "\f", 1); break;
        case 'n': sb_add(&b, "\n", 1); break;
        case 'r': sb_add(&b, "\r", 1); break;
        case 't': sb_add(&b, "\t", 1); break;
        case 'u': {
            uint32_t u, lo;
            if (jp_hex4(j, &u) != 0) { sb_free(&b); return jp_fail(j, "bad \\u escape"); }
            if (u >= 0xD800u && u < 0xDC00u) {
                if (j->end - j->p < 6 || j->p[0] != '\\' || j->p[1] != 'u') { sb_free(&b); return jp_fail(j, "lone high surrogate"); }
                j->p += 2;
                if (jp_hex4(j, &lo) != 0 || lo < 0xDC00u || lo >= 0xE000u) { sb_free(&b); return jp_fail(j, "bad low surrogate"); }
                u = 0x10000u + ((u - 0xD800u) << 10) + (lo - 0xDC00u);
            } else if (u >= 0xDC00u && u < 0xE000u) {
                sb_free(&b);
                return jp_fail(j, "lone low surrogate");
            }
            sb_utf8(&b, u);
            break;
        }
        default: sb_free(&b); return jp_fail(j, "bad escape");
        }
    }
    memset(out, 0, sizeof *out);
    out->t = J_STR;
    out->s = b.p;
    out->len = b.n;
    return 0;
}

static int jp_value(JP* j, JV* out);

static int jp_number(JP* j, JV* out) {
    const char* s = j->p;
    char tmp[80];
    size_t n;
    int is_int = 1;
    if (j->p < j->end && *j->p == '-') j->p++;
    if (j->p >= j->end || *j->p < '0' || *j->p > '9') return jp_fail(j, "bad number");
    if (*j->p == '0') j->p++; else while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
    if (j->p < j->end && *j->p == '.') {
        is_int = 0;
        j->p++;
        if (j->p >= j->end || *j->p < '0' || *j->p > '9') return jp_fail(j, "bad number");
        while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
    }
    if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
        is_int = 0;
        j->p++;
        if (j->p < j->end && (*j->p == '+' || *j->p == '-')) j->p++;
        if (j->p >= j->end || *j->p < '0' || *j->p > '9') return jp_fail(j, "bad number");
        while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
    }
    n = (size_t)(j->p - s);
    if (n >= sizeof tmp) return jp_fail(j, "number too long");
    memcpy(tmp, s, n);
    tmp[n] = 0;
    *out = jv_num(strtod(tmp, NULL), is_int);
    return 0;
}

static int jp_literal(JP* j, const char* word, JV val, JV* out) {
    size_t n = strlen(word);
    if ((size_t)(j->end - j->p) < n || memcmp(j->p, word, n) != 0) return jp_fail(j, "bad literal");
    j->p += n;
    *out = val;
    return 0;
}

static int jp_value(JP* j, JV* out) {
    int rc;
    jp_ws(j);
    if (j->p >= j->end) return jp_fail(j, "unexpected end");
    if (j->depth > 64) return jp_fail(j, "nesting too deep");
    switch (*j->p) {
    case '"': return jp_string(j, out);
    case 't': return jp_literal(j, "true", jv_bool(1), out);
    case 'f': return jp_literal(j, "false", jv_bool(0), out);
    case 'n': return jp_literal(j, "null", jv_new(J_NULL), out);
    case '[': case '{': {
        int obj = *j->p == '{';
        char close = obj ? '}' : ']';
        *out = jv_new(obj ? J_OBJ : J_ARR);
        j->p++;
        j->depth++;
        jp_ws(j);
        if (j->p < j->end && *j->p == close) { j->p++; j->depth--; return 0; }
        for (;;) {
            JV item;
            char* key = NULL;
            jp_ws(j);
            if (obj) {
                JV ks;
                if (j->p >= j->end || *j->p != '"') { jv_free(out); return jp_fail(j, "object key expected"); }
                if (jp_string(j, &ks) != 0) { jv_free(out); return -1; }
                key = ks.s;
                jp_ws(j);
                if (j->p >= j->end || *j->p != ':') { free(key); jv_free(out); return jp_fail(j, "':' expected"); }
                j->p++;
            }
            rc = jp_value(j, &item);
            if (rc != 0) { free(key); jv_free(out); return -1; }
            if (obj) { jv_set(out, key, item); free(key); } else jv_push(out, item);
            jp_ws(j);
            if (j->p < j->end && *j->p == ',') { j->p++; continue; }
            if (j->p < j->end && *j->p == close) { j->p++; break; }
            jv_free(out);
            return jp_fail(j, "',' or closing bracket expected");
        }
        j->depth--;
        return 0;
    }
    default: return jp_number(j, out);
    }
}

/* Parses a whole text (one value, whitespace around it). Returns NULL, or the reason. */
static const char* json_parse(const char* text, size_t n, JV* out) {
    JP j;
    j.p = text; j.end = text + n; j.err = NULL; j.depth = 0;
    if (jp_value(&j, out) != 0) return j.err ? j.err : "parse error";
    jp_ws(&j);
    if (j.p != j.end) { jv_free(out); return "trailing characters"; }
    return NULL;
}

/* Typed access to the vector files. A missing key or a wrong type is a format error of the file (counted,
 * and the run fails), and gives a harmless default so that the run goes on. */
static const JV J_NONE = { J_NULL, 0, 0, 0.0, NULL, 0, NULL, NULL, 0, 0 };

static const JV* jreq(const JV* o, const char* key) {
    const JV* v = jv_get(o, key);
    if (!v) { format_error("missing key", key); return &J_NONE; }
    return v;
}
static const char* jstr(const JV* o, const char* key) {
    const JV* v = jreq(o, key);
    if (v->t != J_STR) { if (v != &J_NONE) format_error("not a string", key); return ""; }
    return v->s;
}
static const char* jstr_or(const JV* o, const char* key, const char* dflt) {
    const JV* v = jv_get(o, key);
    return v && v->t == J_STR ? v->s : dflt;
}
static int jis(const JV* o, const char* key, int dflt) { /* optional boolean */
    const JV* v = jv_get(o, key);
    return v && v->t == J_BOOL ? v->b : dflt;
}
static double jnum_of(const JV* v) {
    if (v->t != J_NUM) { format_error("not a number", "(value)"); return 0; }
    return v->num;
}
static uint32_t ju32(const JV* v) { return (uint32_t)jnum_of(v); }
static uint32_t ju32k(const JV* o, const char* key) { return ju32(jreq(o, key)); }
static uint64_t ju64(const JV* v) { /* a decimal string, or a number */
    uint64_t r = 0;
    size_t i;
    if (v->t == J_NUM) return (uint64_t)v->num;
    if (v->t != J_STR || v->len == 0) { format_error("not a u64", "(value)"); return 0; }
    for (i = 0; i < v->len; i++) {
        if (v->s[i] < '0' || v->s[i] > '9') { format_error("not a decimal u64", v->s); return 0; }
        r = r * 10u + (uint64_t)(v->s[i] - '0');
    }
    return r;
}
static uint64_t ju64k(const JV* o, const char* key) { return ju64(jreq(o, key)); }
static float jf32(const JV* v) { /* a number, or the string "inf" */
    if (v->t == J_STR && strcmp(v->s, "inf") == 0) return (float)HUGE_VAL;
    return (float)jnum_of(v);
}
static void jf32s(const JV* arr, float* out, size_t n, const char* what) {
    size_t i;
    if (arr->t != J_ARR || arr->n != n) { format_error("array of the wrong length", what); for (i = 0; i < n; i++) out[i] = 0; return; }
    for (i = 0; i < n; i++) out[i] = jf32(&arr->v[i]);
}
static void jf32sk(const JV* o, const char* key, float* out, size_t n) { jf32s(jreq(o, key), out, n, key); }

/* ------------------------------------------------------------------ files */

static int read_file(const char* path, uint8_t** out, size_t* len) {
    FILE* f = fopen(path, "rb");
    uint8_t* b = NULL;
    size_t n = 0, cap = 0;
    if (!f) return -1;
    for (;;) {
        size_t k;
        if (cap - n < 65536) { cap = cap ? cap * 2 : 131072; b = (uint8_t*)xrealloc(b, cap); }
        k = fread(b + n, 1, cap - n, f);
        n += k;
        if (k == 0) break;
    }
    fclose(f);
    *out = b;
    *len = n;
    return 0;
}

static char g_dir[1024]; /* the vectors directory */

static void vec_path(char* out, size_t cap, const char* rel) { snprintf(out, cap, "%s/%s", g_dir, rel); }

static int dir_has_vectors(const char* d) {
    char p[1100];
    FILE* f;
    snprintf(p, sizeof p, "%s/SHA256SUMS", d);
    f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int find_vectors_dir(void) {
    const char* env = getenv("RBL_VECTORS_DIR");
    const char* cand[4];
    int i, n = 0;
    if (env && env[0]) { snprintf(g_dir, sizeof g_dir, "%s", env); return dir_has_vectors(g_dir) ? 0 : -1; }
    cand[n++] = "tests/vectors/link";
    cand[n++] = "../tests/vectors/link";
#ifdef RBL_VECTORS_DEFAULT_DIR
    cand[n++] = RBL_VECTORS_DEFAULT_DIR;
#endif
    for (i = 0; i < n; i++) {
        if (dir_has_vectors(cand[i])) { snprintf(g_dir, sizeof g_dir, "%s", cand[i]); return 0; }
    }
    return -1;
}

static JV load_doc(const char* name) { /* the "cases" array of one vector file, with the file kept alive */
    char p[1100];
    uint8_t* data;
    size_t n;
    JV doc, cases;
    const char* err;
    const JV* f;
    memset(&cases, 0, sizeof cases);
    vec_path(p, sizeof p, name);
    if (read_file(p, &data, &n) != 0) { fprintf(stderr, "cannot read %s\n", p); g_format_errors++; return cases; }
    err = json_parse((const char*)data, n, &doc);
    free(data);
    if (err) { fprintf(stderr, "%s: %s\n", p, err); g_format_errors++; return cases; }
    f = jv_get(&doc, "format");
    if (!f || f->t != J_STR || strcmp(f->s, "rbl-link-vectors/1") != 0) {
        fprintf(stderr, "%s: unknown format\n", name);
        g_format_errors++;
    }
    f = jv_get(&doc, "cases");
    if (!f || f->t != J_ARR) { fprintf(stderr, "%s: no cases\n", name); g_format_errors++; jv_free(&doc); return cases; }
    /* take the array out of the document */
    cases = doc.v[(size_t)(f - doc.v)];
    memset(&doc.v[(size_t)(f - doc.v)], 0, sizeof(JV));
    jv_free(&doc);
    return cases;
}

/* ------------------------------------------------------------------ report */

typedef struct { char** v; size_t n, cap; } SL;

static void sl_add(SL* l, const char* s) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 8; l->v = (char**)xrealloc(l->v, l->cap * sizeof(char*)); }
    l->v[l->n++] = xstrdup(s);
}
static void sl_free(SL* l) { size_t i; for (i = 0; i < l->n; i++) free(l->v[i]); free(l->v); memset(l, 0, sizeof *l); }

typedef struct { size_t passed; SL skipped, failed, known; } Report;

static int g_failed_total;

static const char* known_why(const char* id) {
    size_t i;
    for (i = 0; KNOWN_DISAGREEMENTS[i].id; i++) if (strcmp(KNOWN_DISAGREEMENTS[i].id, id) == 0) return KNOWN_DISAGREEMENTS[i].why;
    return NULL;
}

/* rc: 0 passed, 1 failed (message in g_err), 2 skipped (reason in g_err) */
static void record(Report* r, const JV* c, int rc) {
    const char* id = jstr_or(c, "id", "?");
    const char* basis = jstr_or(c, "basis", "MUST");
    const char* section = jstr_or(c, "section", "");
    const char* why = known_why(id);
    SB m;
    memset(&m, 0, sizeof m);
    if (rc == 2) {
        sb_printf(&m, "%s (%s)", id, g_err);
        sl_add(&r->skipped, m.p);
    } else if (rc == 0) {
        if (!why) r->passed++;
        else { sb_printf(&m, "%s: now passes, remove it from KNOWN_DISAGREEMENTS (%s)", id, why); sl_add(&r->failed, m.p); }
    } else if (why) {
        sb_printf(&m, "%s: %s (known: %s)", id, g_err, why);
        sl_add(&r->known, m.p);
    } else {
        sb_printf(&m, "%s [section %s, %s]: %s", id, section, basis, g_err);
        sl_add(&r->failed, m.p);
    }
    sb_free(&m);
}

static void finish(Report* r, const char* what) {
    size_t i;
    printf("%s: %lu passed, %lu known disagreements, %lu skipped, %lu failed\n", what, (unsigned long)r->passed,
           (unsigned long)r->known.n, (unsigned long)r->skipped.n, (unsigned long)r->failed.n);
    for (i = 0; i < r->known.n; i++) printf("  known disagreement: %s\n", r->known.v[i]);
    for (i = 0; i < r->skipped.n; i++) printf("  skipped: %s\n", r->skipped.v[i]);
    for (i = 0; i < r->failed.n; i++) printf("  FAILED: %s\n", r->failed.v[i]);
    fflush(stdout);
    g_failed_total += (int)r->failed.n;
    sl_free(&r->known); sl_free(&r->skipped); sl_free(&r->failed);
}

/* ------------------------------------------------------------------ checksums */

static int check_checksums(void) {
    char p[1100], hex[65];
    uint8_t* sums;
    size_t n, i, listed = 0, line_start = 0;
    int bad = 0;
    vec_path(p, sizeof p, "SHA256SUMS");
    if (read_file(p, &sums, &n) != 0) return failf("cannot read %s", p);
    sha256_hex(sums, n, hex);
    if (strcmp(hex, VECTORS_DIGEST) != 0) {
        bad = failf("the SHA-256 of SHA256SUMS is %s, VECTORS_DIGEST in tests/test_vectors.c is %s "
                    "(the vectors changed without a deliberate edit of that constant)", hex, VECTORS_DIGEST);
    }
    for (i = 0; i <= n; i++) {
        if (i == n || sums[i] == '\n') {
            size_t len = i - line_start;
            const char* line = (const char*)sums + line_start;
            if (len > 0 && line[len - 1] == '\r') len--;
            if (len > 66 && line[64] == ' ' && line[65] == ' ') {
                char name[256], file[1300], want[65];
                uint8_t* data;
                size_t dn;
                if (len - 66 >= sizeof name) { bad = failf("a file name in SHA256SUMS is too long"); }
                else {
                    memcpy(name, line + 66, len - 66); name[len - 66] = 0;
                    memcpy(want, line, 64); want[64] = 0;
                    vec_path(file, sizeof file, name);
                    if (read_file(file, &data, &dn) != 0) { if (!bad) bad = failf("%s: cannot read", name); }
                    else {
                        sha256_hex(data, dn, hex);
                        free(data);
                        if (strcmp(hex, want) != 0 && !bad) bad = failf("%s differs from SHA256SUMS", name);
                    }
                    listed++;
                }
            } else if (len > 0) {
                bad = failf("a line of SHA256SUMS is not in sha256sum format");
            }
            line_start = i + 1;
        }
    }
    free(sums);
    if (listed < 10 && !bad) bad = failf("SHA256SUMS lists too little (%lu files)", (unsigned long)listed);
    return bad;
}

static void test_sha256_known_answers(Report* r) {
    char hex[65];
    JV c = jv_new(J_OBJ);
    jv_set(&c, "id", jv_cstr("runner.sha256_known_answers"));
    sha256_hex((const uint8_t*)"", 0, hex);
    if (strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") != 0) { record(r, &c, failf("empty input gave %s", hex)); jv_free(&c); return; }
    sha256_hex((const uint8_t*)"abc", 3, hex);
    if (strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") != 0) { record(r, &c, failf("abc gave %s", hex)); jv_free(&c); return; }
    {   /* a message that spans two blocks, and the million-a-less test: 112 bytes */
        const char* m = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
        sha256_hex((const uint8_t*)m, strlen(m), hex);
        if (strcmp(hex, "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1") != 0) { record(r, &c, failf("112 bytes gave %s", hex)); jv_free(&c); return; }
    }
    record(r, &c, 0);
    jv_free(&c);
}

/* ------------------------------------------------------------------ control.json */

static rbl_role role_of(const JV* v) { return v->t == J_STR && strcmp(v->s, "host") == 0 ? RBL_ROLE_HOST : RBL_ROLE_GUEST; }
static const char* role_name(rbl_role r) { return r == RBL_ROLE_HOST ? "host" : "guest"; }

static rbl_str rstr(const JV* v) {
    rbl_str s;
    if (v->t == J_STR) { s.ptr = v->s; s.len = v->len; } else { s.ptr = ""; s.len = 0; }
    return s;
}
static JV jv_rstr(rbl_str s) { return jv_str(s.ptr ? s.ptr : "", s.ptr ? s.len : 0); }

/* The control message `m` (the object of a vector) as an rbl_control; the strings point into m. */
static void control_from_jv(const JV* m, rbl_control* c) {
    const char* type = jstr(m, "type");
    const JV* o;
    memset(c, 0, sizeof *c);
    if (strcmp(type, "hello") == 0) {
        c->type = RBL_CTL_HELLO;
        c->u.hello.role = role_of(jreq(m, "role"));
        c->u.hello.game = rstr(jreq(m, "game"));
        c->u.hello.protocol = ju32k(m, "protocol");
        c->u.hello.min_protocol = ju32k(m, "min_protocol");
        c->u.hello.udp_port = ju32k(m, "udp_port");
        c->u.hello.heartbeat_ms = ju32k(m, "heartbeat_ms");
        c->u.hello.timeout_ms = ju32k(m, "timeout_ms");
        if ((o = jv_get(m, "session")) != NULL) { c->u.hello.has_session = 1; c->u.hello.session = ju32(o); }
    } else if (strcmp(type, "spawn") == 0) {
        c->type = RBL_CTL_SPAWN;
        c->u.spawn.entity = ju32k(m, "entity");
        c->u.spawn.provider = rstr(jreq(m, "provider"));
        c->u.spawn.asset = rstr(jreq(m, "asset"));
        if ((o = jv_get(m, "name")) != NULL) { c->u.spawn.has_name = 1; c->u.spawn.name = rstr(o); }
    } else if (strcmp(type, "despawn") == 0) {
        c->type = RBL_CTL_DESPAWN;
        c->u.despawn.entity = ju32k(m, "entity");
    } else if (strcmp(type, "input") == 0) {
        c->type = RBL_CTL_INPUT;
        c->u.input.focus = role_of(jreq(m, "focus"));
        if ((o = jv_get(m, "reason")) != NULL) { c->u.input.has_reason = 1; c->u.input.reason = rstr(o); }
    } else if (strcmp(type, "frame_channel") == 0) {
        c->type = RBL_CTL_FRAME_CHANNEL;
        c->u.frame_channel.path = rstr(jreq(m, "path"));
        c->u.frame_channel.width = ju32k(m, "width");
        c->u.frame_channel.height = ju32k(m, "height");
        c->u.frame_channel.slots = ju32k(m, "slots");
    } else if (strcmp(type, "bye") == 0) {
        c->type = RBL_CTL_BYE;
        c->u.bye.reason = rstr(jreq(m, "reason"));
    } else {
        format_error("control message of unknown type", type);
    }
}

static JV control_to_jv(const rbl_control* c) {
    JV o = jv_new(J_OBJ);
    switch (c->type) {
    case RBL_CTL_HELLO:
        jv_set(&o, "type", jv_cstr("hello"));
        jv_set(&o, "role", jv_cstr(role_name(c->u.hello.role)));
        jv_set(&o, "game", jv_rstr(c->u.hello.game));
        jv_set(&o, "protocol", jv_num(c->u.hello.protocol, 1));
        jv_set(&o, "min_protocol", jv_num(c->u.hello.min_protocol, 1));
        jv_set(&o, "udp_port", jv_num(c->u.hello.udp_port, 1));
        jv_set(&o, "heartbeat_ms", jv_num(c->u.hello.heartbeat_ms, 1));
        jv_set(&o, "timeout_ms", jv_num(c->u.hello.timeout_ms, 1));
        if (c->u.hello.has_session) jv_set(&o, "session", jv_num(c->u.hello.session, 1));
        break;
    case RBL_CTL_SPAWN:
        jv_set(&o, "type", jv_cstr("spawn"));
        jv_set(&o, "entity", jv_num(c->u.spawn.entity, 1));
        jv_set(&o, "provider", jv_rstr(c->u.spawn.provider));
        jv_set(&o, "asset", jv_rstr(c->u.spawn.asset));
        if (c->u.spawn.has_name) jv_set(&o, "name", jv_rstr(c->u.spawn.name));
        break;
    case RBL_CTL_DESPAWN:
        jv_set(&o, "type", jv_cstr("despawn"));
        jv_set(&o, "entity", jv_num(c->u.despawn.entity, 1));
        break;
    case RBL_CTL_INPUT:
        jv_set(&o, "type", jv_cstr("input"));
        jv_set(&o, "focus", jv_cstr(role_name(c->u.input.focus)));
        if (c->u.input.has_reason) jv_set(&o, "reason", jv_rstr(c->u.input.reason));
        break;
    case RBL_CTL_FRAME_CHANNEL:
        jv_set(&o, "type", jv_cstr("frame_channel"));
        jv_set(&o, "path", jv_rstr(c->u.frame_channel.path));
        jv_set(&o, "width", jv_num(c->u.frame_channel.width, 1));
        jv_set(&o, "height", jv_num(c->u.frame_channel.height, 1));
        jv_set(&o, "slots", jv_num(c->u.frame_channel.slots, 1));
        break;
    case RBL_CTL_BYE:
        jv_set(&o, "type", jv_cstr("bye"));
        jv_set(&o, "reason", jv_rstr(c->u.bye.reason));
        break;
    default:
        jv_set(&o, "type", jv_rstr(c->unknown_type));
        break;
    }
    return o;
}

static int check_control(const JV* c) {
    const JV* lj = jv_get(c, "line");
    const char* expect = jstr(c, "expect");
    uint8_t* raw = NULL;
    size_t rawn = 0;
    char* buf;
    rbl_control ctl;
    const char* why = NULL;
    int rc, ret = 0;
    SB msg;
    memset(&msg, 0, sizeof msg);
    if (lj && lj->t == J_STR) {
        raw = (uint8_t*)xrealloc(NULL, lj->len + 1);
        memcpy(raw, lj->s, lj->len);
        rawn = lj->len;
    } else {
        const char* h = jstr(c, "line_hex");
        raw = hex_decode(h, strlen(h), &rawn);
        if (!raw) { format_error("bad hex in", "line_hex"); return failf("bad vector"); }
    }
    if (!rbli_utf8_valid(raw, rawn)) {
        /* the link refuses such a line before it parses it (rbli_ctl_take_line); sessions.json checks that end to end */
        free(raw);
        return strcmp(expect, "reject") == 0 ? 0 : failf("the line is not UTF-8 but the vector expects %s", expect);
    }
    buf = (char*)xrealloc(NULL, rawn + 1);
    memcpy(buf, raw, rawn);
    buf[rawn] = 0;
    free(raw);
    rc = rbl_control_parse(buf, rawn, &ctl, &why);
    if (strcmp(expect, "reject") == 0) {
        if (rc != RBL_OK) ret = 0;
        else {
            JV got = control_to_jv(&ctl);
            jv_dump(&got, &msg);
            ret = failf("accepted the line as %s (%s)", ctl.type == RBL_CTL_UNKNOWN ? "an unknown type" : "a message", msg.p);
            jv_free(&got);
        }
    } else if (strcmp(expect, "unknown") == 0) {
        const char* t = jstr(c, "type");
        if (rc != RBL_OK) ret = failf("rejected (%s), expected the unknown type %s", why ? why : "?", t);
        else if (ctl.type != RBL_CTL_UNKNOWN) ret = failf("parsed as a known message, expected the unknown type %s", t);
        else if (ctl.unknown_type.len != strlen(t) || memcmp(ctl.unknown_type.ptr, t, ctl.unknown_type.len) != 0)
            ret = failf("unknown type reported as a different string, expected %s", t);
    } else if (strcmp(expect, "message") == 0) {
        const JV* want = jreq(c, "message");
        if (rc != RBL_OK) ret = failf("rejected (%s)", why ? why : "?");
        else if (ctl.type == RBL_CTL_UNKNOWN) ret = failf("parsed as an unknown type");
        else {
            JV got = control_to_jv(&ctl);
            if (!jv_eq(&got, want)) {
                jv_dump(&got, &msg);
                sb_puts(&msg, ", expected ");
                jv_dump(want, &msg);
                ret = failf("parsed as %s", msg.p);
            }
            jv_free(&got);
        }
        if (!ret && jis(c, "encode", 0)) {
            rbl_control src;
            size_t cap = 300000;
            char* out = (char*)xrealloc(NULL, cap);
            int n;
            control_from_jv(want, &src);
            n = rbl_control_format(&src, out, cap);
            if (n <= 0) ret = failf("encode failed: %s", rbl_strerror(n));
            else if (out[n - 1] != '\n' || memchr(out, '\n', (size_t)n - 1) || memchr(out, '\r', (size_t)n - 1))
                ret = failf("the encoded line is not a single line ending in a line feed");
            else {
                JV back;
                const char* perr = json_parse(out, (size_t)n - 1, &back);
                if (perr) ret = failf("the encoded line is not JSON (%s)", perr);
                else {
                    if (!jv_eq(&back, want)) {
                        SB d;
                        memset(&d, 0, sizeof d);
                        jv_dump(&back, &d);
                        sb_puts(&d, ", expected ");
                        jv_dump(want, &d);
                        ret = failf("encoded %s", d.p);
                        sb_free(&d);
                    }
                    jv_free(&back);
                }
            }
            free(out);
        }
    } else {
        format_error("unknown expect", expect);
        ret = failf("bad vector");
    }
    sb_free(&msg);
    free(buf);
    return ret;
}

static void test_control(const JV* cases) {
    Report r;
    size_t i;
    memset(&r, 0, sizeof r);
    for (i = 0; i < cases->n; i++) record(&r, &cases->v[i], check_control(&cases->v[i]));
    finish(&r, "control.json");
}

/* ------------------------------------------------------------------ datagrams.json */

static rbl_header header_of(const JV* h) {
    rbl_header x;
    memset(&x, 0, sizeof x);
    x.magic = RBL_MAGIC;
    x.version = (uint16_t)ju32k(h, "version");
    x.flags = jv_get(h, "flags") ? (uint8_t)ju32k(h, "flags") : 0;
    x.session = ju32k(h, "session");
    x.seq = ju32k(h, "seq");
    x.time_us = ju64k(h, "time_us");
    return x;
}
static void camera_of(const JV* b, rbl_camera* c) {
    memset(c, 0, sizeof *c);
    jf32sk(b, "view", c->view, 16);
    jf32sk(b, "proj", c->proj, 16);
    c->fov_y = jf32(jreq(b, "fov_y"));
    c->near_m = jf32(jreq(b, "near"));
    c->far_m = jf32(jreq(b, "far"));
    c->viewport_width = ju32k(b, "viewport_width");
    c->viewport_height = ju32k(b, "viewport_height");
    c->depth_range = ju32k(b, "depth_range");
}
static void pose_of(const JV* p, rbl_pose* o) {
    memset(o, 0, sizeof *o);
    o->entity = ju32k(p, "entity");
    o->flags = ju32k(p, "flags");
    o->time_us = ju64k(p, "time_us");
    jf32sk(p, "translation", o->translation, 3);
    jf32sk(p, "rotation", o->rotation, 4);
    jf32sk(p, "scale", o->scale, 3);
    jf32sk(p, "velocity", o->velocity, 3);
    jf32sk(p, "angular_velocity", o->angular_velocity, 3);
}
static void frame_of(const JV* b, rbl_frame* f) {
    f->frame = ju64k(b, "frame");
    f->slot = ju32k(b, "slot");
    f->camera_seq = ju32k(b, "camera_seq");
    f->width = ju32k(b, "width");
    f->height = ju32k(b, "height");
}
static void collision_of(const JV* b, rbl_collision* c) {
    memset(c, 0, sizeof *c);
    c->query = ju32k(b, "query");
    c->chunk_index = (uint16_t)ju32k(b, "chunk_index");
    c->chunk_count = (uint16_t)ju32k(b, "chunk_count");
    jf32sk(b, "center", c->center, 3);
    c->radius = jf32(jreq(b, "radius"));
}
/* the triangles of a collision body, into a new array (NULL for none); *n gets the count */
static rbl_triangle* triangles_of(const JV* b, size_t* n) {
    const JV* a = jreq(b, "triangles");
    rbl_triangle* t;
    size_t i;
    *n = a->t == J_ARR ? a->n : 0;
    t = (rbl_triangle*)xrealloc(NULL, (*n + 1) * sizeof *t);
    for (i = 0; i < *n; i++) jf32s(&a->v[i], &t[i].v[0][0], 9, "triangle");
    return t;
}

static int floats_differ(const char* what, const float* got, const float* want, size_t n) {
    size_t i;
    for (i = 0; i < n; i++)
        if (!f32_same(got[i], want[i])) return failf("%s[%lu] is %.9g, expected %.9g", what, (unsigned long)i, (double)got[i], (double)want[i]);
    return 0;
}

static int check_decoded_body(const rbl_decoded* d, const JV* body) {
    const char* kind = jstr(body, "kind");
    if (strcmp(kind, "heartbeat") == 0) return d->header.kind == RBL_HEARTBEAT ? 0 : failf("kind is %u, expected heartbeat", d->header.kind);
    if (strcmp(kind, "camera") == 0) {
        rbl_camera w;
        if (d->header.kind != RBL_CAMERA) return failf("kind is %u, expected camera", d->header.kind);
        camera_of(body, &w);
        if (floats_differ("view", d->u.camera.view, w.view, 16) || floats_differ("proj", d->u.camera.proj, w.proj, 16)) return 1;
        if (!f32_same(d->u.camera.fov_y, w.fov_y)) return failf("fov_y is %.9g, expected %.9g", (double)d->u.camera.fov_y, (double)w.fov_y);
        if (!f32_same(d->u.camera.near_m, w.near_m)) return failf("near is %.9g, expected %.9g", (double)d->u.camera.near_m, (double)w.near_m);
        if (!f32_same(d->u.camera.far_m, w.far_m)) return failf("far is %.9g, expected %.9g", (double)d->u.camera.far_m, (double)w.far_m);
        if (d->u.camera.viewport_width != w.viewport_width || d->u.camera.viewport_height != w.viewport_height)
            return failf("viewport is %ux%u, expected %ux%u", d->u.camera.viewport_width, d->u.camera.viewport_height, w.viewport_width, w.viewport_height);
        if (d->u.camera.depth_range != w.depth_range) return failf("depth_range is %u, expected %u", d->u.camera.depth_range, w.depth_range);
        return 0;
    }
    if (strcmp(kind, "pose") == 0) {
        const JV* ps = jreq(body, "poses");
        size_t i;
        if (d->header.kind != RBL_POSE) return failf("kind is %u, expected pose", d->header.kind);
        if (d->u.poses.count != ps->n) return failf("%u pose records, expected %lu", d->u.poses.count, (unsigned long)ps->n);
        for (i = 0; i < ps->n; i++) {
            rbl_pose g, w;
            rbl_wire_pose(d, (uint32_t)i, &g);
            pose_of(&ps->v[i], &w);
            if (g.entity != w.entity || g.flags != w.flags || g.time_us != w.time_us)
                return failf("pose %lu: entity/flags/time_us are %u/%u/%llu, expected %u/%u/%llu", (unsigned long)i, g.entity, g.flags,
                             (unsigned long long)g.time_us, w.entity, w.flags, (unsigned long long)w.time_us);
            if (floats_differ("translation", g.translation, w.translation, 3) || floats_differ("rotation", g.rotation, w.rotation, 4) ||
                floats_differ("scale", g.scale, w.scale, 3) || floats_differ("velocity", g.velocity, w.velocity, 3) ||
                floats_differ("angular_velocity", g.angular_velocity, w.angular_velocity, 3))
                return failw("pose record");
        }
        return 0;
    }
    if (strcmp(kind, "collision") == 0) {
        rbl_collision w;
        size_t n, i;
        rbl_triangle* t;
        int ret = 0;
        if (d->header.kind != RBL_COLLISION) return failf("kind is %u, expected collision", d->header.kind);
        collision_of(body, &w);
        t = triangles_of(body, &n);
        if (d->u.collision.query != w.query || d->u.collision.chunk_index != w.chunk_index || d->u.collision.chunk_count != w.chunk_count)
            ret = failf("query/chunk_index/chunk_count are %u/%u/%u, expected %u/%u/%u", d->u.collision.query, d->u.collision.chunk_index,
                        d->u.collision.chunk_count, w.query, w.chunk_index, w.chunk_count);
        else if (floats_differ("center", d->u.collision.center, w.center, 3)) ret = 1;
        else if (!f32_same(d->u.collision.radius, w.radius)) ret = failf("radius is %.9g, expected %.9g", (double)d->u.collision.radius, (double)w.radius);
        else if (d->u.collision.triangle_count != n) ret = failf("%u triangles, expected %lu", d->u.collision.triangle_count, (unsigned long)n);
        else {
            for (i = 0; i < n && !ret; i++) {
                rbl_triangle g;
                rbl_wire_triangle(d, (uint32_t)i, &g);
                if (floats_differ("triangle", &g.v[0][0], &t[i].v[0][0], 9)) ret = failw("triangle");
            }
        }
        free(t);
        return ret;
    }
    if (strcmp(kind, "frame") == 0) {
        rbl_frame w;
        if (d->header.kind != RBL_FRAME) return failf("kind is %u, expected frame", d->header.kind);
        frame_of(body, &w);
        if (d->u.frame.frame != w.frame || d->u.frame.slot != w.slot || d->u.frame.camera_seq != w.camera_seq ||
            d->u.frame.width != w.width || d->u.frame.height != w.height)
            return failf("frame/slot/camera_seq/width/height are %llu/%u/%u/%u/%u, expected %llu/%u/%u/%u/%u",
                         (unsigned long long)d->u.frame.frame, d->u.frame.slot, d->u.frame.camera_seq, d->u.frame.width, d->u.frame.height,
                         (unsigned long long)w.frame, w.slot, w.camera_seq, w.width, w.height);
        return 0;
    }
    format_error("unknown body kind", kind);
    return failf("bad vector");
}

/* encodes header + body into out; returns the length or a negative status */
static int encode_body(const rbl_header* h, const JV* body, uint8_t* out, size_t cap) {
    const char* kind = jstr(body, "kind");
    if (strcmp(kind, "heartbeat") == 0) return rbl_wire_encode_heartbeat(h, out, cap);
    if (strcmp(kind, "camera") == 0) {
        rbl_camera c;
        camera_of(body, &c);
        return rbl_wire_encode_camera(h, &c, out, cap);
    }
    if (strcmp(kind, "pose") == 0) {
        const JV* ps = jreq(body, "poses");
        rbl_pose* p = (rbl_pose*)xrealloc(NULL, (ps->n + 1) * sizeof *p);
        size_t i;
        int n;
        for (i = 0; i < ps->n; i++) pose_of(&ps->v[i], &p[i]);
        n = rbl_wire_encode_poses(h, p, ps->n, out, cap);
        free(p);
        return n;
    }
    if (strcmp(kind, "collision") == 0) {
        rbl_collision c;
        size_t n;
        rbl_triangle* t;
        int r;
        collision_of(body, &c);
        t = triangles_of(body, &n);
        r = rbl_wire_encode_collision(h, &c, t, n, out, cap);
        free(t);
        return r;
    }
    if (strcmp(kind, "frame") == 0) {
        rbl_frame f;
        frame_of(body, &f);
        return rbl_wire_encode_frame(h, &f, out, cap);
    }
    format_error("unknown body kind", kind);
    return RBL_ERR_ARGS;
}

static int check_datagram(const JV* c) {
    const char* hexs = jstr(c, "hex");
    const char* expect = jstr(c, "expect");
    size_t n = 0;
    uint8_t* bytes = hex_decode(hexs, strlen(hexs), &n);
    rbl_decoded d;
    int rc, ret = 0;
    if (!bytes) { format_error("bad hex in", "hex"); return failf("bad vector"); }
    memset(&d, 0, sizeof d);
    rc = rbl_wire_decode(bytes, n, &d);
    if (strcmp(expect, "reject") == 0) {
        if (rc == RBL_OK) ret = failf("accepted (%s): kind %u", jstr_or(c, "why", ""), d.header.kind);
    } else if (strcmp(expect, "ignore") == 0) {
        if (rc != RBL_OK) ret = failf("rejected an unknown kind (%s)", rbl_strerror(rc));
        else if (d.header.kind != ju32k(c, "kind")) ret = failf("kind is %u, expected %u", d.header.kind, ju32k(c, "kind"));
        else if (d.u.unknown.payload_len != ju32k(c, "payload_len"))
            ret = failf("payload_len is %lu, expected %u", (unsigned long)d.u.unknown.payload_len, ju32k(c, "payload_len"));
    } else if (strcmp(expect, "accept") == 0) {
        /* revision 5: a known kind whose values the decoder does not check; only the kind is compared (a NaN has no JSON form) */
        const char* kind = jstr(c, "kind");
        unsigned want = strcmp(kind, "heartbeat") == 0 ? RBL_HEARTBEAT : strcmp(kind, "camera") == 0 ? RBL_CAMERA :
                        strcmp(kind, "pose") == 0 ? RBL_POSE : strcmp(kind, "collision") == 0 ? RBL_COLLISION :
                        strcmp(kind, "frame") == 0 ? RBL_FRAME : 0u;
        if (want == 0) { format_error("unknown accept kind", kind); ret = failf("bad vector"); }
        else if (rc != RBL_OK) ret = failf("rejected (%s): %s", rbl_strerror(rc), jstr_or(c, "why", ""));
        else if (d.header.kind != want) ret = failf("decoded as kind %u, expected %s", d.header.kind, kind);
    } else if (strcmp(expect, "decode") == 0) {
        const JV* hj = jreq(c, "header");
        const JV* body = jreq(c, "body");
        rbl_header w = header_of(hj);
        if (rc != RBL_OK) ret = failf("rejected: %s", rbl_strerror(rc));
        else if (d.header.version != w.version || d.header.session != w.session || d.header.seq != w.seq || d.header.time_us != w.time_us ||
                 (jv_get(hj, "flags") && d.header.flags != w.flags))
            ret = failf("header version/flags/session/seq/time_us are %u/%u/%u/%u/%llu, expected %u/%u/%u/%u/%llu", d.header.version,
                        d.header.flags, d.header.session, d.header.seq, (unsigned long long)d.header.time_us, w.version, w.flags, w.session,
                        w.seq, (unsigned long long)w.time_us);
        else ret = check_decoded_body(&d, body);
        if (!ret && jis(c, "encode", 0)) {
            uint8_t out[8192 + 64];
            int len = encode_body(&w, body, out, sizeof out);
            if (len < 0) ret = failf("encode failed: %s", rbl_strerror(len));
            else if ((size_t)len != n || memcmp(out, bytes, n) != 0) {
                SB got, want;
                memset(&got, 0, sizeof got);
                memset(&want, 0, sizeof want);
                hex_encode(out, (size_t)len, &got);
                hex_encode(bytes, n, &want);
                ret = failf("encoded %.300s, expected %.300s", got.p, want.p);
                sb_free(&got);
                sb_free(&want);
            }
        }
    } else {
        format_error("unknown expect", expect);
        ret = failf("bad vector");
    }
    free(bytes);
    return ret;
}

static void test_datagrams(const JV* cases) {
    Report r;
    size_t i;
    memset(&r, 0, sizeof r);
    for (i = 0; i < cases->n; i++) record(&r, &cases->v[i], check_datagram(&cases->v[i]));
    finish(&r, "datagrams.json");
}

/* ------------------------------------------------------------------ pixel files */

static int g_temp_n;

typedef struct { char dir[1100]; char made[8][64]; int nmade; } Temp; /* a directory of its own for one case */

static void path_cat(char* out, size_t cap, const char* a, const char* b) { snprintf(out, cap, "%s%s", a, b); }

static void rm_any(const char* p) {
#if RBLI_WINDOWS
    if (!DeleteFileA(p)) RemoveDirectoryA(p);
#else
    if (unlink(p) != 0) rmdir(p);
#endif
}

static int temp_new(Temp* t) {
    char base[900];
    int n = rbl_temp_dir(base, sizeof base);
    memset(t, 0, sizeof *t);
    if (n < 0) return -1;
    snprintf(t->dir, sizeof t->dir, "%srbl-vectors-%u-%llu-%d", base, rbli_pid(), (unsigned long long)rbl_now_us(), g_temp_n++);
#if RBLI_WINDOWS
    if (!CreateDirectoryA(t->dir, NULL)) return -1;
    strcat(t->dir, "\\");
#else
    if (mkdir(t->dir, 0700) != 0) return -1;
    strcat(t->dir, "/");
#endif
    return 0;
}
/* the path of a file name inside the case's directory; remembered for cleanup */
static void temp_path(Temp* t, const char* name, char* out, size_t cap) {
    path_cat(out, cap, t->dir, name);
    if (t->nmade < 8) snprintf(t->made[t->nmade++], 64, "%s", name);
}
static void temp_drop(Temp* t) {
    int i;
    char p[1300];
    for (i = 0; i < t->nmade; i++) { path_cat(p, sizeof p, t->dir, t->made[i]); rm_any(p); }
    snprintf(p, sizeof p, "%s", t->dir);
    p[strlen(p) - 1] = 0; /* the trailing separator */
    rm_any(p);
}

static int copy_file(const char* from, const char* to) {
    uint8_t* data;
    size_t n;
    FILE* f;
    if (read_file(from, &data, &n) != 0) return -1;
    f = fopen(to, "wb");
    if (!f) { free(data); return -1; }
    if (fwrite(data, 1, n, f) != n) { fclose(f); free(data); return -1; }
    fclose(f);
    free(data);
    return 0;
}

/* a file of `length` bytes (zero filled, sparse where the file system allows) with `hdr` at offset 0 */
static int make_recipe(const char* path, const uint8_t* hdr, size_t hdr_len, uint64_t length) {
#if RBLI_WINDOWS
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER pos;
    DWORD got = 0, ret = 0;
    if (h == INVALID_HANDLE_VALUE) return -1;
    if (length > 0) {
        DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &ret, NULL); /* best effort */
        pos.QuadPart = (LONGLONG)length;
        if (!SetFilePointerEx(h, pos, NULL, FILE_BEGIN) || !SetEndOfFile(h)) { CloseHandle(h); return -1; }
        pos.QuadPart = 0;
        SetFilePointerEx(h, pos, NULL, FILE_BEGIN);
    }
    if (hdr_len > 0 && (!WriteFile(h, hdr, (DWORD)hdr_len, &got, NULL) || got != hdr_len)) { CloseHandle(h); return -1; }
    CloseHandle(h);
    return 0;
#else
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    size_t off = 0;
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)length) != 0) { close(fd); return -1; }
    while (off < hdr_len) {
        ssize_t k = write(fd, hdr + off, hdr_len - off);
        if (k <= 0) { close(fd); return -1; }
        off += (size_t)k;
    }
    close(fd);
    return 0;
#endif
}

static int truncate_file(const char* path, uint64_t length) {
#if RBLI_WINDOWS
    (void)path; (void)length;
    return -1; /* a mapped file cannot be shortened on Windows: the vector is skipped there */
#else
    return truncate(path, (off_t)length);
#endif
}

static int skip_large(void) {
    const char* e = getenv("RBL_VECTORS_SKIP_LARGE");
    return e && strcmp(e, "1") == 0;
}

/* one read of a `reads` entry (or a `then` read) */
static int check_read(rbl_pixel_reader* rd, const JV* r) {
    rbl_pixel_info info;
    rbl_pixel_frame got;
    uint8_t* rgba;
    float* depth = NULL;
    size_t rgba_cap, depth_cap;
    int want_depth = jis(r, "want_depth", 0), rc, ret = 0;
    const char* expect = jstr(r, "expect");
    uint64_t frame = ju64k(r, "frame");
    uint32_t slot = ju32k(r, "slot");
    rbl_pixel_reader_info(rd, &info);
    rgba_cap = (size_t)info.width * info.height * 4u;
    depth_cap = (size_t)info.width * info.height;
    rgba = (uint8_t*)xrealloc(NULL, rgba_cap);
    if (want_depth) depth = (float*)xrealloc(NULL, depth_cap * sizeof(float));
    memset(&got, 0, sizeof got);
    {   /* the Frame datagram: its frame, slot and size (the keys of a read are the datagram's) */
        rbl_frame dg;
        memset(&dg, 0, sizeof dg);
        dg.frame = frame;
        dg.slot = slot;
        dg.camera_seq = ju32k(r, "camera_seq");
        dg.width = ju32k(r, "width");
        dg.height = ju32k(r, "height");
        rc = rbl_pixel_reader_read_frame(rd, &dg, rgba, rgba_cap, depth, depth_cap, &got);
    }
    if (strcmp(expect, "skip") == 0) {
        if (rc == RBL_OK) ret = failf("delivered frame %llu (%s)", (unsigned long long)got.frame, jstr_or(r, "why", ""));
        else if (rc < 0) ret = failf("returned the error %s, expected a skip (%s)", rbl_strerror(rc), jstr_or(r, "why", ""));
    } else if (strcmp(expect, "ok") == 0) {
        const JV* wd = jreq(r, "depth");
        const JV* wrgba = jv_get(r, "rgba");
        if (rc != RBL_OK) ret = failf("skipped: %s", rbl_strerror(rc));
        else if (got.frame != frame) ret = failf("frame is %llu, expected %llu", (unsigned long long)got.frame, (unsigned long long)frame);
        else if (got.time_us != ju64k(r, "time_us")) ret = failf("time_us is %llu, expected %llu", (unsigned long long)got.time_us, (unsigned long long)ju64k(r, "time_us"));
        else if (got.camera_seq != ju32k(r, "camera_seq")) ret = failf("camera_seq is %u, expected %u", got.camera_seq, ju32k(r, "camera_seq"));
        else if (got.width != ju32k(r, "width") || got.height != ju32k(r, "height"))
            ret = failf("size is %ux%u, expected %ux%u", got.width, got.height, ju32k(r, "width"), ju32k(r, "height"));
        else {
            size_t clen = (size_t)got.width * got.height * 4u;
            if (wrgba && wrgba->t == J_STR) {
                SB hx;
                memset(&hx, 0, sizeof hx);
                hex_encode(rgba, clen, &hx);
                if (hx.n != wrgba->len || memcmp(hx.p, wrgba->s, hx.n) != 0) ret = failf("colour bytes differ");
                sb_free(&hx);
            } else {
                char hex[65];
                sha256_hex(rgba, clen, hex);
                if (strcmp(hex, jstr(r, "rgba_sha256")) != 0) ret = failf("colour bytes differ (sha256 %s)", hex);
            }
            if (!ret) {
                if (wd->t == J_NULL) {
                    if (got.has_depth) ret = failf("depth delivered, expected none");
                } else if (wd->t == J_ARR) {
                    size_t i, n = (size_t)got.width * got.height;
                    if (!got.has_depth) ret = failf("no depth delivered, expected %lu values", (unsigned long)wd->n);
                    else if (wd->n != n) ret = failf("%lu depth values expected, the frame has %lu pixels", (unsigned long)wd->n, (unsigned long)n);
                    else {
                        for (i = 0; i < n && !ret; i++) {
                            float w = jf32(&wd->v[i]);
                            if (!f32_same(depth[i], w)) ret = failf("depth[%lu] is %.9g, expected %.9g", (unsigned long)i, (double)depth[i], (double)w);
                        }
                    }
                } else format_error("bad depth in a read", "depth");
            }
        }
    } else {
        format_error("unknown read expect", expect);
        ret = failf("bad vector");
    }
    free(rgba);
    free(depth);
    return ret;
}

static int check_info(rbl_pixel_reader* rd, const JV* info) {
    rbl_pixel_info g;
    rbl_pixel_reader_info(rd, &g);
    if (g.slots != ju32k(info, "slots") || g.width != ju32k(info, "width") || g.height != ju32k(info, "height") ||
        g.stride != ju32k(info, "stride") || g.slot_size != ju64(jreq(info, "slot_size")))
        return failf("slots/width/height/stride/slot_size are %u/%u/%u/%u/%llu, expected %u/%u/%u/%u/%llu", g.slots, g.width, g.height, g.stride,
                     (unsigned long long)g.slot_size, ju32k(info, "slots"), ju32k(info, "width"), ju32k(info, "height"), ju32k(info, "stride"),
                     (unsigned long long)ju64(jreq(info, "slot_size")));
    if ((rbl_pixel_reader_has_depth(rd) != 0) != (jis(info, "has_depth", 0) != 0))
        return failf("has_depth is %d, expected %d", rbl_pixel_reader_has_depth(rd), jis(info, "has_depth", 0));
    return 0;
}

static int check_pixel_reader(const JV* c) {
    Temp t;
    char path[1300], open_path[1300], fixture[1300];
    const char* pk = jv_get(c, "path_kind") ? jstr(c, "path_kind") : NULL;
    const char* open = jstr(c, "open");
    const JV* recipe = jv_get(c, "recipe");
    const char* file = jv_get(c, "file") ? jstr(c, "file") : NULL;
    rbl_pixel_reader* rd = NULL;
    int rc, ret = 0, skipped = 0;
    size_t i;
    if (temp_new(&t) != 0) return failf("cannot create a temporary directory");
    temp_path(&t, "pixel.frames", path, sizeof path);
    snprintf(open_path, sizeof open_path, "%s", path);
    if (file) vec_path(fixture, sizeof fixture, file);
    if (!pk) {
        if (recipe) {
            const char* hs = jstr(recipe, "header_hex");
            size_t hn = 0;
            uint8_t* hb = hex_decode(hs, strlen(hs), &hn);
            if (!hb) { format_error("bad hex in", "header_hex"); ret = failf("bad vector"); }
            else {
                if (make_recipe(path, hb, hn, ju64k(recipe, "length")) != 0) ret = failf("cannot create the file of the recipe");
                free(hb);
            }
        } else if (!file) { format_error("neither file nor recipe nor path_kind in", jstr_or(c, "id", "?")); ret = failf("bad vector"); }
        else if (copy_file(fixture, path) != 0) ret = failf("cannot copy %s", file);
    } else if (strcmp(pk, "relative") == 0) {
        /* a valid copy sits in the working directory; only the relative spelling may refuse it */
        char name[100];
        snprintf(name, sizeof name, "rbl-vector-relative-%u.frames", rbli_pid());
        if (!file || copy_file(fixture, name) != 0) ret = failf("cannot copy the fixture to the working directory");
        else {
            rc = rbl_pixel_reader_open(name, &rd);
            if (rc == RBL_OK) { rbl_pixel_reader_close(rd); ret = failf("opened a relative path"); }
            rm_any(name);
        }
        temp_drop(&t);
        return ret;
    } else if (strcmp(pk, "missing") == 0) {
        temp_path(&t, "does-not-exist.frames", open_path, sizeof open_path);
    } else if (strcmp(pk, "empty") == 0) {
        open_path[0] = 0;
    } else if (strcmp(pk, "directory") == 0) {
        temp_path(&t, "a-directory", open_path, sizeof open_path);
#if RBLI_WINDOWS
        if (!CreateDirectoryA(open_path, NULL)) ret = failf("cannot create a directory");
#else
        if (mkdir(open_path, 0700) != 0) ret = failf("cannot create a directory");
#endif
    } else if (strcmp(pk, "symlink") == 0) {
#if RBLI_WINDOWS
        skipped = 1;
#else
        if (!file || copy_file(fixture, path) != 0) ret = failf("cannot copy the fixture");
        else {
            temp_path(&t, "link.frames", open_path, sizeof open_path);
            if (symlink(path, open_path) != 0) ret = failf("cannot create a symlink");
        }
#endif
    } else if (strcmp(pk, "fifo") == 0) {
#if RBLI_WINDOWS
        skipped = 1;
#else
        temp_path(&t, "fifo.frames", open_path, sizeof open_path);
        if (mkfifo(open_path, 0600) != 0) ret = failf("cannot create a FIFO");
#endif
    } else {
        format_error("unknown path_kind", pk);
        ret = failf("bad vector");
    }
    if (skipped) { failf("%s cannot be created on this platform", pk); temp_drop(&t); return 2; }
    if (ret) { temp_drop(&t); return ret; }

    rc = rbl_pixel_reader_open(open_path, &rd);
    if (strcmp(open, "refused") == 0) {
        if (rc == RBL_OK) { rbl_pixel_reader_close(rd); ret = failf("opened the file (%s)", jstr_or(c, "why", "")); }
    } else if (strcmp(open, "ok") == 0) {
        const JV* reads = jv_get(c, "reads");
        const JV* then = jv_get(c, "then");
        if (rc != RBL_OK) ret = failf("refused: %s", rbl_strerror(rc));
        else {
            if (jv_get(c, "info") && check_info(rd, jv_get(c, "info"))) ret = failw("info");
            for (i = 0; reads && i < reads->n && !ret; i++)
                if (check_read(rd, &reads->v[i])) {
                    char pre[100];
                    snprintf(pre, sizeof pre, "read of frame %s slot %u", jstr_or(&reads->v[i], "frame", "?"), ju32k(&reads->v[i], "slot"));
                    ret = failw(pre);
                }
            for (i = 0; then && i < then->n && !ret; i++) {
                const JV* st = &then->v[i];
                const char* op = jstr(st, "op");
                if (strcmp(op, "truncate") == 0) {
                    if (truncate_file(path, ju64k(st, "length")) != 0) ret = failf("cannot truncate the file");
                } else if (strcmp(op, "read") == 0) {
                    if (check_read(rd, st)) ret = failw("after truncation");
                } else { format_error("unknown then op", op); ret = failf("bad vector"); }
            }
            rbl_pixel_reader_close(rd);
        }
    } else { format_error("unknown open", open); ret = failf("bad vector"); }
    temp_drop(&t);
    return ret;
}

static int slurp(const char* path, uint8_t** data, size_t* n) { return read_file(path, data, n); }

static int check_pixel_writer(const JV* c) {
    const JV* w = jreq(c, "writer");
    Temp t;
    char path[1300], fixture[1300];
    uint32_t width = ju32k(w, "width"), height = ju32k(w, "height"), slots = ju32k(w, "slots");
    int with_depth = jis(w, "with_depth", 0), rc, ret = 0;
    const char* expect = jstr_or(c, "expect", "");
    rbl_pixel_writer* wr = NULL;
    const JV* writes = jreq(w, "writes");
    size_t i;
    uint64_t done = 0;
    uint8_t *got = NULL, *want = NULL;
    size_t gn = 0, wn = 0;
    if (temp_new(&t) != 0) return failf("cannot create a temporary directory");
    temp_path(&t, "writer.frames", path, sizeof path);
    if (strcmp(expect, "create_fails_and_file_untouched") == 0) {
        FILE* f = fopen(path, "wb");
        if (!f) { temp_drop(&t); return failf("cannot create the file that must stay"); }
        fwrite("precious", 1, 8, f);
        fclose(f);
        rc = rbl_pixel_writer_create(path, width, height, slots, with_depth, &wr);
        if (rc == RBL_OK) { rbl_pixel_writer_close(wr); ret = failf("created a writer over an existing file"); }
        else if (slurp(path, &got, &gn) != 0 || gn != 8 || memcmp(got, "precious", 8) != 0) ret = failf("the existing file was changed");
        free(got);
        temp_drop(&t);
        return ret;
    }
    if (strcmp(expect, "create_fails") == 0) {
        rc = rbl_pixel_writer_create(path, width, height, slots, with_depth, &wr);
        if (rc == RBL_OK) { rbl_pixel_writer_close(wr); ret = failf("created a writer (%s)", jstr_or(c, "why", "")); }
        else if (slurp(path, &got, &gn) == 0) { free(got); ret = failf("failed but left a file"); }
        temp_drop(&t);
        return ret;
    }
    rc = rbl_pixel_writer_create(path, width, height, slots, with_depth, &wr);
    if (rc != RBL_OK) { temp_drop(&t); return failf("create failed: %s", rbl_strerror(rc)); }
    for (i = 0; i < writes->n && !ret; i++) {
        const JV* wj = &writes->v[i];
        const char* hs = jstr(wj, "rgba");
        size_t rn = 0;
        uint8_t* rgba = hex_decode(hs, strlen(hs), &rn);
        const int refused = strcmp(jstr_or(wj, "expect", ""), "refused") == 0;
        const JV* dj = jreq(wj, "depth");
        float* depth = NULL;
        size_t dn = 0, k;
        uint64_t frame = 0;
        if (!rgba) { format_error("bad hex in", "rgba"); ret = failf("bad vector"); break; }
        if (dj->t == J_ARR) {
            dn = dj->n;
            depth = (float*)xrealloc(NULL, (dn + 1) * sizeof(float));
            for (k = 0; k < dn; k++) depth[k] = jf32(&dj->v[k]);
        }
        /* hex_decode of an empty string still returns a non-NULL buffer, so that a refusal is for the size and not for a NULL pointer */
        rc = rbl_pixel_writer_write(wr, rgba, rn, depth, dn, ju32k(wj, "width"), ju32k(wj, "height"), ju32k(wj, "camera_seq"),
                                    ju64k(wj, "time_us"), &frame);
        if (refused) {
            if (rc == RBL_OK) ret = failf("write %lu was accepted (%s)", (unsigned long)i, jstr_or(wj, "why", ""));
        } else if (rc != RBL_OK) ret = failf("write %lu failed: %s", (unsigned long)i, rbl_strerror(rc));
        else if (frame != ++done) ret = failf("write %lu announced frame %llu", (unsigned long)i, (unsigned long long)frame);
        free(rgba);
        free(depth);
    }
    if (!ret) {
        vec_path(fixture, sizeof fixture, jstr(c, "file"));
        if (slurp(path, &got, &gn) != 0) ret = failf("cannot read the file back");
        else if (read_file(fixture, &want, &wn) != 0) ret = failf("cannot read %s", fixture);
        else if (gn != wn || memcmp(got, want, gn) != 0) {
            size_t at = 0;
            while (at < gn && at < wn && got[at] == want[at]) at++;
            ret = failf("file bytes differ (lengths %lu and %lu, first difference at %lu)", (unsigned long)gn, (unsigned long)wn, (unsigned long)at);
        }
        free(got);
        free(want);
    }
    rbl_pixel_writer_close(wr);
    temp_drop(&t);
    return ret;
}

static void test_pixel_readers(const JV* cases) {
    Report r;
    size_t i;
    memset(&r, 0, sizeof r);
    for (i = 0; i < cases->n; i++) {
        const JV* c = &cases->v[i];
        const JV* so;
        int skip = 0;
        size_t k;
        if (jv_get(c, "writer")) continue;
        if (jis(c, "large", 0) && skip_large()) { failf("large"); record(&r, c, 2); continue; }
        so = jv_get(c, "skip_on");
        for (k = 0; so && k < so->n; k++) {
#if RBLI_WINDOWS
            if (so->v[k].t == J_STR && strcmp(so->v[k].s, "windows") == 0) skip = 1;
#endif
        }
        if (skip) { failf("windows"); record(&r, c, 2); continue; }
        record(&r, c, check_pixel_reader(c));
    }
    finish(&r, "pixelfiles.json (readers)");
}

static void test_pixel_writers(const JV* cases) {
    Report r;
    size_t i;
    memset(&r, 0, sizeof r);
    for (i = 0; i < cases->n; i++) if (jv_get(&cases->v[i], "writer")) record(&r, &cases->v[i], check_pixel_writer(&cases->v[i]));
    finish(&r, "pixelfiles.json (writers)");
}

/* ------------------------------------------------------------------ sessions.json */

#define STEP_WAIT_MS 3000u
#define MAX_CONNS 8
#define MAX_VARS 16

typedef struct { char name[16]; int used; rbli_sock sock; uint8_t* buf; size_t len, cap; int eof; } Raw;
typedef struct { char* type; JV obj; } Ev;
typedef struct { char name[24]; char* val; } Var;

typedef struct {
    rbl_link* ut;
    int host;
    Ev* ev;
    size_t nev, capev;
    Raw conns[MAX_CONNS];
    rbli_sock listener, udp, other;
    uint16_t peer_udp, ut_udp;
    Var vars[MAX_VARS];
    const JV* dgrams; /* the cases of datagrams.json, for `like` */
} Sess;

static void var_set(Sess* s, const char* name, const char* val) {
    int i;
    for (i = 0; i < MAX_VARS; i++) {
        if (s->vars[i].val && strcmp(s->vars[i].name, name) == 0) { free(s->vars[i].val); s->vars[i].val = xstrdup(val); return; }
    }
    for (i = 0; i < MAX_VARS; i++) {
        if (!s->vars[i].val) { snprintf(s->vars[i].name, sizeof s->vars[i].name, "%s", name); s->vars[i].val = xstrdup(val); return; }
    }
    format_error("too many variables", name);
}
static const char* var_get(const Sess* s, const char* name) {
    int i;
    for (i = 0; i < MAX_VARS; i++) if (s->vars[i].val && strcmp(s->vars[i].name, name) == 0) return s->vars[i].val;
    return NULL;
}

/* ${name} -> the variable's value (an unknown name stays as it is) */
static void subst(const Sess* s, const char* in, size_t n, SB* out) {
    size_t i = 0;
    while (i < n) {
        if (in[i] == '$' && i + 1 < n && in[i + 1] == '{') {
            const char* close = (const char*)memchr(in + i + 2, '}', n - i - 2);
            if (close) {
                size_t l = (size_t)(close - (in + i + 2));
                char name[32];
                const char* v;
                if (l < sizeof name) {
                    memcpy(name, in + i + 2, l);
                    name[l] = 0;
                    v = var_get(s, name);
                    if (v) { sb_puts(out, v); i = (size_t)(close + 1 - in); continue; }
                }
            }
        }
        sb_add(out, in + i, 1);
        i++;
    }
}

static int session_var(const Sess* s, uint32_t* out) {
    const char* v = var_get(s, "session");
    uint64_t r = 0;
    if (!v || !*v) return 0;
    for (; *v; v++) { if (*v < '0' || *v > '9') return 0; r = r * 10u + (uint64_t)(*v - '0'); }
    *out = (uint32_t)r;
    return 1;
}

/* subst, then {session_le}, {session_le_plus1} and {session_le_minus1} as 8 hex digits, little-endian */
static int subst_hex(const Sess* s, const char* in, size_t n, SB* out) {
    SB a;
    size_t i = 0;
    uint32_t session = 0;
    int have = session_var(s, &session);
    memset(&a, 0, sizeof a);
    subst(s, in, n, &a);
    sb_add(out, "", 0);
    while (i < a.n) {
        static const char* tags[3] = { "{session_le}", "{session_le_plus1}", "{session_le_minus1}" };
        int t, hit = 0;
        for (t = 0; t < 3 && a.p[i] == '{'; t++) {
            size_t tl = strlen(tags[t]);
            if (i + tl <= a.n && memcmp(a.p + i, tags[t], tl) == 0) {
                uint32_t v = t == 0 ? session : t == 1 ? session + 1u : session - 1u;
                uint8_t le[4];
                if (!have) { sb_free(&a); return -1; }
                le[0] = (uint8_t)v; le[1] = (uint8_t)(v >> 8); le[2] = (uint8_t)(v >> 16); le[3] = (uint8_t)(v >> 24);
                hex_encode(le, 4, out);
                i += tl;
                hit = 1;
                break;
            }
        }
        if (!hit) { sb_add(out, a.p + i, 1); i++; }
    }
    sb_free(&a);
    return 0;
}

static void ev_free(Ev* e) { free(e->type); jv_free(&e->obj); }

static void ev_push(Sess* s, const char* type, JV obj) {
    if (s->nev == s->capev) { s->capev = s->capev ? s->capev * 2 : 32; s->ev = (Ev*)xrealloc(s->ev, s->capev * sizeof(Ev)); }
    s->ev[s->nev].type = xstrdup(type);
    s->ev[s->nev].obj = obj;
    s->nev++;
}

static void u64_str(char* out, size_t cap, uint64_t v) { snprintf(out, cap, "%llu", (unsigned long long)v); }

/* the events of the keys a `match` may use, as the README lists them */
static void take_event(Sess* s, const rbl_event* e) {
    JV o = jv_new(J_OBJ);
    switch (e->type) {
    case RBL_EV_CONNECTED:
        jv_set(&o, "role", jv_cstr(role_name(e->u.connected.role)));
        jv_set(&o, "game", jv_cstr(e->u.connected.game));
        jv_set(&o, "protocol", jv_num(e->u.connected.protocol, 1));
        jv_set(&o, "session", jv_num(e->u.connected.session, 1));
        jv_set(&o, "heartbeat_ms", jv_num(e->u.connected.heartbeat_ms, 1));
        ev_push(s, "connected", o);
        break;
    case RBL_EV_LOST: ev_push(s, "lost", o); break;
    case RBL_EV_HANDSHAKE_FAILED: ev_push(s, "handshake_failed", o); break;
    case RBL_EV_CAMERA:
        jv_set(&o, "seq", jv_num(e->u.camera.header.seq, 1));
        jv_set(&o, "viewport_width", jv_num(e->u.camera.camera.viewport_width, 1));
        jv_set(&o, "viewport_height", jv_num(e->u.camera.camera.viewport_height, 1));
        jv_set(&o, "depth_range", jv_num(e->u.camera.camera.depth_range, 1));
        ev_push(s, "camera", o);
        break;
    case RBL_EV_POSES: {
        JV ents = jv_new(J_ARR);
        uint32_t i;
        for (i = 0; i < e->u.poses.count; i++) jv_push(&ents, jv_num(e->u.poses.poses[i].entity, 1));
        jv_set(&o, "count", jv_num(e->u.poses.count, 1));
        jv_set(&o, "entities", ents);
        ev_push(s, "poses", o);
        break;
    }
    case RBL_EV_COLLISION:
        jv_set(&o, "query", jv_num(e->u.collision.query, 1));
        jv_set(&o, "triangles", jv_num(e->u.collision.triangle_count, 1));
        {
            JV ctr = jv_new(J_ARR);
            int k;
            for (k = 0; k < 3; k++) jv_push(&ctr, jv_num((double)e->u.collision.center[k], 0));
            jv_set(&o, "center", ctr);
        }
        jv_set(&o, "radius", jv_num((double)e->u.collision.radius, 0));
        ev_push(s, "collision", o);
        break;
    case RBL_EV_FRAME: {
        char f[32];
        u64_str(f, sizeof f, e->u.frame.frame.frame);
        jv_set(&o, "frame", jv_cstr(f));
        jv_set(&o, "slot", jv_num(e->u.frame.frame.slot, 1));
        jv_set(&o, "camera_seq", jv_num(e->u.frame.frame.camera_seq, 1));
        jv_set(&o, "width", jv_num(e->u.frame.frame.width, 1));
        jv_set(&o, "height", jv_num(e->u.frame.frame.height, 1));
        ev_push(s, "frame", o);
        break;
    }
    case RBL_EV_SPAWN:
        jv_set(&o, "entity", jv_num(e->u.spawn.entity, 1));
        jv_set(&o, "provider", jv_cstr(e->u.spawn.provider ? e->u.spawn.provider : ""));
        jv_set(&o, "asset", jv_cstr(e->u.spawn.asset ? e->u.spawn.asset : ""));
        if (e->u.spawn.name) jv_set(&o, "name", jv_cstr(e->u.spawn.name));
        ev_push(s, "spawn", o);
        break;
    case RBL_EV_DESPAWN:
        jv_set(&o, "entity", jv_num(e->u.despawn.entity, 1));
        ev_push(s, "despawn", o);
        break;
    case RBL_EV_FOCUS:
        jv_set(&o, "focus", jv_cstr(role_name(e->u.focus.focus)));
        jv_set(&o, "requested", jv_bool(e->u.focus.requested));
        ev_push(s, "focus", o);
        break;
    case RBL_EV_FRAME_CHANNEL:
        if (e->u.frame_channel.path) jv_set(&o, "path", jv_cstr(e->u.frame_channel.path));
        jv_set(&o, "width", jv_num(e->u.frame_channel.width, 1));
        jv_set(&o, "height", jv_num(e->u.frame_channel.height, 1));
        jv_set(&o, "slots", jv_num(e->u.frame_channel.slots, 1));
        ev_push(s, "frame_channel", o);
        break;
    default: jv_free(&o); break;
    }
}

/* One poll of the UT; its events are queued. Called every few milliseconds by every wait. */
static void pump(Sess* s) {
    rbl_event evs[64];
    int n = rbl_link_poll(s->ut, evs, 64), i;
    for (i = 0; i < n; i++) take_event(s, &evs[i]);
}

#define WAIT_FOR(S, MS, COND, OK) do { \
    uint64_t end_ = rbli_mono_ms() + (uint64_t)(MS); (OK) = 0; \
    for (;;) { pump(S); if (COND) { (OK) = 1; break; } if (rbli_mono_ms() >= end_) break; rbli_sleep_ms(2); } } while (0)

/* ---- the raw peer's connections */

static Raw* raw_find(Sess* s, const char* name) {
    int i;
    for (i = 0; i < MAX_CONNS; i++) if (s->conns[i].used && strcmp(s->conns[i].name, name) == 0) return &s->conns[i];
    return NULL;
}
static Raw* raw_add(Sess* s, const char* name, rbli_sock sock) {
    int i;
    for (i = 0; i < MAX_CONNS; i++) {
        if (!s->conns[i].used) {
            Raw* r = &s->conns[i];
            memset(r, 0, sizeof *r);
            r->used = 1;
            r->sock = sock;
            snprintf(r->name, sizeof r->name, "%s", name);
            return r;
        }
    }
    return NULL;
}
static void raw_drop(Raw* r) {
    if (r->used) rbli_sock_close(r->sock);
    free(r->buf);
    memset(r, 0, sizeof *r);
}
static Raw* raw_of(Sess* s, const JV* step) { return raw_find(s, jstr_or(step, "conn", "a")); }

/* reads what is available into the connection's buffer */
static void raw_fill(Raw* r) {
    uint8_t tmp[16384];
    for (;;) {
        int k = rbli_tcp_recv(r->sock, tmp, sizeof tmp);
        if (k > 0) {
            if (r->len + (size_t)k > r->cap) { r->cap = (r->len + (size_t)k) * 2; r->buf = (uint8_t*)xrealloc(r->buf, r->cap); }
            memcpy(r->buf + r->len, tmp, (size_t)k);
            r->len += (size_t)k;
        } else if (k == -1) {
            return;
        } else { /* end of stream, or a reset: the UT's end is gone either way */
            r->eof = 1;
            return;
        }
    }
}
/* takes the next line (with its line feed) out of the buffer, into out; 1 if there was one */
static int raw_take_line(Raw* r, SB* out) {
    size_t i;
    for (i = 0; i < r->len; i++) {
        if (r->buf[i] == '\n') {
            sb_add(out, r->buf, i + 1);
            memmove(r->buf, r->buf + i + 1, r->len - i - 1);
            r->len -= i + 1;
            return 1;
        }
    }
    return 0;
}

/* Writes everything, polling the UT meanwhile. A connection that has died is not an error of the
 * runner: the steps that follow decide whether that was expected. */
static void raw_write(Sess* s, Raw* r, const void* data, size_t n) {
    size_t off = 0;
    uint64_t end = rbli_mono_ms() + 10000u;
    while (off < n) {
        int k = rbli_tcp_send(r->sock, (const char*)data + off, n - off);
        if (k > 0) off += (size_t)k;
        else if (k == -1) { pump(s); rbli_sleep_ms(1); }
        else return;
        if (rbli_mono_ms() > end) return;
    }
}

/* ---- patterns */

static int pattern_ok(const JV* act, const JV* pat) {
    if (pat->t == J_STR && pat->len > 0 && pat->s[0] == '$') {
        int is_int = act->t == J_NUM && act->is_int && act->num >= 0;
        if (strcmp(pat->s, "$string") == 0) return act->t == J_STR;
        if (strcmp(pat->s, "$int") == 0) return is_int;
        if (strcmp(pat->s, "$u32nz") == 0) return is_int && act->num >= 1 && act->num <= 4294967295.0;
        if (strcmp(pat->s, "$u16nz") == 0) return is_int && act->num >= 1 && act->num <= 65535.0;
        if (strcmp(pat->s, "$hb") == 0) return is_int && act->num >= 10 && act->num <= 10000.0;
        format_error("unknown pattern", pat->s);
        return 0;
    }
    return jv_eq(act, pat);
}
static int match_ok(const JV* act, const JV* m) {
    size_t i;
    if (m->t != J_OBJ) return 1;
    for (i = 0; i < m->n; i++) {
        const JV* g = jv_get(act, m->k[i]);
        if (!g || !pattern_ok(g, &m->v[i])) {
            SB d, e, got;
            memset(&d, 0, sizeof d);
            memset(&e, 0, sizeof e);
            memset(&got, 0, sizeof got);
            jv_dump(act, &d);
            jv_dump(&m->v[i], &e);
            if (g) jv_dump(g, &got); else sb_puts(&got, "nothing");
            failf("key \"%s\": expected %s, got %s in %s", m->k[i], e.p, got.p, d.p);
            sb_free(&d); sb_free(&e); sb_free(&got);
            return 0;
        }
    }
    return 1;
}

/* ---- steps; each returns 0 or 1 (failf) */

static int st_connect(Sess* s, const JV* step) {
    rbli_sock sk = rbli_tcp_connect_start(rbl_link_control_port(s->ut));
    int ok = 0, st = 0;
    if (sk == RBLI_BAD_SOCK) return failf("cannot start a connection");
    WAIT_FOR(s, STEP_WAIT_MS, (st = rbli_tcp_connect_check(sk)) != 0, ok);
    if (!ok || st != 1) { rbli_sock_close(sk); return failf("cannot connect to the implementation's control port"); }
    rbli_nodelay(sk);
    if (!raw_add(s, jstr_or(step, "conn", "a"), sk)) { rbli_sock_close(sk); return failf("too many connections"); }
    return 0;
}
static int st_accept(Sess* s, const JV* step) {
    rbli_sock sk = RBLI_BAD_SOCK;
    int ok;
    WAIT_FOR(s, STEP_WAIT_MS, (sk = rbli_tcp_accept(s->listener)) != RBLI_BAD_SOCK, ok);
    if (!ok) return failf("the guest did not connect");
    if (!raw_add(s, jstr_or(step, "conn", "a"), sk)) { rbli_sock_close(sk); return failf("too many connections"); }
    return 0;
}
static int st_send(Sess* s, const JV* step, const char* op) {
    Raw* r = raw_of(s, step);
    SB b;
    memset(&b, 0, sizeof b);
    if (!r) return failf("no such connection");
    if (strcmp(op, "send_line") == 0) {
        const JV* t = jreq(step, "text");
        subst(s, t->s ? t->s : "", t->len, &b);
    } else if (strcmp(op, "send_hex") == 0) {
        const char* h = jstr(step, "hex");
        size_t n;
        uint8_t* d = hex_decode(h, strlen(h), &n);
        if (!d) { format_error("bad hex in", "hex"); return failf("bad vector"); }
        sb_add(&b, d, n);
        free(d);
    } else if (strcmp(op, "send_fill") == 0) {
        const char* fill = jstr(step, "fill");
        uint32_t i, count = ju32k(step, "count");
        sb_puts(&b, jstr(step, "prefix"));
        sb_reserve(&b, count);
        for (i = 0; i < count; i++) sb_add(&b, fill, 1);
        sb_puts(&b, jstr(step, "suffix"));
    } else { /* send_range: the template for i = from ..= to, in one write */
        const JV* t = jreq(step, "template");
        uint32_t i, from = ju32k(step, "from"), to = ju32k(step, "to");
        char num[16];
        for (i = from; i <= to; i++) {
            SB one;
            memset(&one, 0, sizeof one);
            snprintf(num, sizeof num, "%u", i);
            var_set(s, "i", num);
            subst(s, t->s ? t->s : "", t->len, &one);
            sb_add(&b, one.p, one.n);
            sb_free(&one);
        }
    }
    raw_write(s, r, b.p ? b.p : "", b.n);
    sb_free(&b);
    return 0;
}
static int st_expect_line(Sess* s, const JV* step) {
    Raw* r = raw_of(s, step);
    SB line;
    int ok, ret = 0;
    JV v;
    const char* perr;
    const JV* cap;
    memset(&line, 0, sizeof line);
    if (!r) return failf("no such connection");
    WAIT_FOR(s, STEP_WAIT_MS, (raw_fill(r), raw_take_line(r, &line)), ok);
    if (!ok) { sb_free(&line); return failf("no line from the implementation%s", r->eof ? " (the connection is closed)" : ""); }
    perr = json_parse(line.p, line.n, &v);
    if (perr) { ret = failf("not JSON (%s): %.200s", perr, line.p); sb_free(&line); return ret; }
    if (v.t != J_OBJ) ret = failf("not a JSON object: %.200s", line.p);
    else if (!match_ok(&v, jreq(step, "match"))) { char inner[sizeof g_err]; memcpy(inner, g_err, sizeof inner); ret = failf("line %.300s: %s", line.p, inner); }
    else if (jis(step, "exact", 0) && v.n != jreq(step, "match")->n) {
        SB d;
        memset(&d, 0, sizeof d);
        jv_dump(&v, &d);
        ret = failf("keys differ, expected exactly those of the match: %s", d.p);
        sb_free(&d);
    }
    cap = jv_get(step, "capture");
    if (!ret && cap && cap->t == J_OBJ) {
        size_t i;
        for (i = 0; i < cap->n && !ret; i++) {
            const JV* val = jv_get(&v, cap->v[i].s ? cap->v[i].s : "");
            char num[32];
            if (!val || val->t != J_NUM || !val->is_int) { ret = failf("the captured value \"%s\" is not an integer", cap->v[i].s); break; }
            snprintf(num, sizeof num, "%llu", (unsigned long long)val->num);
            if (strcmp(cap->k[i], "ut_udp_port") == 0 && (unsigned long long)val->num != (unsigned long long)s->ut_udp)
                ret = failf("hello announces udp_port %s, the socket is %u", num, (unsigned)s->ut_udp);
            else var_set(s, cap->k[i], num);
        }
    }
    jv_free(&v);
    sb_free(&line);
    return ret;
}
static int st_expect_no_line(Sess* s, const JV* step) {
    Raw* r = raw_of(s, step);
    SB line;
    int ok, ret = 0;
    memset(&line, 0, sizeof line);
    if (!r) return failf("no such connection");
    WAIT_FOR(s, ju32k(step, "ms"), (raw_fill(r), raw_take_line(r, &line) || r->eof), ok);
    if (ok) ret = line.n ? failf("a line arrived: %.200s", line.p) : failf("the connection closed");
    sb_free(&line);
    return ret;
}
static int st_expect_closed(Sess* s, const JV* step) {
    Raw* r = raw_of(s, step);
    int allow = jis(step, "allow_lines", 0), ok, bad = 0;
    uint32_t within = jv_get(step, "within_ms") ? ju32k(step, "within_ms") : 3000u;
    SB line;
    memset(&line, 0, sizeof line);
    if (!r) return failf("no such connection");
    WAIT_FOR(s, within, (raw_fill(r), (!allow && raw_take_line(r, &line) ? (bad = 1) : r->eof)), ok);
    if (bad) { int rc = failf("a line arrived before end of stream: %.200s", line.p); sb_free(&line); return rc; }
    sb_free(&line);
    return ok ? 0 : failf("the connection stayed open");
}
static int st_close(Sess* s, const JV* step) {
    Raw* r = raw_of(s, step);
    if (!r) return failf("no such connection");
    if (jis(step, "half", 0)) rbli_shutdown_write(r->sock);
    else raw_drop(r);
    return 0;
}
static int st_set(Sess* s, const JV* step) {
    const JV* vars = jreq(step, "vars");
    size_t i;
    for (i = 0; vars->t == J_OBJ && i < vars->n; i++) {
        const JV* v = &vars->v[i];
        char num[32];
        if (v->t == J_NUM) { snprintf(num, sizeof num, "%.0f", v->num); var_set(s, vars->k[i], num); }
        else if (v->t == J_STR) var_set(s, vars->k[i], v->s);
    }
    return 0;
}
static int st_sleep_ms(Sess* s, uint32_t ms) {
    int ok;
    WAIT_FOR(s, ms, 0, ok);
    (void)ok;
    return 0;
}
static int st_send_datagram(Sess* s, const JV* step) {
    SB hx;
    size_t n;
    uint8_t* b;
    const JV* h = jreq(step, "hex");
    int ok;
    memset(&hx, 0, sizeof hx);
    if (subst_hex(s, h->s ? h->s : "", h->len, &hx) != 0) { sb_free(&hx); return failf("a session variable is needed here"); }
    b = hex_decode(hx.p, hx.n, &n);
    sb_free(&hx);
    if (!b) { format_error("bad hex in", "send_datagram hex"); return failf("bad vector"); }
    if (strcmp(jstr_or(step, "from", ""), "other") == 0) {
        uint16_t port;
        if (s->other == RBLI_BAD_SOCK) {
            s->other = rbli_udp_open(0, &port);
            if (s->other == RBLI_BAD_SOCK) { free(b); return failf("cannot open a second UDP socket"); }
        }
        ok = rbli_udp_send(s->other, b, n, s->ut_udp);
    } else {
        ok = rbli_udp_send(s->udp, b, n, s->ut_udp);
    }
    free(b);
    return ok ? 0 : failf("the datagram was not sent");
}
static const JV* find_dgram(const Sess* s, const char* id) {
    size_t i;
    for (i = 0; i < s->dgrams->n; i++) if (strcmp(jstr_or(&s->dgrams->v[i], "id", ""), id) == 0) return &s->dgrams->v[i];
    format_error("unknown datagram case", id);
    return NULL;
}
static int st_expect_datagram(Sess* s, const JV* step) {
    uint32_t session = 0;
    uint32_t timeout = jv_get(step, "timeout_ms") ? ju32k(step, "timeout_ms") : 3000u;
    uint32_t count = jv_get(step, "count") ? ju32k(step, "count") : 1u, got = 0;
    uint8_t *like = NULL;
    size_t like_n = 0;
    int kind = jv_get(step, "kind") ? (int)ju32k(step, "kind") : -1;
    int ret = 0, done = 0;
    uint32_t seqs[64];
    uint64_t end;
    if (!session_var(s, &session)) return failf("a session variable is needed here");
    if (jv_get(step, "like")) {
        const JV* dc = find_dgram(s, jstr(step, "like"));
        const char* h = dc ? jstr(dc, "hex") : "";
        like = hex_decode(h, strlen(h), &like_n);
        if (!like || like_n < 24) { free(like); return failf("bad vector"); }
        if (kind < 0) kind = like[6];
    }
    if (count > 64) { free(like); format_error("count too large", "expect_datagram"); return failf("bad vector"); }
    end = rbli_mono_ms() + timeout;
    while (!ret && !done) {
        uint8_t d[16384];
        int n;
        uint32_t addr;
        uint16_t port;
        pump(s);
        for (;;) {
            n = rbli_udp_recv(s->udp, d, sizeof d, &addr, &port);
            if (n < 0) break;
            if (addr != 0x7F000001u || port != s->ut_udp) { ret = failf("datagram from port %u, the UT's port is %u", (unsigned)port, (unsigned)s->ut_udp); break; }
            if (n < 24 || memcmp(d, "RBLK", 4) != 0 || rbli_get16(d + 4) != 1u) {
                SB hx;
                memset(&hx, 0, sizeof hx);
                hex_encode(d, (size_t)(n < 24 ? n : 24), &hx);
                ret = failf("bad header in %s", hx.p);
                sb_free(&hx);
                break;
            }
            if (rbli_get32(d + 8) != session) { ret = failf("wrong session in a datagram from the UT"); break; }
            if (kind >= 0 && d[6] != (uint8_t)kind) continue;
            if (like) {
                if ((size_t)n != like_n || memcmp(d, like, 8) != 0 || memcmp(d + 24, like + 24, like_n - 24) != 0) {
                    SB a, b;
                    memset(&a, 0, sizeof a);
                    memset(&b, 0, sizeof b);
                    hex_encode(d, (size_t)n, &a);
                    hex_encode(like, like_n, &b);
                    ret = failf("datagram %.200s differs from %.200s", a.p, b.p);
                    sb_free(&a);
                    sb_free(&b);
                    break;
                }
            }
            seqs[got++] = rbli_get32(d + 12);
            if (got >= count) { done = 1; break; }
        }
        if (ret || done) break;
        if (rbli_mono_ms() >= end) break;
        rbli_sleep_ms(2);
    }
    free(like);
    if (ret) return ret;
    if (!done) return failf("got %u of %u datagrams", got, count);
    if (jis(step, "seq_consecutive", 0)) {
        uint32_t i;
        for (i = 1; i < got; i++) if (seqs[i] != seqs[i - 1] + 1u) return failf("seq values are not consecutive (%u after %u)", seqs[i], seqs[i - 1]);
    }
    return 0;
}
static int st_expect_no_datagram(Sess* s, const JV* step) {
    uint64_t end = rbli_mono_ms() + ju32k(step, "ms");
    for (;;) {
        uint8_t d[16384];
        uint32_t addr;
        uint16_t port;
        int n;
        pump(s);
        n = rbli_udp_recv(s->udp, d, sizeof d, &addr, &port);
        if (n >= 0) return failf("a %d byte datagram arrived from port %u", n, (unsigned)port);
        if (rbli_mono_ms() >= end) return 0;
        rbli_sleep_ms(2);
    }
}
static int st_expect_focus(Sess* s, const JV* step) {
    const char* got;
    pump(s);
    got = role_name(rbl_link_focus(s->ut));
    return strcmp(got, jstr(step, "focus")) == 0 ? 0 : failf("focus is %s, expected %s", got, jstr(step, "focus"));
}
static int st_ut(Sess* s, const JV* step) {
    const char* what = jstr(step, "do");
    int rc = RBL_OK;
    if (strcmp(what, "spawn") == 0) {
        const JV* nm = jv_get(step, "name");
        rc = rbl_link_spawn(s->ut, ju32k(step, "entity"), jstr(step, "provider"), jstr(step, "asset"), nm && nm->t == J_STR ? nm->s : NULL);
    } else if (strcmp(what, "despawn") == 0) {
        rc = rbl_link_despawn(s->ut, ju32k(step, "entity"));
    } else if (strcmp(what, "set_focus") == 0) {
        rc = rbl_link_set_focus(s->ut, role_of(jreq(step, "focus")));
    } else if (strcmp(what, "announce_frames") == 0) {
        rc = rbl_link_announce_frames(s->ut, jstr(step, "path"), ju32k(step, "width"), ju32k(step, "height"), ju32k(step, "slots"));
    } else if (strcmp(what, "disconnect") == 0) {
        rbl_link_disconnect(s->ut, jstr(step, "reason"));
    } else if (strcmp(what, "send_camera") == 0 || strcmp(what, "send_poses") == 0 || strcmp(what, "send_frame") == 0) {
        const JV* dc = find_dgram(s, jstr(step, "like"));
        const JV* body = dc ? jreq(dc, "body") : &J_NONE;
        if (!dc) return failf("bad vector");
        if (strcmp(what, "send_camera") == 0) {
            rbl_camera c;
            camera_of(body, &c);
            rc = rbl_link_send_camera(s->ut, &c, NULL);
        } else if (strcmp(what, "send_poses") == 0) {
            const JV* ps = jreq(body, "poses");
            rbl_pose* p = (rbl_pose*)xrealloc(NULL, (ps->n + 1) * sizeof *p);
            size_t i;
            for (i = 0; i < ps->n; i++) pose_of(&ps->v[i], &p[i]);
            rc = rbl_link_send_poses(s->ut, p, ps->n);
            free(p);
        } else {
            rbl_frame f;
            frame_of(body, &f);
            rc = rbl_link_send_frame(s->ut, &f);
        }
    } else {
        format_error("unknown ut do", what);
        return failf("bad vector");
    }
    return rc == RBL_OK ? 0 : failf("%s failed: %s", what, rbl_strerror(rc));
}

static size_t count_events(const Sess* s, const char* ty) {
    size_t i, n = 0;
    for (i = 0; i < s->nev; i++) if (strcmp(s->ev[i].type, ty) == 0) n++;
    return n;
}
static void queue_types(const Sess* s, SB* out) {
    size_t i;
    for (i = 0; i < s->nev; i++) { if (i) sb_puts(out, ","); sb_puts(out, s->ev[i].type); }
}
static int st_expect_event(Sess* s, const JV* step) {
    const char* ty = jstr(step, "event");
    uint32_t count = jv_get(step, "count") ? ju32k(step, "count") : 1u, taken = 0;
    const JV* m = jv_get(step, "match");
    size_t i = 0;
    int ok;
    WAIT_FOR(s, STEP_WAIT_MS, count_events(s, ty) >= count, ok);
    if (!ok) {
        SB q;
        int rc;
        memset(&q, 0, sizeof q);
        queue_types(s, &q);
        rc = failf("no %u %s event(s); the queue holds [%s]", count, ty, q.p ? q.p : "");
        sb_free(&q);
        return rc;
    }
    while (taken < count) {
        if (strcmp(s->ev[i].type, ty) == 0) {
            Ev e = s->ev[i];
            int good;
            memmove(&s->ev[i], &s->ev[i + 1], (s->nev - i - 1) * sizeof(Ev));
            s->nev--;
            good = m ? match_ok(&e.obj, m) : 1;
            ev_free(&e);
            if (!good) { char pre[80]; snprintf(pre, sizeof pre, "%s event %u", ty, taken); return failw(pre); }
            taken++;
        } else {
            i++;
        }
    }
    return 0;
}
static int st_expect_no_event(Sess* s, const JV* step) {
    const char* ty = jstr(step, "event");
    size_t i;
    st_sleep_ms(s, ju32k(step, "ms"));
    for (i = 0; i < s->nev; i++) {
        if (strcmp(s->ev[i].type, ty) == 0) {
            SB d;
            int rc;
            memset(&d, 0, sizeof d);
            jv_dump(&s->ev[i].obj, &d);
            rc = failf("unexpected %s event %s", ty, d.p);
            sb_free(&d);
            return rc;
        }
    }
    return 0;
}

static int run_step(Sess* s, const JV* step) {
    const char* op = jstr(step, "op");
    if (strcmp(op, "connect") == 0) return st_connect(s, step);
    if (strcmp(op, "accept") == 0) return st_accept(s, step);
    if (strcmp(op, "send_line") == 0 || strcmp(op, "send_hex") == 0 || strcmp(op, "send_fill") == 0 || strcmp(op, "send_range") == 0)
        return st_send(s, step, op);
    if (strcmp(op, "expect_line") == 0) return st_expect_line(s, step);
    if (strcmp(op, "expect_no_line") == 0) return st_expect_no_line(s, step);
    if (strcmp(op, "expect_closed") == 0) return st_expect_closed(s, step);
    if (strcmp(op, "close") == 0) return st_close(s, step);
    if (strcmp(op, "set") == 0) return st_set(s, step);
    if (strcmp(op, "sleep") == 0) return st_sleep_ms(s, ju32k(step, "ms"));
    if (strcmp(op, "send_datagram") == 0) return st_send_datagram(s, step);
    if (strcmp(op, "expect_datagram") == 0) return st_expect_datagram(s, step);
    if (strcmp(op, "expect_no_datagram") == 0) return st_expect_no_datagram(s, step);
    if (strcmp(op, "ut") == 0) return st_ut(s, step);
    if (strcmp(op, "expect_focus") == 0) return st_expect_focus(s, step);
    if (strcmp(op, "expect_event") == 0) return st_expect_event(s, step);
    if (strcmp(op, "expect_no_event") == 0) return st_expect_no_event(s, step);
    format_error("unknown op", op);
    return failf("bad vector");
}

static void sess_close(Sess* s) {
    size_t i;
    int k;
    for (k = 0; k < MAX_CONNS; k++) raw_drop(&s->conns[k]); /* the peer goes first, so that the UT's closing does not wait for it */
    if (s->ut) rbl_link_close(s->ut);
    if (s->listener != RBLI_BAD_SOCK) rbli_sock_close(s->listener);
    if (s->udp != RBLI_BAD_SOCK) rbli_sock_close(s->udp);
    if (s->other != RBLI_BAD_SOCK) rbli_sock_close(s->other);
    for (i = 0; i < s->nev; i++) ev_free(&s->ev[i]);
    free(s->ev);
    for (k = 0; k < MAX_VARS; k++) free(s->vars[k].val);
    memset(s, 0, sizeof *s);
}

static int sess_open(Sess* s, const JV* c, const JV* dgrams) {
    rbl_config cfg;
    const JV* conf = jv_get(c, "config");
    uint16_t port = 0;
    char num[16];
    int rc;
    memset(s, 0, sizeof *s);
    s->listener = s->udp = s->other = RBLI_BAD_SOCK;
    s->dgrams = dgrams;
    s->host = strcmp(jstr(c, "role_under_test"), "host") == 0;
    s->udp = rbli_udp_open(0, &s->peer_udp);
    if (s->udp == RBLI_BAD_SOCK) return failf("cannot open the raw peer's UDP socket");
    if (!s->host) {
        s->listener = rbli_tcp_listen(0, &port);
        if (s->listener == RBLI_BAD_SOCK) return failf("cannot open the raw peer's TCP listener");
    }
    rbl_config_init(&cfg, "vector ut");
    cfg.control_port = port;
    cfg.udp_port = 0;
    cfg.heartbeat_ms = conf && jv_get(conf, "heartbeat_ms") ? ju32k(conf, "heartbeat_ms") : 250u;
    cfg.timeout_ms = conf && jv_get(conf, "timeout_ms") ? ju32k(conf, "timeout_ms") : 2000u;
    cfg.reconnect_ms = conf && jv_get(conf, "reconnect_ms") ? ju32k(conf, "reconnect_ms") : 200u;
    rc = s->host ? rbl_link_host(&cfg, &s->ut) : rbl_link_guest(&cfg, &s->ut);
    if (rc != RBL_OK) { s->ut = NULL; return failf("cannot create the %s: %s", s->host ? "host" : "guest", rbl_strerror(rc)); }
    s->ut_udp = rbl_link_udp_port(s->ut);
    snprintf(num, sizeof num, "%u", (unsigned)s->peer_udp);
    var_set(s, "peer_udp_port", num);
    snprintf(num, sizeof num, "%u", (unsigned)s->ut_udp);
    var_set(s, "ut_udp_port", num);
    return 0;
}

static int check_session(const JV* c, const JV* dgrams) {
    Sess s;
    const JV* steps = jreq(c, "steps");
    size_t i;
    int ret = 0;
    if (sess_open(&s, c, dgrams) != 0) { sess_close(&s); return 1; }
    for (i = 0; i < steps->n && !ret; i++) {
        ret = run_step(&s, &steps->v[i]);
        if (ret) {
            char pre[80];
            snprintf(pre, sizeof pre, "step %lu (%s)", (unsigned long)i, jstr_or(&steps->v[i], "op", "?"));
            ret = failw(pre);
        }
    }
    sess_close(&s);
    return ret;
}

static void test_sessions(const JV* cases, const JV* dgrams) {
    Report r;
    size_t i;
    memset(&r, 0, sizeof r);
    for (i = 0; i < cases->n; i++) record(&r, &cases->v[i], check_session(&cases->v[i], dgrams));
    finish(&r, "sessions.json");
}

/* ------------------------------------------------------------------ main */

int main(void) {
    uint64_t t0 = rbli_mono_ms(), t1;
    JV control, dgrams, pixels, sessions;
    Report rr;
    JV c;
    if (rbli_net_up() != 0) { fprintf(stderr, "cannot start the network layer\n"); return 2; }
    if (find_vectors_dir() != 0) {
        fprintf(stderr, "test_vectors: cannot find the vectors. Set RBL_VECTORS_DIR, or run from the repository root "
                        "(tests/vectors/link is looked for in the working directory and its parent).\n");
        return 2;
    }
    printf("vectors: %s\n", g_dir);

    memset(&rr, 0, sizeof rr);
    test_sha256_known_answers(&rr);
    c = jv_new(J_OBJ);
    jv_set(&c, "id", jv_cstr("runner.vectors_match_their_checksums"));
    jv_set(&c, "section", jv_cstr("-"));
    record(&rr, &c, check_checksums());
    jv_free(&c);
    finish(&rr, "checksums");

    control = load_doc("control.json");
    dgrams = load_doc("datagrams.json");
    pixels = load_doc("pixelfiles.json");
    sessions = load_doc("sessions.json");
    test_control(&control);
    test_datagrams(&dgrams);
    test_pixel_readers(&pixels);
    test_pixel_writers(&pixels);
    test_sessions(&sessions, &dgrams);
    jv_free(&control); jv_free(&dgrams); jv_free(&pixels); jv_free(&sessions);

    if (g_format_errors) { printf("%d vector format errors (see above)\n", g_format_errors); g_failed_total += g_format_errors; }
    t1 = rbli_mono_ms();
    printf("test_vectors: %s, %lu ms\n", g_failed_total ? "FAILED" : "ok", (unsigned long)(t1 - t0));
    rbli_net_down();
    return g_failed_total ? 1 : 0;
}
