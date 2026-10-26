/* The C side of the cross-language interop test of the link (rewrite-bridge: tests/interop/README.md).
 *
 *   interop_peer host|guest --port PORT --label LABEL
 *
 * One process plays one side of a scripted session: handshake, spawn and focus, five lock-step rounds of
 * camera / poses / collision / frames through a real pixel file with depth, despawn, bye. The host listens on
 * the control port PORT, the guest connects to it (retrying until a host answers). Both use heartbeat 100 ms
 * and timeout 3000 ms and the game name "interop host (LABEL)" / "interop guest (LABEL)".
 *
 * The last line on stdout is  RBL-INTEROP-RESULT {json}  and the exit status is 0 if every check passed, 1
 * if one failed (or a step waited more than 10 seconds, or 30 seconds passed), 2 for a bad command line.
 *
 *   make build/interop_peer
 *   build/interop_peer host --port 47810 --label c &  build/interop_peer guest --port 47810 --label c
 */
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

#define N_ROUNDS 5u
#define FRAME_W 64u
#define FRAME_H 48u
#define FRAME_SLOTS 3u
#define HOST_ENTITY 3u
#define GUEST_ENTITY 7u
#define STEP_LIMIT_MS 10000u
#define CLOSE_POLL_MS 200u
#define TOTAL_LIMIT_MS 30000u
#define BASE_US 1700000000000000ull
#define COLL_TRIANGLES 300u

#if defined(__GNUC__)
#define PRINTF_LIKE(f, a) __attribute__((format(printf, f, a)))
#else
#define PRINTF_LIKE(f, a)
#endif

typedef struct { uint32_t entity; int has; } Seen;

static struct {
    int host;
    const char* label;
    rbl_link* link;
    char game[200];
    uint64_t deadline;
    unsigned long progress; /* bumped by every event: a wait that sees progress gets its 10 seconds again */
    int fatal;              /* the session cannot go on (handshake failed, lost too early) */
    int aborted;            /* a step gave up: the totals at the end would only repeat the failure */
    int connected, lost, expect_lost;
    char peer_game[160];
    uint32_t protocol, session;
    char lost_reason[200];
    uint32_t spawns[8], despawns[8];
    int nspawns, ndespawns;
    unsigned cameras, cameras_seen, poses, poses_seen, frames_ok, frames_bad, frame_events;
    uint32_t collision_triangles;
    int collision_seen;
    char focus[4][24];
    int nfocus, focus_back_sent;
    char fails[32][240];
    int nfail;
    /* guest */
    rbl_pixel_writer* writer;
    /* host */
    rbl_pixel_reader* reader;
    char fc_path[1024];
    uint32_t fc_w, fc_h, fc_slots;
    int have_fc;
    uint32_t cam_seq[N_ROUNDS + 2];
    rbl_frame frames[N_ROUNDS + 2];
} G;

/* The library's counters, read when the rounds are over (a heartbeat still in flight at the end of the session is
 * legitimately counted foreign afterwards): by the host just before its despawn, by the guest on seeing it. */
static rbl_stats g_stats;
static int g_have_stats;
static void snapshot_stats(void) { rbl_link_stats(G.link, &g_stats); g_have_stats = 1; }

