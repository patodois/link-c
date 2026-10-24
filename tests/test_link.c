/* Tests of rewrite_link.h. One translation unit that includes the implementation, so it can also
 * reach the internal (rbli_*) functions. Plain runner: exits 1 if any test fails.
 *
 *   make test            normal build
 *   make asan            the same under -fsanitize=address,undefined
 */
/* strict C99 hides POSIX (clock_gettime, nanosleep, mkfifo, ...): ask for it before the first system header */
#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#endif

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The library's allocator hooks, counting: every allocation the library makes must be freed. */
static size_t g_lib_allocs, g_lib_frees;
static void* t_lib_malloc(size_t n) { void* p = malloc(n); if (p) g_lib_allocs++; return p; }
static void* t_lib_realloc(void* old, size_t n) { void* p = realloc(old, n); if (p && !old) g_lib_allocs++; return p; }
static void t_lib_free(void* p) { if (p) g_lib_frees++; free(p); }
#define REWRITE_LINK_MALLOC(n) t_lib_malloc(n)
#define REWRITE_LINK_REALLOC(p, n) t_lib_realloc((p), (n))
#define REWRITE_LINK_FREE(p) t_lib_free(p)

#define REWRITE_LINK_IMPLEMENTATION
#include "../rewrite_link.h"

#if !RBLI_WINDOWS
#include <pthread.h>
#endif

/* ------------------------------------------------------------------ harness */

static int g_passed, g_failed, g_test_failed;

