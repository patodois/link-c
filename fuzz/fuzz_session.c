/* libFuzzer harness: the control channel of a link, from the bytes of the TCP stream to the events (CONTRACT.md
 * sections 6 and 8): the line framing (rbli_ctl_next_line: the 64 KiB limit, UTF-8, the newline), the read buffer
 * (rbli_ctl_fill), rbl_control_parse, the handshake (rbli_on_hello: token, role, ports, heartbeat, protocol, session) and
 * the live commands (spawn, despawn, input, frame_channel, bye, unknown types).
 *
 * The harness includes the implementation so that it can drive rbli_poll_control() directly. The stream is run twice from the
 * same state: (A) all of it already in the read buffer, (B) through a socket pair, in chunks of the size the input asks for,
 * polling after every chunk, then the end of the stream. A line must mean the same however the bytes were cut, so the two
 * runs must produce the same events, in the same order, with the same contents (the text of an end-of-session reason included).
 *
 * Input: byte 0 is the set-up, byte 1 the chunk size, the rest is the stream the peer sends.
 *   byte 0 bits 0-1: 0 host without token, 1 host that requires the token "tok", 2 guest, 3 guest that sends the token "tok"
 *   byte 0 bit 2:    start in the live state instead of the handshake
 *   byte 1:          chunk = 1 + b * b / 3 bytes (1 .. 21675)
 * Further oracle: a host that requires a token never reports a connection unless the first line was a hello carrying exactly
 * that token; no event follows an end-of-session event; a connection is reported only once; the entity table is consistent and
 * never holds more than 1024 entities; strings of events can be read to their terminator (exact allocations, for AddressSanitizer).
 */
#define REWRITE_LINK_IMPLEMENTATION
#include "../rewrite_link.h"
#include "fuzz_common.h"

#include <fcntl.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <unistd.h>

#define EV_CAP 64
#define TOKEN "tok"
#define TRACE_CAP (1u << 20)

static rbl_link* g_link[4];

static rbl_link* make_link(int variant) {
    rbl_config cfg;
    rbl_link* l = NULL;
    int rc;
    rbl_config_init(&cfg, "fuzz");
    cfg.control_port = 0;
    cfg.token = (variant & 1) ? TOKEN : NULL;
    rc = (variant & 2) ? rbl_link_guest(&cfg, &l) : rbl_link_host(&cfg, &l);
    if (rc != RBL_OK) { fprintf(stderr, "cannot create a link: %s\n", rbl_strerror(rc)); abort(); }
    /* no real UDP traffic from here: the heartbeat a connection starts sends to the peer's announced port */
    if (l->udp != RBLI_BAD_SOCK) rbli_sock_close(l->udp);
    l->udp = RBLI_BAD_SOCK;
    if (l->listener != RBLI_BAD_SOCK) rbli_sock_close(l->listener);
    l->listener = RBLI_BAD_SOCK;
    return l;
}

typedef struct { char* p; size_t n; } Trace;