static void logf_(const char* fmt, ...) PRINTF_LIKE(1, 2);
static void logf_(const char* fmt, ...) {
    va_list ap;
    printf("[%s] ", G.host ? "host" : "guest");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static void fail(const char* fmt, ...) PRINTF_LIKE(1, 2);
static void fail(const char* fmt, ...) {
    char msg[240];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    logf_("FAIL: %s", msg);
    if (G.nfail < (int)(sizeof G.fails / sizeof G.fails[0])) {
        memcpy(G.fails[G.nfail], msg, sizeof msg);
        G.nfail++;
    }
}

/* ------------------------------------------------------------------ the data of the session (README: Data) */

static int same_f(float a, float b) { return memcmp(&a, &b, sizeof a) == 0; }

static void exp_camera(unsigned k, rbl_camera* c) {
    static const float proj[16] = { 1.5f, 0, 0, 0, 0, 2, 0, 0, 0, 0, -1, -1, 0, 0, -0.25f, 0 };
    memset(c, 0, sizeof *c);
    c->view[0] = 1; c->view[5] = 1; c->view[10] = 1; c->view[15] = 1;
    c->view[12] = (float)k; c->view[13] = 0.5f; c->view[14] = -8.0f;
    memcpy(c->proj, proj, sizeof proj);
    c->fov_y = 1.0f;
    c->near_m = 0.25f;
    c->far_m = (k % 2u == 1u) ? (float)HUGE_VAL : 500.0f;
    c->viewport_width = FRAME_W;
    c->viewport_height = FRAME_H;
    c->depth_range = k % 3u;
}

/* Pose k of the host's entity 3, or of the guest's entity 7 */
static void exp_pose(int host_entity, unsigned k, rbl_pose* p) {
    memset(p, 0, sizeof *p);
    p->entity = host_entity ? HOST_ENTITY : GUEST_ENTITY;
    p->flags = k == 1 ? RBL_POSE_TELEPORT : 0;
    p->time_us = BASE_US + 1000ull * k + (host_entity ? 0u : 500u);
    p->translation[0] = host_entity ? (float)k : -(float)k;
    p->translation[1] = host_entity ? 2.0f : 1.0f;
    p->translation[2] = host_entity ? -3.0f : 4.0f;
    p->rotation[3] = 1.0f;
    p->scale[0] = p->scale[1] = p->scale[2] = 1.0f;
    p->velocity[0] = 0.5f;
    p->angular_velocity[1] = 0.25f;
}

static void exp_triangle(unsigned i, rbl_triangle* t) {
    float b = (float)(i % 64u);
    memset(t, 0, sizeof *t);
    t->v[0][0] = b;
    t->v[1][0] = b + 1.0f;
    t->v[2][0] = b; t->v[2][2] = 1.0f;
}

/* Pixels k: rgba[i] = (37 i + 11 k + 5) mod 256 with pixel 0 all zero; depth[p] = +inf for p = 0 else 0.5 (p + 1) + k */
static void exp_pixels(unsigned k, uint8_t* rgba, float* depth) {
    uint32_t i, p;
    for (i = 0; i < FRAME_W * FRAME_H * 4u; i++) rgba[i] = (uint8_t)((37u * i + 11u * k + 5u) % 256u);
    rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0;
    for (p = 0; p < FRAME_W * FRAME_H; p++) depth[p] = p == 0 ? (float)HUGE_VAL : (float)(0.5 * (double)(p + 1u) + (double)k);
}

static int pose_matches(const rbl_pose* g, const rbl_pose* w) {
    int i;
    if (g->entity != w->entity || g->flags != w->flags || g->time_us != w->time_us) return 0;
    for (i = 0; i < 3; i++) {
        if (!same_f(g->translation[i], w->translation[i]) || !same_f(g->scale[i], w->scale[i]) ||
            !same_f(g->velocity[i], w->velocity[i]) || !same_f(g->angular_velocity[i], w->angular_velocity[i])) return 0;
    }
    for (i = 0; i < 4; i++) if (!same_f(g->rotation[i], w->rotation[i])) return 0;
    return 1;
}

/* ------------------------------------------------------------------ events */

static void note_focus(const char* s) {
    if (G.nfocus < 4) snprintf(G.focus[G.nfocus++], sizeof G.focus[0], "%s", s);
}

static void on_connected(const rbl_event* e) {
    const char* want = G.host ? "interop guest (" : "interop host (";
    size_t wl = strlen(want), gl = strlen(e->u.connected.game);
    G.connected = 1;
    snprintf(G.peer_game, sizeof G.peer_game, "%s", e->u.connected.game);
    G.protocol = e->u.connected.protocol;
    G.session = e->u.connected.session;
    logf_("connected: %s, protocol %u, session %u", G.peer_game, G.protocol, G.session);
    /* the peer's label is its own: only the prefix of its game name is known here (the driver knows both labels) */
    if (gl <= wl || strncmp(e->u.connected.game, want, wl) != 0)
        fail("the peer's game is \"%s\", expected it to start with \"%s\"", e->u.connected.game, want);
    if (G.protocol != 1) fail("the negotiated protocol is %u, expected 1", G.protocol);
    if (G.session == 0) fail("the session number is 0");
    if (G.host) {
        int rc = rbl_link_spawn(G.link, HOST_ENTITY, "War Thunder", "/interop/tank", "Host Tank");
        if (rc != RBL_OK) fail("spawn failed: %s", rbl_strerror(rc));
        rc = rbl_link_set_focus(G.link, RBL_ROLE_GUEST);
        if (rc != RBL_OK) fail("giving the focus to the guest failed: %s", rbl_strerror(rc));
    } else {
        int rc = rbl_link_spawn(G.link, GUEST_ENTITY, "Warframe", "/Lotus/Characters/Excalibur", "Excalibur");
        if (rc != RBL_OK) fail("spawn failed: %s", rbl_strerror(rc));
        rc = rbl_pixel_writer_create_temp("interop", FRAME_W, FRAME_H, FRAME_SLOTS, 1, &G.writer);
        if (rc != RBL_OK) { fail("creating the pixel file failed: %s", rbl_strerror(rc)); G.fatal = 1; return; }
        logf_("pixel file %s", rbl_pixel_writer_path(G.writer));
        rc = rbl_link_announce_writer(G.link, G.writer);
        if (rc != RBL_OK) fail("announcing the pixel file failed: %s", rbl_strerror(rc));
    }
}

static void on_spawn(const rbl_event* e) {
    uint32_t want = G.host ? GUEST_ENTITY : HOST_ENTITY;
    const char* prov = G.host ? "Warframe" : "War Thunder";
    const char* asset = G.host ? "/Lotus/Characters/Excalibur" : "/interop/tank";
    const char* name = G.host ? "Excalibur" : "Host Tank";
    if (G.nspawns < 8) G.spawns[G.nspawns++] = e->u.spawn.entity;
    logf_("peer spawned entity %u", e->u.spawn.entity);
    if (e->u.spawn.entity != want) fail("the peer spawned entity %u, expected %u", e->u.spawn.entity, want);
    if (!e->u.spawn.provider || strcmp(e->u.spawn.provider, prov) != 0) fail("spawn provider is \"%s\", expected \"%s\"", e->u.spawn.provider ? e->u.spawn.provider : "(null)", prov);
    if (!e->u.spawn.asset || strcmp(e->u.spawn.asset, asset) != 0) fail("spawn asset is \"%s\", expected \"%s\"", e->u.spawn.asset ? e->u.spawn.asset : "(null)", asset);
    if (!e->u.spawn.name || strcmp(e->u.spawn.name, name) != 0) fail("spawn name is \"%s\", expected \"%s\"", e->u.spawn.name ? e->u.spawn.name : "(null)", name);
}

static void on_focus(const rbl_event* e) {
    char s[32];
    if (G.host) {
        /* the guest asks for the focus back (input host): a request, named by the focus value it carries */
        if (!e->u.focus.requested) fail("a focus event on the host that is not a request");
        if (e->u.focus.focus != RBL_ROLE_HOST) fail("the guest's request carries focus %s, expected host", e->u.focus.focus == RBL_ROLE_HOST ? "host" : "guest");
        snprintf(s, sizeof s, "%s:request", e->u.focus.focus == RBL_ROLE_HOST ? "host" : "guest");
        note_focus(s);
        logf_("the guest asks for the focus: setting it to the host");
        {   int rc = rbl_link_set_focus(G.link, RBL_ROLE_HOST);
            if (rc != RBL_OK) fail("setting the focus to the host failed: %s", rbl_strerror(rc)); }
    } else {
        if (e->u.focus.requested) fail("a focus event on the guest that is a request");
        snprintf(s, sizeof s, "%s:set", e->u.focus.focus == RBL_ROLE_HOST ? "host" : "guest");
        note_focus(s);
        logf_("focus event %s", s);
        if (e->u.focus.focus == RBL_ROLE_GUEST && !G.focus_back_sent) {
            int rc = rbl_link_set_focus(G.link, RBL_ROLE_HOST); /* ask for it back */
            G.focus_back_sent = 1;
            if (rc != RBL_OK) fail("requesting the focus back failed: %s", rbl_strerror(rc));
        }
    }
}

static void on_camera(const rbl_event* e) {
    unsigned k = ++G.cameras_seen;
    rbl_camera want;
    rbl_pose p;
    uint8_t rgba[FRAME_W * FRAME_H * 4u];
    float depth[FRAME_W * FRAME_H];
    uint64_t frame = 0;
    rbl_frame f;
    int rc, ok = 1;
    if (G.host) { fail("the host received a camera"); return; }
    if (k > N_ROUNDS) { fail("camera %u received, expected %u", k, N_ROUNDS); return; }
    exp_camera(k, &want);
    {   const rbl_camera* c = &e->u.camera.camera;
        if (memcmp(c->view, want.view, sizeof want.view) != 0 || memcmp(c->proj, want.proj, sizeof want.proj) != 0 ||
            !same_f(c->fov_y, want.fov_y) || !same_f(c->near_m, want.near_m) || !same_f(c->far_m, want.far_m) ||
            c->viewport_width != want.viewport_width || c->viewport_height != want.viewport_height || c->depth_range != want.depth_range) {
            fail("camera %u differs from the expected one", k);
            ok = 0;
        }
    }
    if (ok) G.cameras++;
    /* the reply: Pose k of entity 7, then frame k into the pixel file and its Frame datagram */
    exp_pose(0, k, &p);
    rc = rbl_link_send_poses(G.link, &p, 1);
    if (rc != RBL_OK) fail("sending pose %u failed: %s", k, rbl_strerror(rc));
    exp_pixels(k, rgba, depth);
    rc = rbl_pixel_writer_write(G.writer, rgba, sizeof rgba, depth, FRAME_W * FRAME_H, FRAME_W, FRAME_H, e->u.camera.header.seq,
                                BASE_US + 16666ull * k, &frame);
    if (rc != RBL_OK) { fail("writing frame %u failed: %s", k, rbl_strerror(rc)); return; }
    if (frame != k) fail("the pixel writer numbered the frame %llu, expected %u", (unsigned long long)frame, k);
    f.frame = frame; f.slot = (uint32_t)(frame % FRAME_SLOTS); f.camera_seq = e->u.camera.header.seq; f.width = FRAME_W; f.height = FRAME_H;
    rc = rbl_link_send_frame(G.link, &f);
    if (rc != RBL_OK) fail("announcing frame %u failed: %s", k, rbl_strerror(rc));
}

static void on_poses(const rbl_event* e) {
    uint32_t i;
    for (i = 0; i < e->u.poses.count; i++) {
        unsigned k = ++G.poses_seen;
        rbl_pose want;
        if (k > N_ROUNDS) { fail("pose %u received, expected %u", k, N_ROUNDS); continue; }
        exp_pose(!G.host, k, &want); /* the peer's entity: the host's 3 for the guest, the guest's 7 for the host */
        if (!pose_matches(&e->u.poses.poses[i], &want)) fail("pose %u of entity %u differs from the expected one", k, e->u.poses.poses[i].entity);
        else G.poses++;
    }
}

static void on_collision(const rbl_event* e) {
    uint32_t i;
    int ok = 1;
    G.collision_seen = 1;
    G.collision_triangles = e->u.collision.triangle_count;
    if (G.host) { fail("the host received a collision snapshot"); return; }
    if (e->u.collision.triangle_count != COLL_TRIANGLES) { fail("the collision snapshot has %u triangles, expected %u", e->u.collision.triangle_count, COLL_TRIANGLES); return; }
    if (!same_f(e->u.collision.center[0], 1.0f) || !same_f(e->u.collision.center[1], 2.0f) || !same_f(e->u.collision.center[2], 3.0f) ||
        !same_f(e->u.collision.radius, 50.0f)) { fail("the collision snapshot's center or radius is wrong"); ok = 0; }
    for (i = 0; i < COLL_TRIANGLES && ok; i++) {
        rbl_triangle t;
        exp_triangle(i, &t);
        if (memcmp(&t, &e->u.collision.triangles[i], sizeof t) != 0) { fail("collision triangle %u differs from the expected one", i); ok = 0; }
    }
    if (ok) logf_("collision snapshot of %u triangles verified", e->u.collision.triangle_count);
}

static void on_event(const rbl_event* e) {
    G.progress++;
    switch (e->type) {
    case RBL_EV_CONNECTED: on_connected(e); break;
    case RBL_EV_LOST:
        G.lost = 1;
        snprintf(G.lost_reason, sizeof G.lost_reason, "%s", e->u.lost.text);
        logf_("lost: %s", G.lost_reason);
        if (!G.expect_lost) { fail("the session was lost: %s", G.lost_reason); G.fatal = 1; }
        break;
    case RBL_EV_HANDSHAKE_FAILED: fail("the handshake failed: %s", e->u.handshake_failed.text); G.fatal = 1; break;
    case RBL_EV_CAMERA: on_camera(e); break;
    case RBL_EV_POSES: on_poses(e); break;
    case RBL_EV_COLLISION: on_collision(e); break;
    case RBL_EV_FRAME:
        if (G.frame_events < N_ROUNDS + 1u) G.frames[++G.frame_events] = e->u.frame.frame; else fail("a frame event too many");
        break;
    case RBL_EV_SPAWN: on_spawn(e); break;
    case RBL_EV_DESPAWN:
        if (G.ndespawns < 8) G.despawns[G.ndespawns++] = e->u.despawn.entity;
        logf_("peer despawned entity %u", e->u.despawn.entity);
        break;
    case RBL_EV_FOCUS: on_focus(e); break;
    case RBL_EV_FRAME_CHANNEL:
        if (!e->u.frame_channel.path) { fail("the frame_channel path is NULL"); break; }
        snprintf(G.fc_path, sizeof G.fc_path, "%s", e->u.frame_channel.path);
        G.fc_w = e->u.frame_channel.width; G.fc_h = e->u.frame_channel.height; G.fc_slots = e->u.frame_channel.slots;
        G.have_fc = 1;
        logf_("frame channel %s %ux%u, %u slots", G.fc_path, G.fc_w, G.fc_h, G.fc_slots);
        break;
    default: break;
    }
}

static void pump(void) {
    rbl_event evs[64];
    int n = rbl_link_poll(G.link, evs, 64), i;
    for (i = 0; i < n; i++) on_event(&evs[i]);
}

/* Polls until cond; fails (and leaves the run function) after 10 seconds without progress or 30 seconds in all. */
#define WAIT_UNTIL(what, cond) do { \
    uint64_t t0_ = rbli_mono_ms(); unsigned long seen_ = G.progress; \
    for (;;) { \
        pump(); \
        if (cond) break; \
        if (G.fatal) { G.aborted = 1; goto done; } \
        if (rbli_mono_ms() - t0_ > STEP_LIMIT_MS) { fail("waited more than %u seconds for %s", STEP_LIMIT_MS / 1000u, what); G.aborted = 1; goto done; } \
        if (rbli_mono_ms() > G.deadline) { fail("%u seconds passed while waiting for %s", TOTAL_LIMIT_MS / 1000u, what); G.aborted = 1; goto done; } \
        if (G.progress != seen_) { seen_ = G.progress; t0_ = rbli_mono_ms(); } \
        rbli_sleep_ms(2); \
    } } while (0)

