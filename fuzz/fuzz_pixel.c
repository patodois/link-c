/* libFuzzer harness: the pixel file reader, rbl_pixel_reader_open / rbl_pixel_reader_read_frame / rbl_pixel_reader_read
 * (CONTRACT.md section 9.6; the header checks are listed above rbl_pixel_reader_open in rewrite_link.h).
 *
 * The input is the whole file, byte for byte (the seeds are the pixel files of tests/vectors/link/pixelfiles). It is
 * written to /dev/shm and opened as a peer's frame_channel path would be.
 *
 * Oracle 1, open: an independent reference of the header rules (64 bit arithmetic in 128 bits, so nothing overflows) must
 * agree with the reader on accept and refuse, and on the geometry it reports.
 * Oracle 2, read: for every slot of an accepted file, the slot header is parsed here too; a Frame datagram that names
 * what the slot header says must read exactly the bytes of the file (colour and depth), if the lock is even and the
 * header's size is 1..the file's; and the near misses (another frame, frame 0, a size that differs from the datagram's, a
 * slot out of range, buffers too small, an odd lock) must all be skipped or refused, never delivered.
 * Oracle 3: after the file is truncated below its mapping, every read is a skip (RBL_MISS_GONE or an earlier miss), never a fault.
 */
#define REWRITE_LINK_IMPLEMENTATION
#include "../rewrite_link.h"
#include "fuzz_common.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { uint32_t slots, w, h, stride, depth_format, depth_stride; uint64_t slot_size, depth_offset; } Ref;

static int ref_open(const uint8_t* f, uint64_t size, Ref* r) {
    unsigned __int128 t;
    uint64_t colour_end;
    uint32_t format;
    if (size < 64) return 0;
    if (memcmp(f, "RBFRAME\0", 8) != 0) return 0;
    if (fz_u32(f + 8) != 1) return 0;
    r->slots = fz_u32(f + 12);
    r->w = fz_u32(f + 16);
    r->h = fz_u32(f + 20);
    r->stride = fz_u32(f + 24);
    format = fz_u32(f + 28);
    r->slot_size = fz_u64(f + 32);
    r->depth_format = fz_u32(f + 40);
    r->depth_stride = fz_u32(f + 44);
    r->depth_offset = fz_u64(f + 48);
    if (r->slots < 1 || r->slots > 16) return 0;
    if (r->w < 1 || r->w > 16384 || r->h < 1 || r->h > 16384) return 0;
    if ((unsigned __int128)r->w * r->h > ((unsigned __int128)1 << 26)) return 0;
    if (format != 1) return 0;
    if ((unsigned __int128)r->stride != (unsigned __int128)r->w * 4) return 0;
    colour_end = 64 + (uint64_t)r->stride * r->h; /* at most 64 + 2^28 */
    if (r->slot_size % 64 != 0 || r->slot_size < colour_end) return 0;
    t = 64 + (unsigned __int128)r->slots * r->slot_size;
    if (t > size) return 0;
    if (r->depth_format == 1) {
        if (r->depth_stride != r->w * 4u) return 0;
        if (r->depth_offset % 64 != 0 || r->depth_offset < colour_end) return 0;
        t = (unsigned __int128)r->depth_offset + (unsigned __int128)r->depth_stride * r->h;
        if (t > r->slot_size) return 0;
    } else {
        r->depth_format = 0; r->depth_stride = 0; r->depth_offset = 0;
    }
    return 1;
}

