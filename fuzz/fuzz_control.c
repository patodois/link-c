/* libFuzzer harness: the JSON control line parser, rbl_control_parse, and rbl_control_format (CONTRACT.md section 8).
 *
 * The input is one line as the control channel would hand it to the parser (a link has already checked that it is
 * valid UTF-8 and has cut it at its first '\n'; both are done here too, so that the oracle only runs on input the
 * parser may be given; invalid UTF-8 and embedded newlines are still parsed, for memory safety only).
 *
 * Checks, on an accepted line:
 *   - every string points into the line buffer, is NUL terminated at its length, holds no NUL, is valid UTF-8;
 *   - the integer fields are within their wire types;
 *   - rbl_control_format() of the result is one line ending in '\n' that parses again, to the same message
 *     (a round trip), or is refused as too big.
 * The accept/reject decision itself is compared with an independent reference by check_corpus.py, which runs over the
 * corpus this harness grows (a second JSON reader inside the harness would share the author's misreadings of the
 * contract; Python's is somebody else's).
 */
#define REWRITE_LINK_IMPLEMENTATION
#include "../rewrite_link.h"
#include "fuzz_common.h"

static void in_buf(const char* base, size_t n, rbl_str s, const char* what) {
    if (s.len == 0) return;
    FUZZ_CHECK(s.ptr >= base && s.ptr + s.len <= base + n, "%s points outside the line", what);
}

static void check_str(const char* base, size_t n, rbl_str s, const char* what) {
    FUZZ_CHECK(s.ptr != NULL, "%s has no pointer", what);
    in_buf(base, n, s, what);
    FUZZ_CHECK(strlen(s.ptr) == s.len, "%s: length %zu but strlen %zu (a NUL, or no terminator)", what, s.len, strlen(s.ptr));
    FUZZ_CHECK(s.ptr[s.len] == 0, "%s not terminated", what);
}