static int has_entity(const uint32_t* list, int n, uint32_t id) {
    int i;
    for (i = 0; i < n; i++) if (list[i] == id) return 1;
    return 0;
}

/* ------------------------------------------------------------------ the two roles */

static void host_check_frame(unsigned k) {
    uint8_t rgba[FRAME_W * FRAME_H * 4u], want_rgba[FRAME_W * FRAME_H * 4u];
    float depth[FRAME_W * FRAME_H], want_depth[FRAME_W * FRAME_H];
    const rbl_frame* f = &G.frames[k];
    rbl_pixel_frame info;
    int rc, bad = 0;
    memset(&info, 0, sizeof info);
    if (f->frame != k || f->camera_seq != G.cam_seq[k] || f->width != FRAME_W || f->height != FRAME_H || f->slot != k % FRAME_SLOTS) {
        fail("frame event %u: frame %llu slot %u camera_seq %u %ux%u, expected frame %u slot %u camera_seq %u %ux%u", k, (unsigned long long)f->frame,
             f->slot, f->camera_seq, f->width, f->height, k, k % FRAME_SLOTS, G.cam_seq[k], FRAME_W, FRAME_H);
        bad = 1;
    }
    rc = rbl_pixel_reader_read_frame(G.reader, f, rgba, sizeof rgba, depth, FRAME_W * FRAME_H, &info);
    if (rc != RBL_OK) { fail("reading frame %u from the pixel file: %s", k, rbl_strerror(rc)); G.frames_bad++; return; }
    exp_pixels(k, want_rgba, want_depth);
    if (info.frame != k || info.camera_seq != G.cam_seq[k] || info.width != FRAME_W || info.height != FRAME_H || info.time_us != BASE_US + 16666ull * k) {
        fail("frame %u in the pixel file: frame %llu camera_seq %u %ux%u time_us %llu, expected frame %u camera_seq %u %ux%u time_us %llu", k,
             (unsigned long long)info.frame, info.camera_seq, info.width, info.height, (unsigned long long)info.time_us, k, G.cam_seq[k], FRAME_W, FRAME_H,
             (unsigned long long)(BASE_US + 16666ull * k));
        bad = 1;
    }
    if (!info.has_depth) { fail("frame %u was read without depth", k); bad = 1; }
    if (memcmp(rgba, want_rgba, sizeof rgba) != 0) { fail("the colour of frame %u differs from the expected pixels", k); bad = 1; }
    if (info.has_depth && memcmp(depth, want_depth, sizeof depth) != 0) { fail("the depth of frame %u differs from the expected values", k); bad = 1; }
    if (bad) G.frames_bad++; else G.frames_ok++;
}