static void t_fail(const char* file, int line, const char* what) {
    printf("   FAIL %s:%d: %s\n", file, line, what);
    fflush(stdout);
    g_test_failed = 1;
}
#define CHECK(c) do { if (!(c)) t_fail(__FILE__, __LINE__, #c); } while (0)
#define REQUIRE(c) do { if (!(c)) { t_fail(__FILE__, __LINE__, #c); return; } } while (0)
#define CHECK_EQ(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); if (a_ != b_) { \
    char m_[200]; snprintf(m_, sizeof m_, "%s == %s (%lld vs %lld)", #a, #b, a_, b_); t_fail(__FILE__, __LINE__, m_); } } while (0)
#define CHECK_STR(a, b) do { const char* a_ = (a); const char* b_ = (b); if (strcmp(a_, b_) != 0) { \
    char m_[300]; snprintf(m_, sizeof m_, "%s == %s (\"%s\" vs \"%s\")", #a, #b, a_, b_); t_fail(__FILE__, __LINE__, m_); } } while (0)

typedef void (*test_fn)(void);
static void run_test(const char* name, test_fn fn) {
    g_test_failed = 0;
    fn();
    if (g_test_failed) { g_failed++; printf("FAIL %s\n", name); }
    else { g_passed++; printf("ok   %s\n", name); }
    fflush(stdout);
}
#define TEST(name) static void name(void)
#define RUN(name) run_test(#name, name)

#define WAIT_UNTIL(ms, step, cond, result) do { \
    uint64_t end_ = rbli_mono_ms() + (uint64_t)(ms); (result) = 0; \
    for (;;) { step; if (cond) { (result) = 1; break; } if (rbli_mono_ms() >= end_) break; rbli_sleep_ms(2); } } while (0)

static void hex(const char* s, uint8_t* out, size_t* n) {
    size_t k = 0;
    while (*s) {
        int hi, lo;
        if (*s == ' ') { s++; continue; }
#define HV(c) ((c) >= '0' && (c) <= '9' ? (c) - '0' : (c) >= 'A' && (c) <= 'F' ? (c) - 'A' + 10 : (c) - 'a' + 10)
        hi = HV(s[0]); lo = HV(s[1]);
        out[k++] = (uint8_t)(hi * 16 + lo);
        s += 2;
    }
    *n = k;
}

/* the worked example of CONTRACT.md section 11 */
static const char* EXAMPLE_HEARTBEAT = "52 42 4C 4B 01 00 01 00 01 00 CD AB 04 00 00 00 00 40 1E 18 24 0A 06 00";

static uint32_t le32(const uint8_t* b) { return rbli_get32(b); }
static float lef(const uint8_t* b) { return rbli_getf(b); }

static rbl_header mk_header(uint32_t session, uint32_t seq, uint64_t t) {
    rbl_header h;
    memset(&h, 0, sizeof h);
    h.magic = RBL_MAGIC; h.version = 1; h.session = session; h.seq = seq; h.time_us = t;
    return h;
}

/* ------------------------------------------------------------------ layouts and the wire format */

TEST(layout_offsets) {
    CHECK_EQ(sizeof(rbl_header), 24); CHECK_EQ(offsetof(rbl_header, magic), 0); CHECK_EQ(offsetof(rbl_header, version), 4);
    CHECK_EQ(offsetof(rbl_header, kind), 6); CHECK_EQ(offsetof(rbl_header, flags), 7); CHECK_EQ(offsetof(rbl_header, session), 8);
    CHECK_EQ(offsetof(rbl_header, seq), 12); CHECK_EQ(offsetof(rbl_header, time_us), 16);
    CHECK_EQ(sizeof(rbl_camera), 152); CHECK_EQ(offsetof(rbl_camera, view), 0); CHECK_EQ(offsetof(rbl_camera, proj), 64);
    CHECK_EQ(offsetof(rbl_camera, fov_y), 128); CHECK_EQ(offsetof(rbl_camera, near_m), 132); CHECK_EQ(offsetof(rbl_camera, far_m), 136);
    CHECK_EQ(offsetof(rbl_camera, viewport_width), 140); CHECK_EQ(offsetof(rbl_camera, viewport_height), 144);
    CHECK_EQ(offsetof(rbl_camera, depth_range), 148);
    CHECK_EQ(sizeof(rbl_pose_header), 8); CHECK_EQ(offsetof(rbl_pose_header, count), 0); CHECK_EQ(offsetof(rbl_pose_header, reserved), 4);
    CHECK_EQ(sizeof(rbl_pose), 80); CHECK_EQ(offsetof(rbl_pose, entity), 0); CHECK_EQ(offsetof(rbl_pose, flags), 4);
    CHECK_EQ(offsetof(rbl_pose, time_us), 8); CHECK_EQ(offsetof(rbl_pose, translation), 16); CHECK_EQ(offsetof(rbl_pose, rotation), 28);
    CHECK_EQ(offsetof(rbl_pose, scale), 44); CHECK_EQ(offsetof(rbl_pose, velocity), 56); CHECK_EQ(offsetof(rbl_pose, angular_velocity), 68);
    CHECK_EQ(sizeof(rbl_collision), 32); CHECK_EQ(offsetof(rbl_collision, query), 0); CHECK_EQ(offsetof(rbl_collision, chunk_index), 4);
    CHECK_EQ(offsetof(rbl_collision, chunk_count), 6); CHECK_EQ(offsetof(rbl_collision, center), 8); CHECK_EQ(offsetof(rbl_collision, radius), 20);
    CHECK_EQ(offsetof(rbl_collision, triangle_count), 24); CHECK_EQ(offsetof(rbl_collision, reserved), 28);
    CHECK_EQ(sizeof(rbl_triangle), 36);
    CHECK_EQ(sizeof(rbl_frame), 24); CHECK_EQ(offsetof(rbl_frame, frame), 0); CHECK_EQ(offsetof(rbl_frame, slot), 8);
    CHECK_EQ(offsetof(rbl_frame, camera_seq), 12); CHECK_EQ(offsetof(rbl_frame, width), 16); CHECK_EQ(offsetof(rbl_frame, height), 20);
    CHECK_EQ(sizeof(rbl_frame_file), 64); CHECK_EQ(offsetof(rbl_frame_file, magic), 0); CHECK_EQ(offsetof(rbl_frame_file, version), 8);
    CHECK_EQ(offsetof(rbl_frame_file, slots), 12); CHECK_EQ(offsetof(rbl_frame_file, width), 16); CHECK_EQ(offsetof(rbl_frame_file, height), 20);
    CHECK_EQ(offsetof(rbl_frame_file, stride), 24); CHECK_EQ(offsetof(rbl_frame_file, format), 28); CHECK_EQ(offsetof(rbl_frame_file, slot_size), 32);
    CHECK_EQ(offsetof(rbl_frame_file, depth_format), 40); CHECK_EQ(offsetof(rbl_frame_file, depth_stride), 44);
    CHECK_EQ(offsetof(rbl_frame_file, depth_offset), 48); CHECK_EQ(offsetof(rbl_frame_file, reserved), 56);
    CHECK_EQ(sizeof(rbl_frame_slot), 64); CHECK_EQ(offsetof(rbl_frame_slot, lock), 0); CHECK_EQ(offsetof(rbl_frame_slot, frame), 8);
    CHECK_EQ(offsetof(rbl_frame_slot, time_us), 16); CHECK_EQ(offsetof(rbl_frame_slot, camera_seq), 24);
    CHECK_EQ(offsetof(rbl_frame_slot, width), 28); CHECK_EQ(offsetof(rbl_frame_slot, height), 32); CHECK_EQ(offsetof(rbl_frame_slot, reserved), 36);
}

TEST(constants_match_contract) {
    CHECK_EQ(RBL_MAGIC, 0x4B4C4252u); CHECK_EQ(RBL_MAX_POSES, 102); CHECK_EQ(RBL_MAX_TRIANGLES, 226);
    CHECK_EQ(RBL_MAX_POSES, (RBL_MAX_DATAGRAM - 24 - 8) / 80); CHECK_EQ(RBL_MAX_TRIANGLES, (RBL_MAX_DATAGRAM - 24 - 32) / 36);
    CHECK_EQ(RBL_DEFAULT_CONTROL_PORT, 47800); CHECK_EQ(RBL_MAX_DATAGRAM, 8192); CHECK_EQ(RBL_MAX_ENTITIES, 1024);
    CHECK_EQ(RBL_MAX_FRAME_DIM, 16384); CHECK_EQ(RBL_MAX_FRAME_PIXELS, 67108864u); CHECK_EQ(RBL_COLLISION_PER_POLL, 32);
    CHECK_EQ(RBL_HEARTBEAT, 1); CHECK_EQ(RBL_CAMERA, 2); CHECK_EQ(RBL_POSE, 3); CHECK_EQ(RBL_COLLISION, 4); CHECK_EQ(RBL_FRAME, 5);
    CHECK_EQ(RBL_DEPTH_REVERSED, 2); CHECK_EQ(RBL_DEPTH_LINEAR_F32, 1); CHECK_EQ(RBL_POSE_TELEPORT, 1);
}

TEST(worked_example_encodes_byte_for_byte) {
    uint8_t want[64], got[64];
    size_t n;
    rbl_header h = mk_header(0xABCD0001u, 4, 1700000000000000ull);
    int len;
    hex(EXAMPLE_HEARTBEAT, want, &n);
    CHECK_EQ(n, 24);
    len = rbl_wire_encode_heartbeat(&h, got, sizeof got);
    REQUIRE(len == 24);
    CHECK(memcmp(want, got, 24) == 0);
    CHECK(memcmp(got, "RBLK", 4) == 0);
}

TEST(worked_example_decodes_byte_for_byte) {
    uint8_t b[64];
    size_t n;
    rbl_decoded d;
    hex(EXAMPLE_HEARTBEAT, b, &n);
    REQUIRE(rbl_wire_decode(b, n, &d) == RBL_OK);
    CHECK_EQ(d.header.kind, RBL_HEARTBEAT); CHECK_EQ(d.header.version, 1); CHECK_EQ(d.header.flags, 0);
    CHECK_EQ(d.header.session, 0xABCD0001u); CHECK_EQ(d.header.seq, 4); CHECK(d.header.time_us == 1700000000000000ull);
}

TEST(camera_datagram_offsets) {
    rbl_camera c, back;
    uint8_t b[256];
    rbl_decoded d;
    int i, len;
    memset(&c, 0, sizeof c);
    for (i = 0; i < 16; i++) { c.view[i] = (float)(i + 1); c.proj[i] = (float)(100 + i); }
    c.fov_y = 1.0f; c.near_m = 0.1f; c.far_m = (float)HUGE_VAL; c.viewport_width = 1280; c.viewport_height = 720;
    c.depth_range = RBL_DEPTH_REVERSED;
    {   rbl_header h = mk_header(7, 9, 5);
        len = rbl_wire_encode_camera(&h, &c, b, sizeof b); }
    CHECK_EQ(len, 176);
    CHECK(lef(b + 24) == 1.0f); CHECK(lef(b + 24 + 15 * 4) == 16.0f); CHECK(lef(b + 24 + 64) == 100.0f);
    CHECK(lef(b + 24 + 128) == 1.0f); CHECK(lef(b + 24 + 132) == 0.1f); CHECK(lef(b + 24 + 136) > 1e38f);
    CHECK_EQ(le32(b + 24 + 140), 1280); CHECK_EQ(le32(b + 24 + 144), 720); CHECK_EQ(le32(b + 24 + 148), 2);
    REQUIRE(rbl_wire_decode(b, (size_t)len, &d) == RBL_OK);
    back = d.u.camera;
    CHECK(memcmp(back.view, c.view, sizeof c.view) == 0); CHECK(memcmp(back.proj, c.proj, sizeof c.proj) == 0);
    CHECK_EQ(back.depth_range, RBL_DEPTH_REVERSED);
    {   rbl_header h = mk_header(7, 9, 5);
        c.depth_range = 3;
        CHECK_EQ(rbl_wire_encode_camera(&h, &c, b, sizeof b), RBL_ERR_MALFORMED); }
}

TEST(pose_datagram_offsets_and_length) {
    rbl_pose p, q;
    rbl_pose two[2];
    uint8_t b[8192];
    rbl_decoded d;
    rbl_header h = mk_header(7, 1, 2);
    int len;
    memset(&p, 0, sizeof p);
    p.entity = 0xFFFFFFFEu; p.flags = RBL_POSE_TELEPORT; p.time_us = 1700000000123456ull;
    p.translation[0] = 1; p.translation[1] = 2; p.translation[2] = 3;
    p.rotation[1] = 0.5f; p.rotation[3] = 0.5f; p.scale[0] = 1; p.scale[1] = 1; p.scale[2] = 2;
    p.velocity[0] = 4; p.velocity[1] = 5; p.velocity[2] = 6; p.angular_velocity[0] = 7; p.angular_velocity[1] = 8; p.angular_velocity[2] = 9;
    two[0] = p; two[1] = p;
    len = rbl_wire_encode_poses(&h, two, 2, b, sizeof b);
    CHECK_EQ(len, 24 + 8 + 160);
    CHECK_EQ(le32(b + 24), 2);
    CHECK_EQ(le32(b + 32), 0xFFFFFFFEu); CHECK_EQ(le32(b + 36), 1); CHECK(rbli_get64(b + 40) == 1700000000123456ull);
    CHECK(lef(b + 32 + 16) == 1.0f); CHECK(lef(b + 32 + 28 + 4) == 0.5f); CHECK(lef(b + 32 + 28 + 12) == 0.5f);
    CHECK(lef(b + 32 + 44 + 8) == 2.0f); CHECK(lef(b + 32 + 56) == 4.0f); CHECK(lef(b + 32 + 68 + 8) == 9.0f);
    REQUIRE(rbl_wire_decode(b, (size_t)len, &d) == RBL_OK);
    CHECK_EQ(d.u.poses.count, 2);
    rbl_wire_pose(&d, 1, &q);
    CHECK(memcmp(&q, &p, sizeof p) == 0);
}

TEST(pose_full_datagram_is_8192_bytes) {
    rbl_pose ps[102];
    uint8_t b[8192];
    rbl_header h = mk_header(1, 0, 0);
    int i;
    for (i = 0; i < 102; i++) rbl_pose_identity(&ps[i], (uint32_t)i, 10);
    CHECK_EQ(rbl_wire_encode_poses(&h, ps, 102, b, sizeof b), 8192);
    CHECK_EQ(rbl_wire_encode_poses(&h, ps, 103, b, sizeof b), RBL_ERR_MALFORMED);
    CHECK_EQ(rbl_wire_encode_poses(&h, ps, 0, b, sizeof b), RBL_ERR_MALFORMED);
    CHECK_EQ(rbl_wire_encode_poses(&h, ps, 102, b, 8191), RBL_ERR_TOO_BIG);
}

TEST(collision_chunking_offsets_and_length) {
    enum { N = 800 };
    static rbl_triangle tris[N];
    uint8_t b[8192];
    rbl_decoded d;
    rbl_collision c;
    rbl_triangle t0, t1;
    rbl_header h = mk_header(1, 0, 0);
    int i, len;
    size_t last;
    for (i = 0; i < N; i++) {
        float f[9] = {(float)i, 0, 0, (float)i, 0, 1, (float)i + 1, 0, 1};
        memcpy(&tris[i], f, sizeof f);
    }
    CHECK_EQ(rbl_wire_chunk_count(N), 4); CHECK_EQ(rbl_wire_chunk_count(0), 1); CHECK_EQ(rbl_wire_chunk_count(226), 1);
    CHECK_EQ(rbl_wire_chunk_count(227), 2);
    memset(&c, 0, sizeof c);
    c.query = 5; c.chunk_index = 3; c.chunk_count = 4; c.center[0] = 1; c.center[1] = 2; c.center[2] = 3; c.radius = 10;
    last = N - 3 * 226;
    len = rbl_wire_encode_collision(&h, &c, tris + 3 * 226, last, b, sizeof b);
    CHECK_EQ(len, 24 + 32 + 36 * (int)last);
    CHECK_EQ(le32(b + 24), 5);
    CHECK_EQ(b[28] | (b[29] << 8), 3); CHECK_EQ(b[30] | (b[31] << 8), 4);
    CHECK(lef(b + 32) == 1.0f); CHECK(lef(b + 44) == 10.0f); CHECK_EQ(le32(b + 48), last);
    REQUIRE(rbl_wire_decode(b, (size_t)len, &d) == RBL_OK);
    rbl_wire_triangle(&d, 0, &t0);
    t1 = tris[3 * 226];
    CHECK(memcmp(&t0, &t1, sizeof t0) == 0);
    c.chunk_index = 4;
    CHECK_EQ(rbl_wire_encode_collision(&h, &c, tris, 1, b, sizeof b), RBL_ERR_MALFORMED);
    c.chunk_index = 0; c.chunk_count = 0;
    CHECK_EQ(rbl_wire_encode_collision(&h, &c, tris, 1, b, sizeof b), RBL_ERR_MALFORMED);
    c.chunk_count = 1;
    CHECK_EQ(rbl_wire_encode_collision(&h, &c, tris, 227, b, sizeof b), RBL_ERR_MALFORMED);
    CHECK_EQ(rbl_wire_encode_collision(&h, &c, NULL, 0, b, sizeof b), 24 + 32);
}

TEST(frame_datagram_is_48_bytes) {
    rbl_frame f, g;
    uint8_t b[64];
    rbl_decoded d;
    rbl_header h = mk_header(1, 0, 0);
    f.frame = 3; f.slot = 0; f.camera_seq = 77; f.width = 64; f.height = 36;
    CHECK_EQ(rbl_wire_encode_frame(&h, &f, b, sizeof b), 48);
    REQUIRE(rbl_wire_decode(b, 48, &d) == RBL_OK);
    g = d.u.frame;
    CHECK(memcmp(&f, &g, sizeof f) == 0);
    f.frame = 0;
    CHECK_EQ(rbl_wire_encode_frame(&h, &f, b, sizeof b), RBL_ERR_MALFORMED);
}

TEST(malformed_datagrams_are_rejected) {
    uint8_t ok[64], b[400], cam[176], pose[112], fr[48];
    size_t n;
    rbl_decoded d;
    rbl_camera c;
    rbl_pose p;
    rbl_frame f;
    rbl_header h = mk_header(1, 0, 0);
    float eye[3] = {0, 0, 5}, tgt[3] = {0, 0, 0};
    hex(EXAMPLE_HEARTBEAT, ok, &n);
    CHECK_EQ(rbl_wire_decode(ok, 23, &d), RBL_ERR_MALFORMED);
    memcpy(b, ok, n); b[0] = 'X';
    CHECK_EQ(rbl_wire_decode(b, n, &d), RBL_ERR_MALFORMED);
    memcpy(b, ok, n); b[4] = 2;
    CHECK_EQ(rbl_wire_decode(b, n, &d), RBL_ERR_MALFORMED);
    memcpy(b, ok, n); b[24] = 0;
    CHECK_EQ(rbl_wire_decode(b, 25, &d), RBL_ERR_MALFORMED);
    rbl_camera_look_at(&c, eye, tgt, 1.0f, 0.1f, 100.0f, 16, 9);
    rbl_wire_encode_camera(&h, &c, cam, sizeof cam);
    CHECK_EQ(rbl_wire_decode(cam, 175, &d), RBL_ERR_MALFORMED);
    memcpy(b, cam, sizeof cam); b[24 + 148] = 3;
    CHECK_EQ(rbl_wire_decode(b, 176, &d), RBL_ERR_MALFORMED);
    rbl_pose_identity(&p, 1, 1);
    rbl_wire_encode_poses(&h, &p, 1, pose, sizeof pose);
    memcpy(b, pose, sizeof pose); b[24] = 0;
    CHECK_EQ(rbl_wire_decode(b, sizeof pose, &d), RBL_ERR_MALFORMED);
    memcpy(b, pose, sizeof pose); b[24] = 2;
    CHECK_EQ(rbl_wire_decode(b, sizeof pose, &d), RBL_ERR_MALFORMED);
    f.frame = 1; f.slot = 0; f.camera_seq = 0; f.width = 1; f.height = 1;
    rbl_wire_encode_frame(&h, &f, fr, sizeof fr);
    memset(fr + 24, 0, 8);
    CHECK_EQ(rbl_wire_decode(fr, sizeof fr, &d), RBL_ERR_MALFORMED);
    memcpy(b, ok, n); b[6] = 99; /* an unknown kind is not an error */
    CHECK_EQ(rbl_wire_decode(b, n, &d), RBL_OK);
    CHECK_EQ(d.header.kind, 99);
}

/* A small deterministic generator, so a failure reproduces. */
static uint64_t g_rng = 42;
static uint32_t rnd(void) {
    g_rng = g_rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(g_rng >> 33);
}

TEST(fuzz_datagrams_never_crash) {
    uint8_t ok[64], valid[4][8192];
    size_t okn, vlen[4];
    long i, accepted = 0, rejected = 0;
    rbl_header h = mk_header(1, 0, 0);
    rbl_camera c;
    rbl_pose ps[3];
    rbl_collision col;
    rbl_triangle tris[5];
    rbl_frame f;
    float eye[3] = {0, 0, 5}, tgt[3] = {0, 0, 0};
    hex(EXAMPLE_HEARTBEAT, ok, &okn);
    rbl_camera_look_at(&c, eye, tgt, 1.0f, 0.1f, 100.0f, 16, 9);
    vlen[0] = (size_t)rbl_wire_encode_camera(&h, &c, valid[0], sizeof valid[0]);
    { int k; for (k = 0; k < 3; k++) rbl_pose_identity(&ps[k], (uint32_t)k, 5);
      vlen[1] = (size_t)rbl_wire_encode_poses(&h, ps, 3, valid[1], sizeof valid[1]);
      for (k = 0; k < 5; k++) memset(&tris[k], k, sizeof tris[k]); }
    memset(&col, 0, sizeof col); col.query = 3; col.chunk_index = 1; col.chunk_count = 2; col.radius = 4;
    vlen[2] = (size_t)rbl_wire_encode_collision(&h, &col, tris, 5, valid[2], sizeof valid[2]);
    f.frame = 9; f.slot = 1; f.camera_seq = 4; f.width = 8; f.height = 8;
    vlen[3] = (size_t)rbl_wire_encode_frame(&h, &f, valid[3], sizeof valid[3]);
    /* 20000 random buffers, half of them with a plausible header (as the Java fuzz) */
    for (i = 0; i < 20000; i++) {
        uint8_t junk[300];
        size_t len = rnd() % 300, k;
        rbl_decoded d;
        for (k = 0; k < len; k++) junk[k] = (uint8_t)rnd();
        if (len >= 8 && (rnd() & 1)) { memcpy(junk, ok, 6); junk[6] = (uint8_t)(1 + rnd() % 6); }
        if (rbl_wire_decode(junk, len, &d) == RBL_OK) accepted++; else rejected++;
    }
    /* every prefix of every valid datagram, and 20000 mutations of them */
    {   int v;
        for (v = 0; v < 4; v++) {
            size_t cut;
            for (cut = 0; cut <= vlen[v]; cut++) {
                rbl_decoded d;
                int r = rbl_wire_decode(valid[v], cut, &d);
                CHECK((cut == vlen[v]) == (r == RBL_OK));
            }
        }
    }
    for (i = 0; i < 20000; i++) {
        uint8_t m[8192];
        int v = (int)(rnd() % 4);
        size_t len = vlen[v], flips = 1 + rnd() % 4, k;
        rbl_decoded d;
        memcpy(m, valid[v], len);
        for (k = 0; k < flips; k++) m[rnd() % len] = (uint8_t)rnd();
        if (rnd() % 8 == 0) len = rnd() % (len + 1);
        if (rbl_wire_decode(m, len, &d) == RBL_OK) {
            /* what is accepted must be readable without leaving the buffer */
            if (d.header.kind == RBL_POSE) { uint32_t j; rbl_pose p; for (j = 0; j < d.u.poses.count; j++) rbl_wire_pose(&d, j, &p); }
            if (d.header.kind == RBL_COLLISION) { uint32_t j; rbl_triangle t; for (j = 0; j < d.u.collision.triangle_count; j++) rbl_wire_triangle(&d, j, &t); }
            accepted++;
        } else rejected++;
    }
    printf("     fuzz: %ld accepted, %ld rejected\n", accepted, rejected);
    CHECK(rejected > 20000);
}

TEST(serial_arithmetic_and_negotiation) {
    CHECK(rbl_seq_newer(1, 0)); CHECK(!rbl_seq_newer(0, 1)); CHECK(rbl_seq_newer(0, 0xFFFFFFFFu)); CHECK(!rbl_seq_newer(5, 5));
    CHECK(rbl_seq_newer(0x80000000u - 1, 0)); CHECK(!rbl_seq_newer(0x80000000u, 0));
    CHECK_EQ(rbl_negotiate(1, 1, 1, 1), 1); CHECK_EQ(rbl_negotiate(1, 3, 1, 2), 2); CHECK_EQ(rbl_negotiate(2, 3, 1, 1), -1);
}

TEST(camera_helpers_and_quaternions) {
    rbl_camera c;
    float eye[3] = {3, 4, 5}, tgt[3] = {3, 4, 0}, e[3], f[3], z[3] = {0, 0, 1}, y[3] = {0, 1, 0}, q[4], r[3], t[3] = {0.3f, 0.2f, -0.9f};
    float n, back[3], flip[4], neg[3] = {0, 0, -1};
    int i;
    rbl_camera_look_at(&c, eye, tgt, 1.0f, 0.1f, 100.0f, 16, 9);
    rbl_camera_eye(&c, e); rbl_camera_forward(&c, f);
    for (i = 0; i < 3; i++) CHECK(fabsf(e[i] - eye[i]) < 1e-4f);
    CHECK(fabsf(f[2] + 1) < 1e-5f && fabsf(f[0]) < 1e-5f);
    rbl_quat_axis_angle(q, y, (float)(3.14159265358979 / 2));
    rbl_quat_rotate(r, q, z);
    CHECK(fabsf(r[0] - 1) < 1e-5f && fabsf(r[1]) < 1e-5f && fabsf(r[2]) < 1e-5f);
    rbl_quat_from_to(q, z, t);
    rbl_quat_rotate(r, q, z);
    n = sqrtf(0.09f + 0.04f + 0.81f);
    for (i = 0; i < 3; i++) CHECK(fabsf(r[i] - t[i] / n) < 1e-5f);
    rbl_quat_from_to(flip, z, neg);
    rbl_quat_rotate(back, flip, z);
    CHECK(fabsf(back[2] + 1) < 1e-5f);
    /* an infinite far plane */
    rbl_camera_look_at(&c, eye, tgt, 1.0f, 0.1f, (float)HUGE_VAL, 16, 9);
    CHECK(c.proj[10] == -1.0f && fabsf(c.proj[14] + 0.2f) < 1e-6f);
}

/* ------------------------------------------------------------------ JSON and control lines */

static int json_accepts(const char* doc) {
    size_t n = strlen(doc);
    char* buf = (char*)malloc(n + 1);
    rbli_jp p;
    int ok;
    memcpy(buf, doc, n + 1);
    p.s = buf; p.n = n; p.i = 0; p.why = "";
    rbli_jws(&p);
    ok = rbli_jvalue(&p, 0, NULL, NULL, NULL, 0) == 0;
    if (ok) { rbli_jws(&p); ok = p.i == n; }
    free(buf);
    return ok;
}

/* The documents and whether serde_json (the Rust side) accepts them: the table of link-java's tests. */
static const struct { const char* doc; int ok; } JSON_CORPUS[] = {
    /* numbers */
    {"0", 1}, {"-0", 1}, {"-1", 1}, {"1.5", 1}, {"1e5", 1}, {"1E+5", 1}, {"1e-5", 1}, {"0e0", 1}, {"-0.0", 1},
    {"1e-999", 1}, {"123456789012345678901234567890", 1},
    {"01", 0}, {"-01", 0}, {"00", 0}, {"1.", 0}, {"1.e5", 0}, {"-.5", 0}, {".5", 0}, {"1e", 0}, {"1.0e+", 0}, {"+1", 0},
    {"-", 0}, {"- 1", 0}, {"0x10", 0}, {"1e999", 0}, {"Infinity", 0}, {"NaN", 0},
    /* string escapes */
    {"\"\\u0041\"", 1}, {"\"\\u00E9\"", 1}, {"\"\\u0000\"", 1}, {"\"\\/\"", 1}, {"\"\\ud800\\udc00\"", 1},
    {"\"\\ud83d\\ude00\"", 1}, {"\"\\uD834\\uDD1E\"", 1},
    {"\"\\u004\"", 0}, {"\"\\u00zz\"", 0}, {"\"\\u+041\"", 0}, {"\"\\u-041\"", 0}, {"\"\\u D800\"", 0}, {"\"\\ud800\"", 0},
    {"\"\\udc00\"", 0}, {"\"\\ud800x\"", 0}, {"\"\\ud800\\u0041\"", 0}, {"\"\\ud800\\ud800\"", 0}, {"\"\\udc00\\ud800\"", 0},
    {"\"\\x41\"", 0}, {"\"\\U0041\"", 0}, {"\"a\nb\"", 0},
    /* structure */
    {"true", 1}, {"[true,false,null]", 1}, {"\t1\n", 1}, {"truex", 0}, {"tru", 0}, {"[1,]", 0}, {"[,1]", 0},
    {"{\"a\":1,}", 0}, {"{\"a\":1 \"b\":2}", 0}, {"{'a':1}", 0}, {"\xEF\xBB\xBF" "1", 0},
};

TEST(json_matches_serde_json) {
    size_t i;
    int n;
    for (i = 0; i < sizeof JSON_CORPUS / sizeof JSON_CORPUS[0]; i++) {
        if (json_accepts(JSON_CORPUS[i].doc) != JSON_CORPUS[i].ok) {
            char m[200];
            snprintf(m, sizeof m, "\"%s\" should be %s", JSON_CORPUS[i].doc, JSON_CORPUS[i].ok ? "accepted" : "rejected");
            t_fail(__FILE__, __LINE__, m);
        }
    }
    {   int sizes[] = {1, 32, 33, 100, 127, 128, 200};
        size_t k;
        for (k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
            char* arrays = (char*)malloc(4096);
            char* objects = (char*)malloc(4096);
            n = sizes[k];
            memset(arrays, '[', (size_t)n); memset(arrays + n, ']', (size_t)n); arrays[2 * n] = 0;
            objects[0] = 0;
            { int j; for (j = 0; j < n; j++) strcat(objects, "{\"a\":"); strcat(objects, "1"); for (j = 0; j < n; j++) strcat(objects, "}"); }
            CHECK_EQ(json_accepts(arrays), n <= 127);
            CHECK_EQ(json_accepts(objects), n <= 127);
            free(arrays); free(objects);
        }
    }
}

static char g_pbuf[2][131072];
static int g_pi;

static int parse_line(const char* s, rbl_control* out, const char** why) {
    char* b = g_pbuf[g_pi ^= 1];
    size_t n = strlen(s);
    memcpy(b, s, n + 1);
    return rbl_control_parse(b, n, out, why);
}

static int str_is(rbl_str s, const char* lit) { return s.len == strlen(lit) && memcmp(s.ptr, lit, s.len) == 0; }

TEST(control_contract_examples_parse) {
    rbl_control c;
    const char* why = "";
    REQUIRE(parse_line("{\"type\":\"hello\",\"role\":\"guest\",\"game\":\"Warframe rewrite\",\"protocol\":1,\"min_protocol\":1,"
                       "\"udp_port\":53001,\"heartbeat_ms\":250,\"timeout_ms\":2000}\n", &c, &why) == RBL_OK);
    CHECK_EQ(c.type, RBL_CTL_HELLO); CHECK_EQ(c.u.hello.role, RBL_ROLE_GUEST); CHECK(str_is(c.u.hello.game, "Warframe rewrite"));
    CHECK_EQ(c.u.hello.protocol, 1); CHECK_EQ(c.u.hello.udp_port, 53001); CHECK_EQ(c.u.hello.heartbeat_ms, 250); CHECK(!c.u.hello.has_session);
    REQUIRE(parse_line("{\"type\":\"hello\",\"role\":\"host\",\"game\":\"War Thunder rewrite\",\"protocol\":1,\"min_protocol\":1,"
                       "\"udp_port\":53000,\"heartbeat_ms\":250,\"timeout_ms\":2000,\"session\":2882400001}\r\n", &c, &why) == RBL_OK);
    CHECK(c.u.hello.has_session); CHECK_EQ(c.u.hello.session, 2882400001u);
    REQUIRE(parse_line("{\"type\":\"spawn\",\"entity\":7,\"provider\":\"Warframe\",\"asset\":\"/Lotus/Characters/Excalibur\","
                       "\"name\":\"Excalibur\"}", &c, &why) == RBL_OK);
    CHECK_EQ(c.type, RBL_CTL_SPAWN); CHECK_EQ(c.u.spawn.entity, 7); CHECK(str_is(c.u.spawn.provider, "Warframe"));
    CHECK(str_is(c.u.spawn.asset, "/Lotus/Characters/Excalibur")); CHECK(c.u.spawn.has_name); CHECK(str_is(c.u.spawn.name, "Excalibur"));
    REQUIRE(parse_line("{\"type\":\"despawn\",\"entity\":7}", &c, &why) == RBL_OK);
    CHECK_EQ(c.type, RBL_CTL_DESPAWN); CHECK_EQ(c.u.despawn.entity, 7);
    REQUIRE(parse_line("{\"type\":\"input\",\"focus\":\"guest\"}", &c, &why) == RBL_OK);
    CHECK_EQ(c.type, RBL_CTL_INPUT); CHECK_EQ(c.u.input.focus, RBL_ROLE_GUEST); CHECK(!c.u.input.has_reason);
    REQUIRE(parse_line("{\"type\":\"bye\",\"reason\":\"shutting down\"}", &c, &why) == RBL_OK);
    CHECK_EQ(c.type, RBL_CTL_BYE); CHECK(str_is(c.u.bye.reason, "shutting down"));
    REQUIRE(parse_line("{\"type\":\"bye\"}", &c, &why) == RBL_OK);
    CHECK(str_is(c.u.bye.reason, ""));
    REQUIRE(parse_line("{\"type\":\"frame_channel\",\"path\":\"/tmp/x.frames\",\"width\":1280,\"height\":720,\"slots\":3}", &c, &why) == RBL_OK);
    CHECK_EQ(c.type, RBL_CTL_FRAME_CHANNEL); CHECK(str_is(c.u.frame_channel.path, "/tmp/x.frames")); CHECK_EQ(c.u.frame_channel.width, 1280);
    REQUIRE(parse_line("{\"type\":\"future_thing\",\"x\":1}", &c, &why) == RBL_OK);
    CHECK_EQ(c.type, RBL_CTL_UNKNOWN); CHECK(str_is(c.unknown_type, "future_thing"));
    REQUIRE(parse_line("{\"type\":\"despawn\",\"entity\":7,\"extra\":[1,{\"a\":null}]}", &c, &why) == RBL_OK);
    CHECK_EQ(c.u.despawn.entity, 7);
    /* the last of a repeated key wins, as in serde_json */
    REQUIRE(parse_line("{\"type\":\"despawn\",\"entity\":1,\"entity\":2}", &c, &why) == RBL_OK);
    CHECK_EQ(c.u.despawn.entity, 2);
    REQUIRE(parse_line("{\"type\":\"hello\",\"role\":\"guest\",\"game\":\"g\",\"protocol\":1,\"min_protocol\":1,\"udp_port\":5,"
                       "\"heartbeat_ms\":250,\"timeout_ms\":2000,\"session\":null}", &c, &why) == RBL_OK);
    CHECK(!c.u.hello.has_session);
    /* a type with an embedded NUL is not "hello" */
    REQUIRE(parse_line("{\"type\":\"hello\\u0000x\"}", &c, &why) == RBL_OK);
    CHECK_EQ(c.type, RBL_CTL_UNKNOWN);
    /* escapes decode in place */
    REQUIRE(parse_line("{\"type\":\"spawn\",\"entity\":1,\"provider\":\"a\\\"b\",\"asset\":\"\\u00e9\\ud83d\\ude00\\n\"}", &c, &why) == RBL_OK);
    CHECK(str_is(c.u.spawn.provider, "a\"b")); CHECK(str_is(c.u.spawn.asset, "\xC3\xA9\xF0\x9F\x98\x80\n"));
}

TEST(control_broken_lines_are_protocol_errors) {
    static const char* const bad[] = {
        "not json", "[]", "{\"no\":\"type\"}", "{\"type\":5}", "{\"type\":\"despawn\"}", "{\"type\":\"despawn\",\"entity\":4294967296}",
        "{\"type\":\"despawn\",\"entity\":-1}", "{\"type\":\"despawn\",\"entity\":1.5}", "{\"type\":\"despawn\",\"entity\":-0}",
        "{\"type\":\"despawn\",\"entity\":1e2}", "{\"type\":\"despawn\",\"entity\":\"7\"}", "{\"type\":\"input\",\"focus\":\"both\"}",
        "{\"type\":\"hello\",\"role\":\"host\",\"game\":\"g\",\"protocol\":70000,\"min_protocol\":1,\"udp_port\":1,\"heartbeat_ms\":1,\"timeout_ms\":1}",
        "{\"type\":\"hello\",\"role\":\"host\",\"game\":\"g\",\"protocol\":1,\"min_protocol\":1,\"udp_port\":65536,\"heartbeat_ms\":1,\"timeout_ms\":1}",
        "{\"type\":\"hello\",\"role\":\"nobody\",\"game\":\"g\",\"protocol\":1,\"min_protocol\":1,\"udp_port\":1,\"heartbeat_ms\":1,\"timeout_ms\":1}",
        "{\"type\":\"spawn\",\"entity\":1,\"provider\":\"p\"}", "{\"type\":\"spawn\",\"entity\":1,\"provider\":\"p\",\"asset\":\"a\",\"name\":5}",
        "{\"type\":\"bye\",\"reason\":5}", "{\"type\":\"frame_channel\",\"path\":\"/x\",\"width\":1,\"height\":1}",
        "{\"type\":\"despawn\",\"entity\":1} trailing", "{\"type\":\"despawn\",\"entity\":1}{", "", "   ", "{", "}", "\"type\"", "null",
    };
    size_t i;
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        rbl_control c;
        const char* why = NULL;
        if (parse_line(bad[i], &c, &why) != RBL_ERR_MALFORMED || !why) {
            char m[300];
            snprintf(m, sizeof m, "should be a protocol error: %s", bad[i]);
            t_fail(__FILE__, __LINE__, m);
        }
    }
    {   /* a line of 65536 bytes or more is too long, one byte less is not */
        rbl_control c;
        const char* why;
        char* big = (char*)malloc(RBL_MAX_LINE + 64);
        size_t n;
        memset(big, ' ', RBL_MAX_LINE + 32);
        memcpy(big, "{\"type\":\"x\"}", 12);
        n = RBL_MAX_LINE - 1;
        big[n] = 0;
        memcpy(g_pbuf[0], big, n + 1);
        CHECK_EQ(rbl_control_parse(g_pbuf[0], n, &c, &why), RBL_OK);
        n = RBL_MAX_LINE;
        memcpy(g_pbuf[0], big, n); g_pbuf[0][n] = 0;
        CHECK_EQ(rbl_control_parse(g_pbuf[0], n, &c, &why), RBL_ERR_MALFORMED);
        free(big);
    }
}

