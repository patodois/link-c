/* Differential-testing helper (see check_corpus.py): reads cases from stdin, one per line, as lowercase hex, runs the
 * library on each and prints one line per case with the verdict, as JSON.
 *
 *   verdict control    the case is a control line; prints {"ok":false} or {"ok":true,"msg":{...}}
 *   verdict endpoint   the case is the content of an endpoint file (mode 0600); prints {"ok":false} or {"ok":true,"port":N,"token":"hex"}
 *
 * Strings are printed as hex of their bytes, so that no JSON writer of this file can be wrong about escapes.
 */
#define REWRITE_LINK_IMPLEMENTATION
#include "../rewrite_link.h"
#include "fuzz_common.h"

#include <fcntl.h>
#include <unistd.h>

static void hexs(rbl_str s) {
    size_t i;
    putchar('"');
    for (i = 0; i < s.len; i++) printf("%02x", (unsigned char)s.ptr[i]);
    putchar('"');
}

static void print_control(const rbl_control* c) {
    switch (c->type) {
    case RBL_CTL_HELLO:
        printf("{\"type\":\"hello\",\"role\":\"%s\",\"game\":", c->u.hello.role == RBL_ROLE_HOST ? "host" : "guest");
        hexs(c->u.hello.game);
        printf(",\"protocol\":%u,\"min_protocol\":%u,\"udp_port\":%u,\"heartbeat_ms\":%u,\"timeout_ms\":%u", c->u.hello.protocol, c->u.hello.min_protocol,
               c->u.hello.udp_port, c->u.hello.heartbeat_ms, c->u.hello.timeout_ms);
        if (c->u.hello.has_session) printf(",\"session\":%u", c->u.hello.session);
        if (c->u.hello.has_token) { printf(",\"token\":"); hexs(c->u.hello.token); }
        putchar('}');
        break;
    case RBL_CTL_SPAWN:
        printf("{\"type\":\"spawn\",\"entity\":%u,\"provider\":", c->u.spawn.entity);
        hexs(c->u.spawn.provider);
        printf(",\"asset\":");
        hexs(c->u.spawn.asset);
        if (c->u.spawn.has_name) { printf(",\"name\":"); hexs(c->u.spawn.name); }
        putchar('}');
        break;
    case RBL_CTL_DESPAWN: printf("{\"type\":\"despawn\",\"entity\":%u}", c->u.despawn.entity); break;
    case RBL_CTL_INPUT:
        printf("{\"type\":\"input\",\"focus\":\"%s\"", c->u.input.focus == RBL_ROLE_HOST ? "host" : "guest");
        if (c->u.input.has_reason) { printf(",\"reason\":"); hexs(c->u.input.reason); }
        putchar('}');
        break;
    case RBL_CTL_FRAME_CHANNEL:
        printf("{\"type\":\"frame_channel\",\"path\":");
        hexs(c->u.frame_channel.path);
        printf(",\"width\":%u,\"height\":%u,\"slots\":%u}", c->u.frame_channel.width, c->u.frame_channel.height, c->u.frame_channel.slots);
        break;
    case RBL_CTL_BYE: printf("{\"type\":\"bye\",\"reason\":"); hexs(c->u.bye.reason); putchar('}'); break;
    default: printf("{\"type\":\"unknown\",\"unknown_type\":"); hexs(c->unknown_type); putchar('}'); break;
    }
}

int main(int argc, char** argv) {
    static char hex[2 * 200000 + 16];
    int endpoint = argc > 1 && strcmp(argv[1], "endpoint") == 0;
    char path[128];
    snprintf(path, sizeof path, "/dev/shm/rbl-verdict-%d", (int)getpid());
    if (argc != 2 || (!endpoint && strcmp(argv[1], "control") != 0)) { fprintf(stderr, "usage: verdict control|endpoint < cases\n"); return 2; }
    while (fgets(hex, sizeof hex, stdin)) {
        size_t n = strlen(hex), i, len;
        uint8_t* b;
        while (n && (hex[n - 1] == '\n' || hex[n - 1] == '\r')) hex[--n] = 0;
        len = n / 2;
        b = (uint8_t*)malloc(len ? len : 1);
        for (i = 0; i < len; i++) { unsigned v; sscanf(hex + 2 * i, "%2x", &v); b[i] = (uint8_t)v; }
        if (!endpoint) {
            rbl_control c;
            const char* why = NULL;
            rbli_ctl ctl;
            char* line = NULL;
            size_t llen = 0;
            int framed;
            /* the link's own framing first: the line ends at its first '\n', at most 64 KiB, valid UTF-8 */
            rbli_ctl_init(&ctl, RBLI_BAD_SOCK);
            ctl.rbuf = b; ctl.rlen = ctl.rcap = len;
            framed = rbli_ctl_next_line(&ctl, &line, &llen);
            if (framed == 1 && rbl_control_parse(line, llen, &c, &why) == RBL_OK) { printf("{\"ok\":true,\"msg\":"); print_control(&c); printf("}\n"); }
            else printf("{\"ok\":false}\n");
        } else {
            uint16_t port = 0;
            char tok[257];
            int fd;
            unlink(path);
            fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (fd < 0 || (len && write(fd, b, len) != (ssize_t)len)) { perror("write"); return 2; }
            close(fd);
            if (rbl_endpoint_read(path, &port, tok) == RBL_OK) {
                rbl_str t; t.ptr = tok; t.len = strlen(tok);
                printf("{\"ok\":true,\"port\":%u,\"token\":", port); hexs(t); printf("}\n");
            } else printf("{\"ok\":false}\n");
        }
        free(b);
        fflush(stdout);
    }
    unlink(path);
    return 0;
}