static void run_host(void) {
    unsigned k;
    WAIT_UNTIL("the guest's hello", G.connected);
    /* step 3: the guest's spawn and frame_channel, then the pixel file */
    WAIT_UNTIL("the guest's spawn and frame_channel", has_entity(G.spawns, G.nspawns, GUEST_ENTITY) && G.have_fc);
    if (G.fc_w != FRAME_W || G.fc_h != FRAME_H || G.fc_slots != FRAME_SLOTS)
        fail("the announced pixel file is %ux%u with %u slots, expected %ux%u with %u", G.fc_w, G.fc_h, G.fc_slots, FRAME_W, FRAME_H, FRAME_SLOTS);
    {   int rc = rbl_pixel_reader_open(G.fc_path, &G.reader);
        if (rc != RBL_OK) { fail("opening the guest's pixel file: %s", rbl_strerror(rc)); G.aborted = 1; goto done; }
        if (!rbl_pixel_reader_has_depth(G.reader)) fail("the guest's pixel file has no depth plane"); }
    /* step 4: lock-step rounds */
    for (k = 1; k <= N_ROUNDS; k++) {
        rbl_camera cam;
        rbl_pose p;
        uint32_t seq = 0;
        int rc;
        exp_camera(k, &cam);
        rc = rbl_link_send_camera(G.link, &cam, &seq);
        if (rc != RBL_OK) { fail("sending camera %u failed: %s", k, rbl_strerror(rc)); G.aborted = 1; goto done; }
        G.cam_seq[k] = seq;
        exp_pose(1, k, &p);
        rc = rbl_link_send_poses(G.link, &p, 1);
        if (rc != RBL_OK) { fail("sending pose %u failed: %s", k, rbl_strerror(rc)); G.aborted = 1; goto done; }
        if (k == 1) {
            static rbl_triangle tris[COLL_TRIANGLES];
            static const float center[3] = { 1.0f, 2.0f, 3.0f };
            uint32_t i, q = 0;
            for (i = 0; i < COLL_TRIANGLES; i++) exp_triangle(i, &tris[i]);
            rc = rbl_link_send_collision(G.link, center, 50.0f, tris, COLL_TRIANGLES, &q);
            if (rc != RBL_OK) { fail("sending the collision snapshot failed: %s", rbl_strerror(rc)); G.aborted = 1; goto done; }
        }
        WAIT_UNTIL("the guest's pose and frame", G.poses_seen >= k && G.frame_events >= k);
        host_check_frame(k);
        logf_("round %u done (camera seq %u)", k, G.cam_seq[k]);
    }
    /* the focus exchange must have run its course */
    WAIT_UNTIL("the guest's focus request", G.nfocus >= 1);
    if (rbl_link_focus(G.link) != RBL_ROLE_HOST) fail("the host's focus is not the host after it took it back");
    /* step 5: everything the guest sent has been read; the host despawns its entity, the guest answers with its own */
    G.expect_lost = 1; /* the guest's despawn and its bye can come in one poll */
    snapshot_stats();
    {   int rc = rbl_link_despawn(G.link, HOST_ENTITY);
        if (rc != RBL_OK) fail("despawn failed: %s", rbl_strerror(rc)); }
    WAIT_UNTIL("the guest's despawn", has_entity(G.despawns, G.ndespawns, GUEST_ENTITY));
    /* step 6: the guest's bye */
    WAIT_UNTIL("the guest's bye", G.lost);
    if (!strstr(G.lost_reason, "bye")) fail("the reason of the loss is \"%s\", expected it to contain \"bye\"", G.lost_reason);
done:
    return;
}