static int ctl_equal(const rbl_control* a, const rbl_control* b) {
    if (a->type != b->type) return 0;
    switch (a->type) {
    case RBL_CTL_HELLO:
        return a->u.hello.role == b->u.hello.role && str_is(a->u.hello.game, b->u.hello.game.ptr) && a->u.hello.protocol == b->u.hello.protocol &&
               a->u.hello.min_protocol == b->u.hello.min_protocol && a->u.hello.udp_port == b->u.hello.udp_port &&
               a->u.hello.heartbeat_ms == b->u.hello.heartbeat_ms && a->u.hello.timeout_ms == b->u.hello.timeout_ms &&
               a->u.hello.has_session == b->u.hello.has_session && a->u.hello.session == b->u.hello.session;
    case RBL_CTL_SPAWN:
        return a->u.spawn.entity == b->u.spawn.entity && str_is(a->u.spawn.provider, b->u.spawn.provider.ptr) &&
               str_is(a->u.spawn.asset, b->u.spawn.asset.ptr) && a->u.spawn.has_name == b->u.spawn.has_name &&
               (!a->u.spawn.has_name || str_is(a->u.spawn.name, b->u.spawn.name.ptr));
    case RBL_CTL_DESPAWN: return a->u.despawn.entity == b->u.despawn.entity;
    case RBL_CTL_INPUT: return a->u.input.focus == b->u.input.focus && a->u.input.has_reason == b->u.input.has_reason &&
                               (!a->u.input.has_reason || str_is(a->u.input.reason, b->u.input.reason.ptr));
    case RBL_CTL_BYE: return str_is(a->u.bye.reason, b->u.bye.reason.ptr);
    case RBL_CTL_FRAME_CHANNEL:
        return str_is(a->u.frame_channel.path, b->u.frame_channel.path.ptr) && a->u.frame_channel.width == b->u.frame_channel.width &&
               a->u.frame_channel.height == b->u.frame_channel.height && a->u.frame_channel.slots == b->u.frame_channel.slots;
    default: return 1;
    }
}

TEST(control_lines_round_trip) {
    rbl_control in[6], back;
    char line[1024];
    int i, n;
    memset(in, 0, sizeof in);
    in[0].type = RBL_CTL_HELLO; in[0].u.hello.role = RBL_ROLE_HOST; in[0].u.hello.game.ptr = "Host \"one\"\\"; in[0].u.hello.game.len = strlen(in[0].u.hello.game.ptr);
    in[0].u.hello.protocol = 1; in[0].u.hello.min_protocol = 1; in[0].u.hello.udp_port = 53000; in[0].u.hello.heartbeat_ms = 250;
    in[0].u.hello.timeout_ms = 2000; in[0].u.hello.has_session = 1; in[0].u.hello.session = 0xABCD0001u;
    in[1].type = RBL_CTL_SPAWN; in[1].u.spawn.entity = 1; in[1].u.spawn.provider.ptr = "a\"b"; in[1].u.spawn.provider.len = 3;
    in[1].u.spawn.asset.ptr = "c\\d\n\t\x01"; in[1].u.spawn.asset.len = strlen(in[1].u.spawn.asset.ptr);
    in[2].type = RBL_CTL_DESPAWN; in[2].u.despawn.entity = 4294967295u;
    in[3].type = RBL_CTL_INPUT; in[3].u.input.focus = RBL_ROLE_HOST; in[3].u.input.has_reason = 1; in[3].u.input.reason.ptr = "why"; in[3].u.input.reason.len = 3;
    in[4].type = RBL_CTL_BYE; in[4].u.bye.reason.ptr = "\xC3\xA9 \xC3\xBC \xE6\x97\xA5\xE6\x9C\xAC"; in[4].u.bye.reason.len = strlen(in[4].u.bye.reason.ptr);
    in[5].type = RBL_CTL_FRAME_CHANNEL; in[5].u.frame_channel.path.ptr = "/tmp/a b.frames"; in[5].u.frame_channel.path.len = 15;
    in[5].u.frame_channel.width = 64; in[5].u.frame_channel.height = 32; in[5].u.frame_channel.slots = 3;
    for (i = 0; i < 6; i++) {
        const char* why;
        n = rbl_control_format(&in[i], line, sizeof line);
        REQUIRE(n > 0);
        CHECK(line[n - 1] == '\n' && memchr(line, '\n', (size_t)n) == line + n - 1);
        line[n] = 0;
        if (parse_line(line, &back, &why) != RBL_OK) { char m[300]; snprintf(m, sizeof m, "re-parse failed: %s (%s)", line, why); t_fail(__FILE__, __LINE__, m); continue; }
        if (!ctl_equal(&in[i], &back)) { char m[300]; snprintf(m, sizeof m, "round trip differs: %s", line); t_fail(__FILE__, __LINE__, m); }
    }
    CHECK_EQ(rbl_control_format(&in[0], line, 20), RBL_ERR_TOO_BIG);
    /* invalid UTF-8 in a string we send is repaired, so the peer's from_utf8 never fails */
    memset(&in[1], 0, sizeof in[1]);
    in[1].type = RBL_CTL_BYE; in[1].u.bye.reason.ptr = "a\xFF" "b\xC3"; in[1].u.bye.reason.len = 4;
    n = rbl_control_format(&in[1], line, sizeof line);
    REQUIRE(n > 0);
    CHECK(rbli_utf8_valid((const uint8_t*)line, (size_t)n));
    CHECK(strstr(line, "a\xEF\xBF\xBD" "b\xEF\xBF\xBD") != NULL);
}

TEST(utf8_validation_is_strict) {
    static const struct { const char* s; size_t n; int ok; } cases[] = {
        {"abc", 3, 1}, {"\xC3\xA9", 2, 1}, {"\xE2\x82\xAC", 3, 1}, {"\xF0\x9F\x98\x80", 4, 1}, {"\xC0\x80", 2, 0}, {"\xC1\xBF", 2, 0},
        {"\xE0\x80\x80", 3, 0}, {"\xED\xA0\x80", 3, 0}, {"\xF4\x90\x80\x80", 4, 0}, {"\xF5\x80\x80\x80", 4, 0}, {"\x80", 1, 0},
        {"\xC3", 1, 0}, {"\xE2\x82", 2, 0}, {"\xFF", 1, 0}, {"\xF0\x80\x80\x80", 4, 0}, {"\xEF\xBF\xBF", 3, 1},
    };
    size_t i;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) CHECK_EQ(rbli_utf8_valid((const uint8_t*)cases[i].s, cases[i].n), cases[i].ok);
}

TEST(fuzz_control_lines_never_crash) {
    static const char* const seeds[] = {
        "{\"type\":\"hello\",\"role\":\"guest\",\"game\":\"G\",\"protocol\":1,\"min_protocol\":1,\"udp_port\":5,\"heartbeat_ms\":250,\"timeout_ms\":2000}\n",
        "{\"type\":\"spawn\",\"entity\":7,\"provider\":\"P\",\"asset\":\"/a/b\\u00e9\",\"name\":\"n\"}\n",
        "{\"type\":\"frame_channel\",\"path\":\"/tmp/x\",\"width\":1,\"height\":2,\"slots\":3}\n",
        "{\"type\":\"bye\",\"reason\":\"r\"}\n", "{\"type\":\"input\",\"focus\":\"host\",\"extra\":[1,2,{\"a\":[]}]}\n",
    };
    long i, ok = 0, bad = 0;
    for (i = 0; i < 100000; i++) {
        char* b = g_pbuf[0];
        const char* s = seeds[rnd() % 5];
        size_t n = strlen(s), k, flips = 1 + rnd() % 4;
        rbl_control c;
        memcpy(b, s, n);
        for (k = 0; k < flips; k++) {
            size_t at = rnd() % n;
            switch (rnd() % 4) {
            case 0: b[at] = (char)rnd(); break;
            case 1: b[at] = "{}[]\",:\\ue0-+.9"[rnd() % 16]; break;
            case 2: if (n > 1) { memmove(b + at, b + at + 1, n - at - 1); n--; } break;
            default: if (n < 1000) { memmove(b + at + 1, b + at, n - at); b[at] = (char)rnd(); n++; } break;
            }
        }
        if (rnd() % 7 == 0) n = rnd() % (n + 1);
        if (rbli_utf8_valid((const uint8_t*)b, n)) { /* a link only parses valid UTF-8 */
            if (rbl_control_parse(b, n, &c, NULL) == RBL_OK) ok++; else bad++;
        }
    }
    printf("     fuzz: %ld lines parsed, %ld refused\n", ok, bad);
    CHECK(bad > 1000);
}

