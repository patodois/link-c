/* libFuzzer harness: the datagram receive path of a live link, over a real loopback UDP socket (CONTRACT.md sections 2
 * and 9): the source address and session checks, rbl_wire_decode, the camera/pose/frame ordering rules, the
 * entity filter and the collision snapshot assembly (section 9.5), up to the events the application gets.
 *
 * Input: byte 0 is the set-up (bit 0: the receiver limit of one snapshot is 500 triangles instead of 925696), the rest is a
 * list of records, each:  flags (1 byte), length (2 bytes, little endian), that many bytes of datagram.
 *   flags bit 0: overwrite magic, version and session with the live session's, so that the datagram gets past the header checks
 *   flags bit 1: overwrite the entity of every pose record with 1..8, the entities this harness has spawned
 *   flags bit 2: send from a second socket (a source port that was not announced): the datagram must be ignored as foreign
 * Each datagram is sent, then rbli_poll_udp() is run until the library has counted it.
 *
 * Oracle: events are well formed; a datagram from the wrong port yields nothing; camera sequence numbers and frame numbers
 * delivered are strictly newer (RFC 1982 for the camera and the collision query) than the previous ones; a pose is delivered only
 * for a spawned entity and its time never goes back; a collision snapshot is delivered once per query, newer than the previous
 * one, with every triangle taken (bit for bit) from a datagram of that query and no more than 4096 * 226 of them (or the lowered
 * limit); everything delivered can be read (the arena is exact for AddressSanitizer).
 */
#define REWRITE_LINK_IMPLEMENTATION
#include "../rewrite_link.h"
#include "fuzz_common.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define EV_CAP 16
#define SESSION 0x1234u
#define MAXSEEN 512

static rbl_link* g_link;
static int g_tx[2] = { -1, -1 };
static uint16_t g_tx_port[2];

static int bind_sender(uint16_t* port) {
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) abort();
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr*)&a, sizeof a) != 0 || getsockname(s, (struct sockaddr*)&a, &al) != 0) abort();
    *port = ntohs(a.sin_port);
    return s;
}