static void run_guest(void) {
    WAIT_UNTIL("the host's hello", G.connected);
    WAIT_UNTIL("the host's spawn", has_entity(G.spawns, G.nspawns, HOST_ENTITY));
    /* step 5: the host's despawn says it has read everything this side sent; answer with the despawn of entity 7 */
    WAIT_UNTIL("the host's despawn", has_entity(G.despawns, G.ndespawns, HOST_ENTITY));
    if (rbl_link_focus(G.link) != RBL_ROLE_HOST) fail("the guest's idea of the focus is not the host after the host took it back");
    snapshot_stats();
    {   int rc = rbl_link_despawn(G.link, GUEST_ENTITY);
        if (rc != RBL_OK) fail("despawn failed: %s", rbl_strerror(rc)); }
done:
    return;
}

/* ------------------------------------------------------------------ result */

static void json_str(char* out, size_t cap, size_t* n, const char* s) {
    size_t i;
    if (*n + 2 < cap) out[(*n)++] = '"';
    for (i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        char tmp[8];
        size_t l;
        if (c == '"' || c == '\\') { tmp[0] = '\\'; tmp[1] = (char)c; l = 2; }
        else if (c < 0x20) { snprintf(tmp, sizeof tmp, "\\u%04x", (unsigned)c); l = 6; }
        else { tmp[0] = (char)c; l = 1; }
        if (*n + l + 2 >= cap) break;
        memcpy(out + *n, tmp, l);
        *n += l;
    }
    if (*n + 2 < cap) out[(*n)++] = '"';
    out[*n] = 0;
}
static void js_add(char* out, size_t cap, size_t* n, const char* fmt, ...) PRINTF_LIKE(4, 5);
static void js_add(char* out, size_t cap, size_t* n, const char* fmt, ...) {
    va_list ap;
    int k;
    va_start(ap, fmt);
    k = vsnprintf(out + *n, cap - *n, fmt, ap);
    va_end(ap);
    if (k > 0) *n = (*n + (size_t)k < cap) ? *n + (size_t)k : cap - 1;
}
static void js_ids(char* out, size_t cap, size_t* n, const uint32_t* v, int count) {
    int i;
    js_add(out, cap, n, "[");
    for (i = 0; i < count; i++) js_add(out, cap, n, "%s%u", i ? "," : "", v[i]);
    js_add(out, cap, n, "]");
}