/* ------------------------------------------------------------------ live sessions (loopback, in process) */

typedef struct Col {
    int n[16];
    int cameras; uint32_t last_camera_seq;
    int pose_events; int pose_recs[2048]; float last_pose_x;
    int spawns; uint32_t last_spawn_entity; char last_spawn_provider[64], last_spawn_asset[64], last_spawn_name[64]; int last_spawn_has_name;
    int despawns;
    int collisions; uint32_t coll_query, coll_tris; float coll_first_x; float coll_radius;
    int frames; rbl_frame last_frame;
    int frame_channels; char fc_path[512]; uint32_t fc_w, fc_h, fc_slots;
    int lost; char lost_reason[200];
    int hsfail; char hsfail_reason[200];
    int connected;
    int focus_events, focus_requested, focus_value;
} Col;

static void col_add(Col* c, const rbl_event* e) {
    c->n[e->type & 15]++;
    switch (e->type) {
    case RBL_EV_CONNECTED: c->connected++; break;
    case RBL_EV_LOST: c->lost++; snprintf(c->lost_reason, sizeof c->lost_reason, "%s", e->u.lost.text); break;
    case RBL_EV_HANDSHAKE_FAILED: c->hsfail++; snprintf(c->hsfail_reason, sizeof c->hsfail_reason, "%s", e->u.handshake_failed.text); break;
    case RBL_EV_CAMERA: c->cameras++; c->last_camera_seq = e->u.camera.header.seq; break;
    case RBL_EV_POSES: {
        uint32_t i;
        c->pose_events++;
        for (i = 0; i < e->u.poses.count; i++) {
            if (e->u.poses.poses[i].entity < 2048) c->pose_recs[e->u.poses.poses[i].entity]++;
            c->last_pose_x = e->u.poses.poses[i].translation[0];
        }
        break;
    }
    case RBL_EV_SPAWN:
        c->spawns++;
        c->last_spawn_entity = e->u.spawn.entity;
        snprintf(c->last_spawn_provider, sizeof c->last_spawn_provider, "%s", e->u.spawn.provider);
        snprintf(c->last_spawn_asset, sizeof c->last_spawn_asset, "%s", e->u.spawn.asset);
        c->last_spawn_has_name = e->u.spawn.name != NULL;
        if (e->u.spawn.name) snprintf(c->last_spawn_name, sizeof c->last_spawn_name, "%s", e->u.spawn.name);
        break;
    case RBL_EV_DESPAWN: c->despawns++; break;
    case RBL_EV_COLLISION:
        c->collisions++;
        c->coll_query = e->u.collision.query;
        c->coll_tris = e->u.collision.triangle_count;
        c->coll_radius = e->u.collision.radius;
        c->coll_first_x = e->u.collision.triangle_count ? e->u.collision.triangles[0].v[0][0] : -1;
        break;
    case RBL_EV_FRAME: c->frames++; c->last_frame = e->u.frame.frame; break;
    case RBL_EV_FRAME_CHANNEL:
        c->frame_channels++;
        snprintf(c->fc_path, sizeof c->fc_path, "%s", e->u.frame_channel.path ? e->u.frame_channel.path : "(null)");
        c->fc_w = e->u.frame_channel.width; c->fc_h = e->u.frame_channel.height; c->fc_slots = e->u.frame_channel.slots;
        break;
    case RBL_EV_FOCUS: c->focus_events++; c->focus_requested = e->u.focus.requested; c->focus_value = (int)e->u.focus.focus; break;
    default: break;
    }
}

static int col_poll_cap(rbl_link* l, Col* c, size_t cap) {
    rbl_event* ev = (rbl_event*)malloc(sizeof(rbl_event) * cap);
    int n = rbl_link_poll(l, ev, cap), i;
    for (i = 0; i < n; i++) col_add(c, &ev[i]);
    free(ev);
    return n;
}
static int col_poll(rbl_link* l, Col* c) { return col_poll_cap(l, c, 256); }

typedef struct Pair { rbl_link* host; rbl_link* guest; Col hc, gc; } Pair;

static void pair_pump(Pair* p) { col_poll(p->host, &p->hc); col_poll(p->guest, &p->gc); }

static int pair_open_cfg(Pair* p, const rbl_config* hcfg, const rbl_config* gcfg_in) {
    rbl_config g;
    int ok;
    memset(p, 0, sizeof *p);
    if (rbl_link_host(hcfg, &p->host) != RBL_OK) return 0;
    g = *gcfg_in;
    g.control_port = rbl_link_control_port(p->host);
    if (rbl_link_guest(&g, &p->guest) != RBL_OK) return 0;
    WAIT_UNTIL(3000, pair_pump(p), rbl_link_is_connected(p->host) && rbl_link_is_connected(p->guest), ok);
    return ok;
}

static int pair_open(Pair* p) {
    rbl_config h, g;
    rbl_config_init(&h, "Test Host"); h.control_port = 0; h.timeout_ms = 4000;
    rbl_config_init(&g, "Test Guest"); g.timeout_ms = 4000;
    return pair_open_cfg(p, &h, &g);
}

static void pair_close(Pair* p) {
    rbl_link_close(p->guest);
    rbl_link_close(p->host);
    p->guest = p->host = NULL;
}

static rbl_stats stats_of(rbl_link* l) { rbl_stats s; rbl_link_stats(l, &s); return s; }

/* ---- a raw peer: plain sockets, to say things a correct implementation never would ---- */

typedef struct Raw { rbli_sock tcp, udp; uint16_t udp_port; char buf[70000]; size_t len; int eof, reset; } Raw;

/* The raw peer holds its own Winsock reference (WSAStartup is counted): without it, the WSACleanup of
 * the last rbl_link_close() shuts Winsock down under the raw sockets, which then fail with
 * WSANOTINITIALISED, and a test that reads the host's last bytes after rbl_link_close() sees none. */
static int raw_open(Raw* r, uint16_t host_port) {
    uint64_t end = rbli_mono_ms() + 2000;
    memset(r, 0, sizeof *r);
    r->tcp = r->udp = RBLI_BAD_SOCK;
    if (rbli_net_up() != 0) return 0;
    r->udp = rbli_udp_open(0, &r->udp_port);
    if (r->udp == RBLI_BAD_SOCK) return 0;
    r->tcp = rbli_tcp_connect_start(host_port);
    if (r->tcp == RBLI_BAD_SOCK) return 0;
    while (rbli_tcp_connect_check(r->tcp) != 1) {
        if (rbli_mono_ms() > end) return 0;
        rbli_sleep_ms(1);
    }
    return 1;
}
static void raw_send(Raw* r, const char* s) {
    size_t n = strlen(s), off = 0;
    uint64_t end = rbli_mono_ms() + 2000;
    while (off < n && rbli_mono_ms() < end) {
        int k = rbli_tcp_send(r->tcp, s + off, n - off);
        if (k > 0) off += (size_t)k; else if (k == -2) return; else rbli_sleep_ms(1);
    }
}
static void raw_pump(Raw* r) {
    for (;;) {
        int k;
        if (r->len + 4096 > sizeof r->buf) return;
        k = rbli_tcp_recv(r->tcp, r->buf + r->len, 4096);
        if (k > 0) r->len += (size_t)k; else { if (k == 0 || k == -2) r->eof = 1; if (k == -2) r->reset = 1; return; }
    }
}
/* pops a complete line (without the newline); 1 if there was one */
static int raw_line(Raw* r, char* out, size_t cap) {
    char* nl = (char*)memchr(r->buf, '\n', r->len);
    size_t n;
    if (!nl) return 0;
    n = (size_t)(nl - r->buf);
    snprintf(out, cap, "%.*s", (int)(n < cap - 1 ? n : cap - 1), r->buf);
    memmove(r->buf, nl + 1, r->len - n - 1);
    r->len -= n + 1;
    return 1;
}
static void raw_close(Raw* r) {
    if (r->tcp != RBLI_BAD_SOCK) rbli_sock_close(r->tcp);
    if (r->udp != RBLI_BAD_SOCK) rbli_sock_close(r->udp);
    rbli_net_down();
}
/* pumps `host` (collecting into c) until the raw peer has a line, or ms pass */
static int raw_wait_line(Raw* r, rbl_link* host, Col* c, int ms, char* out, size_t cap) {
    int ok;
    WAIT_UNTIL(ms, (col_poll(host, c), raw_pump(r)), raw_line(r, out, cap), ok);
    return ok;
}
static int raw_wait_eof(Raw* r, rbl_link* host, Col* c, int ms) {
    int ok;
    WAIT_UNTIL(ms, (col_poll(host, c), raw_pump(r)), r->eof, ok);
    return ok;
}
static void hello_line(char* out, size_t cap, const char* role, int protocol, int min_protocol, unsigned udp_port, long long hb) {
    snprintf(out, cap, "{\"type\":\"hello\",\"role\":\"%s\",\"game\":\"raw\",\"protocol\":%d,\"min_protocol\":%d,\"udp_port\":%u,"
                       "\"heartbeat_ms\":%lld,\"timeout_ms\":2000}\n", role, protocol, min_protocol, udp_port, hb);
}

TEST(handshake_state_flow_and_clean_bye) {
    Pair p;
    int ok;
    rbl_peer_info gp, hp;
    rbl_camera cam;
    rbl_pose pose, old;
    float eye[3] = {1, 2, 3}, tgt[3] = {1, 2, 0};
    uint32_t old_session;
    uint64_t t;
    REQUIRE(pair_open(&p));
    REQUIRE(rbl_link_peer(p.guest, &gp) && rbl_link_peer(p.host, &hp));
    CHECK_STR(gp.game, "Test Host"); CHECK_STR(hp.game, "Test Guest");
    CHECK_EQ(gp.session, hp.session); CHECK(gp.session != 0); CHECK_EQ(gp.protocol, 1); CHECK_EQ(gp.role, RBL_ROLE_HOST); CHECK_EQ(hp.role, RBL_ROLE_GUEST);
    CHECK_EQ(hp.udp_port, rbl_link_udp_port(p.guest)); CHECK_EQ(gp.udp_port, rbl_link_udp_port(p.host));
    CHECK_EQ(p.hc.connected, 1); CHECK_EQ(p.gc.connected, 1);
    old_session = hp.session;

    rbl_camera_look_at(&cam, eye, tgt, 1.0f, 0.1f, 50.0f, 640, 360);
    CHECK_EQ(rbl_link_send_camera(p.host, &cam, NULL), RBL_OK);
    CHECK_EQ(rbl_link_spawn(p.guest, 9, "Test", "demo/cube", "Cube"), RBL_OK);
    t = rbl_now_us();
    rbl_pose_identity(&pose, 9, t);
    pose.translation[0] = 42;
    CHECK_EQ(rbl_link_send_poses(p.guest, &pose, 1), RBL_OK);
    WAIT_UNTIL(2000, pair_pump(&p), p.gc.cameras > 0 && p.hc.pose_events > 0 && p.hc.spawns > 0, ok);
    REQUIRE(ok);
    CHECK(p.hc.last_pose_x == 42.0f);
    CHECK_EQ(p.hc.last_spawn_entity, 9); CHECK_STR(p.hc.last_spawn_provider, "Test"); CHECK_STR(p.hc.last_spawn_asset, "demo/cube");
    CHECK(p.hc.last_spawn_has_name); CHECK_STR(p.hc.last_spawn_name, "Cube");

    /* a stale pose (older time) is dropped */
    old = pose; old.time_us = t - 1000;
    {   int before = p.hc.pose_events; rbl_stats s0 = stats_of(p.host);
        CHECK_EQ(rbl_link_send_poses(p.guest, &old, 1), RBL_OK);
        WAIT_UNTIL(300, pair_pump(&p), stats_of(p.host).stale > s0.stale, ok);
        CHECK(ok); CHECK_EQ(p.hc.pose_events, before); }

    /* focus: the guest asks, the host grants */
    CHECK_EQ(rbl_link_set_focus(p.guest, RBL_ROLE_GUEST), RBL_OK);
    WAIT_UNTIL(2000, pair_pump(&p), p.hc.focus_events > 0, ok);
    REQUIRE(ok);
    CHECK(p.hc.focus_requested); CHECK_EQ(p.hc.focus_value, RBL_ROLE_GUEST);
    CHECK_EQ(rbl_link_focus(p.host), RBL_ROLE_HOST); /* a request changes nothing by itself */
    CHECK_EQ(rbl_link_set_focus(p.host, RBL_ROLE_GUEST), RBL_OK);
    WAIT_UNTIL(2000, pair_pump(&p), p.gc.focus_events > 0, ok);
    REQUIRE(ok);
    CHECK(!p.gc.focus_requested); CHECK_EQ(rbl_link_focus(p.guest), RBL_ROLE_GUEST); CHECK_EQ(rbl_link_focus(p.host), RBL_ROLE_GUEST);

    /* despawn */
    CHECK_EQ(rbl_link_despawn(p.guest, 9), RBL_OK);
    WAIT_UNTIL(2000, pair_pump(&p), p.hc.despawns == 1, ok);
    CHECK(ok);

    /* bye: the guest disconnects; the host sees "peer said bye", not a reset */
    rbl_link_disconnect(p.guest, "test over");
    WAIT_UNTIL(2000, pair_pump(&p), p.hc.lost > 0, ok);
    REQUIRE(ok);
    CHECK(strstr(p.hc.lost_reason, "peer said bye: test over") != NULL);
    CHECK_EQ(rbl_link_focus(p.host), RBL_ROLE_HOST);
    CHECK_EQ(p.gc.lost, 0);

    /* the guest reconnects on its own, with a new session */
    WAIT_UNTIL(4000, pair_pump(&p), rbl_link_is_connected(p.host) && rbl_link_is_connected(p.guest), ok);
    REQUIRE(ok);
    rbl_link_peer(p.host, &hp);
    CHECK(hp.session != old_session);
    pair_close(&p);
}

TEST(second_connection_gets_bye_busy) {
    Pair p;
    Raw r;
    char line[256];
    int ok;
    REQUIRE(pair_open(&p));
    REQUIRE(raw_open(&r, rbl_link_control_port(p.host)));
    REQUIRE(raw_wait_line(&r, p.host, &p.hc, 3000, line, sizeof line));
    CHECK_STR(line, "{\"type\":\"bye\",\"reason\":\"busy\"}");
    ok = raw_wait_eof(&r, p.host, &p.hc, 2000);
    CHECK(ok); /* then end of stream: a clean close, not a reset */
    CHECK(rbl_link_is_connected(p.host));
    raw_close(&r);
    pair_close(&p);
}

TEST(hello_with_wrong_role_protocol_or_garbage_gets_bye) {
    static const struct { const char* line; const char* word; } cases[] = {
        {"{\"type\":\"hello\",\"role\":\"host\",\"game\":\"x\",\"protocol\":1,\"min_protocol\":1,\"udp_port\":5,\"heartbeat_ms\":250,\"timeout_ms\":2000}\n", "role"},
        {"{\"type\":\"hello\",\"role\":\"guest\",\"game\":\"x\",\"protocol\":9,\"min_protocol\":2,\"udp_port\":5,\"heartbeat_ms\":250,\"timeout_ms\":2000}\n", "protocol"},
        {"garbage\n", "protocol error"},
        {"{\"type\":\"spawn\",\"entity\":1,\"provider\":\"p\",\"asset\":\"a\"}\n", "expected hello"},
        {"{\"type\":\"hello\",\"role\":\"guest\",\"game\":\"x\",\"protocol\":1,\"min_protocol\":1,\"udp_port\":0,\"heartbeat_ms\":250,\"timeout_ms\":2000}\n", "udp_port"},
    };
    size_t i;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        rbl_config h;
        rbl_link* host;
        Col c;
        Raw r;
        char line[400];
        memset(&c, 0, sizeof c);
        rbl_config_init(&h, "Strict Host"); h.control_port = 0;
        REQUIRE(rbl_link_host(&h, &host) == RBL_OK);
        REQUIRE(raw_open(&r, rbl_link_control_port(host)));
        raw_send(&r, cases[i].line);
        REQUIRE(raw_wait_line(&r, host, &c, 3000, line, sizeof line));
        if (strncmp(line, "{\"type\":\"bye\"", 13) != 0 || !strstr(line, cases[i].word)) {
            char m[500]; snprintf(m, sizeof m, "expected a bye mentioning \"%s\", got %s", cases[i].word, line); t_fail(__FILE__, __LINE__, m);
        }
        CHECK(raw_wait_eof(&r, host, &c, 2000));
        CHECK_EQ(c.hsfail, 1);
        raw_close(&r);
        rbl_link_close(host);
    }
}

