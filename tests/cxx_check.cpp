// The header, including its implementation, must compile as C++ and be usable from it (a plugin for
// SKSE, F4SE, ScriptHookV or Unreal is C++). This also links a second translation unit that only
// includes the declarations, to catch duplicate definitions.
#define REWRITE_LINK_IMPLEMENTATION
#include "../rewrite_link.h"

#include <cstdio>
#include <cstring>

int other_unit_uses_the_api(void);

int main() {
    rbl_config cfg;
    rbl_config_init(&cfg, "C++ host");
    cfg.control_port = 0;
    rbl_link* host = nullptr;
    if (rbl_link_host(&cfg, &host) != RBL_OK) { std::puts("cannot create a host"); return 1; }
    rbl_event events[16];
    int n = rbl_link_poll(host, events, 16);
    std::printf("polled %d events, control port %u\n", n, rbl_link_control_port(host));
    rbl_camera cam;
    const float eye[3] = {0, 1, 5}, target[3] = {0, 1, 0};
    rbl_camera_look_at(&cam, eye, target, 1.0f, 0.1f, 100.0f, 640, 360);
    uint8_t buf[RBL_MAX_DATAGRAM];
    rbl_header h;
    std::memset(&h, 0, sizeof h);
    h.version = RBL_VERSION; h.session = 0xABCD0001u; h.seq = 4; h.time_us = 1700000000000000ull;
    int len = rbl_wire_encode_heartbeat(&h, buf, sizeof buf);
    rbl_link_close(host);
    return (n >= 0 && len == 24 && other_unit_uses_the_api() == 0) ? 0 : 1;
}