typedef struct { uint32_t query; uint32_t n; uint8_t tri[226 * 36]; } Seen;

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    rbl_link* l;
    rbl_event ev[EV_CAP];
    size_t pos = 1;
    int i;
    int have_cam = 0, have_col = 0;
    uint32_t last_cam = 0, last_col = 0;
    uint64_t last_frame = 0;
    uint64_t ent_time[9];
    static Seen* seen;
    size_t nseen = 0, nrec = 0;
    int seen_overflow = 0;
    struct sockaddr_in to;
    if (size < 1) return 0;
    if (!seen) seen = (Seen*)malloc(sizeof(Seen) * MAXSEEN);
    if (!g_link) {
        rbl_config cfg;
        rbl_config_init(&cfg, "fuzz");
        cfg.control_port = 0;
        if (rbl_link_host(&cfg, &g_link) != RBL_OK) abort();
        if (g_link->listener != RBLI_BAD_SOCK) rbli_sock_close(g_link->listener);
        g_link->listener = RBLI_BAD_SOCK;
        g_tx[0] = bind_sender(&g_tx_port[0]);
        g_tx[1] = bind_sender(&g_tx_port[1]);
    }
    l = g_link;
    l->cfg.max_collision_triangles = (data[0] & 1) ? 500 : RBL_MAX_COLLISION_TRIANGLES;

    rbli_ctl_free(&l->ctl);
    rbli_ctl_init(&l->ctl, RBLI_BAD_SOCK);
    rbli_arena_reset(l, 0);
    rbli_reset_session(l);
    memset(&l->stats, 0, sizeof l->stats);
    memset(&l->peer, 0, sizeof l->peer);
    l->state = RBLI_LIVE;
    l->peer.role = RBL_ROLE_GUEST;
    l->peer.protocol = 1;
    l->peer.session = SESSION;
    l->peer.udp_port = g_tx_port[0];
    l->peer.heartbeat_ms = 250;
    l->timeout_ms = 1000000;
    l->last_heard = l->last_beat = rbli_mono_ms();
    for (i = 1; i <= 8; i++) rbli_ent_insert(l, (uint32_t)i);
    memset(ent_time, 0, sizeof ent_time);

    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    to.sin_port = htons(l->udp_port);

    while (pos + 3 <= size && nrec++ < 256) {
        unsigned flags = data[pos];
        size_t len = (size_t)data[pos + 1] | ((size_t)data[pos + 2] << 8), tries;
        uint8_t* dg;
        uint64_t before, after;
        int from_other = (flags >> 2) & 1;
        pos += 3;
        if (len > size - pos) len = size - pos;
        if (len > 2 * RBL_MAX_DATAGRAM) len = 2 * RBL_MAX_DATAGRAM;
        dg = fuzz_exact_copy(data + pos, len);
        pos += len;
        if ((flags & 1) && len >= 24) {
            dg[0] = 'R'; dg[1] = 'B'; dg[2] = 'L'; dg[3] = 'K'; dg[4] = 1; dg[5] = 0;
            dg[8] = SESSION & 0xFF; dg[9] = (SESSION >> 8) & 0xFF; dg[10] = 0; dg[11] = 0;
        }
        if ((flags & 2) && len >= 32 + 24 && dg[6] == RBL_POSE) {
            size_t off = 24 + 8, k = 0;
            for (; off + 80 <= len; off += 80, k++) {
                uint32_t id = 1 + (uint32_t)(k & 7);
                dg[off] = (uint8_t)id; dg[off + 1] = dg[off + 2] = dg[off + 3] = 0;
            }
        }
        if (len >= 24 && dg[6] == RBL_COLLISION && len >= 24 + 32) { /* remember what a snapshot could be made of */
            uint32_t n = fz_u32(dg + 24 + 24);
            if (n <= 226 && len == 24 + 32 + 36u * n && nseen >= MAXSEEN) seen_overflow = 1;
            if (n <= 226 && len == 24 + 32 + 36u * n && nseen < MAXSEEN) {
                seen[nseen].query = fz_u32(dg + 24);
                seen[nseen].n = n;
                memcpy(seen[nseen].tri, dg + 24 + 32, 36u * n);
                nseen++;
            }
        }
        before = l->stats.foreign + l->stats.malformed + l->stats.datagrams_received;
        {   /* a datagram of any length is accepted by sendto up to 65507 */
            if (sendto(g_tx[from_other], dg, len, 0, (struct sockaddr*)&to, sizeof to) < 0) { free(dg); continue; }
        }
        free(dg);
        for (tries = 0; tries < 200; tries++) {
            l->ev = ev; l->ev_cap = EV_CAP; l->ev_n = 0;
            rbli_poll_udp(l, rbli_mono_ms());
            /* check what was delivered */
            for (i = 0; i < (int)l->ev_n; i++) {
                const rbl_event* e = &ev[i];
                FUZZ_CHECK(!from_other, "an event (type %d) from a datagram of an unannounced port", (int)e->type);
                switch (e->type) {
                case RBL_EV_CAMERA:
                    FUZZ_CHECK(e->u.camera.header.kind == RBL_CAMERA && e->u.camera.camera.depth_range <= 2, "camera event");
                    if (have_cam) FUZZ_CHECK(rbl_seq_newer(e->u.camera.header.seq, last_cam), "camera seq %u after %u", e->u.camera.header.seq, last_cam);
                    have_cam = 1; last_cam = e->u.camera.header.seq;
                    break;
                case RBL_EV_POSES: {
                    uint32_t k;
                    FUZZ_CHECK(e->u.poses.count >= 1 && e->u.poses.count <= RBL_MAX_POSES && e->u.poses.poses, "pose event count %u", e->u.poses.count);
                    for (k = 0; k < e->u.poses.count; k++) {
                        const rbl_pose* p = &e->u.poses.poses[k];
                        FUZZ_CHECK(p->entity >= 1 && p->entity <= 8, "pose for entity %u, never spawned", p->entity);
                        FUZZ_CHECK(p->time_us >= ent_time[p->entity], "entity %u time went back: %llu after %llu", p->entity,
                                   (unsigned long long)p->time_us, (unsigned long long)ent_time[p->entity]);
                        ent_time[p->entity] = p->time_us;
                    }
                    break;
                }
                case RBL_EV_FRAME:
                    FUZZ_CHECK(e->u.frame.frame.frame != 0 && e->u.frame.frame.frame > last_frame, "frame %llu after %llu",
                               (unsigned long long)e->u.frame.frame.frame, (unsigned long long)last_frame);
                    last_frame = e->u.frame.frame.frame;
                    break;
                case RBL_EV_COLLISION: {
                    uint32_t k;
                    size_t m;
                    uint64_t limit = l->cfg.max_collision_triangles;
                    if (have_col) FUZZ_CHECK(rbl_seq_newer(e->u.collision.query, last_col), "collision query %u after %u", e->u.collision.query, last_col);
                    have_col = 1; last_col = e->u.collision.query;
                    FUZZ_CHECK(e->u.collision.triangle_count <= limit, "%u triangles over the limit %llu", e->u.collision.triangle_count, (unsigned long long)limit);
                    FUZZ_CHECK(e->u.collision.triangle_count == 0 || e->u.collision.triangles, "triangles missing");
                    for (k = 0; k < e->u.collision.triangle_count; k++) {
                        int found = 0;
                        for (m = 0; m < nseen && !found; m++) {
                            uint32_t t;
                            if (seen[m].query != e->u.collision.query) continue;
                            for (t = 0; t < seen[m].n && !found; t++)
                                found = memcmp(seen[m].tri + 36u * t, &e->u.collision.triangles[k], 36) == 0;
                        }
                        FUZZ_CHECK(found || seen_overflow, "triangle %u of query %u was in no datagram of that query", k, e->u.collision.query);
                    }
                    break;
                }
                default:
                    FUZZ_CHECK(0, "event type %d from the datagram path", (int)e->type);
                }
            }
            l->ev = NULL;
            rbli_arena_reset(l, 1);
            after = l->stats.foreign + l->stats.malformed + l->stats.datagrams_received;
            if (after > before) break;
            usleep(50);
        }
    }
    rbli_asm_clear(l);
    l->ev = NULL;
    return 0;
}