TEST(watchdog_loses_a_silent_guest) {
    rbl_config h;
    rbl_link* host;
    Col c;
    Raw r;
    char line[400];
    uint64_t t0;
    int ok;
    memset(&c, 0, sizeof c);
    rbl_config_init(&h, "Watch Host"); h.control_port = 0; h.timeout_ms = 400; h.heartbeat_ms = 100;
    REQUIRE(rbl_link_host(&h, &host) == RBL_OK);
    REQUIRE(raw_open(&r, rbl_link_control_port(host)));
    /* the guest says hello with a heartbeat of 50 ms (effective timeout max(400, 200)) and goes silent */
    hello_line(line, sizeof line, "guest", 1, 1, 9, 50);
    raw_send(&r, line);
    t0 = rbli_mono_ms();
    WAIT_UNTIL(3000, (col_poll(host, &c), raw_pump(&r)), c.lost > 0, ok);
    REQUIRE(ok);
    {   uint64_t ms = rbli_mono_ms() - t0;
        CHECK(ms >= 350 && ms < 1500);
        if (!(ms >= 350 && ms < 1500)) printf("     took %lu ms\n", (unsigned long)ms); }
    CHECK(strncmp(c.lost_reason, "watchdog", 8) == 0);
    REQUIRE(raw_wait_line(&r, host, &c, 1000, line, sizeof line));
    CHECK(strstr(line, "\"role\":\"host\"") && strstr(line, "\"session\":"));
    REQUIRE(raw_wait_line(&r, host, &c, 2000, line, sizeof line));
    CHECK(strstr(line, "\"type\":\"bye\"") && strstr(line, "watchdog"));
    raw_close(&r);
    rbl_link_close(host);
}

TEST(bye_is_delivered_and_the_close_is_clean_while_the_closer_has_unread_data) {
    /* CONTRACT.md 6.1: close after bye = half-close, drain, close; a socket closed with unread data makes the OS send a reset */
    rbl_config h;
    rbl_link* host;
    Col c;
    Raw r;
    char line[400], junk[64];
    int i, ok;
    uint64_t t0;
    memset(&c, 0, sizeof c);
    rbl_config_init(&h, "H"); h.control_port = 0;
    REQUIRE(rbl_link_host(&h, &host) == RBL_OK);
    REQUIRE(raw_open(&r, rbl_link_control_port(host)));
    hello_line(line, sizeof line, "guest", 1, 1, r.udp_port, 250);
    raw_send(&r, line);
    REQUIRE(raw_wait_line(&r, host, &c, 2000, line, sizeof line)); /* the host's hello */
    snprintf(junk, sizeof junk, "{\"type\":\"future_thing\",\"n\":1}\n");
    for (i = 0; i < 2000; i++) raw_send(&r, junk); /* ~64 KB the host has not read yet */
    rbl_link_disconnect(host, "done");
    t0 = rbli_mono_ms();
    WAIT_UNTIL(2000, (col_poll(host, &c), raw_pump(&r)), raw_line(&r, line, sizeof line), ok);
    REQUIRE(ok);
    CHECK_STR(line, "{\"type\":\"bye\",\"reason\":\"done\"}");
    WAIT_UNTIL(2000, (col_poll(host, &c), raw_pump(&r)), r.eof, ok);
    CHECK(ok);
    CHECK(!r.reset); /* an end of stream, not a connection reset */
    raw_send(&r, junk); /* the peer keeps talking while the host drains: still no reset for the host */
    WAIT_UNTIL(400, (col_poll(host, &c), raw_pump(&r)), 0, ok);
    CHECK(rbli_mono_ms() - t0 < 2000);
    CHECK_EQ(c.lost, 0); /* the caller knows: no event for its own disconnect */
    raw_close(&r);
    rbl_link_close(host);
}

TEST(close_returns_within_the_linger_even_if_the_peer_never_closes) {
    rbl_config h;
    rbl_link* host;
    Col c;
    Raw r;
    char line[400];
    uint64_t t0, ms;
    memset(&c, 0, sizeof c);
    rbl_config_init(&h, "H"); h.control_port = 0;
    REQUIRE(rbl_link_host(&h, &host) == RBL_OK);
    REQUIRE(raw_open(&r, rbl_link_control_port(host)));
    hello_line(line, sizeof line, "guest", 1, 1, r.udp_port, 250);
    raw_send(&r, line);
    REQUIRE(raw_wait_line(&r, host, &c, 2000, line, sizeof line));
    t0 = rbli_mono_ms();
    rbl_link_close(host); /* the raw peer never closes its end */
    ms = rbli_mono_ms() - t0;
    CHECK(ms <= 400);
    raw_pump(&r);
    CHECK(strstr(r.buf, "\"reason\":\"closing\"") != NULL); /* the bye still arrived */
    CHECK(r.eof);
    raw_close(&r);
}

TEST(datagrams_from_another_address_are_dropped) {
    Pair p;
    rbli_sock stranger;
    uint16_t sport;
    uint8_t b[256];
    rbl_pose pose;
    rbl_header h;
    rbl_peer_info hp;
    int n, ok;
    rbl_stats before;
    REQUIRE(pair_open(&p));
    rbl_link_peer(p.host, &hp);
    stranger = rbli_udp_open(0, &sport);
    REQUIRE(stranger != RBLI_BAD_SOCK);
    rbl_pose_identity(&pose, 1, rbl_now_us());
    h = mk_header(hp.session, 1, rbl_now_us());
    n = rbl_wire_encode_poses(&h, &pose, 1, b, sizeof b);
    before = stats_of(p.host);
    CHECK(rbli_udp_send(stranger, b, (size_t)n, rbl_link_udp_port(p.host)));
    WAIT_UNTIL(500, pair_pump(&p), stats_of(p.host).foreign > before.foreign, ok);
    CHECK(ok);
    CHECK_EQ(p.hc.pose_events, 0);
    rbli_sock_close(stranger);
    pair_close(&p);
}

TEST(peer_address_must_be_127_0_0_1_and_the_announced_port) {
    CHECK(rbli_is_peer_addr(0x7F000001u, 5000, 5000));
    CHECK(!rbli_is_peer_addr(0x7F000001u, 5001, 5000));
    CHECK(!rbli_is_peer_addr(0x7F000002u, 5000, 5000));
    CHECK(!rbli_is_peer_addr(0x7F010203u, 5000, 5000));
    CHECK(!rbli_is_peer_addr(0x0A000001u, 5000, 5000));
    CHECK(!rbli_is_peer_addr(0, 5000, 5000));
}

/* ------------------------------------------------------------------ revision 4: the pixel file */

static void tmp_path(char* out, size_t cap, const char* name) {
    static unsigned n;
    char dir[1024];
    rbl_temp_dir(dir, sizeof dir);
    snprintf(out, cap, "%slink-c-test-%u-%u-%s", dir, rbli_pid(), ++n, name);
}
static uint8_t* slurp(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    uint8_t* b;
    long sz;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    b = (uint8_t*)malloc((size_t)sz + 1);
    *n = fread(b, 1, (size_t)sz, f);
    fclose(f);
    return b;
}
static int exists(const char* path) { FILE* f = fopen(path, "rb"); if (f) fclose(f); return f != NULL; }
static void header_bytes(uint8_t* h, uint32_t slots, uint32_t w, uint32_t hh, uint32_t stride, uint64_t slot_size) {
    memset(h, 0, 64);
    memcpy(h, "RBFRAME\0", 8);
    rbli_put32(h + 8, 1); rbli_put32(h + 12, slots); rbli_put32(h + 16, w); rbli_put32(h + 20, hh);
    rbli_put32(h + 24, stride); rbli_put32(h + 28, 1); rbli_put64(h + 32, slot_size);
}

TEST(pixel_file_colour_only_round_trip) {
    char path[512];
    rbl_pixel_writer* w;
    rbl_pixel_reader* r;
    rbl_pixel_info wi, ri;
    uint8_t px[32], got[32], *raw;
    rbl_pixel_frame fi;
    uint64_t f;
    size_t n;
    int i;
    tmp_path(path, sizeof path, "a.frames");
    REQUIRE(rbl_pixel_writer_create(path, 4, 2, 3, 0, &w) == RBL_OK);
    rbl_pixel_writer_info(w, &wi);
    CHECK(wi.slot_size == 128); /* 64 slot header + 4*4*2 pixels = 96, rounded up to 128 */
    for (i = 0; i < 32; i++) px[i] = (uint8_t)(i * 3);
    REQUIRE(rbl_pixel_writer_write(w, px, 32, NULL, 0, 4, 2, 77, 123456789ull, &f) == RBL_OK);
    CHECK(f == 1);
    raw = slurp(path, &n);
    REQUIRE(raw != NULL);
    CHECK_EQ(n, 64 + 3 * 128);
    CHECK_EQ(le32(raw + 8), 1); CHECK_EQ(le32(raw + 12), 3); CHECK_EQ(le32(raw + 16), 4); CHECK_EQ(le32(raw + 20), 2);
    CHECK_EQ(le32(raw + 24), 16); CHECK_EQ(le32(raw + 28), 1); CHECK(rbli_get64(raw + 32) == 128);
    CHECK_EQ(le32(raw + 40), 0); CHECK_EQ(le32(raw + 44), 0); CHECK(rbli_get64(raw + 48) == 0);
    CHECK(memcmp(raw, "RBFRAME", 8) == 0);
    CHECK(rbli_get64(raw + 64 + 128 * 1 + 8) == 1); /* frame 1 went to slot 1 % 3 */
    CHECK((rbli_get64(raw + 64 + 128 * 1) & 1) == 0);
    free(raw);
    REQUIRE(rbl_pixel_reader_open(path, &r) == RBL_OK);
    rbl_pixel_reader_info(r, &ri);
    CHECK(!rbl_pixel_reader_has_depth(r)); CHECK_EQ(ri.slots, 3); CHECK_EQ(ri.stride, 16);
    CHECK_EQ(rbl_pixel_reader_read(r, 1, 1, got, sizeof got, NULL, 0, &fi), RBL_OK);
    CHECK_EQ(fi.camera_seq, 77); CHECK(fi.time_us == 123456789ull); CHECK_EQ(fi.width, 4); CHECK_EQ(fi.height, 2); CHECK(fi.frame == 1);
    CHECK(memcmp(got, px, 32) == 0); CHECK(!fi.has_depth);
    CHECK_EQ(rbl_pixel_reader_read(r, 2, 2, got, sizeof got, NULL, 0, &fi), RBL_MISS_OTHER_FRAME);
    CHECK_EQ(rbl_pixel_reader_read(r, 1, 99, got, sizeof got, NULL, 0, &fi), RBL_MISS_OTHER_FRAME);
    CHECK_EQ(rbl_pixel_reader_read(r, 1, 1, got, 16, NULL, 0, &fi), RBL_ERR_TOO_BIG);
    rbl_pixel_reader_close(r);
    rbl_pixel_writer_close(w);
    CHECK(!exists(path)); /* the writer deletes the file on close */
}

TEST(pixel_file_depth_plane) {
    char path[512];
    rbl_pixel_writer* w;
    rbl_pixel_reader* r;
    rbl_pixel_info wi;
    uint8_t px[60], got[60], *raw;
    float d[15], gd[15];
    rbl_pixel_frame fi;
    size_t n;
    int i;
    tmp_path(path, sizeof path, "b.frames");
    REQUIRE(rbl_pixel_writer_create(path, 5, 3, 3, 1, &w) == RBL_OK);
    rbl_pixel_writer_info(w, &wi);
    CHECK(wi.slot_size == 192); /* colour end 64 + 60 = 124 -> depth_offset 128; plane 60 -> 188 -> slot_size 192 */
    memset(px, 7, sizeof px);
    for (i = 0; i < 15; i++) d[i] = (float)i + 0.5f;
    d[7] = (float)HUGE_VAL;
    REQUIRE(rbl_pixel_writer_write(w, px, 60, d, 15, 5, 3, 1, 5, NULL) == RBL_OK);
    raw = slurp(path, &n);
    REQUIRE(raw != NULL);
    CHECK_EQ(le32(raw + 40), 1); CHECK_EQ(le32(raw + 44), 20); CHECK(rbli_get64(raw + 48) == 128); CHECK(rbli_get64(raw + 32) == 192);
    free(raw);
    REQUIRE(rbl_pixel_reader_open(path, &r) == RBL_OK);
    CHECK(rbl_pixel_reader_has_depth(r));
    CHECK_EQ(rbl_pixel_reader_read(r, 1, 1, got, sizeof got, gd, 15, &fi), RBL_OK);
    CHECK(fi.has_depth); CHECK(gd[14] == 14.5f && gd[0] == 0.5f); CHECK(gd[7] > 1e38f);
    CHECK_EQ(rbl_pixel_reader_read(r, 1, 1, got, sizeof got, NULL, 0, &fi), RBL_OK);
    CHECK(!fi.has_depth);
    CHECK_EQ(rbl_pixel_reader_read(r, 1, 1, got, sizeof got, gd, 14, &fi), RBL_ERR_TOO_BIG);
    rbl_pixel_reader_close(r);
    /* a frame written without depth in a file that has a plane stores +inf everywhere */
    REQUIRE(rbl_pixel_writer_write(w, px, 60, NULL, 0, 5, 3, 1, 6, NULL) == RBL_OK);
    REQUIRE(rbl_pixel_reader_open(path, &r) == RBL_OK);
    CHECK_EQ(rbl_pixel_reader_read(r, 2, 2, got, sizeof got, gd, 15, &fi), RBL_OK);
    for (i = 0; i < 15; i++) CHECK(gd[i] > 1e38f);
    rbl_pixel_reader_close(r);
    rbl_pixel_writer_close(w);
}

TEST(pixel_file_odd_lock_is_skipped) {
    char path[512];
    rbl_pixel_writer* w;
    rbl_pixel_reader* r;
    uint8_t got[16];
    uint8_t px[16] = {0};
    tmp_path(path, sizeof path, "c.frames");
    REQUIRE(rbl_pixel_writer_create(path, 2, 2, 2, 0, &w) == RBL_OK);
    REQUIRE(rbl_pixel_reader_open(path, &r) == RBL_OK);
    CHECK_EQ(rbl_pixel_writer_write(w, px, 16, NULL, 0, 2, 2, 0, 1, NULL), RBL_OK);
    CHECK_EQ(rbl_pixel_reader_read(r, 1, 1, got, sizeof got, NULL, 0, NULL), RBL_OK);
    /* set slot 1's lock odd by hand, as a writer in the middle of a write */
    rbli_st_release(w->map + 64 + w->info.slot_size, 3);
    CHECK_EQ(rbl_pixel_reader_read(r, 1, 1, got, sizeof got, NULL, 0, NULL), RBL_MISS_BUSY);
    rbli_st_release(w->map + 64 + w->info.slot_size, 4);
    CHECK_EQ(rbl_pixel_reader_read(r, 1, 1, got, sizeof got, NULL, 0, NULL), RBL_OK);
    rbl_pixel_reader_close(r);
    rbl_pixel_writer_close(w);
}

