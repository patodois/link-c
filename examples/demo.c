/* demo.c - a command-line stand-in host or guest for rewrite_link.h, the C twin of link-java's
 * LinkDemo and of the Rust fake_host / fake_guest examples. Used for the interop tests.
 *
 *   demo host  [--port 47800] [--seconds 6] [--frames] [--big-collision 400]
 *   demo guest [--port 47800] [--seconds 6] [--frames] [--orbit]
 *
 * guest --frames renders a disc with linear depth into a 320x240 pixel file and announces it for every
 * camera it gets; the host reads such frames and reports what is in them. host --big-collision N sends
 * a snapshot of N full chunks each second instead of the small ground patch. guest --orbit circles the
 * camera at 8 m instead of floating 3 m ahead of it.
 */
/* strict C99 hides POSIX (nanosleep): ask for it before the first system header */
#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif
#endif

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../rewrite_link.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static void sleep_ms(unsigned ms) { Sleep(ms); }
#else
#include <time.h>
static void sleep_ms(unsigned ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

#define FRAME_W 320
#define FRAME_H 240

static uint64_t g_start_us;
static int g_orbit, g_frames;
static unsigned g_big_collision;

static double elapsed(void) { return (double)(rbl_now_us() - g_start_us) / 1e6; }

#if defined(__GNUC__)
static void logf_(const char* role, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
#endif
static void logf_(const char* role, const char* fmt, ...) {
    va_list ap;
    printf("[c-%s %6.2fs] ", role, elapsed());
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static const char* arg(int argc, char** argv, const char* name, const char* def) {
    int i;
    for (i = 1; i < argc - 1; i++) if (strcmp(argv[i], name) == 0) return argv[i + 1];
    return def;
}
static int has_flag(int argc, char** argv, const char* name) {
    int i;
    for (i = 1; i < argc; i++) if (strcmp(argv[i], name) == 0) return 1;
    return 0;
}

/* ------------------------------------------------------------------------------------------ host */

static void describe(char* out, size_t cap, const rbl_pixel_frame* f, const uint8_t* rgba, const float* depth) {
    unsigned opaque = 0, finite = 0, i;
    float nearest = (float)HUGE_VAL, farthest = 0;
    for (i = 0; i < f->width * f->height; i++) {
        if (rgba[i * 4 + 3] > 0) opaque++;
        if (f->has_depth && depth[i] - depth[i] == 0.0f) {
            finite++;
            if (depth[i] < nearest) nearest = depth[i];
            if (depth[i] > farthest) farthest = depth[i];
        }
    }
    if (finite > 0)
        snprintf(out, cap, "frame %llu %ux%u camera_seq %u, %u drawn pixels, %u with finite depth (%.2f..%.2f m)", (unsigned long long)f->frame, f->width,
                 f->height, f->camera_seq, opaque, finite, (double)nearest, (double)farthest);
    else
        snprintf(out, cap, "frame %llu %ux%u camera_seq %u, %u drawn pixels, %u with finite depth", (unsigned long long)f->frame, f->width, f->height,
                 f->camera_seq, opaque, finite);
}

static int run_host(rbl_link* link, double seconds) {
    static rbl_event ev[256];
    int cameras = 0, pose_msgs = 0, poses = 0, frames_read = 0, frames_missed = 0;
    char last_frame[256] = "none";
    rbl_pixel_reader* reader = NULL;
    uint8_t* px = NULL;
    float* depth = NULL;
    size_t px_cap = 0, depth_cap = 0;
    float last_pose[3] = {0, 0, 0};
    int have_pose = 0;
    double next_report = 1, next_collision = 0, give_back_at = -1;
    for (;;) {
        double t = elapsed();
        int n, i;
        if (seconds > 0 && t > seconds) break;
        n = rbl_link_poll(link, ev, 256);
        for (i = 0; i < n; i++) {
            const rbl_event* e = &ev[i];
            switch (e->type) {
            case RBL_EV_CONNECTED:
                logf_("host", "connected: guest \"%s\", protocol v%u, session 0x%08x, guest udp %u", e->u.connected.game, e->u.connected.protocol,
                      e->u.connected.session, e->u.connected.udp_port);
                next_collision = t;
                break;
            case RBL_EV_FRAME_CHANNEL: {
                rbl_pixel_info info;
                if (reader) { rbl_pixel_reader_close(reader); reader = NULL; }
                if (e->u.frame_channel.path && rbl_pixel_reader_open(e->u.frame_channel.path, &reader) == RBL_OK) {
                    size_t need;
                    rbl_pixel_reader_info(reader, &info);
                    need = (size_t)info.width * info.height;
                    px = (uint8_t*)realloc(px, need * 4);
                    depth = (float*)realloc(depth, need * sizeof(float));
                    px_cap = need * 4;
                    depth_cap = need;
                    logf_("host", "guest pixel file %s (%ux%u, %u slots, depth plane: %s)", e->u.frame_channel.path, e->u.frame_channel.width,
                          e->u.frame_channel.height, e->u.frame_channel.slots, rbl_pixel_reader_has_depth(reader) ? "true" : "false");
                } else {
                    reader = NULL;
                    logf_("host", "cannot open the guest's pixel file");
                }
                break;
            }
            case RBL_EV_FRAME: {
                rbl_pixel_frame fi;
                if (reader && rbl_pixel_reader_read(reader, e->u.frame.frame.frame, e->u.frame.frame.slot, px, px_cap, depth, depth_cap, &fi) == RBL_OK) {
                    frames_read++;
                    describe(last_frame, sizeof last_frame, &fi, px, depth);
                } else {
                    frames_missed++;
                }
                break;
            }
            case RBL_EV_LOST:
                if (reader) { rbl_pixel_reader_close(reader); reader = NULL; }
                logf_("host", "LOST the guest: %s", e->u.lost.text);
                break;
            case RBL_EV_HANDSHAKE_FAILED: logf_("host", "handshake failed: %s", e->u.handshake_failed.text); break;
            case RBL_EV_SPAWN:
                logf_("host", "guest spawned entity %u = %s asset \"%s\"", e->u.spawn.entity, e->u.spawn.provider, e->u.spawn.asset);
                break;
            case RBL_EV_DESPAWN: logf_("host", "guest despawned entity %u", e->u.despawn.entity); break;
            case RBL_EV_POSES:
                pose_msgs++;
                poses += (int)e->u.poses.count;
                memcpy(last_pose, e->u.poses.poses[e->u.poses.count - 1].translation, sizeof last_pose);
                have_pose = 1;
                break;
            case RBL_EV_FOCUS:
                if (e->u.focus.requested) {
                    logf_("host", "guest asks for input focus; granting it to the %s", e->u.focus.focus == RBL_ROLE_HOST ? "host" : "guest");
                    rbl_link_set_focus(link, e->u.focus.focus);
                    give_back_at = t + 2;
                }
                break;
            default: break;
            }
        }
        if (rbl_link_is_connected(link)) {
            double a = 0.5 * t;
            float eye[3], target[3] = {0, 1, 0};
            rbl_camera cam;
            eye[0] = (float)(10 * cos(a)); eye[1] = 3; eye[2] = (float)(10 * sin(a));
            rbl_camera_look_at(&cam, eye, target, 1.0f, 0.1f, 1000.0f, 1280, 720);
            rbl_link_send_camera(link, &cam, NULL);
            cameras++;
            if (t >= next_collision) {
                size_t count = 0, k;
                rbl_triangle* tris;
                float cx = roundf(eye[0]), cz = roundf(eye[2]), center[3];
                next_collision = t + 1;
                if (g_big_collision > 0) {
                    count = (size_t)g_big_collision * RBL_MAX_TRIANGLES;
                    tris = (rbl_triangle*)malloc(sizeof(rbl_triangle) * count);
                    for (k = 0; k < count; k++) {
                        float x = (float)(k % 4096);
                        float v[9] = {x, 0, 0, x + 1, 0, 0, x, 0, 1};
                        memcpy(&tris[k], v, sizeof v);
                    }
                } else {
                    int ii, jj;
                    tris = (rbl_triangle*)malloc(sizeof(rbl_triangle) * 800);
                    for (ii = -10; ii < 10; ii++)
                        for (jj = -10; jj < 10; jj++) {
                            float x = cx + (float)ii, z = cz + (float)jj;
                            float a1[9] = {x, 0, z, x, 0, z + 1, x + 1, 0, z + 1};
                            float a2[9] = {x, 0, z, x + 1, 0, z + 1, x + 1, 0, z};
                            memcpy(&tris[count++], a1, sizeof a1);
                            memcpy(&tris[count++], a2, sizeof a2);
                        }
                }
                center[0] = cx; center[1] = 0; center[2] = cz;
                rbl_link_send_collision(link, center, 10, tris, count, NULL);
                free(tris);
            }
            if (give_back_at >= 0 && t >= give_back_at) {
                give_back_at = -1;
                logf_("host", "taking input focus back");
                rbl_link_set_focus(link, RBL_ROLE_HOST);
            }
        }
        if (t >= next_report) {
            next_report += 1;
            if (rbl_link_is_connected(link)) {
                rbl_stats s;
                char pose[96];
                rbl_link_stats(link, &s);
                if (have_pose) snprintf(pose, sizeof pose, "[%s%.4g, %.4g, %.4g]", "", (double)last_pose[0], (double)last_pose[1], (double)last_pose[2]);
                else snprintf(pose, sizeof pose, "none");
                logf_("host", "sent %d cameras; got %d poses in %d datagrams; last pose %s; malformed %llu stale %llu foreign %llu unknown_entity %llu", cameras,
                      poses, pose_msgs, pose, (unsigned long long)s.malformed, (unsigned long long)s.stale, (unsigned long long)s.foreign,
                      (unsigned long long)s.unknown_entity);
                if (frames_read + frames_missed > 0) logf_("host", "frames read %d, missed %d; last: %s", frames_read, frames_missed, last_frame);
            }
            cameras = pose_msgs = poses = frames_read = frames_missed = 0;
        }
        sleep_ms(16);
    }
    if (reader) rbl_pixel_reader_close(reader);
    free(px);
    free(depth);
    return 0;
}

/* ----------------------------------------------------------------------------------------- guest */

/* A coloured disc, 3 m away, on nothing: transparent with depth +inf outside it. */
static void render(uint8_t* rgba, float* depth, int n) {
    int cx = FRAME_W / 2 + (n % 40) - 20, cy = FRAME_H / 2, r = 60, x, y;
    for (y = 0; y < FRAME_H; y++) {
        for (x = 0; x < FRAME_W; x++) {
            int i = y * FRAME_W + x;
            int inside = (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
            rgba[i * 4] = (uint8_t)(inside ? 40 + x / 2 : 0);
            rgba[i * 4 + 1] = (uint8_t)(inside ? 120 : 0);
            rgba[i * 4 + 2] = (uint8_t)(inside ? 200 - y / 3 : 0);
            rgba[i * 4 + 3] = (uint8_t)(inside ? 255 : 0);
            depth[i] = inside ? 3.0f : (float)HUGE_VAL;
        }
    }
}

static int run_guest(rbl_link* link, double seconds) {
    static rbl_event ev[256];
    static uint8_t rgba[FRAME_W * FRAME_H * 4];
    static float depth[FRAME_W * FRAME_H];
    const uint32_t cube = 1;
    int cameras = 0, poses_sent = 0, frames_sent = 0;
    double next_report = 1, ask_focus_at = -1;
    float last_pos[3] = {0, 0, 0};
    int have_last = 0;
    uint64_t last_t = 0;
    rbl_pixel_writer* writer = NULL;
    for (;;) {
        double t = elapsed();
        int n, i;
        if (seconds > 0 && t > seconds) break;
        n = rbl_link_poll(link, ev, 256);
        for (i = 0; i < n; i++) {
            const rbl_event* e = &ev[i];
            switch (e->type) {
            case RBL_EV_CONNECTED:
                logf_("guest", "connected: host \"%s\", protocol v%u, session 0x%08x", e->u.connected.game, e->u.connected.protocol, e->u.connected.session);
                rbl_link_spawn(link, cube, "C Guest", "demo/cube", "Cube");
                ask_focus_at = t + 2;
                have_last = 0;
                if (g_frames) {
                    char tag[32];
                    if (writer) { rbl_pixel_writer_close(writer); writer = NULL; }
                    snprintf(tag, sizeof tag, "c-guest-%08x", e->u.connected.session);
                    if (rbl_pixel_writer_create_temp(tag, FRAME_W, FRAME_H, 3, 1, &writer) == RBL_OK) {
                        rbl_link_announce_writer(link, writer);
                        logf_("guest", "pixel file %s (%dx%d, depth: true)", rbl_pixel_writer_path(writer), FRAME_W, FRAME_H);
                    } else {
                        logf_("guest", "no pixel file");
                    }
                }
                break;
            case RBL_EV_LOST:
                if (writer) { rbl_pixel_writer_close(writer); writer = NULL; }
                logf_("guest", "LOST the host: %s", e->u.lost.text);
                break;
            case RBL_EV_HANDSHAKE_FAILED: logf_("guest", "handshake failed: %s", e->u.handshake_failed.text); break;
            case RBL_EV_CAMERA: {
                float eye[3], fwd[3], pos[3];
                uint64_t now = rbl_now_us();
                rbl_pose pose;
                cameras++;
                rbl_camera_eye(&e->u.camera.camera, eye);
                rbl_camera_forward(&e->u.camera.camera, fwd);
                pos[0] = eye[0] + 3 * fwd[0]; pos[1] = eye[1] + 3 * fwd[1]; pos[2] = eye[2] + 3 * fwd[2];
                rbl_pose_identity(&pose, cube, now);
                if (g_orbit) {
                    double a = 0.6 * t;
                    float heading[3], z[3] = {0, 0, 1};
                    pos[0] = eye[0] + (float)(8 * cos(a)); pos[1] = eye[1]; pos[2] = eye[2] + (float)(8 * sin(a));
                    /* The front (+Z) points along the direction of travel. */
                    heading[0] = (float)-sin(a); heading[1] = 0; heading[2] = (float)cos(a);
                    rbl_quat_from_to(pose.rotation, z, heading);
                }
                memcpy(pose.translation, pos, sizeof pos);
                if (have_last) {
                    float dt = (float)(now - last_t) * 1e-6f;
                    if (dt > 0) { int k; for (k = 0; k < 3; k++) pose.velocity[k] = (pos[k] - last_pos[k]) / dt; }
                }
                memcpy(last_pos, pos, sizeof pos);
                have_last = 1;
                last_t = now;
                rbl_link_send_poses(link, &pose, 1);
                poses_sent++;
                if (writer) {
                    uint64_t number;
                    rbl_frame fr;
                    render(rgba, depth, frames_sent);
                    if (rbl_pixel_writer_write(writer, rgba, sizeof rgba, depth, FRAME_W * FRAME_H, FRAME_W, FRAME_H, e->u.camera.header.seq, rbl_now_us(), &number) == RBL_OK) {
                        fr.frame = number; fr.slot = (uint32_t)(number % 3); fr.camera_seq = e->u.camera.header.seq; fr.width = FRAME_W; fr.height = FRAME_H;
                        rbl_link_send_frame(link, &fr);
                        frames_sent++;
                    }
                }
                break;
            }
            case RBL_EV_COLLISION:
                logf_("guest", "collision query %u: %u triangles within %g m of [%g, %g, %g]", e->u.collision.query, e->u.collision.triangle_count,
                      (double)e->u.collision.radius, (double)e->u.collision.center[0], (double)e->u.collision.center[1], (double)e->u.collision.center[2]);
                break;
            case RBL_EV_FOCUS:
                logf_("guest", "host says input focus is now with the %s", e->u.focus.focus == RBL_ROLE_HOST ? "host" : "guest");
                break;
            default: break;
            }
        }
        if (ask_focus_at >= 0 && t >= ask_focus_at) {
            ask_focus_at = -1;
            logf_("guest", "asking the host for input focus");
            rbl_link_set_focus(link, RBL_ROLE_GUEST);
        }
        if (t >= next_report) {
            next_report += 1;
            if (rbl_link_is_connected(link))
                logf_("guest", "got %d cameras; sent %d poses, %d frames; focus with the %s", cameras, poses_sent, frames_sent,
                      rbl_link_focus(link) == RBL_ROLE_HOST ? "host" : "guest");
            cameras = poses_sent = frames_sent = 0;
        }
        sleep_ms(4);
    }
    if (writer) rbl_pixel_writer_close(writer);
    return 0;
}

int main(int argc, char** argv) {
    int host, rc;
    rbl_config cfg;
    rbl_link* link = NULL;
    double seconds;
    int status;
    if (argc < 2 || (strcmp(argv[1], "host") != 0 && strcmp(argv[1], "guest") != 0)) {
        fprintf(stderr, "usage: demo host|guest [--port N] [--seconds S] [--frames] [--orbit] [--big-collision N]\n");
        return 2;
    }
    host = strcmp(argv[1], "host") == 0;
    rbl_config_init(&cfg, host ? "C Host" : "C Guest");
    cfg.control_port = (uint16_t)atoi(arg(argc, argv, "--port", "47800"));
    seconds = atof(arg(argc, argv, "--seconds", "0"));
    g_orbit = has_flag(argc, argv, "--orbit");
    g_frames = has_flag(argc, argv, "--frames");
    g_big_collision = (unsigned)atoi(arg(argc, argv, "--big-collision", "0"));
    g_start_us = rbl_now_us();
    status = host ? rbl_link_host(&cfg, &link) : rbl_link_guest(&cfg, &link);
    if (status != RBL_OK) {
        fprintf(stderr, "cannot create the link: %s (os error %d)\n", rbl_strerror(status), rbl_os_error());
        return 1;
    }
    if (host) logf_("host", "listening on 127.0.0.1:%u", rbl_link_control_port(link));
    else logf_("guest", "connecting to 127.0.0.1:%u", cfg.control_port);
    rc = host ? run_host(link, seconds) : run_guest(link, seconds);
    logf_(host ? "host" : "guest", "done");
    rbl_link_close(link);
    return rc;
}
