/* libFuzzer harness: the UDP datagram decoder, rbl_wire_decode (CONTRACT.md sections 9.1 to 9.6, 10).
 *
 * Oracle: an independent reference of the length and range rules (written here from the datagram tables and
 * the vectors in tests/vectors/link/datagrams.json, sharing nothing with the library). The decoder must
 * accept exactly what the reference accepts; when it accepts, every decoded field must equal the bytes, and
 * encoding the decoded values must give the input back (the reserved words, which a reader ignores, aside).
 */
#define REWRITE_LINK_IMPLEMENTATION
#include "../rewrite_link.h"
#include "fuzz_common.h"

/* 1 = valid, 0 = malformed. Nothing about the sender's values is checked beyond what the tables say. */
static int ref_valid(const uint8_t* b, size_t n) {
    uint8_t kind;
    size_t payload;
    if (n < 24) return 0;
    if (fz_u32(b) != 0x4B4C4252u) return 0;
    if (fz_u16(b + 4) != 1) return 0;
    kind = b[6];
    payload = n - 24;
    switch (kind) {
    case 1: return payload == 0;
    case 2: return payload == 152 && fz_u32(b + 24 + 148) <= 2;
    case 3: {
        uint64_t count;
        if (payload < 8) return 0;
        count = fz_u32(b + 24);
        return count >= 1 && count <= 102 && payload == 8 + 80 * count;
    }
    case 4: {
        uint64_t tris;
        uint32_t index, count;
        if (payload < 32) return 0;
        index = fz_u16(b + 24 + 4);
        count = fz_u16(b + 24 + 6);
        tris = fz_u32(b + 24 + 24);
        if (count < 1 || count > 4096 || index >= count) return 0;
        return tris <= 226 && payload == 32 + 36 * tris;
    }
    case 5: return payload == 24 && fz_u64(b + 24) != 0;
    default: return 1; /* an unknown kind is ignored, at any length */
    }
}

static int same_bits(float a, uint32_t bits) { uint32_t x; memcpy(&x, &a, 4); return x == bits; }

static void check_floats(const float* f, const uint8_t* p, size_t n, const char* what) {
    size_t i;
    for (i = 0; i < n; i++) FUZZ_CHECK(same_bits(f[i], fz_u32(p + 4 * i)), "%s[%zu] differs from the bytes", what, i);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    uint8_t* buf = fuzz_exact_copy(data, size);
    rbl_decoded d;
    int rc = rbl_wire_decode(buf, size, &d);
    int want = ref_valid(buf, size);
    FUZZ_CHECK((rc == RBL_OK) == (want != 0), "decoder says %d, reference says %s (len %zu, kind %u)", rc, want ? "valid" : "malformed", size,
               size > 6 ? buf[6] : 0u);
    if (rc == RBL_OK) {
        const uint8_t* p = buf + 24;
        uint8_t kind = buf[6];
        uint8_t* out = (uint8_t*)malloc(size ? size : 1);
        uint8_t* want_bytes = fuzz_exact_copy(buf, size);
        int n = -1;
        FUZZ_CHECK(d.header.kind == kind && d.header.flags == buf[7], "header kind/flags");
        FUZZ_CHECK(d.header.version == 1 && d.header.magic == 0x4B4C4252u, "header magic/version");
        FUZZ_CHECK(d.header.session == fz_u32(buf + 8) && d.header.seq == fz_u32(buf + 12) && d.header.time_us == fz_u64(buf + 16),
                   "header session/seq/time");
        switch (kind) {
        case 1:
            n = rbl_wire_encode_heartbeat(&d.header, out, size);
            break;
        case 2:
            check_floats(d.u.camera.view, p, 16, "view");
            check_floats(d.u.camera.proj, p + 64, 16, "proj");
            FUZZ_CHECK(same_bits(d.u.camera.fov_y, fz_u32(p + 128)) && same_bits(d.u.camera.near_m, fz_u32(p + 132)) &&
                           same_bits(d.u.camera.far_m, fz_u32(p + 136)),
                       "camera scalars");
            FUZZ_CHECK(d.u.camera.viewport_width == fz_u32(p + 140) && d.u.camera.viewport_height == fz_u32(p + 144) &&
                           d.u.camera.depth_range == fz_u32(p + 148),
                       "camera integers");
            n = rbl_wire_encode_camera(&d.header, &d.u.camera, out, size);
            break;
        case 3: {
            uint32_t i, count = fz_u32(p);
            rbl_pose* poses = (rbl_pose*)malloc(sizeof(rbl_pose) * count);
            FUZZ_CHECK(d.u.poses.count == count, "pose count");
            for (i = 0; i < count; i++) {
                const uint8_t* r = p + 8 + 80u * i;
                rbl_wire_pose(&d, i, &poses[i]);
                FUZZ_CHECK(poses[i].entity == fz_u32(r) && poses[i].flags == fz_u32(r + 4) && poses[i].time_us == fz_u64(r + 8), "pose %u ints", i);
                check_floats(poses[i].translation, r + 16, 3, "translation");
                check_floats(poses[i].rotation, r + 28, 4, "rotation");
                check_floats(poses[i].scale, r + 44, 3, "scale");
                check_floats(poses[i].velocity, r + 56, 3, "velocity");
                check_floats(poses[i].angular_velocity, r + 68, 3, "angular_velocity");
            }
            memset(want_bytes + 24 + 4, 0, 4); /* the reserved word is ignored on read and written as 0 */
            n = rbl_wire_encode_poses(&d.header, poses, count, out, size);
            free(poses);
            break;
        }
        case 4: {
            uint32_t i, tris = fz_u32(p + 24);
            rbl_triangle* t = (rbl_triangle*)malloc(sizeof(rbl_triangle) * (tris ? tris : 1));
            FUZZ_CHECK(d.u.collision.query == fz_u32(p) && d.u.collision.chunk_index == fz_u16(p + 4) && d.u.collision.chunk_count == fz_u16(p + 6) &&
                           d.u.collision.triangle_count == tris,
                       "collision header ints");
            check_floats(d.u.collision.center, p + 8, 3, "center");
            FUZZ_CHECK(same_bits(d.u.collision.radius, fz_u32(p + 20)), "radius");
            for (i = 0; i < tris; i++) {
                rbl_wire_triangle(&d, i, &t[i]);
                check_floats(&t[i].v[0][0], p + 32 + 36u * i, 9, "triangle");
            }
            memset(want_bytes + 24 + 28, 0, 4);
            n = rbl_wire_encode_collision(&d.header, &d.u.collision, t, tris, out, size);
            free(t);
            break;
        }
        case 5:
            FUZZ_CHECK(d.u.frame.frame == fz_u64(p) && d.u.frame.slot == fz_u32(p + 8) && d.u.frame.camera_seq == fz_u32(p + 12) &&
                           d.u.frame.width == fz_u32(p + 16) && d.u.frame.height == fz_u32(p + 20),
                       "frame fields");
            n = rbl_wire_encode_frame(&d.header, &d.u.frame, out, size);
            break;
        default:
            FUZZ_CHECK(d.u.unknown.payload_len == size - 24, "unknown payload_len");
            break;
        }
        if (kind >= 1 && kind <= 5) {
            FUZZ_CHECK(n == (int)size, "re-encode returned %d for %zu bytes (kind %u)", n, size, kind);
            FUZZ_CHECK(memcmp(out, want_bytes, size) == 0, "re-encoded bytes differ (kind %u)", kind);
        }
        free(out);
        free(want_bytes);
    }
    free(buf);
    return 0;
}