TEST(pixel_file_malformed_files_are_refused) {
    char path[512];
    rbl_pixel_writer* w;
    rbl_pixel_reader* r;
    uint8_t px[64] = {0};
    float d[16] = {0};
    rbl_pixel_frame fi;
    uint8_t got[64];
    float gd[16];
    tmp_path(path, sizeof path, "d.frames");
    REQUIRE(rbl_pixel_writer_create(path, 4, 4, 2, 1, &w) == RBL_OK);
    CHECK_EQ(rbl_pixel_writer_write(w, px, 64, d, 16, 4, 4, 0, 1, NULL), RBL_OK);
    rbli_put32(w->map + 40, 9); /* a future depth format */
    REQUIRE(rbl_pixel_reader_open(path, &r) == RBL_OK);
    CHECK(!rbl_pixel_reader_has_depth(r)); /* unknown depth_format is colour only, not an error */
    CHECK_EQ(rbl_pixel_reader_read(r, 1, 1, got, sizeof got, gd, 16, &fi), RBL_OK);
    CHECK(!fi.has_depth);
    rbl_pixel_reader_close(r);
    rbli_put32(w->map + 40, 1); rbli_put32(w->map + 44, 13); /* wrong depth_stride */
    CHECK_EQ(rbl_pixel_reader_open(path, &r), RBL_ERR_REFUSED);
    rbli_put32(w->map + 44, 16);
    rbli_put64(w->map + 48, 100); /* depth_offset not a multiple of 64 */
    CHECK_EQ(rbl_pixel_reader_open(path, &r), RBL_ERR_REFUSED);
    rbli_put64(w->map + 48, 64); /* inside the colour plane */
    CHECK_EQ(rbl_pixel_reader_open(path, &r), RBL_ERR_REFUSED);
    rbli_put64(w->map + 48, 128); /* the plane would end after the slot */
    rbli_put64(w->map + 32, 128);
    CHECK_EQ(rbl_pixel_reader_open(path, &r), RBL_ERR_REFUSED);
    rbli_put64(w->map + 32, (uint64_t)w->info.slot_size);
    rbli_put64(w->map + 48, w->info.depth_offset);
    rbli_put32(w->map + 44, 16);
    REQUIRE(rbl_pixel_reader_open(path, &r) == RBL_OK);
    rbl_pixel_reader_close(r);
    w->map[0] = 'X'; /* bad magic */
    CHECK_EQ(rbl_pixel_reader_open(path, &r), RBL_ERR_REFUSED);
    w->map[0] = 'R';
    rbli_put32(w->map + 28, 2); /* a format we do not know */
    CHECK_EQ(rbl_pixel_reader_open(path, &r), RBL_ERR_REFUSED);
    rbli_put32(w->map + 28, 1);
    rbli_put32(w->map + 8, 2); /* version 2 */
    CHECK_EQ(rbl_pixel_reader_open(path, &r), RBL_ERR_REFUSED);
    rbl_pixel_writer_close(w);
    CHECK_EQ(rbl_pixel_reader_open("relative.frames", &r), RBL_ERR_REFUSED);
    CHECK_EQ(rbl_pixel_reader_open("./relative.frames", &r), RBL_ERR_REFUSED);
}

TEST(pixel_file_a_truncated_copy_is_refused) {
    char path[512], cut[512];
    rbl_pixel_writer* w;
    rbl_pixel_reader* r;
    uint8_t* raw;
    size_t n;
    FILE* f;
    tmp_path(path, sizeof path, "e.frames");
    tmp_path(cut, sizeof cut, "e-cut.frames");
    REQUIRE(rbl_pixel_writer_create(path, 4, 4, 3, 0, &w) == RBL_OK);
    raw = slurp(path, &n);
    REQUIRE(raw != NULL);
    f = fopen(cut, "wb");
    REQUIRE(f != NULL);
    fwrite(raw, 1, 64 + (size_t)w->info.slot_size, f); /* header and one slot of three */
    fclose(f);
    free(raw);
    CHECK_EQ(rbl_pixel_reader_open(cut, &r), RBL_ERR_REFUSED);
    remove(cut);
    rbl_pixel_writer_close(w);
    /* a file shorter than its own header */
    f = fopen(cut, "wb");
    REQUIRE(f != NULL);
    fwrite("RBFRAME", 1, 8, f);
    fclose(f);
    CHECK_EQ(rbl_pixel_reader_open(cut, &r), RBL_ERR_REFUSED);
    remove(cut);
}

TEST(review_pixel_header_limits_and_overflow) {
    /* the header limits are checked before any size is computed from them (revision 4) */
    static const struct { uint32_t w, h; int ok; } cases[] = {
        {16384, 1, 1}, {1, 16384, 1}, {8192, 8192, 1}, {16384, 4096, 1},
        {16384, 8192, 0}, {8193, 8192, 0}, {16385, 1, 0}, {1, 16385, 0}, {32768, 1, 0}, {1, 32768, 0}, {0, 1, 0}, {1, 0, 0},
        {0x40000000u, 1, 0}, {0xFFFFFFFFu, 0xFFFFFFFFu, 0},
    };
    size_t i;
    uint8_t h[64];
    rbl_pixel_info info;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint32_t w = cases[i].w, hh = cases[i].h;
        uint64_t stride = (uint64_t)w * 4u;
        uint64_t slot = (64u + stride * hh + 63u) & ~(uint64_t)63u;
        int r;
        header_bytes(h, 1, w, hh, (uint32_t)stride, slot);
        r = rbli_pf_validate(h, 64u + slot, &info);
        if ((r == RBL_OK) != cases[i].ok) {
            char m[200]; snprintf(m, sizeof m, "%ux%u should be %s", w, hh, cases[i].ok ? "accepted" : "refused"); t_fail(__FILE__, __LINE__, m);
        }
    }
    /* J2: slot_size such that 3 * slot_size wraps to 128 in 64 bits, and is a multiple of 64 */
    header_bytes(h, 3, 1, 1, 4, 0x5555555555555580ull);
    CHECK_EQ((uint64_t)3 * 0x5555555555555580ull, 128);
    CHECK_EQ(0x5555555555555580ull % 64, 0);
    CHECK_EQ(rbli_pf_validate(h, 4096, &info), RBL_ERR_REFUSED);
    /* a huge slot_size with a large file */
    header_bytes(h, 16, 1, 1, 4, 0xFFFFFFFFFFFFFFC0ull);
    CHECK_EQ(rbli_pf_validate(h, 1ull << 40, &info), RBL_ERR_REFUSED);
    header_bytes(h, 2, 4, 4, 4 * 4, 128 - 64); /* slot_size below the colour end */
    CHECK_EQ(rbli_pf_validate(h, 4096, &info), RBL_ERR_REFUSED);
    /* stride must be exactly width * 4 */
    header_bytes(h, 1, 4, 4, 20, 192);
    CHECK_EQ(rbli_pf_validate(h, 64 + 192, &info), RBL_ERR_REFUSED);
    header_bytes(h, 1, 4, 4, 12, 192);
    CHECK_EQ(rbli_pf_validate(h, 64 + 192, &info), RBL_ERR_REFUSED);
    header_bytes(h, 1, 4, 4, 16, 192);
    CHECK_EQ(rbli_pf_validate(h, 64 + 192, &info), RBL_OK);
    CHECK_EQ(rbli_pf_validate(h, 64 + 191, &info), RBL_ERR_REFUSED);
    /* slots 0 and 17 */
    header_bytes(h, 0, 4, 4, 16, 192);
    CHECK_EQ(rbli_pf_validate(h, 4096, &info), RBL_ERR_REFUSED);
    header_bytes(h, 17, 4, 4, 16, 192);
    CHECK_EQ(rbli_pf_validate(h, 1 << 20, &info), RBL_ERR_REFUSED);
    /* depth: a plane that does not fit; an offset that overflows */
    header_bytes(h, 1, 4, 4, 16, 192);
    rbli_put32(h + 40, 1); rbli_put32(h + 44, 16); rbli_put64(h + 48, 0xFFFFFFFFFFFFFFC0ull);
    CHECK_EQ(rbli_pf_validate(h, 64 + 192, &info), RBL_ERR_REFUSED);
    /* reserved depth fields of a revision 1 or 2 file are ignored when depth_format is 0 */
    header_bytes(h, 1, 4, 4, 16, 128);
    rbli_put32(h + 44, 999); rbli_put64(h + 48, 12345);
    CHECK_EQ(rbli_pf_validate(h, 64 + 128, &info), RBL_OK);
    CHECK_EQ(info.depth_format, 0);
}

TEST(review_writer_limits_and_create_new) {
    char path[512];
    rbl_pixel_writer* w;
    rbl_pixel_writer* w2;
    uint8_t* before;
    uint8_t* after;
    size_t n1, n2;
    tmp_path(path, sizeof path, "big.frames");
    CHECK_EQ(rbl_pixel_writer_create(path, 16385, 1, 1, 0, &w), RBL_ERR_ARGS); CHECK(!exists(path));
    CHECK_EQ(rbl_pixel_writer_create(path, 1, 16385, 1, 0, &w), RBL_ERR_ARGS);
    CHECK_EQ(rbl_pixel_writer_create(path, 16384, 8192, 1, 0, &w), RBL_ERR_ARGS); CHECK(!exists(path));
    CHECK_EQ(rbl_pixel_writer_create(path, 8193, 8192, 1, 1, &w), RBL_ERR_ARGS); CHECK(!exists(path));
    CHECK_EQ(rbl_pixel_writer_create(path, 0, 8, 1, 0, &w), RBL_ERR_ARGS);
    CHECK_EQ(rbl_pixel_writer_create(path, 8, 8, 0, 0, &w), RBL_ERR_ARGS);
    CHECK_EQ(rbl_pixel_writer_create(path, 8, 8, 17, 0, &w), RBL_ERR_ARGS);
    CHECK(!exists(path));
    /* create_new: an existing path is never opened, truncated or replaced */
    tmp_path(path, sizeof path, "cn.frames");
    REQUIRE(rbl_pixel_writer_create(path, 8, 8, 2, 0, &w) == RBL_OK);
    {   uint8_t px[8 * 8 * 4]; memset(px, 0x5A, sizeof px);
        CHECK_EQ(rbl_pixel_writer_write(w, px, sizeof px, NULL, 0, 8, 8, 0, 1, NULL), RBL_OK); }
    before = slurp(path, &n1);
    CHECK_EQ(rbl_pixel_writer_create(path, 4, 4, 3, 1, &w2), RBL_ERR_EXISTS);
    CHECK(w2 == NULL);
    after = slurp(path, &n2);
    REQUIRE(before && after);
    CHECK_EQ(n1, n2);
    CHECK(memcmp(before, after, n1) == 0);
    free(before); free(after);
    rbl_pixel_writer_close(w);
    CHECK(!exists(path));
    /* a relative path is announced as an absolute one */
#if !RBLI_WINDOWS
    REQUIRE(rbl_pixel_writer_create("link-c-relative-test.frames", 4, 4, 1, 0, &w) == RBL_OK);
    CHECK(rbl_pixel_writer_path(w)[0] == '/');
    CHECK(strstr(rbl_pixel_writer_path(w), "link-c-relative-test.frames") != NULL);
    rbl_pixel_writer_close(w);
#endif
    /* temp files carry the process id and a random part */
    REQUIRE(rbl_pixel_writer_create_temp("tag/with:odd chars", 4, 4, 1, 0, &w) == RBL_OK);
    {   char want[64]; snprintf(want, sizeof want, "rewrite-bridge-%u-tag_with_odd_chars-", rbli_pid());
        CHECK(strstr(rbl_pixel_writer_path(w), want) != NULL);
        CHECK(strstr(rbl_pixel_writer_path(w), ".frames") != NULL); }
    rbl_pixel_writer_close(w);
}

TEST(review_writer_validates_and_never_leaves_the_lock_odd) {
    char path[512];
    rbl_pixel_writer* w;
    uint8_t ok[4 * 2 * 4] = {0};
    uint8_t big[5 * 2 * 4] = {0};
    uint8_t tall[4 * 3 * 4] = {0};
    float okd[8] = {0}, d3[3] = {0}, d10[10] = {0}, d12[12] = {0};
    uint64_t f = 0;
    int slot;
    uint8_t* raw;
    size_t n;
    tmp_path(path, sizeof path, "w.frames");
    REQUIRE(rbl_pixel_writer_create(path, 4, 2, 3, 1, &w) == RBL_OK);
    CHECK_EQ(rbl_pixel_writer_write(w, ok, 5, okd, 8, 4, 2, 0, 1, &f), RBL_ERR_ARGS);      /* short colour */
    CHECK_EQ(rbl_pixel_writer_write(w, ok, 32, d3, 3, 4, 2, 0, 1, &f), RBL_ERR_ARGS);       /* short depth */
    CHECK_EQ(rbl_pixel_writer_write(w, big, 40, d10, 10, 5, 2, 0, 1, &f), RBL_ERR_ARGS);    /* wider than the file */
    CHECK_EQ(rbl_pixel_writer_write(w, tall, 48, d12, 12, 4, 3, 0, 1, &f), RBL_ERR_ARGS);   /* taller than the file */
    CHECK_EQ(rbl_pixel_writer_write(w, ok, 0, okd, 0, 0, 0, 0, 1, &f), RBL_ERR_ARGS);       /* empty */
    CHECK_EQ(rbl_pixel_writer_write(w, NULL, 32, okd, 8, 4, 2, 0, 1, &f), RBL_ERR_ARGS);
    raw = slurp(path, &n);
    REQUIRE(raw != NULL);
    for (slot = 0; slot < 3; slot++) CHECK_EQ(rbli_get64(raw + 64 + (size_t)slot * w->info.slot_size) & 1, 0);
    free(raw);
    CHECK_EQ(rbl_pixel_writer_write(w, ok, 32, okd, 8, 4, 2, 0, 1, &f), RBL_OK);
    CHECK(f == 1); /* the refused writes used no frame number */
    rbl_pixel_writer_close(w);
    /* a colour-only file refuses a depth plane */
    REQUIRE(rbl_pixel_writer_create(path, 4, 2, 3, 0, &w) == RBL_OK);
    CHECK_EQ(rbl_pixel_writer_write(w, ok, 32, okd, 8, 4, 2, 0, 1, &f), RBL_ERR_ARGS);
    rbl_pixel_writer_close(w);
}

#if !RBLI_WINDOWS
TEST(review_symlink_directory_and_fifo_are_refused) {
    char real[512], link[512], fifo[512], dir[1024];
    rbl_pixel_writer* w;
    rbl_pixel_reader* r;
    uint64_t t0;
    tmp_path(real, sizeof real, "real.frames");
    tmp_path(link, sizeof link, "link.frames");
    tmp_path(fifo, sizeof fifo, "fifo.frames");
    REQUIRE(rbl_pixel_writer_create(real, 4, 4, 2, 0, &w) == RBL_OK);
    REQUIRE(symlink(real, link) == 0);
    CHECK_EQ(rbl_pixel_reader_open(link, &r), RBL_ERR_REFUSED); /* a symlink is not a regular file */
    REQUIRE(rbl_pixel_reader_open(real, &r) == RBL_OK);
    rbl_pixel_reader_close(r);
    unlink(link);
    rbl_pixel_writer_close(w);
    REQUIRE(mkfifo(fifo, 0600) == 0);
    t0 = rbli_mono_ms();
    CHECK_EQ(rbl_pixel_reader_open(fifo, &r), RBL_ERR_REFUSED); /* and a FIFO must not hang the receiver */
    CHECK(rbli_mono_ms() - t0 < 2000);
    unlink(fifo);
    rbl_temp_dir(dir, sizeof dir);
    CHECK_EQ(rbl_pixel_reader_open(dir, &r), RBL_ERR_REFUSED);
    CHECK_EQ(rbl_pixel_reader_open("/dev/null", &r), RBL_ERR_REFUSED);
}

TEST(review_a_file_truncated_under_a_reader_is_gone_not_a_crash) {
    char path[512];
    rbl_pixel_writer* w;
    rbl_pixel_reader* r;
    uint8_t px[64 * 64 * 4] = {0}, got[64 * 64 * 4];
    static float d[64 * 64], gd[64 * 64];
    uint64_t f;
    rbl_pixel_stats st;
    tmp_path(path, sizeof path, "trunc.frames");
    REQUIRE(rbl_pixel_writer_create(path, 64, 64, 3, 1, &w) == RBL_OK);
    CHECK_EQ(rbl_pixel_writer_write(w, px, sizeof px, d, 64 * 64, 64, 64, 0, 1, &f), RBL_OK);
    REQUIRE(rbl_pixel_reader_open(path, &r) == RBL_OK);
    CHECK_EQ(rbl_pixel_reader_read(r, f, (uint32_t)(f % 3), got, sizeof got, gd, 64 * 64, NULL), RBL_OK);
    CHECK(!rbl_pixel_reader_is_gone(r));
    REQUIRE(truncate(path, 64) == 0); /* the peer truncates the file after the reader mapped it */
    CHECK(rbl_pixel_reader_is_gone(r));
    CHECK_EQ(rbl_pixel_reader_read(r, f, (uint32_t)(f % 3), got, sizeof got, gd, 64 * 64, NULL), RBL_MISS_GONE);
    rbl_pixel_reader_stats(r, &st);
    CHECK_EQ(st.gone, 1);
    rbl_pixel_reader_close(r);
    rbl_pixel_writer_close(w); /* it unmaps the whole mapping, never touching the cut pages */
}
#endif

/* ------------------------------------------------------------------ seqlock contention */