static void print_result(const rbl_stats* st, int have_stats) {
    static char out[16384];
    size_t n = 0, cap = sizeof out;
    int i;
    js_add(out, cap, &n, "{\"role\":");
    json_str(out, cap, &n, G.host ? "host" : "guest");
    js_add(out, cap, &n, ",\"label\":");
    json_str(out, cap, &n, G.label);
    js_add(out, cap, &n, ",\"ok\":%s,\"failures\":[", G.nfail == 0 ? "true" : "false");
    for (i = 0; i < G.nfail; i++) { if (i) js_add(out, cap, &n, ","); json_str(out, cap, &n, G.fails[i]); }
    js_add(out, cap, &n, "],\"peer_game\":");
    if (G.connected) json_str(out, cap, &n, G.peer_game); else js_add(out, cap, &n, "null");
    if (G.connected) js_add(out, cap, &n, ",\"protocol\":%u,\"session\":%u", G.protocol, G.session);
    else js_add(out, cap, &n, ",\"protocol\":null,\"session\":null");
    js_add(out, cap, &n, ",\"spawns\":");
    js_ids(out, cap, &n, G.spawns, G.nspawns);
    js_add(out, cap, &n, ",\"despawns\":");
    js_ids(out, cap, &n, G.despawns, G.ndespawns);
    js_add(out, cap, &n, ",\"cameras\":%u,\"poses\":%u,\"collision_triangles\":%u,\"frames_ok\":%u,\"frames_bad\":%u,\"focus\":[", G.cameras, G.poses,
           (unsigned)G.collision_triangles, G.frames_ok, G.frames_bad);
    for (i = 0; i < G.nfocus; i++) { if (i) js_add(out, cap, &n, ","); json_str(out, cap, &n, G.focus[i]); }
    js_add(out, cap, &n, "],\"lost_reason\":");
    if (G.lost) json_str(out, cap, &n, G.lost_reason); else js_add(out, cap, &n, "null");
    if (have_stats)
        js_add(out, cap, &n, ",\"stats\":{\"malformed\":%llu,\"foreign\":%llu,\"stale\":%llu,\"unknown_entity\":%llu,\"spawn_ignored\":%llu}}",
               (unsigned long long)st->malformed, (unsigned long long)st->foreign, (unsigned long long)st->stale, (unsigned long long)st->unknown_entity,
               (unsigned long long)st->spawn_ignored);
    else js_add(out, cap, &n, ",\"stats\":null}");
    printf("RBL-INTEROP-RESULT %s\n", out);
    fflush(stdout);
}