static void check_read(rbl_pixel_reader* rd, const Ref* ref, const uint8_t* file, uint32_t slot, int sized, int mut) {
    const uint8_t* s = file + 64 + (uint64_t)slot * ref->slot_size;
    uint64_t lock = fz_u64(s), frame = fz_u64(s + 8), time_us = fz_u64(s + 16);
    uint32_t cam = fz_u32(s + 24), w = fz_u32(s + 28), h = fz_u32(s + 32);
    int hdr_ok = frame != 0 && w >= 1 && w <= ref->w && h >= 1 && h <= ref->h;
    int expect_ok = hdr_ok && !(lock & 1);
    size_t cap = (size_t)ref->w * ref->h * 4;
    uint8_t* rgba = (uint8_t*)malloc(cap);
    float* depth = (float*)malloc(sizeof(float) * (size_t)ref->w * ref->h);
    rbl_frame f;
    rbl_pixel_frame info;
    uint64_t qframe = frame;
    uint32_t qslot = slot, qw = w, qh = h;
    int rc, want_depth = ref->depth_format == 1;
    size_t cap_given = cap, dcap_given = (size_t)ref->w * ref->h;
    /* mutations that must turn a delivery into a skip (or a refusal) */
    switch (mut) {
    case 0: break;
    case 1: qframe = frame + 1; expect_ok = 0; break;
    case 2: qframe = 0; expect_ok = 0; break;
    case 3: qslot = ref->slots + (slot & 3); expect_ok = 0; break;
    case 4: qw = w + 1; if (sized) expect_ok = 0; break;
    case 5: qh = h ? h - 1 : 1; if (sized) expect_ok = 0; break;
    case 6: qw = 0; if (sized) expect_ok = 0; break;
    case 7: qh = ref->h + 1; if (sized) expect_ok = 0; break;
    case 8: cap_given = (size_t)w * h * 4 - 1; break; /* too small: a refusal (or a skip), not a delivery */
    default: break;
    }
    f.frame = qframe; f.slot = qslot; f.camera_seq = cam; f.width = qw; f.height = qh;
    memset(rgba, 0xA5, cap);
    memset(&info, 0, sizeof info);
    if (mut == 8 && hdr_ok) {
        rc = sized ? rbl_pixel_reader_read_frame(rd, &f, rgba, cap_given, want_depth ? depth : NULL, dcap_given, &info)
                   : rbl_pixel_reader_read(rd, qframe, qslot, rgba, cap_given, want_depth ? depth : NULL, dcap_given, &info);
        FUZZ_CHECK(rc != RBL_OK, "a %zu byte buffer was accepted for a %ux%u frame", cap_given, w, h);
    } else {
        rc = sized ? rbl_pixel_reader_read_frame(rd, &f, rgba, cap_given, want_depth ? depth : NULL, dcap_given, &info)
                   : rbl_pixel_reader_read(rd, qframe, qslot, rgba, cap_given, want_depth ? depth : NULL, dcap_given, &info);
        FUZZ_CHECK((rc == RBL_OK) == (expect_ok != 0), "slot %u mut %d sized %d: read returned %d, reference says %s (lock %llu frame %llu %ux%u of %ux%u)",
                   slot, mut, sized, rc, expect_ok ? "deliver" : "skip", (unsigned long long)lock, (unsigned long long)frame, w, h, ref->w, ref->h);
        if (rc == RBL_OK) {
            uint32_t y, x;
            FUZZ_CHECK(info.frame == frame && info.time_us == time_us && info.camera_seq == cam && info.width == w && info.height == h,
                       "info differs from the slot header");
            FUZZ_CHECK(info.has_depth == want_depth, "has_depth");
            for (y = 0; y < h; y++)
                FUZZ_CHECK(memcmp(rgba + (size_t)y * w * 4, s + 64 + (uint64_t)y * ref->stride, (size_t)w * 4) == 0, "row %u of the colour plane differs", y);
            if (want_depth)
                for (y = 0; y < h; y++)
                    for (x = 0; x < w; x++) {
                        uint32_t bits;
                        memcpy(&bits, &depth[(size_t)y * w + x], 4);
                        FUZZ_CHECK(bits == fz_u32(s + ref->depth_offset + (uint64_t)y * ref->depth_stride + 4u * x), "depth (%u,%u) differs", x, y);
                    }
        } else {
            FUZZ_CHECK(rc > 0 || rc == RBL_ERR_TOO_BIG, "unexpected status %d", rc);
        }
    }
    free(rgba);
    free(depth);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static char path[128];
    int fd;
    Ref ref;
    int want;
    rbl_pixel_reader* rd = NULL;
    int rc;
    uint8_t* file;
    if (!path[0]) snprintf(path, sizeof path, "/dev/shm/rbl-fuzz-pf-%d", (int)getpid());
    unlink(path);
    fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) abort();
    if (size && write(fd, data, size) != (ssize_t)size) abort();
    file = fuzz_exact_copy(data, size);
    memset(&ref, 0, sizeof ref);
    want = ref_open(file, size, &ref);
    rc = rbl_pixel_reader_open(path, &rd);
    FUZZ_CHECK((rc == RBL_OK) == (want != 0), "open returned %d, reference says %s", rc, want ? "accept" : "refuse");
    if (rc != RBL_OK) {
        FUZZ_CHECK(rc == RBL_ERR_REFUSED, "a bad file gave %d, not RBL_ERR_REFUSED", rc);
        FUZZ_CHECK(rd == NULL, "a reader for a refused file");
    } else {
        rbl_pixel_info info;
        uint32_t slot, m;
        rbl_pixel_reader_info(rd, &info);
        FUZZ_CHECK(info.slots == ref.slots && info.width == ref.w && info.height == ref.h && info.stride == ref.stride &&
                       info.slot_size == ref.slot_size && info.depth_format == ref.depth_format && info.depth_stride == ref.depth_stride &&
                       info.depth_offset == ref.depth_offset,
                   "geometry differs from the header");
        FUZZ_CHECK(rbl_pixel_reader_has_depth(rd) == (ref.depth_format == 1), "has_depth");
        FUZZ_CHECK(!rbl_pixel_reader_is_gone(rd), "a fresh reader is gone");
        for (slot = 0; slot < ref.slots; slot++)
            for (m = 0; m < 9; m++) {
                check_read(rd, &ref, file, slot, 1, (int)m);
                check_read(rd, &ref, file, slot, 0, (int)m);
            }
        /* the peer truncates the file under the reader */
        if (size > 64) {
            uint8_t rgba[4];
            rbl_frame f;
            rbl_pixel_frame pinfo;
            int r2;
            FUZZ_CHECK(ftruncate(fd, (off_t)(size - 1)) == 0, "ftruncate");
            FUZZ_CHECK(rbl_pixel_reader_is_gone(rd), "a truncated file is not gone");
            f.frame = 1; f.slot = 0; f.camera_seq = 0; f.width = 1; f.height = 1;
            r2 = rbl_pixel_reader_read_frame(rd, &f, rgba, sizeof rgba, NULL, 0, &pinfo);
            FUZZ_CHECK(r2 == RBL_MISS_GONE, "read of a truncated file returned %d", r2);
            r2 = rbl_pixel_reader_read(rd, 1, 0, rgba, sizeof rgba, NULL, 0, &pinfo);
            FUZZ_CHECK(r2 == RBL_MISS_GONE, "read of a truncated file returned %d", r2);
        }
        rbl_pixel_reader_close(rd);
    }
    free(file);
    close(fd);
    unlink(path);
    return 0;
}