#if RBLI_WINDOWS
typedef HANDLE t_thread;
typedef struct t_start { void (*fn)(void*); void* arg; } t_start;
static DWORD WINAPI t_tramp(LPVOID p) { t_start s = *(t_start*)p; free(p); s.fn(s.arg); return 0; }
static t_thread t_spawn(void (*fn)(void*), void* arg) {
    t_start* s = (t_start*)malloc(sizeof *s);
    s->fn = fn; s->arg = arg;
    return CreateThread(NULL, 0, t_tramp, s, 0, NULL);
}
static void t_join(t_thread t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
#else
typedef pthread_t t_thread;
typedef struct t_start { void (*fn)(void*); void* arg; } t_start;
static void* t_tramp(void* p) { t_start s = *(t_start*)p; free(p); s.fn(s.arg); return NULL; }
static t_thread t_spawn(void (*fn)(void*), void* arg) {
    t_start* s = (t_start*)malloc(sizeof *s);
    t_thread t;
    s->fn = fn; s->arg = arg;
    pthread_create(&t, NULL, t_tramp, s);
    return t;
}
static void t_join(t_thread t) { pthread_join(t, NULL); }
#endif

#define CW 96
#define CH 64
#define CSLOTS 2

typedef struct Cont {
    rbl_pixel_writer* w;
    volatile int stop;
    uint64_t announce; /* the newest finished frame, published with release/acquire like the Frame datagram */
    int failure;
} Cont;

/* Frame f is f in every 8 bytes of colour and in every depth float. */
static void c_fill(uint64_t f, uint8_t* px, float* depth) {
    size_t i;
    int b;
    for (i = 0; i < (size_t)CW * CH * 4; i += 8) for (b = 0; b < 8; b++) px[i + (size_t)b] = (uint8_t)(f >> (8 * b));
    for (i = 0; i < (size_t)CW * CH; i++) depth[i] = (float)f;
}
static int c_whole(uint64_t f, const uint8_t* px, const float* depth) {
    size_t i;
    int b;
    for (i = 0; i < (size_t)CW * CH * 4; i += 8) for (b = 0; b < 8; b++) if (px[i + (size_t)b] != (uint8_t)(f >> (8 * b))) return 0;
    for (i = 0; i < (size_t)CW * CH; i++) if (depth[i] != (float)f) return 0;
    return 1;
}

static void c_writer_main(void* arg) {
    Cont* c = (Cont*)arg;
    static uint8_t px[CW * CH * 4];
    static float depth[CW * CH];
    uint64_t f = 0;
    while (!c->stop) {
        c_fill(f + 1, px, depth);
        if (rbl_pixel_writer_write(c->w, px, sizeof px, depth, CW * CH, CW, CH, (uint32_t)((f + 1) * 3), (f + 1) * 1000 + 7, &f) != RBL_OK) {
            c->failure = 1;
            return;
        }
        rbli_st_release(&c->announce, f);
    }
}

static void contention(int seqlock) {
    char path[512];
    Cont c;
    rbl_pixel_reader* r;
    t_thread th;
    static uint8_t px[CW * CH * 4];
    static float depth[CW * CH];
    uint64_t accepted = 0, rejected = 0, torn = 0, wrong = 0, attempts = 0, end, ms;
    const char* env = getenv("RBL_CONTENTION_MS");
    rbl_pixel_info info;
    rbl_pixel_stats st;
    memset(&c, 0, sizeof c);
    ms = env ? (uint64_t)atoi(env) : 1500u;
    tmp_path(path, sizeof path, "contention.frames");
    REQUIRE(rbl_pixel_writer_create(path, CW, CH, CSLOTS, 1, &c.w) == RBL_OK);
    REQUIRE(rbl_pixel_reader_open(path, &r) == RBL_OK);
    rbl_pixel_reader_info(r, &info);
    th = t_spawn(c_writer_main, &c);
    end = rbli_mono_ms() + ms;
    while (rbli_mono_ms() < end) {
        uint64_t f = rbli_ld_acquire(&c.announce);
        uint32_t slot;
        if (f == 0) continue;
        attempts++;
        slot = (uint32_t)(f % CSLOTS);
        if (seqlock) {
            rbl_pixel_frame fi;
            int rc = rbl_pixel_reader_read(r, f, slot, px, sizeof px, depth, CW * CH, &fi);
            if (rc != RBL_OK) { rejected++; continue; }
            accepted++;
            if (fi.frame != f || fi.time_us != f * 1000 + 7 || fi.camera_seq != (uint32_t)(f * 3) || fi.width != CW || fi.height != CH) wrong++;
            if (!c_whole(f, px, depth)) torn++;
        } else {
            /* the same copy with no lock check, to show that the writer does tear what is being read */
            const uint8_t* base = r->map + 64 + (size_t)slot * (size_t)info.slot_size;
            if (rbli_get64(base + 8) != f) { rejected++; continue; }
            memcpy(px, base + 64, sizeof px);
            memcpy(depth, base + info.depth_offset, sizeof depth);
            accepted++;
            if (!c_whole(f, px, depth)) torn++;
        }
    }
    c.stop = 1;
    t_join(th);
    rbl_pixel_reader_stats(r, &st);
    printf("     %s: %lu frames written, %lu reads tried, %lu accepted, %lu refused, %lu torn%s", seqlock ? "seqlock reader        " : "unchecked copy (compare)",
           (unsigned long)rbli_ld_acquire(&c.announce), (unsigned long)attempts, (unsigned long)accepted, (unsigned long)rejected, (unsigned long)torn,
           seqlock ? "" : "\n");
    if (seqlock) printf(" (busy %lu, other frame %lu, overwritten while copying %lu; wrong header %lu)\n", (unsigned long)st.busy,
                        (unsigned long)st.other_frame, (unsigned long)st.overwritten, (unsigned long)wrong);
    CHECK(!c.failure);
    if (seqlock) {
        CHECK(rbli_ld_acquire(&c.announce) > 1000);
        CHECK(accepted > 0);
        CHECK(st.busy + st.overwritten > 0); /* the writer never collided with the reader: the test would prove nothing */
        CHECK_EQ(torn, 0);
        CHECK_EQ(wrong, 0);
    }
    rbl_pixel_reader_close(r);
    rbl_pixel_writer_close(c.w);
}

TEST(pixel_file_seqlock_under_contention) {
    contention(1);
    contention(0);
}

/* ------------------------------------------------------------------ revision 4: the link */

TEST(review_a_new_frame_channel_restarts_frame_numbers) {
    Pair p;
    int ok;
    uint64_t f;
    REQUIRE(pair_open(&p));
    for (f = 1; f <= 10; f++) { rbl_frame fr; fr.frame = f; fr.slot = (uint32_t)(f % 3); fr.camera_seq = 0; fr.width = 16; fr.height = 8; rbl_link_send_frame(p.guest, &fr); }
    WAIT_UNTIL(2000, pair_pump(&p), p.hc.frames == 10, ok);
    CHECK(ok);
    /* the same numbers again without a new channel are stale */
    { rbl_frame fr; rbl_stats s0 = stats_of(p.host); fr.frame = 5; fr.slot = 2; fr.camera_seq = 0; fr.width = 16; fr.height = 8;
      rbl_link_send_frame(p.guest, &fr);
      WAIT_UNTIL(300, pair_pump(&p), stats_of(p.host).stale > s0.stale, ok); CHECK(ok); CHECK_EQ(p.hc.frames, 10); }
    p.hc.frames = 0;
    CHECK_EQ(rbl_link_announce_frames(p.guest, "/tmp/second.frames", 32, 16, 3), RBL_OK);
    WAIT_UNTIL(2000, pair_pump(&p), p.hc.frame_channels == 1, ok);
    REQUIRE(ok);
    CHECK_STR(p.hc.fc_path, "/tmp/second.frames"); CHECK_EQ(p.hc.fc_w, 32); CHECK_EQ(p.hc.fc_h, 16); CHECK_EQ(p.hc.fc_slots, 3);
    for (f = 1; f <= 5; f++) { rbl_frame fr; fr.frame = f; fr.slot = (uint32_t)(f % 3); fr.camera_seq = 0; fr.width = 32; fr.height = 16; rbl_link_send_frame(p.guest, &fr); }
    WAIT_UNTIL(2000, pair_pump(&p), p.hc.frames >= 5, ok);
    CHECK(ok);
    CHECK_EQ(p.hc.frames, 5); /* frames 1..5 of the new channel */
    pair_close(&p);
}

static rbl_triangle* make_tris(size_t n) {
    rbl_triangle* t = (rbl_triangle*)malloc(sizeof(rbl_triangle) * n);
    size_t i;
    for (i = 0; i < n; i++) {
        float f[9];
        f[0] = (float)i; f[1] = 0; f[2] = 0; f[3] = (float)i + 1; f[4] = 0; f[5] = 0; f[6] = (float)i; f[7] = 0; f[8] = -1;
        memcpy(&t[i], f, sizeof f);
    }
    return t;
}

TEST(review_a_400_chunk_collision_snapshot_completes) {
    Pair p;
    int ok;
    uint32_t q = 0;
    size_t n = 400 * RBL_MAX_TRIANGLES;
    rbl_triangle* tris = make_tris(n);
    float zero[3] = {0, 0, 0};
    REQUIRE(pair_open(&p));
    CHECK_EQ(rbl_link_send_collision(p.host, zero, 16.0f, tris, n, &q), RBL_OK);
    free(tris); /* the link keeps its own copy */
    WAIT_UNTIL(15000, pair_pump(&p), p.gc.collisions > 0, ok);
    REQUIRE(ok);
    CHECK_EQ(p.gc.coll_query, q); CHECK_EQ(p.gc.coll_tris, n); CHECK(p.gc.coll_radius == 16.0f); CHECK(p.gc.coll_first_x == 0.0f);
    pair_close(&p);
}

TEST(review_collision_chunks_are_paced_and_a_newer_snapshot_replaces_queued_ones) {
    Pair p;
    int ok, n;
    uint32_t q2 = 0;
    size_t big_n = 400 * RBL_MAX_TRIANGLES;
    rbl_triangle* big = make_tris(big_n);
    float zero[3] = {0, 0, 0}, one[3] = {1, 1, 1};
    rbl_stats s0, s1, s2;
    REQUIRE(pair_open(&p));
    rbli_sleep_ms(100);
    pair_pump(&p);
    s0 = stats_of(p.host);
    CHECK_EQ(rbl_link_send_collision(p.host, zero, 16.0f, big, big_n, NULL), RBL_OK);
    CHECK_EQ(stats_of(p.host).datagrams_sent, s0.datagrams_sent); /* queued, nothing on the wire yet */
    col_poll(p.host, &p.hc);
    s1 = stats_of(p.host);
    n = (int)(s1.datagrams_sent - s0.datagrams_sent);
    CHECK(n >= 1 && n <= 33); /* one poll puts at most 32 collision datagrams (and maybe a heartbeat) on the wire */
    /* a second snapshot while the first still waits: the first's chunks are dropped */
    CHECK_EQ(rbl_link_send_collision(p.host, one, 8.0f, big, 2 * RBL_MAX_TRIANGLES, &q2), RBL_OK);
    WAIT_UNTIL(5000, pair_pump(&p), p.gc.collisions > 0, ok);
    CHECK(ok);
    WAIT_UNTIL(200, pair_pump(&p), 0, ok);
    s2 = stats_of(p.host);
    CHECK(s2.datagrams_sent - s1.datagrams_sent < 60); /* the replaced snapshot's chunks were not sent anyway */
    CHECK_EQ(p.gc.collisions, 1); /* only the replacing query completes */
    CHECK_EQ(p.gc.coll_query, q2); CHECK_EQ(p.gc.coll_tris, 2 * RBL_MAX_TRIANGLES);
    free(big);
    pair_close(&p);
}

TEST(review_empty_collision_snapshot_is_one_chunk) {
    Pair p;
    int ok;
    float zero[3] = {0, 0, 0};
    uint32_t q;
    REQUIRE(pair_open(&p));
    CHECK_EQ(rbl_link_send_collision(p.host, zero, 5.0f, NULL, 0, &q), RBL_OK);
    WAIT_UNTIL(2000, pair_pump(&p), p.gc.collisions == 1, ok);
    REQUIRE(ok);
    CHECK_EQ(p.gc.coll_tris, 0); CHECK_EQ(p.gc.coll_query, q);
    pair_close(&p);
}

TEST(review_sending_without_a_peer_is_an_error) {
    rbl_config g;
    rbl_link* l;
    float zero[3] = {0, 0, 0};
    rbl_camera cam;
    rbl_pose pose;
    rbl_frame fr = {1, 0, 0, 1, 1};
    rbl_config_init(&g, "lonely"); g.control_port = 1;
    REQUIRE(rbl_link_guest(&g, &l) == RBL_OK);
    CHECK_EQ(rbl_link_send_collision(l, zero, 1.0f, NULL, 0, NULL), RBL_ERR_NOT_CONNECTED);
    memset(&cam, 0, sizeof cam); rbl_pose_identity(&pose, 1, 1);
    CHECK_EQ(rbl_link_send_camera(l, &cam, NULL), RBL_ERR_NOT_CONNECTED);
    CHECK_EQ(rbl_link_send_poses(l, &pose, 1), RBL_ERR_NOT_CONNECTED);
    CHECK_EQ(rbl_link_send_frame(l, &fr), RBL_ERR_NOT_CONNECTED);
    CHECK_EQ(rbl_link_spawn(l, 1, "p", "a", NULL), RBL_ERR_NOT_CONNECTED);
    CHECK_EQ(rbl_link_despawn(l, 1), RBL_ERR_NOT_CONNECTED);
    CHECK_EQ(rbl_link_set_focus(l, RBL_ROLE_GUEST), RBL_ERR_NOT_CONNECTED);
    CHECK_EQ(rbl_link_announce_frames(l, "/tmp/x", 1, 1, 1), RBL_ERR_NOT_CONNECTED);
    CHECK(!rbl_link_is_connected(l));
    rbl_link_close(l);
}

TEST(review_only_spawned_entities_have_pose_state_at_most_1024) {
    Pair p;
    int ok, e;
    uint64_t t;
    rbl_pose two[2];
    REQUIRE(pair_open(&p));
    CHECK_EQ(rbl_link_spawn(p.guest, 5, "P", "a", NULL), RBL_OK);
    t = rbl_now_us();
    rbl_pose_identity(&two[0], 5, t); rbl_pose_identity(&two[1], 6, t);
    CHECK_EQ(rbl_link_send_poses(p.guest, two, 2), RBL_OK);
    WAIT_UNTIL(300, pair_pump(&p), p.hc.pose_recs[5] == 1 && stats_of(p.host).unknown_entity == 1, ok);
    CHECK(ok);
    CHECK_EQ(p.hc.pose_recs[5], 1); /* the spawned entity */
    CHECK_EQ(p.hc.pose_recs[6], 0); /* never spawned */
    p.hc.spawns = 0;
    for (e = 0; e < 1030; e++) if (e != 5) rbl_link_spawn(p.guest, (uint32_t)e, "P", "a", NULL);
    WAIT_UNTIL(3000, pair_pump(&p), p.hc.spawns >= 1023 && stats_of(p.host).spawn_ignored >= 6, ok);
    CHECK(ok);
    CHECK_EQ(p.hc.spawns, 1023); /* spawns up to the table size (entity 5 was already live) */
    rbl_pose_identity(&two[0], 0, t + 5); rbl_pose_identity(&two[1], 1029, t + 5);
    CHECK_EQ(rbl_link_send_poses(p.guest, two, 2), RBL_OK);
    WAIT_UNTIL(500, pair_pump(&p), p.hc.pose_recs[0] == 1, ok);
    CHECK_EQ(p.hc.pose_recs[0], 1);    /* a live entity */
    CHECK_EQ(p.hc.pose_recs[1029], 0); /* its spawn was ignored */
    /* a despawn frees a place; a live id can always be spawned again */
    CHECK_EQ(rbl_link_despawn(p.guest, 0), RBL_OK);
    CHECK_EQ(rbl_link_spawn(p.guest, 1029, "P", "a", NULL), RBL_OK);
    CHECK_EQ(rbl_link_spawn(p.guest, 1, "P", "b", NULL), RBL_OK);
    p.hc.spawns = 0;
    WAIT_UNTIL(1000, pair_pump(&p), p.hc.spawns >= 2, ok);
    CHECK_EQ(p.hc.spawns, 2);
    rbl_pose_identity(&two[0], 1029, t + 10);
    CHECK_EQ(rbl_link_send_poses(p.guest, two, 1), RBL_OK);
    WAIT_UNTIL(500, pair_pump(&p), p.hc.pose_recs[1029] == 1, ok);
    CHECK_EQ(p.hc.pose_recs[1029], 1);
    pair_close(&p);
}

TEST(review_unknown_entity_and_spawn_ignored_counters) {
    Pair p;
    int e, ok;
    rbl_pose ps[3];
    uint64_t t;
    rbl_stats s;
    REQUIRE(pair_open(&p));
    rbl_link_spawn(p.guest, 5, "P", "a", NULL);
    t = rbl_now_us();
    rbl_pose_identity(&ps[0], 5, t); rbl_pose_identity(&ps[1], 6, t); rbl_pose_identity(&ps[2], 7, t);
    rbl_link_send_poses(p.guest, ps, 3);
    for (e = 100; e < 100 + 1030; e++) rbl_link_spawn(p.guest, (uint32_t)e, "P", "a", NULL);
    WAIT_UNTIL(3000, pair_pump(&p), stats_of(p.host).spawn_ignored >= 7, ok);
    s = stats_of(p.host);
    CHECK_EQ(s.unknown_entity, 2); /* poses of 6 and 7 */
    CHECK_EQ(s.spawn_ignored, 7);  /* 1 + 1030 spawns for 1024 places */
    pair_close(&p);
}

TEST(review_our_own_heartbeat_is_within_what_a_hello_may_announce) {
    rbl_config h, g;
    Pair p;
    rbl_peer_info hp;
    rbl_config_init(&h, "H"); h.control_port = 0;
    rbl_config_init(&g, "G"); g.heartbeat_ms = 1; /* outside 10..10000 */
    REQUIRE(pair_open_cfg(&p, &h, &g)); /* a guest configured with heartbeat 1 ms still connects */
    rbl_link_peer(p.host, &hp);
    CHECK_EQ(hp.heartbeat_ms, 10); /* it announced the nearest legal value */
    pair_close(&p);
    g.heartbeat_ms = 3600000u;
    REQUIRE(pair_open_cfg(&p, &h, &g));
    rbl_link_peer(p.host, &hp);
    CHECK_EQ(hp.heartbeat_ms, 10000);
    pair_close(&p);
}

/* Says hello as a raw guest with the given heartbeat; returns the host's HandshakeFailed reason, or NULL if it connected. */
static const char* hello_with_heartbeat(long long ms, Col* c) {
    rbl_config h;
    rbl_link* host;
    Raw r;
    char line[300];
    int ok;
    memset(c, 0, sizeof *c);
    rbl_config_init(&h, "H"); h.control_port = 0;
    if (rbl_link_host(&h, &host) != RBL_OK) return "no host";
    if (!raw_open(&r, rbl_link_control_port(host))) { rbl_link_close(host); return "no connection"; }
    hello_line(line, sizeof line, "guest", 1, 1, 9, ms);
    raw_send(&r, line);
    WAIT_UNTIL(2000, col_poll(host, c), c->connected + c->hsfail > 0, ok);
    raw_close(&r);
    rbl_link_close(host);
    if (!ok) return "no event";
    return c->hsfail ? c->hsfail_reason : NULL;
}

TEST(review_heartbeat_ms_outside_10_to_10000_is_a_handshake_error) {
    /* long long: 4294967295L is -1 where long has 32 bits (LLP64, Windows) */
    static const long long bad[] = {0, 1, 9, 10001, 3600000LL, 4294967295LL};
    static const long long good[] = {10, 250, 10000};
    size_t i;
    Col c;
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        const char* reason = hello_with_heartbeat(bad[i], &c);
        if (!reason || !strstr(reason, "heartbeat_ms")) { char m[200]; snprintf(m, sizeof m, "heartbeat_ms %lld: %s", bad[i], reason ? reason : "connected"); t_fail(__FILE__, __LINE__, m); }
    }
    for (i = 0; i < sizeof good / sizeof good[0]; i++) {
        const char* reason = hello_with_heartbeat(good[i], &c);
        if (reason) { char m[200]; snprintf(m, sizeof m, "heartbeat_ms %lld is fine but: %s", good[i], reason); t_fail(__FILE__, __LINE__, m); }
    }
}