static int usage(void) {
    fprintf(stderr, "usage: interop_peer host|guest --port PORT --label LABEL\n");
    return 2;
}

int main(int argc, char** argv) {
    rbl_config cfg;
    int i, rc;
    long port = -1;
    const char* label = NULL;
    uint64_t start = rbli_mono_ms();
    if (argc < 2 || (strcmp(argv[1], "host") != 0 && strcmp(argv[1], "guest") != 0)) return usage();
    G.host = strcmp(argv[1], "host") == 0;
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            char* end = NULL;
            port = strtol(argv[++i], &end, 10);
            if (!end || *end || port < 1 || port > 65535) return usage();
        } else if (strcmp(argv[i], "--label") == 0 && i + 1 < argc) {
            label = argv[++i];
        } else {
            return usage();
        }
    }
    if (port < 0 || !label) return usage();
    G.label = label;
    G.deadline = start + TOTAL_LIMIT_MS;
    snprintf(G.game, sizeof G.game, "interop %s (%s)", G.host ? "host" : "guest", label);

    rbl_config_init(&cfg, G.game);
    cfg.control_port = (uint16_t)port;
    cfg.udp_port = 0;
    cfg.heartbeat_ms = 100;
    cfg.timeout_ms = 3000;
    cfg.reconnect_ms = 100;
    rc = G.host ? rbl_link_host(&cfg, &G.link) : rbl_link_guest(&cfg, &G.link);
    if (rc != RBL_OK) {
        G.link = NULL;
        fail("cannot start the %s on port %ld: %s", G.host ? "host" : "guest", port, rbl_strerror(rc));
    } else {
        logf_("%s, control port %ld, udp port %u", G.game, port, (unsigned)rbl_link_udp_port(G.link));
        if (G.host) run_host(); else run_guest();
        pump();
        if (!G.host && G.connected) {
            /* step 6 on the guest: bye, poll for 200 ms so that the closing drains; a Lost event for it would be a failure */
            uint64_t until = rbli_mono_ms() + CLOSE_POLL_MS;
            rbl_link_disconnect(G.link, "done");
            while (rbli_mono_ms() < until) { pump(); rbli_sleep_ms(2); }
        }
    }
    /* the totals */
    if (G.aborted || !G.link) {
        /* nothing to add */
    } else if (G.host) {
        if (G.nspawns != 1 || G.spawns[0] != GUEST_ENTITY) fail("the guest's spawns were not exactly [7]");
        if (G.ndespawns != 1 || G.despawns[0] != GUEST_ENTITY) fail("the guest's despawns were not exactly [7]");
        if (G.poses != N_ROUNDS) fail("%u of %u guest poses verified", G.poses, N_ROUNDS);
        if (G.frames_ok != N_ROUNDS || G.frames_bad != 0) fail("frames: %u ok, %u bad, expected %u ok", G.frames_ok, G.frames_bad, N_ROUNDS);
        if (G.nfocus != 1 || strcmp(G.focus[0], "host:request") != 0) fail("the host's focus events were not exactly [host:request]");
    } else {
        if (G.nspawns != 1 || G.spawns[0] != HOST_ENTITY) fail("the host's spawns were not exactly [3]");
        if (G.ndespawns != 1 || G.despawns[0] != HOST_ENTITY) fail("the host's despawns were not exactly [3]");
        if (G.cameras != N_ROUNDS) fail("%u of %u cameras verified", G.cameras, N_ROUNDS);
        if (G.poses != N_ROUNDS) fail("%u of %u host poses verified", G.poses, N_ROUNDS);
        if (G.collision_triangles != COLL_TRIANGLES) fail("the collision snapshot had %u triangles, expected %u", (unsigned)G.collision_triangles, COLL_TRIANGLES);
        if (G.nfocus != 2 || strcmp(G.focus[0], "guest:set") != 0 || strcmp(G.focus[1], "host:set") != 0) fail("the guest's focus events were not exactly [guest:set, host:set]");
        if (G.lost) fail("the guest got a Lost event");
    }
    print_result(&g_stats, g_have_stats);
    if (G.reader) rbl_pixel_reader_close(G.reader);
    rbl_link_close(G.link);
    if (G.writer) rbl_pixel_writer_close(G.writer);
    return G.nfail == 0 ? 0 : 1;
}