static void tr(Trace* t, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void tr(Trace* t, const char* fmt, ...) {
    va_list ap;
    int w;
    if (t->n + 512 > TRACE_CAP) return;
    va_start(ap, fmt);
    w = vsnprintf(t->p + t->n, 512, fmt, ap);
    va_end(ap);
    if (w > 0) t->n += (size_t)(w < 511 ? w : 511);
}

static void reset_state(rbl_link* l, int live, int variant) {
    rbli_ctl_free(&l->ctl);
    rbli_ctl_init(&l->ctl, RBLI_BAD_SOCK);
    rbli_arena_reset(l, 0);
    rbli_reset_session(l);
    l->spawn_ignored_logged = 1; /* keep stderr quiet */
    memset(&l->stats, 0, sizeof l->stats);
    l->pending_type = 0;
    memset(&l->peer, 0, sizeof l->peer);
    l->state = RBLI_HANDSHAKE;
    l->since = l->last_heard = l->last_beat = rbli_mono_ms();
    l->timeout_ms = 2000;
    l->heartbeat_ms = 250;
    l->seq = 0;
    if (live) {
        l->state = RBLI_LIVE;
        l->peer.role = (variant & 2) ? RBL_ROLE_HOST : RBL_ROLE_GUEST;
        l->peer.protocol = 1;
        l->peer.session = 0x1234;
        l->peer.udp_port = 40000;
        l->peer.heartbeat_ms = 250;
        l->peer.timeout_ms = 2000;
    }
}

/* Drains what poll left in ev[] into the trace, checking each event; returns 1 once the session has ended. */
static int take_events(rbl_link* l, rbl_event* ev, size_t n, Trace* t, int live, int variant, int first_hello_ok, int* connected, int* ended) {
    size_t i;
    for (i = 0; i < n; i++) {
        const rbl_event* e = &ev[i];
        FUZZ_CHECK(!*ended, "event (type %d) after the end of the session", (int)e->type);
        switch (e->type) {
        case RBL_EV_CONNECTED:
            FUZZ_CHECK(!live && !*connected, "a connection reported twice, or by a link that was live already");
            *connected = 1;
            FUZZ_CHECK(first_hello_ok, "connected, but the first line was not an acceptable hello");
            FUZZ_CHECK(e->u.connected.udp_port != 0, "peer udp_port 0");
            FUZZ_CHECK(e->u.connected.heartbeat_ms >= RBL_MIN_HEARTBEAT_MS && e->u.connected.heartbeat_ms <= RBL_MAX_HEARTBEAT_MS, "heartbeat range");
            FUZZ_CHECK(e->u.connected.session != 0 && e->u.connected.protocol == 1, "session/protocol");
            FUZZ_CHECK(e->u.connected.role == ((variant & 2) ? RBL_ROLE_HOST : RBL_ROLE_GUEST), "peer role");
            FUZZ_CHECK(strlen(e->u.connected.game) < sizeof e->u.connected.game, "game not terminated");
            tr(t, "connected %s %u %u %u %u\n", e->u.connected.game, e->u.connected.protocol, e->u.connected.udp_port, e->u.connected.heartbeat_ms,
               e->u.connected.timeout_ms);
            break;
        case RBL_EV_LOST:
        case RBL_EV_HANDSHAKE_FAILED:
            *ended = 1;
            FUZZ_CHECK(strlen(e->u.lost.text) < sizeof e->u.lost.text, "reason not terminated");
            FUZZ_CHECK((e->type == RBL_EV_LOST) == (live || *connected), "end event type %d, live=%d connected=%d", (int)e->type, live, *connected);
            tr(t, "end %d %s\n", (int)e->type, e->u.lost.text);
            break;
        case RBL_EV_SPAWN:
            FUZZ_CHECK(live || *connected, "spawn before the handshake finished");
            FUZZ_CHECK(e->u.spawn.provider && e->u.spawn.asset, "spawn without provider or asset");
            tr(t, "spawn %u %.80s %.80s %.80s\n", e->u.spawn.entity, e->u.spawn.provider, e->u.spawn.asset, e->u.spawn.name ? e->u.spawn.name : "(none)");
            tr(t, "  lens %zu %zu %zu\n", strlen(e->u.spawn.provider), strlen(e->u.spawn.asset), e->u.spawn.name ? strlen(e->u.spawn.name) : 0);
            break;
        case RBL_EV_DESPAWN:
            FUZZ_CHECK(live || *connected, "despawn before the handshake finished");
            tr(t, "despawn %u\n", e->u.despawn.entity);
            break;
        case RBL_EV_FOCUS:
            FUZZ_CHECK(live || *connected, "focus before the handshake finished");
            tr(t, "focus %d %d\n", (int)e->u.focus.focus, e->u.focus.requested);
            break;
        case RBL_EV_FRAME_CHANNEL:
            FUZZ_CHECK(live || *connected, "frame_channel before the handshake finished");
            tr(t, "frame_channel %.80s %zu %u %u %u\n", e->u.frame_channel.path ? e->u.frame_channel.path : "(null)",
               e->u.frame_channel.path ? strlen(e->u.frame_channel.path) : 0, e->u.frame_channel.width, e->u.frame_channel.height, e->u.frame_channel.slots);
            break;
        default:
            FUZZ_CHECK(0, "event type %d cannot come from the control channel", (int)e->type);
        }
    }
    (void)l;
    return *ended;
}

static void poll_once(rbl_link* l, rbl_event* ev, Trace* t, int live, int variant, int first_hello_ok, int* connected, int* ended, size_t* nev) {
    l->ev = ev;
    l->ev_cap = EV_CAP;
    l->ev_n = 0;
    rbli_poll_control(l, rbli_mono_ms());
    *nev = l->ev_n;
    l->ev = NULL;
    take_events(l, ev, *nev, t, live, variant, first_hello_ok, connected, ended);
    rbli_arena_reset(l, 0);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static Trace ta, tb;
    int variant, live, i, first_hello_ok = 0, first_hello_parsed = 0;
    rbl_link* l;
    rbl_event ev[EV_CAP];
    const uint8_t* stream;
    size_t slen, chunk;
    int connected, ended;
    size_t nev;
    if (size < 2) return 0;
    if (!ta.p) { ta.p = (char*)malloc(TRACE_CAP); tb.p = (char*)malloc(TRACE_CAP); }
    variant = data[0] & 3;
    live = (data[0] >> 2) & 1;
    chunk = 1 + ((size_t)data[1] * data[1]) / 3;
    stream = data + 2;
    slen = size - 2;
    if (!g_link[variant]) g_link[variant] = make_link(variant);
    l = g_link[variant];

    {   /* is the first line a hello this link has to accept at the token check? (parsed here by the library's own parser) */
        size_t k;
        for (k = 0; k < slen; k++) if (stream[k] == '\n') break;
        if (k < slen && k < RBL_MAX_LINE && fz_utf8_valid(stream, k + 1)) {
            char* cp = (char*)fuzz_exact_copy(stream, k + 1);
            rbl_control c;
            const char* why;
            if (rbl_control_parse(cp, k + 1, &c, &why) == RBL_OK && c.type == RBL_CTL_HELLO) {
                first_hello_ok = 1;
                first_hello_parsed = 1;
                if (!(variant & 2) && (variant & 1))
                    first_hello_ok = c.u.hello.has_token && c.u.hello.token.len == strlen(TOKEN) && memcmp(c.u.hello.token.ptr, TOKEN, strlen(TOKEN)) == 0;
            }
            free(cp);
        }
    }

    /* A: the whole stream is in the buffer */
    ta.n = 0; ta.p[0] = 0;
    reset_state(l, live, variant);
    l->ctl.rbuf = fuzz_exact_copy(stream, slen);
    l->ctl.rlen = l->ctl.rcap = slen;
    l->ctl.end = "connection closed";
    {   /* the control socket is a real one (the link writes its replies to it), but nothing is read from it */
        int sv[2], fl;
        char sink[4096];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) abort();
        for (fl = 0; fl < 2; fl++) fcntl(sv[fl], F_SETFL, fcntl(sv[fl], F_GETFL) | O_NONBLOCK);
        l->ctl.sock = sv[0];
        connected = ended = 0;
        for (i = 0; i < 100000; i++) {
            poll_once(l, ev, &ta, live, variant, first_hello_ok, &connected, &ended, &nev);
            while (recv(sv[1], sink, sizeof sink, 0) > 0) {}
            if (nev < EV_CAP - 2 || ended) break; /* the poll stopped for lack of room only if it filled the array */
        }
        close(sv[1]);
        if (l->ctl.sock != RBLI_BAD_SOCK) { close(sv[0]); l->ctl.sock = RBLI_BAD_SOCK; }
        for (i = 0; i < l->nclosing; i++) rbli_closing_free(&l->closing[i]);
        l->nclosing = 0;
    }
    /* contract 6.2: a host with a token checks it first (before role, version, ports, heartbeat) and answers with the fixed word */
    if (!live && !(variant & 2) && (variant & 1) && first_hello_parsed && !first_hello_ok)
        FUZZ_CHECK(strncmp(ta.p, "end 3 unauthorized\n", 19) == 0, "a hello without the token did not end the handshake with \"unauthorized\" first:\n%.300s", ta.p);
    FUZZ_CHECK(l->nent <= RBL_MAX_ENTITIES, "%u entities", l->nent);
    {   /* the entity table: every used slot is findable and counted */
        uint32_t used = 0;
        int k;
        for (k = 0; k < RBLI_ENT_CAP; k++) if (l->ent[k].used) { used++; FUZZ_CHECK(rbli_ent_find(l, l->ent[k].id) >= 0, "entity %u not findable", l->ent[k].id); }
        FUZZ_CHECK(used == l->nent, "nent %u but %u used", l->nent, used);
    }

    /* B: through a socket pair, cut in chunks */
    tb.n = 0; tb.p[0] = 0;
    reset_state(l, live, variant);
    {
        int sv[2], fl, guard = 0;
        size_t off = 0;
        char sink[4096];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) abort();
        for (fl = 0; fl < 2; fl++) fcntl(sv[fl], F_SETFL, fcntl(sv[fl], F_GETFL) | O_NONBLOCK);
        l->ctl.sock = sv[0];
        connected = ended = 0;
        while (off < slen && !ended && guard++ < 1000000) {
            size_t n = slen - off < chunk ? slen - off : chunk;
            ssize_t w = send(sv[1], stream + off, n, MSG_NOSIGNAL);
            if (w > 0) off += (size_t)w; /* else the socket buffer is full: the link reads below */
            poll_once(l, ev, &tb, live, variant, first_hello_ok, &connected, &ended, &nev);
            while (recv(sv[1], sink, sizeof sink, 0) > 0) {} /* what the link wrote to the peer */
        }
        shutdown(sv[1], SHUT_WR);
        for (i = 0; i < 8 && !ended; i++) poll_once(l, ev, &tb, live, variant, first_hello_ok, &connected, &ended, &nev);
        close(sv[1]);
        if (l->ctl.sock != RBLI_BAD_SOCK) { close(sv[0]); l->ctl.sock = RBLI_BAD_SOCK; }
        for (i = 0; i < l->nclosing; i++) rbli_closing_free(&l->closing[i]); /* the retired control socket */
        l->nclosing = 0;
    }
    if (!(ta.n == tb.n && memcmp(ta.p, tb.p, ta.n) == 0)) {
        size_t k = 0, from;
        while (k < ta.n && k < tb.n && ta.p[k] == tb.p[k]) k++;
        from = k > 300 ? k - 300 : 0;
        FUZZ_CHECK(0, "the events depend on how the stream was cut (chunk %zu); traces of %zu and %zu bytes differ at byte %zu:\n--- whole, from %zu ---\n%.600s\n--- chunked ---\n%.600s",
                   chunk, ta.n, tb.n, k, from, ta.p + from, tb.p + from);
    }
    reset_state(l, 0, variant);
    return 0;
}