TEST(announce_frames_sends_a_frame_channel_line_and_the_file_reads) {
    Pair p;
    char path[512];
    rbl_pixel_writer* w;
    rbl_pixel_reader* r;
    int ok;
    uint64_t f;
    static uint8_t px[32 * 16 * 4], got[32 * 16 * 4];
    static float d[32 * 16], gd[32 * 16];
    rbl_pixel_frame fi;
    REQUIRE(pair_open(&p));
    tmp_path(path, sizeof path, "announce.frames");
    REQUIRE(rbl_pixel_writer_create(path, 32, 16, 3, 1, &w) == RBL_OK);
    CHECK_EQ(rbl_link_announce_writer(p.guest, w), RBL_OK);
    WAIT_UNTIL(2000, pair_pump(&p), p.hc.frame_channels == 1, ok);
    REQUIRE(ok);
    CHECK_STR(p.hc.fc_path, rbl_pixel_writer_path(w)); CHECK_EQ(p.hc.fc_w, 32); CHECK_EQ(p.hc.fc_h, 16); CHECK_EQ(p.hc.fc_slots, 3);
    CHECK_EQ(rbl_pixel_writer_write(w, px, sizeof px, d, 32 * 16, 32, 16, 7, 99, &f), RBL_OK);
    REQUIRE(rbl_pixel_reader_open(p.hc.fc_path, &r) == RBL_OK);
    CHECK_EQ(rbl_pixel_reader_read(r, f, (uint32_t)(f % 3), got, sizeof got, gd, 32 * 16, &fi), RBL_OK);
    CHECK_EQ(fi.camera_seq, 7);
    rbl_pixel_reader_close(r);
    rbl_pixel_writer_close(w);
    pair_close(&p);
}

TEST(a_frame_channel_path_with_a_nul_is_delivered_as_null) {
    Pair p;
    Raw r;
    rbl_config h;
    rbl_link* host;
    Col c;
    char line[400];
    int ok;
    uint32_t session = 0;
    rbl_control hello;
    const char* why;
    memset(&c, 0, sizeof c);
    rbl_config_init(&h, "H"); h.control_port = 0;
    REQUIRE(rbl_link_host(&h, &host) == RBL_OK);
    REQUIRE(raw_open(&r, rbl_link_control_port(host)));
    hello_line(line, sizeof line, "guest", 1, 1, r.udp_port, 250);
    raw_send(&r, line);
    REQUIRE(raw_wait_line(&r, host, &c, 2000, line, sizeof line));
    REQUIRE(parse_line(line, &hello, &why) == RBL_OK);
    session = hello.u.hello.session;
    CHECK(session != 0);
    raw_send(&r, "{\"type\":\"frame_channel\",\"path\":\"/tmp/a\\u0000/etc/passwd\",\"width\":1,\"height\":1,\"slots\":1}\n");
    WAIT_UNTIL(2000, col_poll(host, &c), c.frame_channels == 1, ok);
    REQUIRE(ok);
    CHECK_STR(c.fc_path, "(null)");
    raw_close(&r);
    rbl_link_close(host);
    (void)p;
}

TEST(poll_with_a_small_array_loses_nothing) {
    Pair p;
    int ok, i;
    rbl_camera cam;
    float eye[3] = {0, 0, 5}, tgt[3] = {0, 0, 0};
    REQUIRE(pair_open(&p));
    rbl_camera_look_at(&cam, eye, tgt, 1.0f, 0.1f, 100.0f, 64, 64);
    for (i = 0; i < 100; i++) rbl_link_send_camera(p.host, &cam, NULL);
    for (i = 0; i < 60; i++) rbl_link_spawn(p.guest, (uint32_t)i, "P", "a", NULL);
    WAIT_UNTIL(5000, (col_poll_cap(p.host, &p.hc, RBL_MIN_EVENTS), col_poll_cap(p.guest, &p.gc, RBL_MIN_EVENTS)), p.gc.cameras == 100 && p.hc.spawns == 60, ok);
    CHECK(ok);
    CHECK_EQ(p.gc.cameras, 100); CHECK_EQ(p.hc.spawns, 60);
    { rbl_event ev[2]; CHECK_EQ(rbl_link_poll(p.host, ev, 2), RBL_ERR_ARGS); }
    pair_close(&p);
}

TEST(a_snapshot_above_the_receivers_limit_is_dropped) {
    rbl_config h, g;
    Pair p;
    int ok;
    size_t big = 2000, small = 500;
    rbl_triangle* tris = make_tris(big);
    float zero[3] = {0, 0, 0};
    rbl_config_init(&h, "H"); h.control_port = 0;
    rbl_config_init(&g, "G"); g.max_collision_triangles = 1000;
    REQUIRE(pair_open_cfg(&p, &h, &g));
    CHECK_EQ(rbl_link_send_collision(p.host, zero, 5.0f, tris, big, NULL), RBL_OK);
    WAIT_UNTIL(500, pair_pump(&p), stats_of(p.guest).malformed > 0, ok);
    CHECK(ok);
    WAIT_UNTIL(200, pair_pump(&p), 0, ok);
    CHECK_EQ(p.gc.collisions, 0);
    CHECK_EQ(rbl_link_send_collision(p.host, zero, 5.0f, tris, small, NULL), RBL_OK);
    WAIT_UNTIL(2000, pair_pump(&p), p.gc.collisions == 1, ok);
    CHECK(ok);
    CHECK_EQ(p.gc.coll_tris, small);
    free(tris);
    pair_close(&p);
}

TEST(fuzz_a_live_host_with_junk_datagrams_and_lines) {
    rbl_config h;
    rbl_link* host;
    Col c;
    Raw r;
    char line[400];
    rbl_control hello;
    const char* why;
    uint32_t session;
    uint8_t valid[3][8192], buf[8192];
    size_t vlen[3];
    long i;
    rbl_stats s;
    rbl_header hd;
    rbl_pose ps[2];
    rbl_camera cam;
    rbl_frame fr = {3, 0, 0, 4, 4};
    float eye[3] = {0, 0, 5}, tgt[3] = {0, 0, 0};
    memset(&c, 0, sizeof c);
    rbl_config_init(&h, "H"); h.control_port = 0; h.timeout_ms = 20000;
    REQUIRE(rbl_link_host(&h, &host) == RBL_OK);
    REQUIRE(raw_open(&r, rbl_link_control_port(host)));
    hello_line(line, sizeof line, "guest", 1, 1, r.udp_port, 250);
    raw_send(&r, line);
    REQUIRE(raw_wait_line(&r, host, &c, 2000, line, sizeof line));
    REQUIRE(parse_line(line, &hello, &why) == RBL_OK);
    session = hello.u.hello.session;
    hd = mk_header(session, 1, rbl_now_us());
    rbl_pose_identity(&ps[0], 1, 1); rbl_pose_identity(&ps[1], 2, 2);
    rbl_camera_look_at(&cam, eye, tgt, 1.0f, 0.1f, 100.0f, 8, 8);
    vlen[0] = (size_t)rbl_wire_encode_poses(&hd, ps, 2, valid[0], sizeof valid[0]);
    vlen[1] = (size_t)rbl_wire_encode_camera(&hd, &cam, valid[1], sizeof valid[1]);
    vlen[2] = (size_t)rbl_wire_encode_frame(&hd, &fr, valid[2], sizeof valid[2]);
    raw_send(&r, "{\"type\":\"spawn\",\"entity\":1,\"provider\":\"P\",\"asset\":\"a\"}\n");
    for (i = 0; i < 30000; i++) {
        size_t len;
        int kind = (int)(rnd() % 4);
        if (kind == 0) { /* pure junk */
            size_t k;
            len = rnd() % 600;
            for (k = 0; k < len; k++) buf[k] = (uint8_t)rnd();
        } else { /* a valid datagram with a few bytes changed, or cut short */
            int v = (int)(rnd() % 3);
            size_t k, flips = rnd() % 3;
            len = vlen[v];
            memcpy(buf, valid[v], len);
            for (k = 0; k < flips; k++) buf[rnd() % len] = (uint8_t)rnd();
            if (rnd() % 6 == 0) len = rnd() % (len + 1);
            if (rnd() % 10 == 0) memset(buf + 8, 0, 4); /* a wrong session */
        }
        rbli_udp_send(r.udp, buf, len, rbl_link_udp_port(host));
        if (i % 40 == 0) { col_poll(host, &c); raw_pump(&r); }
    }
    { int ok; WAIT_UNTIL(500, col_poll(host, &c), 0, ok); }
    s = stats_of(host);
    printf("     host after the fuzz: %lu received, %lu malformed, %lu foreign, %lu stale, %lu unknown entity\n", (unsigned long)s.datagrams_received,
           (unsigned long)s.malformed, (unsigned long)s.foreign, (unsigned long)s.stale, (unsigned long)s.unknown_entity);
    CHECK(rbl_link_is_connected(host));
    CHECK(s.malformed + s.foreign > 1000);
    raw_close(&r);
    rbl_link_close(host);
}

TEST(the_library_frees_everything_it_allocated) {
    printf("     %lu allocations, %lu frees\n", (unsigned long)g_lib_allocs, (unsigned long)g_lib_frees);
    CHECK(g_lib_allocs > 100);
    CHECK_EQ(g_lib_allocs, g_lib_frees);
}

int main(void) {
    printf("rewrite_link tests\n");
    RUN(layout_offsets);
    RUN(constants_match_contract);
    RUN(worked_example_encodes_byte_for_byte);
    RUN(worked_example_decodes_byte_for_byte);
    RUN(camera_datagram_offsets);
    RUN(pose_datagram_offsets_and_length);
    RUN(pose_full_datagram_is_8192_bytes);
    RUN(collision_chunking_offsets_and_length);
    RUN(frame_datagram_is_48_bytes);
    RUN(malformed_datagrams_are_rejected);
    RUN(fuzz_datagrams_never_crash);
    RUN(serial_arithmetic_and_negotiation);
    RUN(camera_helpers_and_quaternions);
    RUN(json_matches_serde_json);
    RUN(control_contract_examples_parse);
    RUN(control_broken_lines_are_protocol_errors);
    RUN(control_lines_round_trip);
    RUN(utf8_validation_is_strict);
    RUN(fuzz_control_lines_never_crash);
    RUN(handshake_state_flow_and_clean_bye);
    RUN(second_connection_gets_bye_busy);
    RUN(hello_with_wrong_role_protocol_or_garbage_gets_bye);
    RUN(watchdog_loses_a_silent_guest);
    RUN(bye_is_delivered_and_the_close_is_clean_while_the_closer_has_unread_data);
    RUN(close_returns_within_the_linger_even_if_the_peer_never_closes);
    RUN(datagrams_from_another_address_are_dropped);
    RUN(peer_address_must_be_127_0_0_1_and_the_announced_port);
    RUN(pixel_file_colour_only_round_trip);
    RUN(pixel_file_depth_plane);
    RUN(pixel_file_odd_lock_is_skipped);
    RUN(pixel_file_malformed_files_are_refused);
    RUN(pixel_file_a_truncated_copy_is_refused);
    RUN(review_pixel_header_limits_and_overflow);
    RUN(review_writer_limits_and_create_new);
    RUN(review_writer_validates_and_never_leaves_the_lock_odd);
#if !RBLI_WINDOWS
    RUN(review_symlink_directory_and_fifo_are_refused);
    RUN(review_a_file_truncated_under_a_reader_is_gone_not_a_crash);
#endif
    RUN(pixel_file_seqlock_under_contention);
    RUN(review_a_new_frame_channel_restarts_frame_numbers);
    RUN(review_a_400_chunk_collision_snapshot_completes);
    RUN(review_collision_chunks_are_paced_and_a_newer_snapshot_replaces_queued_ones);
    RUN(review_empty_collision_snapshot_is_one_chunk);
    RUN(review_sending_without_a_peer_is_an_error);
    RUN(review_only_spawned_entities_have_pose_state_at_most_1024);
    RUN(review_unknown_entity_and_spawn_ignored_counters);
    RUN(review_our_own_heartbeat_is_within_what_a_hello_may_announce);
    RUN(review_heartbeat_ms_outside_10_to_10000_is_a_handshake_error);
    RUN(announce_frames_sends_a_frame_channel_line_and_the_file_reads);
    RUN(a_frame_channel_path_with_a_nul_is_delivered_as_null);
    RUN(poll_with_a_small_array_loses_nothing);
    RUN(a_snapshot_above_the_receivers_limit_is_dropped);
    RUN(fuzz_a_live_host_with_junk_datagrams_and_lines);
    RUN(the_library_frees_everything_it_allocated);
    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