/* A canonical text of a parsed message, to compare two parses. */
static void dump(const rbl_control* c, char* out, size_t cap) {
    size_t off = 0;
#define P(...) do { int w_ = snprintf(out + off, off < cap ? cap - off : 0, __VA_ARGS__); if (w_ > 0) off += (size_t)w_; } while (0)
#define S(s) P("[%zu:", (s).len); if ((s).len && off < cap) { size_t k_ = (s).len < cap - off ? (s).len : cap - off - 1; memcpy(out + off, (s).ptr, k_); off += k_; } P("]")
    switch (c->type) {
    case RBL_CTL_HELLO:
        P("hello %d ", (int)c->u.hello.role); S(c->u.hello.game);
        P(" %u %u %u %u %u s%d:%u t%d ", c->u.hello.protocol, c->u.hello.min_protocol, c->u.hello.udp_port, c->u.hello.heartbeat_ms,
          c->u.hello.timeout_ms, c->u.hello.has_session, c->u.hello.has_session ? c->u.hello.session : 0, c->u.hello.has_token);
        if (c->u.hello.has_token) { S(c->u.hello.token); }
        break;
    case RBL_CTL_SPAWN:
        P("spawn %u ", c->u.spawn.entity); S(c->u.spawn.provider); S(c->u.spawn.asset); P(" n%d ", c->u.spawn.has_name);
        if (c->u.spawn.has_name) { S(c->u.spawn.name); }
        break;
    case RBL_CTL_DESPAWN: P("despawn %u", c->u.despawn.entity); break;
    case RBL_CTL_INPUT:
        P("input %d r%d ", (int)c->u.input.focus, c->u.input.has_reason);
        if (c->u.input.has_reason) { S(c->u.input.reason); }
        break;
    case RBL_CTL_FRAME_CHANNEL: P("fc "); S(c->u.frame_channel.path); P(" %u %u %u", c->u.frame_channel.width, c->u.frame_channel.height, c->u.frame_channel.slots); break;
    case RBL_CTL_BYE: P("bye "); S(c->u.bye.reason); break;
    default: P("unknown "); S(c->unknown_type); break;
    }
#undef S
#undef P
    if (off >= cap) off = cap - 1;
    out[off] = 0;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    char* line = (char*)fuzz_exact_copy(data, size);
    size_t n = size;
    rbl_control c;
    const char* why = NULL;
    int rc;
    size_t nl;
    /* the framing: the line ends at its first '\n', which it keeps */
    for (nl = 0; nl < size; nl++) if (line[nl] == '\n') { n = nl + 1; break; }
    {
        int clean = fz_utf8_valid((const uint8_t*)line, n);
        if (clean) {
            /* a clean line has its '\n' only at the end */
            size_t i;
            for (i = 0; i + 1 < n; i++) if (line[i] == '\n') clean = 0;
        }
        rc = rbl_control_parse(line, n, &c, &why);
        FUZZ_CHECK(rc == RBL_OK || rc == RBL_ERR_MALFORMED, "unexpected status %d", rc);
        if (rc != RBL_OK) {
            FUZZ_CHECK(why != NULL && why[0], "no reason for a refused line");
            free(line);
            return 0;
        }
        if (!clean) { free(line); return 0; }
    }
    switch (c.type) {
    case RBL_CTL_HELLO:
        check_str(line, n, c.u.hello.game, "game");
        FUZZ_CHECK(c.u.hello.role == RBL_ROLE_HOST || c.u.hello.role == RBL_ROLE_GUEST, "role");
        FUZZ_CHECK(c.u.hello.protocol <= 0xFFFF && c.u.hello.min_protocol <= 0xFFFF && c.u.hello.udp_port <= 0xFFFF, "16 bit fields");
        if (c.u.hello.has_token) check_str(line, n, c.u.hello.token, "token");
        else FUZZ_CHECK(c.u.hello.token.len == 0, "absent token with a length");
        if (!c.u.hello.has_session) FUZZ_CHECK(c.u.hello.session == 0, "absent session with a value");
        break;
    case RBL_CTL_SPAWN:
        check_str(line, n, c.u.spawn.provider, "provider");
        check_str(line, n, c.u.spawn.asset, "asset");
        if (c.u.spawn.has_name) check_str(line, n, c.u.spawn.name, "name");
        break;
    case RBL_CTL_INPUT:
        FUZZ_CHECK(c.u.input.focus == RBL_ROLE_HOST || c.u.input.focus == RBL_ROLE_GUEST, "focus");
        if (c.u.input.has_reason) check_str(line, n, c.u.input.reason, "reason");
        break;
    case RBL_CTL_FRAME_CHANNEL: check_str(line, n, c.u.frame_channel.path, "path"); break;
    case RBL_CTL_BYE: check_str(line, n, c.u.bye.reason, "bye reason"); break;
    case RBL_CTL_UNKNOWN: check_str(line, n, c.unknown_type, "unknown type"); break;
    case RBL_CTL_DESPAWN: break;
    default: FUZZ_CHECK(0, "type %d", (int)c.type);
    }
    {   /* round trip */
        static char text[2 * RBL_MAX_LINE + 64], d1[4096], d2[4096];
        int len = rbl_control_format(&c, text, sizeof text);
        FUZZ_CHECK(len > 0 || len == RBL_ERR_TOO_BIG, "format returned %d", len);
        if (len > 0) {
            rbl_control c2;
            char* again;
            FUZZ_CHECK(text[len - 1] == '\n' && memchr(text, '\n', (size_t)len) == text + len - 1, "formatted text is not one line");
            FUZZ_CHECK((size_t)len <= RBL_MAX_LINE, "formatted line of %d bytes", len);
            again = (char*)fuzz_exact_copy((const uint8_t*)text, (size_t)len);
            FUZZ_CHECK(rbl_control_parse(again, (size_t)len, &c2, &why) == RBL_OK, "formatted line is refused: %s", why);
            dump(&c, d1, sizeof d1);
            dump(&c2, d2, sizeof d2);
            FUZZ_CHECK(strcmp(d1, d2) == 0, "round trip changed the message:\n  %s\n  %s", d1, d2);
            free(again);
        }
    }
    free(line);
    return 0;
}
