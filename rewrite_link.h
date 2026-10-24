/* rewrite_link.h - the rewrite-bridge passthrough link protocol v1 (revision 4), C99, single header.
 *
 * https://github.com/patodois/link-c  -  MIT OR Apache-2.0
 *
 * One side of a link between two cooperating processes on one machine: a HOST (the game the
 * player is in) or a GUEST (a second game or a rewrite that contributes entities, poses or
 * rendered frames). The protocol is defined by docs/CONTRACT.md of the rewrite-bridge repository;
 * this library is one implementation of it (Rust, Java and C# are others) and implements the
 * wire format, the control channel, the session lifecycle, the watchdog and the pixel file.
 *
 * USE
 *   In exactly one .c (or .cpp) file:
 *       #define REWRITE_LINK_IMPLEMENTATION
 *       #include "rewrite_link.h"
 *   everywhere else just #include "rewrite_link.h". Define REWRITE_LINK_STATIC before both to make
 *   every function static (one translation unit only). On Windows, link ws2_32 (and bcrypt for
 *   MinGW; MSVC picks both up by itself), and keep <windows.h> out of the file that holds the
 *   implementation unless you include it with WIN32_LEAN_AND_MEAN.
 *
 *   The header compiles as C99, C11 and C++ (extern "C"). Nothing here needs a C runtime beyond
 *   <stdlib.h>, <string.h>, <stdio.h> (snprintf) and <math.h>.
 *
 *   Link: Linux/macOS need nothing extra (-lm on Linux for the camera helper).
 *
 * MODEL
 *   - Everything binds and connects to 127.0.0.1 only.
 *   - rbl_link_poll() never blocks. It reads the sockets, runs the handshake, sends heartbeats,
 *     paces collision chunks (at most 32 per call), checks the watchdog and writes what happened
 *     into an array of rbl_event that YOU provide. Call it every frame or tick; a side that does
 *     not poll looks dead to its peer, as intended.
 *   - If more events are waiting than your array holds, poll leaves the rest for the next call
 *     (in the socket buffers), so a larger array only means fewer calls. Use 64 or more.
 *   - Pointers inside an event (poses, triangles, spawn and frame_channel strings) point into
 *     memory owned by the link. They are valid until the next call of rbl_link_poll() or
 *     rbl_link_close() on that link. Copy what you keep.
 *   - Not thread safe: use one link from one thread, or lock around every call yourself.
 *
 * MEMORY (what the library allocates; everything else lives in your buffers)
 *   - rbl_link_host/guest: the link object (about 64 KiB), freed by rbl_link_close().
 *   - While a session runs: the control channel's read and write buffers (bounded by the
 *     contract: 64 KiB lines, 1 MiB unsent), the events' strings, poses and assembled collision
 *     triangles for the length of one poll, the copy of a collision snapshot you queue with
 *     rbl_link_send_collision() until its chunks are sent, and the chunks of a snapshot being
 *     received (at most cfg.max_collision_triangles).
 *   - rbl_pixel_reader_open / rbl_pixel_writer_create: one object each.
 *   - Define REWRITE_LINK_MALLOC, REWRITE_LINK_REALLOC and REWRITE_LINK_FREE (all three) before
 *     the implementation to use your own allocator.
 *   - There is no global or static mutable state.
 *
 * LIMITS YOU CAN TRIP
 *   - Strings in control messages are UTF-8 and at most 65536 bytes per line. A string with an
 *     invalid UTF-8 sequence that you pass in is sent with U+FFFD in its place.
 *   - A string that arrives with an embedded NUL ("\u0000") is cut at the NUL in the C string;
 *     a frame_channel path with one is delivered as NULL (and refused by the reader).
 *
 * See the README for a host and a guest example, and for how to embed this in a game plugin.
 */
#ifndef REWRITE_LINK_H
#define REWRITE_LINK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef REWRITE_LINK_STATIC
#define RBLDEF static
#else
#define RBLDEF extern
#endif

#define REWRITE_LINK_VERSION "0.1.0"

/* ------------------------------------------------------------------------------------------
 * Constants and layouts (CONTRACT.md section 10)
 * ---------------------------------------------------------------------------------------- */

#define RBL_MAGIC          0x4B4C4252u /* "RBLK" read as little-endian u32 */
#define RBL_VERSION        1
#define RBL_MIN_VERSION    1
#define RBL_DEFAULT_CONTROL_PORT 47800
#define RBL_MAX_DATAGRAM   8192
#define RBL_MAX_POSES      102
#define RBL_MAX_TRIANGLES  226
#define RBL_HEADER_LEN     24
#define RBL_CAMERA_LEN     152
#define RBL_POSE_LEN       80
#define RBL_TRIANGLE_LEN   36
#define RBL_FRAME_LEN      24
#define RBL_MAX_LINE       65536
#define RBL_MAX_PENDING_CONTROL (1 << 20)
#define RBL_LINGER_MS      200
#define RBL_MAX_ENTITIES   1024
#define RBL_MIN_HEARTBEAT_MS 10
#define RBL_MAX_HEARTBEAT_MS 10000
#define RBL_COLLISION_PER_POLL 32
#define RBL_RECV_BUFFER_BYTES (4 << 20)
#define RBL_MAX_FRAME_DIM  16384u
#define RBL_MAX_FRAME_PIXELS 67108864u /* 2^26 */
#define RBL_MAX_SLOTS      16u
#define RBL_MIN_EVENTS     8 /* the smallest events array rbl_link_poll accepts */
#define RBL_POSE_TELEPORT  1u

enum rbl_kind { RBL_HEARTBEAT = 1, RBL_CAMERA = 2, RBL_POSE = 3, RBL_COLLISION = 4, RBL_FRAME = 5 };
enum rbl_depth_range { RBL_DEPTH_NEG_ONE_TO_ONE = 0, RBL_DEPTH_ZERO_TO_ONE = 1, RBL_DEPTH_REVERSED = 2 };
enum rbl_depth_format { RBL_DEPTH_NONE = 0, RBL_DEPTH_LINEAR_F32 = 1 }; /* pixel file depth_format */

typedef struct rbl_header {      /* 24 bytes, offset 0 of every datagram */
    uint32_t magic;              /*  0 */
    uint16_t version;            /*  4 */
    uint8_t  kind;               /*  6 */
    uint8_t  flags;              /*  7 */
    uint32_t session;            /*  8 */
    uint32_t seq;                /* 12 */
    uint64_t time_us;            /* 16 */
} rbl_header;

typedef struct rbl_camera {      /* 152 bytes, after the header */
    float    view[16];           /*   0  world to view, column-major */
    float    proj[16];           /*  64  view to clip, column-major, +y up in NDC */
    float    fov_y;              /* 128 */
    float    near_m;             /* 132 */
    float    far_m;              /* 136  +inf for an infinite projection */
    uint32_t viewport_width;     /* 140 */
    uint32_t viewport_height;    /* 144 */
    uint32_t depth_range;        /* 148  enum rbl_depth_range */
} rbl_camera;

typedef struct rbl_pose_header { /* 8 bytes, after the header, then `count` rbl_pose */
    uint32_t count;              /* 0 */
    uint32_t reserved;           /* 4 */
} rbl_pose_header;

typedef struct rbl_pose {        /* 80 bytes */
    uint32_t entity;             /*  0 */
    uint32_t flags;              /*  4 */
    uint64_t time_us;            /*  8 */
    float    translation[3];     /* 16 */
    float    rotation[4];        /* 28, x y z w */
    float    scale[3];           /* 44 */
    float    velocity[3];        /* 56 */
    float    angular_velocity[3];/* 68 */
} rbl_pose;

typedef struct rbl_collision {   /* 32 bytes, after the header, then `triangle_count` rbl_triangle */
    uint32_t query;              /*  0 */
    uint16_t chunk_index;        /*  4 */
    uint16_t chunk_count;        /*  6 */
    float    center[3];          /*  8 */
    float    radius;             /* 20 */
    uint32_t triangle_count;     /* 24 */
    uint32_t reserved;           /* 28 */
} rbl_collision;

typedef struct rbl_triangle {    /* 36 bytes */
    float    v[3][3];            /* a, b, c; each x y z */
} rbl_triangle;

typedef struct rbl_frame {       /* 24 bytes, after the header */
    uint64_t frame;              /*  0 */
    uint32_t slot;               /*  8 */
    uint32_t camera_seq;         /* 12 */
    uint32_t width;              /* 16 */
    uint32_t height;             /* 20 */
} rbl_frame;

typedef struct rbl_frame_file {  /* 64 bytes at offset 0 of the pixel file */
    char     magic[8];           /*  0, "RBFRAME\0" */
    uint32_t version;            /*  8 */
    uint32_t slots;              /* 12 */
    uint32_t width;              /* 16 */
    uint32_t height;             /* 20 */
    uint32_t stride;             /* 24 */
    uint32_t format;             /* 28 */
    uint64_t slot_size;          /* 32 */
    uint32_t depth_format;       /* 40, rbl_depth_format; 0 in revision 1 and 2 files */
    uint32_t depth_stride;       /* 44, width * 4, or 0 */
    uint64_t depth_offset;       /* 48, from the slot start, multiple of 64, or 0 */
    uint8_t  reserved[8];        /* 56 */
} rbl_frame_file;

typedef struct rbl_frame_slot {  /* 64 bytes at 64 + i * slot_size; pixels follow, then
                                    the f32 depth plane at depth_offset, if the file has one */
    uint64_t lock;               /*  0, atomic */
    uint64_t frame;              /*  8 */
    uint64_t time_us;            /* 16 */
    uint32_t camera_seq;         /* 24 */
    uint32_t width;              /* 28 */
    uint32_t height;             /* 32 */
    uint8_t  reserved[28];       /* 36 */
} rbl_frame_slot;

/* The layouts above must have no padding. These fail to compile (array of size -1) if they do. */
#define RBL_STATIC_ASSERT(name, cond) typedef char rbl_static_assert_##name[(cond) ? 1 : -1]
RBL_STATIC_ASSERT(header, sizeof(rbl_header) == 24);
RBL_STATIC_ASSERT(camera, sizeof(rbl_camera) == 152);
RBL_STATIC_ASSERT(pose_header, sizeof(rbl_pose_header) == 8);
RBL_STATIC_ASSERT(pose, sizeof(rbl_pose) == 80);
RBL_STATIC_ASSERT(collision, sizeof(rbl_collision) == 32);
RBL_STATIC_ASSERT(triangle, sizeof(rbl_triangle) == 36);
RBL_STATIC_ASSERT(frame, sizeof(rbl_frame) == 24);
RBL_STATIC_ASSERT(frame_file, sizeof(rbl_frame_file) == 64);
RBL_STATIC_ASSERT(frame_slot, sizeof(rbl_frame_slot) == 64);

/* ------------------------------------------------------------------------------------------
 * Status codes
 * ---------------------------------------------------------------------------------------- */

enum rbl_status {
    RBL_OK = 0,
    RBL_ERR_ARGS = -1,          /* a bad argument */
    RBL_ERR_NOT_CONNECTED = -2, /* no session */
    RBL_ERR_MALFORMED = -3,     /* data that does not follow the contract */
    RBL_ERR_TOO_BIG = -4,       /* does not fit (a datagram, a line, a buffer) */
    RBL_ERR_IO = -5,            /* an operating system call failed */
    RBL_ERR_NOMEM = -6,
    RBL_ERR_EXISTS = -7,        /* creating a pixel file: the path exists */
    RBL_ERR_REFUSED = -8        /* a pixel file the reader refuses (see rbl_strerror for why) */
};

/* Results of rbl_pixel_reader_read() other than RBL_OK and the negative errors: the frame was
 * skipped, wait for the next Frame datagram. */
enum rbl_miss {
    RBL_MISS_BUSY = 1,        /* the slot is being written (odd lock) */
    RBL_MISS_OTHER_FRAME = 2, /* the slot holds another frame */
    RBL_MISS_OVERWRITTEN = 3, /* the writer reached the slot while it was copied */
    RBL_MISS_GONE = 4         /* the file is shorter than the mapping: drop the reader */
};

/* A short description of a status or miss code (a static string). */
RBLDEF const char* rbl_strerror(int code);

/* ------------------------------------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------------------------------- */

/* Microseconds since the Unix epoch, wall clock (CONTRACT.md section 5). */
RBLDEF uint64_t rbl_now_us(void);
/* RFC 1982 serial comparison on 32-bit counters: is a newer than b? */
RBLDEF int rbl_seq_newer(uint32_t a, uint32_t b);
/* The session version two sides agree on, or -1 when their ranges do not overlap. */
RBLDEF int rbl_negotiate(int host_min, int host_max, int guest_min, int guest_max);

/* A right-handed look-at camera with an OpenGL-style perspective projection (depth range 0).
 * far_m may be INFINITY. */
RBLDEF void rbl_camera_look_at(rbl_camera* out, const float eye[3], const float target[3], float fov_y,
                               float near_m, float far_m, uint32_t width, uint32_t height);
/* The camera's world position, assuming view is a rigid transform; and the direction it looks in. */
RBLDEF void rbl_camera_eye(const rbl_camera* c, float out[3]);
RBLDEF void rbl_camera_forward(const rbl_camera* c, float out[3]);
/* An identity pose at the origin, at rest. */
RBLDEF void rbl_pose_identity(rbl_pose* out, uint32_t entity, uint64_t time_us);
/* Quaternions x, y, z, w; +Z is an entity's front. */
RBLDEF void rbl_quat_axis_angle(float out[4], const float axis[3], float angle);
RBLDEF void rbl_quat_from_to(float out[4], const float a[3], const float b[3]);
RBLDEF void rbl_quat_rotate(float out[3], const float q[4], const float v[3]);

/* ------------------------------------------------------------------------------------------
 * Wire format: datagrams (CONTRACT.md section 9). Exposed for tests and for tools; a link
 * uses these itself.
 * ---------------------------------------------------------------------------------------- */

/* The encoders write a whole datagram into out and return its length, or a negative rbl_status
 * (RBL_ERR_TOO_BIG if cap is too small, RBL_ERR_MALFORMED for a value the contract forbids: a
 * pose count outside 1..102, more than 226 triangles, chunk_index >= chunk_count, a depth_range
 * above 2, frame number 0). h->magic and h->kind are ignored; they are always written right. */
RBLDEF int rbl_wire_encode_heartbeat(const rbl_header* h, uint8_t* out, size_t cap);
RBLDEF int rbl_wire_encode_camera(const rbl_header* h, const rbl_camera* c, uint8_t* out, size_t cap);
RBLDEF int rbl_wire_encode_poses(const rbl_header* h, const rbl_pose* poses, size_t count, uint8_t* out, size_t cap);
/* c->triangle_count is ignored; `count` triangles are written. */
RBLDEF int rbl_wire_encode_collision(const rbl_header* h, const rbl_collision* c, const rbl_triangle* tris,
                                     size_t count, uint8_t* out, size_t cap);
RBLDEF int rbl_wire_encode_frame(const rbl_header* h, const rbl_frame* f, uint8_t* out, size_t cap);
/* Number of datagrams a snapshot of n triangles needs (at least 1). */
RBLDEF size_t rbl_wire_chunk_count(size_t n_triangles);

typedef struct rbl_decoded {
    rbl_header header;           /* kind says which member of u is set */
    union {
        rbl_camera camera;                         /* RBL_CAMERA */
        struct { uint32_t count; } poses;          /* RBL_POSE: read records with rbl_wire_pose() */
        rbl_collision collision;                   /* RBL_COLLISION: read triangles with rbl_wire_triangle() */
        rbl_frame frame;                           /* RBL_FRAME */
        struct { size_t payload_len; } unknown;    /* any other kind: ignored, but proof of life */
    } u;
    const uint8_t* records;      /* the buffer you decoded, at the first pose or triangle */
} rbl_decoded;

/* Checks length, magic, version and every count; returns RBL_OK or RBL_ERR_MALFORMED. Never reads
 * outside buf[0..len), whatever the bytes are. */
RBLDEF int rbl_wire_decode(const uint8_t* buf, size_t len, rbl_decoded* out);
RBLDEF void rbl_wire_pose(const rbl_decoded* d, uint32_t i, rbl_pose* out);
RBLDEF void rbl_wire_triangle(const rbl_decoded* d, uint32_t i, rbl_triangle* out);

/* ------------------------------------------------------------------------------------------
 * Control messages (CONTRACT.md section 8): one JSON object per line. Exposed for tests.
 * ---------------------------------------------------------------------------------------- */

typedef enum rbl_role { RBL_ROLE_HOST = 0, RBL_ROLE_GUEST = 1 } rbl_role;

typedef enum rbl_control_type {
    RBL_CTL_UNKNOWN = 0, RBL_CTL_HELLO, RBL_CTL_SPAWN, RBL_CTL_DESPAWN, RBL_CTL_INPUT,
    RBL_CTL_FRAME_CHANNEL, RBL_CTL_BYE
} rbl_control_type;

/* A string inside a parsed line: ptr[0..len), also NUL terminated, may contain NULs. */
typedef struct rbl_str { const char* ptr; size_t len; } rbl_str;

typedef struct rbl_control {
    rbl_control_type type;
    rbl_str unknown_type;                          /* RBL_CTL_UNKNOWN */
    union {
        struct { rbl_role role; rbl_str game; uint32_t protocol, min_protocol, udp_port, heartbeat_ms, timeout_ms;
                 int has_session; uint32_t session; } hello;
        struct { uint32_t entity; rbl_str provider, asset, name; int has_name; } spawn;
        struct { uint32_t entity; } despawn;
        struct { rbl_role focus; rbl_str reason; int has_reason; } input;
        struct { rbl_str path; uint32_t width, height, slots; } frame_channel;
        struct { rbl_str reason; } bye;
    } u;
} rbl_control;

/* Parses one line (with or without its line ending). The line is edited in place (JSON escapes
 * are decoded where they stand) and the strings of *out point into it. It must be valid UTF-8,
 * the caller checks that (a link does). Unknown types give RBL_CTL_UNKNOWN; a line that is not a
 * JSON object with a string "type", or a known type with a missing or out-of-range field, gives
 * RBL_ERR_MALFORMED and *why (if not NULL) points to a static reason. */
RBLDEF int rbl_control_parse(char* line, size_t len, rbl_control* out, const char** why);
/* Writes the control message as one line (with its '\n') and returns its length, RBL_ERR_TOO_BIG
 * if cap is too small. Strings with invalid UTF-8 are repaired with U+FFFD. */
RBLDEF int rbl_control_format(const rbl_control* c, char* out, size_t cap);

/* ------------------------------------------------------------------------------------------
 * The link
 * ---------------------------------------------------------------------------------------- */

typedef struct rbl_link rbl_link;

typedef struct rbl_config {
    const char* game;            /* this side's game, sent in hello (copied, at most 255 bytes used) */
    uint16_t control_port;       /* host: TCP port to listen on (0 = any free port); guest: the host's port */
    uint16_t udp_port;           /* this side's UDP port (0 = any free port), announced in hello */
    uint32_t heartbeat_ms;       /* heartbeat period; clamped to 10..10000 */
    uint32_t timeout_ms;         /* own watchdog timeout; the effective one is max(this, 4 * peer heartbeat) */
    uint32_t reconnect_ms;       /* guest: how often to retry connecting while no host answers */
    uint32_t max_collision_triangles; /* receiver limit on one assembled snapshot (a snapshot above it is dropped) */
} rbl_config;

/* Fills in the defaults: control port 47800, UDP port 0, heartbeat 250 ms, timeout 2000 ms,
 * reconnect 500 ms, 2097152 triangles. */
RBLDEF void rbl_config_init(rbl_config* cfg, const char* game);

typedef struct rbl_peer_info {
    char game[128];              /* the peer's game, cut to fit */
    rbl_role role;
    uint32_t protocol;           /* the session version */
    uint32_t session;
    uint16_t udp_port;
    uint32_t heartbeat_ms;
    uint32_t timeout_ms;         /* the watchdog timeout in force for this peer */
} rbl_peer_info;

typedef enum rbl_event_type {
    RBL_EV_NONE = 0,
    RBL_EV_CONNECTED,         /* u.connected */
    RBL_EV_LOST,              /* u.lost: the session ended */
    RBL_EV_HANDSHAKE_FAILED,  /* u.handshake_failed: it never started */
    RBL_EV_CAMERA,            /* u.camera */
    RBL_EV_POSES,             /* u.poses: only for entities the peer spawned, stale ones left out */
    RBL_EV_COLLISION,         /* u.collision: a complete snapshot */
    RBL_EV_FRAME,             /* u.frame: a frame is ready in the peer's pixel file */
    RBL_EV_SPAWN,             /* u.spawn */
    RBL_EV_DESPAWN,           /* u.despawn */
    RBL_EV_FOCUS,             /* u.focus */
    RBL_EV_FRAME_CHANNEL      /* u.frame_channel: the peer announced a pixel file */
} rbl_event_type;

typedef struct rbl_reason { char text[192]; } rbl_reason;

typedef struct rbl_event {
    rbl_event_type type;
    union {
        rbl_peer_info connected;
        rbl_reason lost;
        rbl_reason handshake_failed;
        struct { rbl_header header; rbl_camera camera; } camera;
        struct { rbl_header header; const rbl_pose* poses; uint32_t count; } poses;
        struct { uint32_t query; float center[3]; float radius; const rbl_triangle* triangles;
                 uint32_t triangle_count; } collision;
        struct { rbl_header header; rbl_frame frame; } frame;
        struct { uint32_t entity; const char* provider; const char* asset; const char* name; /* NULL if none */ } spawn;
        struct { uint32_t entity; } despawn;
        /* host: the guest REQUESTS the focus (requested = 1); guest: the host SET it (requested = 0) */
        struct { rbl_role focus; int requested; } focus;
        struct { const char* path; /* NULL if it held a NUL */ uint32_t width, height, slots; } frame_channel;
    } u;
} rbl_event;

typedef struct rbl_stats {
    uint64_t datagrams_sent, datagrams_received;
    uint64_t malformed;      /* datagrams that failed validation */
    uint64_t stale;          /* late or out-of-order state that was dropped */
    uint64_t foreign;        /* datagrams from another address, session or version */
    uint64_t send_dropped;   /* datagrams the OS would not take */
    uint64_t lines_sent, lines_received;
    uint64_t unknown_entity; /* pose records for an entity the peer never spawned */
    uint64_t spawn_ignored;  /* spawns beyond the 1024-entity table */
    uint64_t dropped;        /* events or data dropped for lack of memory */
} rbl_stats;

/* Creates a host: listens on 127.0.0.1:control_port, binds the UDP socket. *out is NULL on
 * failure; rbl_strerror(result) says why, and rbl_os_error() has the OS error code. */
RBLDEF int rbl_link_host(const rbl_config* cfg, rbl_link** out);
/* Creates a guest: binds the UDP socket and connects to 127.0.0.1:control_port, retrying every
 * reconnect_ms until a host answers. */
RBLDEF int rbl_link_guest(const rbl_config* cfg, rbl_link** out);
/* The OS error code (errno or WSAGetLastError()) at the moment a failed create returned; call it
 * right after the failure, before any other call that could change errno. */
RBLDEF int rbl_os_error(void);

/* Does one round of work and writes the events into events[0..cap). Returns how many, or a
 * negative status (RBL_ERR_ARGS: bad link or cap < RBL_MIN_EVENTS). */
RBLDEF int rbl_link_poll(rbl_link* link, rbl_event* events, size_t cap);

/* Send. All return RBL_OK or a negative status; RBL_ERR_NOT_CONNECTED while there is no session. */
/* Sends the camera; *seq_out (if not NULL) gets the datagram's seq, which a guest quotes in
 * rbl_frame.camera_seq. */
RBLDEF int rbl_link_send_camera(rbl_link* link, const rbl_camera* camera, uint32_t* seq_out);
/* Sends poses, 102 per datagram. The entity must be spawned first (rbl_link_spawn). */
RBLDEF int rbl_link_send_poses(rbl_link* link, const rbl_pose* poses, size_t count);
/* Queues a collision snapshot (copying the triangles) and returns its query number in
 * *query_out. The chunks go out 32 per rbl_link_poll(), so keep polling; a snapshot queued while
 * an older one still has chunks waiting replaces them. */
RBLDEF int rbl_link_send_collision(rbl_link* link, const float center[3], float radius,
                                   const rbl_triangle* triangles, size_t count, uint32_t* query_out);
/* Announces a frame you wrote into the pixel file (after rbl_pixel_writer_write). */
RBLDEF int rbl_link_send_frame(rbl_link* link, const rbl_frame* frame);
/* name may be NULL. A second spawn of the same entity replaces the first. */
RBLDEF int rbl_link_spawn(rbl_link* link, uint32_t entity, const char* provider, const char* asset, const char* name);
RBLDEF int rbl_link_despawn(rbl_link* link, uint32_t entity);
/* Host: gives the focus to `focus`. Guest: asks the host for it. */
RBLDEF int rbl_link_set_focus(rbl_link* link, rbl_role focus);
/* Sends the frame_channel line for a pixel file (an absolute path). */
RBLDEF int rbl_link_announce_frames(rbl_link* link, const char* path, uint32_t width, uint32_t height, uint32_t slots);

/* Ends the session with a bye (half-close, drain, close: CONTRACT.md 6.1), without blocking; no
 * LOST event is reported for it. A guest reconnects after reconnect_ms. */
RBLDEF void rbl_link_disconnect(rbl_link* link, const char* reason);
/* Says bye, drains and closes the connection (this may block for up to about 250 ms), closes the
 * sockets and frees the link and everything it owns. link may be NULL. */
RBLDEF void rbl_link_close(rbl_link* link);

RBLDEF rbl_role rbl_link_role(const rbl_link* link);
RBLDEF uint16_t rbl_link_control_port(const rbl_link* link); /* host: the port actually bound */
RBLDEF uint16_t rbl_link_udp_port(const rbl_link* link);
RBLDEF int rbl_link_is_connected(const rbl_link* link);
/* Copies the peer's info; returns 0 if there is no session. */
RBLDEF int rbl_link_peer(const rbl_link* link, rbl_peer_info* out);
RBLDEF rbl_role rbl_link_focus(const rbl_link* link);
RBLDEF void rbl_link_stats(const rbl_link* link, rbl_stats* out);

/* ------------------------------------------------------------------------------------------
 * The pixel file (CONTRACT.md section 9.6)
 * ---------------------------------------------------------------------------------------- */

typedef struct rbl_pixel_reader rbl_pixel_reader;
typedef struct rbl_pixel_writer rbl_pixel_writer;

typedef struct rbl_pixel_info {
    uint32_t slots, width, height, stride;
    uint64_t slot_size;
    uint32_t depth_format;       /* 0 = colour only (also what an unknown format reads as), 1 = linear f32 */
    uint32_t depth_stride;
    uint64_t depth_offset;
} rbl_pixel_info;

typedef struct rbl_pixel_frame {
    uint64_t frame, time_us;
    uint32_t camera_seq, width, height;
    int has_depth;               /* depth was requested and the file has a plane */
} rbl_pixel_frame;

/* Diagnostics: why reads returned a miss. */
typedef struct rbl_pixel_stats { uint64_t busy, other_frame, overwritten, gone; } rbl_pixel_stats;

/* Maps a pixel file read-only after the checks of revision 4: the path must be absolute and name a
 * regular file (a symlink, FIFO, device, directory or socket is refused; on Unix the file is
 * checked before and after it is opened, with O_NONBLOCK, and both must be the same device and
 * inode), width and height 1..16384, at most 2^26 pixels, stride == width * 4, slots 1..16, every
 * size computed with overflow checks and the file at least as long as its slots. An unknown
 * depth_format is not an error: the file reads as colour only. RBL_ERR_REFUSED for a file that
 * fails; RBL_ERR_IO if it cannot be opened. */
RBLDEF int rbl_pixel_reader_open(const char* path, rbl_pixel_reader** out);
RBLDEF void rbl_pixel_reader_info(const rbl_pixel_reader* r, rbl_pixel_info* out);
RBLDEF int rbl_pixel_reader_has_depth(const rbl_pixel_reader* r);
/* 1 when the file is now shorter than the mapping (the peer truncated it) or cannot be examined.
 * rbl_pixel_reader_read() checks it before every read. */
RBLDEF int rbl_pixel_reader_is_gone(const rbl_pixel_reader* r);
/* Copies frame `frame` out of `slot` with the seqlock. rgba must hold slot width * height * 4
 * bytes (the file's width * height * 4 is always enough); depth may be NULL (not wanted) or hold
 * width * height floats. info (may be NULL) gets the slot's header. Returns RBL_OK, an
 * RBL_MISS_* (skip the frame), RBL_ERR_TOO_BIG if a buffer is too small, or RBL_ERR_ARGS. */
RBLDEF int rbl_pixel_reader_read(rbl_pixel_reader* r, uint64_t frame, uint32_t slot, uint8_t* rgba,
                                 size_t rgba_cap, float* depth, size_t depth_cap, rbl_pixel_frame* info);
RBLDEF void rbl_pixel_reader_stats(const rbl_pixel_reader* r, rbl_pixel_stats* out);
RBLDEF void rbl_pixel_reader_close(rbl_pixel_reader* r);

/* Creates a pixel file for writing: exclusively (an existing path gives RBL_ERR_EXISTS), sized
 * once and never resized, mode 0600, mapped read-write. width and height are 1..16384 with at
 * most 2^26 pixels, slots 1..16 (3 is recommended). A relative path is made absolute against the
 * current directory. */
RBLDEF int rbl_pixel_writer_create(const char* path, uint32_t width, uint32_t height, uint32_t slots,
                                   int with_depth, rbl_pixel_writer** out);
/* The same in the temp directory, named rewrite-bridge-<pid>-<tag>-<random>.frames. */
RBLDEF int rbl_pixel_writer_create_temp(const char* tag, uint32_t width, uint32_t height, uint32_t slots,
                                        int with_depth, rbl_pixel_writer** out);
/* Writes the next frame (the first is 1) into slot frame % slots with the seqlock. rgba is w * h * 4
 * bytes, tightly packed; depth is w * h floats or NULL for "nothing here" everywhere (+inf), and
 * must be NULL in a file without a depth plane. w and h may be below the file's. Nothing is written
 * and no frame number used if an argument is wrong (RBL_ERR_ARGS). *frame_out gets the number; then
 * send rbl_frame{frame, frame % slots, camera_seq, w, h} with rbl_link_send_frame(). */
RBLDEF int rbl_pixel_writer_write(rbl_pixel_writer* w, const uint8_t* rgba, size_t rgba_len,
                                  const float* depth, size_t depth_len, uint32_t width, uint32_t height,
                                  uint32_t camera_seq, uint64_t time_us, uint64_t* frame_out);
/* The absolute path, to announce with rbl_link_announce_frames(). */
RBLDEF const char* rbl_pixel_writer_path(const rbl_pixel_writer* w);
RBLDEF void rbl_pixel_writer_info(const rbl_pixel_writer* w, rbl_pixel_info* out);
/* Announces the writer's file: rbl_link_announce_frames(path, width, height, slots). */
RBLDEF int rbl_link_announce_writer(rbl_link* link, const rbl_pixel_writer* w);
/* Unmaps, closes and deletes the file. w may be NULL. */
RBLDEF void rbl_pixel_writer_close(rbl_pixel_writer* w);

/* The temp directory, with a trailing separator, written to out; returns the length or <0. */
RBLDEF int rbl_temp_dir(char* out, size_t cap);
/* Whether path is absolute under the Unix rules (starts with '/') or the Windows rules (a drive
 * letter, a colon and a separator, or two leading separators). The reader applies the rules of
 * the platform it runs on; both are exposed so they can be tested everywhere. */
RBLDEF int rbl_path_is_absolute(const char* path, int windows_rules);

#ifdef __cplusplus
}
#endif

#endif /* REWRITE_LINK_H */

/* ==========================================================================================
 * IMPLEMENTATION
 * ====================================================================================== */
#ifdef REWRITE_LINK_IMPLEMENTATION
#ifndef REWRITE_LINK_IMPLEMENTATION_DONE
#define REWRITE_LINK_IMPLEMENTATION_DONE

#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#ifdef __APPLE__
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#endif
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define RBLI_WINDOWS 1
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#if defined(_MSC_VER)
#include <intrin.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")
#endif
#else
#define RBLI_WINDOWS 0
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#endif

#if !defined(REWRITE_LINK_MALLOC) || !defined(REWRITE_LINK_REALLOC) || !defined(REWRITE_LINK_FREE)
#undef REWRITE_LINK_MALLOC
#undef REWRITE_LINK_REALLOC
#undef REWRITE_LINK_FREE
#define REWRITE_LINK_MALLOC(n) malloc(n)
#define REWRITE_LINK_REALLOC(p, n) realloc((p), (n))
#define REWRITE_LINK_FREE(p) free(p)
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define RBLI_MSVC 1
#else
#define RBLI_MSVC 0
#endif

/* Little-endian only for the pixel plane copies below; the datagram codec is byte-order clean. */
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define RBLI_BIG_ENDIAN 1
#else
#define RBLI_BIG_ENDIAN 0
#endif

#define RBLI_U32_MAX 0xFFFFFFFFu

/* ------------------------------------------------------------------------------------------
 * Little-endian codec
 * ---------------------------------------------------------------------------------------- */

static void rbli_put16(uint8_t* b, uint32_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static void rbli_put32(uint8_t* b, uint32_t v) {
    b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
}
static void rbli_put64(uint8_t* b, uint64_t v) { rbli_put32(b, (uint32_t)v); rbli_put32(b + 4, (uint32_t)(v >> 32)); }
static void rbli_putf(uint8_t* b, float f) { uint32_t u; memcpy(&u, &f, 4); rbli_put32(b, u); }
static uint32_t rbli_get16(const uint8_t* b) { return (uint32_t)b[0] | ((uint32_t)b[1] << 8); }
static uint32_t rbli_get32(const uint8_t* b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static uint64_t rbli_get64(const uint8_t* b) { return (uint64_t)rbli_get32(b) | ((uint64_t)rbli_get32(b + 4) << 32); }
static float rbli_getf(const uint8_t* b) { uint32_t u = rbli_get32(b); float f; memcpy(&f, &u, 4); return f; }

static void rbli_putfs(uint8_t* b, const float* f, size_t n) { size_t i; for (i = 0; i < n; i++) rbli_putf(b + 4 * i, f[i]); }
static void rbli_getfs(const uint8_t* b, float* f, size_t n) { size_t i; for (i = 0; i < n; i++) f[i] = rbli_getf(b + 4 * i); }

/* ------------------------------------------------------------------------------------------
 * Errors, small helpers
 * ---------------------------------------------------------------------------------------- */

RBLDEF const char* rbl_strerror(int code) {
    switch (code) {
    case RBL_OK: return "ok";
    case RBL_ERR_ARGS: return "bad argument";
    case RBL_ERR_NOT_CONNECTED: return "not connected";
    case RBL_ERR_MALFORMED: return "malformed data, or a value the contract forbids";
    case RBL_ERR_TOO_BIG: return "does not fit";
    case RBL_ERR_IO: return "operating system error";
    case RBL_ERR_NOMEM: return "out of memory";
    case RBL_ERR_EXISTS: return "the path already exists";
    case RBL_ERR_REFUSED: return "the pixel file is refused";
    case RBL_MISS_BUSY: return "the slot is being written";
    case RBL_MISS_OTHER_FRAME: return "the slot holds another frame";
    case RBL_MISS_OVERWRITTEN: return "the slot was overwritten while copying";
    case RBL_MISS_GONE: return "the pixel file is gone or shorter than its mapping";
    default: return "unknown status";
    }
}

RBLDEF int rbl_seq_newer(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }

RBLDEF int rbl_negotiate(int host_min, int host_max, int guest_min, int guest_max) {
    int v = host_max < guest_max ? host_max : guest_max;
    int lo = host_min > guest_min ? host_min : guest_min;
    return v >= lo ? v : -1;
}

/* Overflow-checked unsigned 64-bit arithmetic; each returns 1 on overflow. */
static int rbli_add_u64(uint64_t a, uint64_t b, uint64_t* out) { *out = a + b; return *out < a; }
static int rbli_mul_u64(uint64_t a, uint64_t b, uint64_t* out) {
    if (a != 0 && b > UINT64_MAX / a) { *out = 0; return 1; }
    *out = a * b;
    return 0;
}

/* ------------------------------------------------------------------------------------------
 * Platform layer: clocks, sleep, random, sockets
 * ---------------------------------------------------------------------------------------- */

#if RBLI_WINDOWS

typedef SOCKET rbli_sock;
#define RBLI_BAD_SOCK INVALID_SOCKET

RBLDEF uint64_t rbl_now_us(void) {
    FILETIME ft;
    ULARGE_INTEGER u;
#if defined(_WIN32_WINNT) && _WIN32_WINNT >= 0x0602
    GetSystemTimePreciseAsFileTime(&ft);
#else
    GetSystemTimeAsFileTime(&ft);
#endif
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    /* 100 ns ticks since 1601-01-01 -> microseconds since 1970-01-01 */
    return (u.QuadPart - 116444736000000000ULL) / 10ULL;
}

static uint64_t rbli_mono_ms(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / f.QuadPart) * 1000u + (uint64_t)(c.QuadPart % f.QuadPart) * 1000u / (uint64_t)f.QuadPart;
}
static void rbli_sleep_ms(unsigned ms) { Sleep(ms); }
static int rbli_os_errno(void) { return WSAGetLastError(); }
static int rbli_would_block(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEINTR; }
static int rbli_in_progress(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
static int rbli_net_up(void) { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0 ? 0 : -1; }
static void rbli_net_down(void) { WSACleanup(); }
static void rbli_sock_close(rbli_sock s) { closesocket(s); }
static int rbli_sock_nonblock(rbli_sock s) { u_long one = 1; return ioctlsocket(s, FIONBIO, &one) == 0 ? 0 : -1; }
static void rbli_random(uint8_t* out, size_t n) {
    if (BCryptGenRandom(NULL, out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        uint64_t t = rbl_now_us() ^ ((uint64_t)GetCurrentProcessId() << 32);
        size_t i;
        for (i = 0; i < n; i++) { t = t * 6364136223846793005ULL + 1442695040888963407ULL; out[i] = (uint8_t)(t >> 33); }
    }
}
static unsigned rbli_pid(void) { return (unsigned)GetCurrentProcessId(); }

#else /* POSIX */

typedef int rbli_sock;
#define RBLI_BAD_SOCK (-1)

RBLDEF uint64_t rbl_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
static uint64_t rbli_mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
static void rbli_sleep_ms(unsigned ms) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
}
static int rbli_os_errno(void) { return errno; }
static int rbli_would_block(int e) { return e == EWOULDBLOCK || e == EAGAIN || e == EINPROGRESS || e == EINTR; }
static int rbli_in_progress(int e) { return e == EINPROGRESS || e == EWOULDBLOCK || e == EAGAIN || e == EINTR; }
static int rbli_net_up(void) { return 0; }
static void rbli_net_down(void) {}
static void rbli_sock_close(rbli_sock s) { close(s); }
static int rbli_sock_nonblock(rbli_sock s) {
    int fl = fcntl(s, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(s, F_SETFL, fl | O_NONBLOCK) == 0 ? 0 : -1;
}
static void rbli_random(uint8_t* out, size_t n) {
#if defined(__APPLE__)
    arc4random_buf(out, n);
#else
    int fd = open("/dev/urandom", O_RDONLY
#ifdef O_CLOEXEC
                  | O_CLOEXEC
#endif
    );
    size_t got = 0;
    if (fd >= 0) {
        while (got < n) {
            ssize_t r = read(fd, out + got, n - got);
            if (r <= 0) break;
            got += (size_t)r;
        }
        close(fd);
    }
    if (got < n) {
        uint64_t t = rbl_now_us() ^ ((uint64_t)getpid() << 32);
        for (; got < n; got++) { t = t * 6364136223846793005ULL + 1442695040888963407ULL; out[got] = (uint8_t)(t >> 33); }
    }
#endif
}
static unsigned rbli_pid(void) { return (unsigned)getpid(); }

#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* TCP/UDP helpers shared by both platforms. Addresses are always 127.0.0.1. */

static void rbli_loopback(struct sockaddr_in* a, uint16_t port) {
    memset(a, 0, sizeof *a);
    a->sin_family = AF_INET;
    a->sin_port = htons(port);
    a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
}

static void rbli_sock_options(rbli_sock s) {
#if defined(SO_NOSIGPIPE)
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, (const char*)&one, sizeof one);
#else
    (void)s;
#endif
}

static void rbli_nodelay(rbli_sock s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
}

static int rbli_local_port(rbli_sock s, uint16_t* port) {
    struct sockaddr_in a;
    socklen_t n = (socklen_t)sizeof a;
    if (getsockname(s, (struct sockaddr*)&a, &n) != 0) return -1;
    *port = ntohs(a.sin_port);
    return 0;
}

/* UDP socket bound to 127.0.0.1:port, non-blocking, asking for a 4 MiB receive buffer. */
static rbli_sock rbli_udp_open(uint16_t port, uint16_t* bound) {
    struct sockaddr_in a;
    rbli_sock s = socket(AF_INET, SOCK_DGRAM, 0);
    int buf = RBL_RECV_BUFFER_BYTES;
    if (s == RBLI_BAD_SOCK) return s;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&buf, sizeof buf); /* best effort: the OS may grant less */
    rbli_loopback(&a, port);
    if (bind(s, (struct sockaddr*)&a, sizeof a) != 0 || rbli_sock_nonblock(s) != 0 || rbli_local_port(s, bound) != 0) {
        rbli_sock_close(s);
        return RBLI_BAD_SOCK;
    }
#if RBLI_WINDOWS
    {   /* ICMP port-unreachable from an earlier send must not make recvfrom fail */
        DWORD ret = 0;
        BOOL off = FALSE;
        WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12), &off, sizeof off, NULL, 0, &ret, NULL, NULL);
    }
#endif
    return s;
}

static rbli_sock rbli_tcp_listen(uint16_t port, uint16_t* bound) {
    struct sockaddr_in a;
    rbli_sock s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == RBLI_BAD_SOCK) return s;
#if !RBLI_WINDOWS
    { int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one); }
#endif
    rbli_loopback(&a, port);
    if (bind(s, (struct sockaddr*)&a, sizeof a) != 0 || listen(s, 8) != 0 || rbli_sock_nonblock(s) != 0 ||
        rbli_local_port(s, bound) != 0) {
        rbli_sock_close(s);
        return RBLI_BAD_SOCK;
    }
    return s;
}

/* >0 bytes, 0 end of stream, -1 nothing now, -2 error */
static int rbli_tcp_recv(rbli_sock s, void* buf, size_t n) {
    int r = (int)recv(s, (char*)buf, (int)n, 0);
    if (r > 0) return r;
    if (r == 0) return 0;
    return rbli_would_block(rbli_os_errno()) ? -1 : -2;
}
/* >=0 bytes sent, -1 would block, -2 error */
static int rbli_tcp_send(rbli_sock s, const void* buf, size_t n) {
    int r = (int)send(s, (const char*)buf, (int)n, MSG_NOSIGNAL);
    if (r >= 0) return r;
    return rbli_would_block(rbli_os_errno()) ? -1 : -2;
}
static void rbli_shutdown_write(rbli_sock s) {
#if RBLI_WINDOWS
    shutdown(s, SD_SEND);
#else
    shutdown(s, SHUT_WR);
#endif
}

/* Starts a non-blocking connect to 127.0.0.1:port. Returns the socket or RBLI_BAD_SOCK. */
static rbli_sock rbli_tcp_connect_start(uint16_t port) {
    struct sockaddr_in a;
    rbli_sock s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == RBLI_BAD_SOCK) return s;
    rbli_sock_options(s);
    if (rbli_sock_nonblock(s) != 0) { rbli_sock_close(s); return RBLI_BAD_SOCK; }
    rbli_loopback(&a, port);
    if (connect(s, (struct sockaddr*)&a, sizeof a) != 0 && !rbli_in_progress(rbli_os_errno())) {
        rbli_sock_close(s);
        return RBLI_BAD_SOCK;
    }
    return s;
}

/* 1 connected, 0 still in progress, -1 failed */
static int rbli_tcp_connect_check(rbli_sock s) {
#if RBLI_WINDOWS
    fd_set w, x;
    struct timeval tv;
    int err = 0, n;
    int len = (int)sizeof err;
    FD_ZERO(&w); FD_ZERO(&x);
    FD_SET(s, &w); FD_SET(s, &x);
    tv.tv_sec = 0; tv.tv_usec = 0;
    n = select(0, NULL, &w, &x, &tv);
    if (n < 0) return -1;
    if (n == 0) return 0;
    if (FD_ISSET(s, &x)) return -1;
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &len) != 0 || err != 0) return -1;
    return 1;
#else
    struct pollfd p;
    int err = 0, n;
    socklen_t len = (socklen_t)sizeof err;
    p.fd = s; p.events = POLLOUT; p.revents = 0;
    n = poll(&p, 1, 0);
    if (n < 0) return rbli_os_errno() == EINTR ? 0 : -1;
    if (n == 0) return 0;
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) return -1;
    return (p.revents & (POLLOUT | POLLERR | POLLHUP)) ? ((p.revents & (POLLERR | POLLHUP)) && !(p.revents & POLLOUT) ? -1 : 1) : 0;
#endif
}

static rbli_sock rbli_tcp_accept(rbli_sock listener) {
    rbli_sock s = accept(listener, NULL, NULL);
    if (s == RBLI_BAD_SOCK) return s;
    if (rbli_sock_nonblock(s) != 0) { rbli_sock_close(s); return RBLI_BAD_SOCK; }
    rbli_sock_options(s);
    rbli_nodelay(s);
    return s;
}

/* Receives one datagram; returns its length, -1 if none is waiting, -2 on a (skippable) error.
 * *addr and *port get the source. */
static int rbli_udp_recv(rbli_sock s, uint8_t* buf, size_t cap, uint32_t* addr, uint16_t* port) {
    struct sockaddr_in a;
    socklen_t al = (socklen_t)sizeof a;
    int r = (int)recvfrom(s, (char*)buf, (int)cap, 0, (struct sockaddr*)&a, &al);
    if (r >= 0) { *addr = ntohl(a.sin_addr.s_addr); *port = ntohs(a.sin_port); return r; }
    return rbli_would_block(rbli_os_errno()) ? -1 : -2;
}
/* 1 sent, 0 the OS would not take it */
static int rbli_udp_send(rbli_sock s, const uint8_t* buf, size_t n, uint16_t port) {
    struct sockaddr_in a;
    rbli_loopback(&a, port);
    return sendto(s, (const char*)buf, (int)n, 0, (struct sockaddr*)&a, sizeof a) >= 0 ? 1 : 0;
}

/* Exactly 127.0.0.1 and the peer's announced UDP port (CONTRACT.md 2): not another loopback
 * address, not ::1. addr is in host order. */
static int rbli_is_peer_addr(uint32_t addr, uint16_t port, uint16_t peer_port) {
    return addr == 0x7F000001u && port == peer_port;
}

/* ------------------------------------------------------------------------------------------
 * Camera, pose and quaternion helpers
 * ---------------------------------------------------------------------------------------- */

#define RBLI_FINITE(x) (((x) - (x)) == 0.0f)

static void rbli_v_sub(const float* a, const float* b, float* o) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
static float rbli_v_dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void rbli_v_cross(const float* a, const float* b, float* o) {
    float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x; o[1] = y; o[2] = z;
}
static void rbli_v_norm(const float* a, float* o) {
    float l = (float)sqrt((double)rbli_v_dot(a, a));
    if (l > 0) { o[0] = a[0] / l; o[1] = a[1] / l; o[2] = a[2] / l; }
    else { o[0] = a[0]; o[1] = a[1]; o[2] = a[2]; }
}

RBLDEF void rbl_camera_look_at(rbl_camera* c, const float eye[3], const float target[3], float fov_y, float near_m,
                               float far_m, uint32_t width, uint32_t height) {
    static const float up[3] = {0, 1, 0};
    float f[3], s[3], u[3], t, aspect;
    float* v;
    float* p;
    int i;
    rbli_v_sub(target, eye, f);
    rbli_v_norm(f, f);
    rbli_v_cross(f, up, s);
    rbli_v_norm(s, s);
    rbli_v_cross(s, f, u);
    v = c->view;
    v[0] = s[0];  v[1] = u[0];  v[2] = -f[0]; v[3] = 0;
    v[4] = s[1];  v[5] = u[1];  v[6] = -f[1]; v[7] = 0;
    v[8] = s[2];  v[9] = u[2];  v[10] = -f[2]; v[11] = 0;
    v[12] = -rbli_v_dot(s, eye); v[13] = -rbli_v_dot(u, eye); v[14] = rbli_v_dot(f, eye); v[15] = 1;
    aspect = (float)width / (float)(height > 0 ? height : 1);
    t = (float)(1.0 / tan((double)fov_y * 0.5));
    p = c->proj;
    for (i = 0; i < 16; i++) p[i] = 0;
    p[0] = t / aspect;
    p[5] = t;
    p[11] = -1;
    if (RBLI_FINITE(far_m)) {
        p[10] = (far_m + near_m) / (near_m - far_m);
        p[14] = 2 * far_m * near_m / (near_m - far_m);
    } else {
        p[10] = -1;
        p[14] = -2 * near_m;
    }
    c->fov_y = fov_y;
    c->near_m = near_m;
    c->far_m = far_m;
    c->viewport_width = width;
    c->viewport_height = height;
    c->depth_range = RBL_DEPTH_NEG_ONE_TO_ONE;
}

RBLDEF void rbl_camera_eye(const rbl_camera* c, float out[3]) {
    const float* v = c->view;
    int col, r;
    float t[3], e[3];
    t[0] = v[12]; t[1] = v[13]; t[2] = v[14];
    for (col = 0; col < 3; col++) {
        float s = 0;
        for (r = 0; r < 3; r++) s += v[col * 4 + r] * t[r];
        e[col] = -s;
    }
    out[0] = e[0]; out[1] = e[1]; out[2] = e[2];
}

RBLDEF void rbl_camera_forward(const rbl_camera* c, float out[3]) {
    out[0] = -c->view[2]; out[1] = -c->view[6]; out[2] = -c->view[10];
}

RBLDEF void rbl_pose_identity(rbl_pose* p, uint32_t entity, uint64_t time_us) {
    memset(p, 0, sizeof *p);
    p->entity = entity;
    p->time_us = time_us;
    p->rotation[3] = 1;
    p->scale[0] = p->scale[1] = p->scale[2] = 1;
}

RBLDEF void rbl_quat_axis_angle(float out[4], const float axis[3], float angle) {
    float s = (float)sin((double)angle * 0.5);
    out[0] = axis[0] * s; out[1] = axis[1] * s; out[2] = axis[2] * s;
    out[3] = (float)cos((double)angle * 0.5);
}

RBLDEF void rbl_quat_rotate(float out[3], const float q[4], const float v[3]) {
    float ux = q[0], uy = q[1], uz = q[2], w = q[3];
    float cx = uy * v[2] - uz * v[1];
    float cy = uz * v[0] - ux * v[2];
    float cz = ux * v[1] - uy * v[0];
    float dx = uy * cz - uz * cy;
    float dy = uz * cx - ux * cz;
    float dz = ux * cy - uy * cx;
    out[0] = v[0] + 2 * (w * cx + dx);
    out[1] = v[1] + 2 * (w * cy + dy);
    out[2] = v[2] + 2 * (w * cz + dz);
}

RBLDEF void rbl_quat_from_to(float out[4], const float a[3], const float b[3]) {
    float u[3], v[3], d, c[3], n;
    rbli_v_norm(a, u);
    rbli_v_norm(b, v);
    d = rbli_v_dot(u, v);
    if (d < -0.999999f) {
        static const float ax[3] = {1, 0, 0}, ay[3] = {0, 1, 0};
        float axis[3];
        rbli_v_cross(u, fabsf(u[0]) < 0.9f ? ax : ay, axis);
        rbli_v_norm(axis, axis);
        out[0] = axis[0]; out[1] = axis[1]; out[2] = axis[2]; out[3] = 0;
        return;
    }
    rbli_v_cross(u, v, c);
    n = (float)sqrt((double)(c[0] * c[0] + c[1] * c[1] + c[2] * c[2] + (1 + d) * (1 + d)));
    out[0] = c[0] / n; out[1] = c[1] / n; out[2] = c[2] / n; out[3] = (1 + d) / n;
}

/* ------------------------------------------------------------------------------------------
 * Wire format: datagrams
 * ---------------------------------------------------------------------------------------- */

static void rbli_put_header(uint8_t* b, const rbl_header* h, uint8_t kind) {
    rbli_put32(b, RBL_MAGIC);
    rbli_put16(b + 4, h->version);
    b[6] = kind;
    b[7] = h->flags;
    rbli_put32(b + 8, h->session);
    rbli_put32(b + 12, h->seq);
    rbli_put64(b + 16, h->time_us);
}

RBLDEF int rbl_wire_encode_heartbeat(const rbl_header* h, uint8_t* out, size_t cap) {
    if (!h || !out) return RBL_ERR_ARGS;
    if (cap < RBL_HEADER_LEN) return RBL_ERR_TOO_BIG;
    rbli_put_header(out, h, RBL_HEARTBEAT);
    return RBL_HEADER_LEN;
}

RBLDEF int rbl_wire_encode_camera(const rbl_header* h, const rbl_camera* c, uint8_t* out, size_t cap) {
    uint8_t* p;
    if (!h || !c || !out) return RBL_ERR_ARGS;
    if (c->depth_range > 2) return RBL_ERR_MALFORMED;
    if (cap < RBL_HEADER_LEN + RBL_CAMERA_LEN) return RBL_ERR_TOO_BIG;
    rbli_put_header(out, h, RBL_CAMERA);
    p = out + RBL_HEADER_LEN;
    rbli_putfs(p, c->view, 16);
    rbli_putfs(p + 64, c->proj, 16);
    rbli_putf(p + 128, c->fov_y);
    rbli_putf(p + 132, c->near_m);
    rbli_putf(p + 136, c->far_m);
    rbli_put32(p + 140, c->viewport_width);
    rbli_put32(p + 144, c->viewport_height);
    rbli_put32(p + 148, c->depth_range);
    return RBL_HEADER_LEN + RBL_CAMERA_LEN;
}

RBLDEF int rbl_wire_encode_poses(const rbl_header* h, const rbl_pose* poses, size_t count, uint8_t* out, size_t cap) {
    uint8_t* p;
    size_t i, total;
    if (!h || !poses || !out) return RBL_ERR_ARGS;
    if (count < 1 || count > RBL_MAX_POSES) return RBL_ERR_MALFORMED;
    total = RBL_HEADER_LEN + 8 + RBL_POSE_LEN * count;
    if (cap < total) return RBL_ERR_TOO_BIG;
    rbli_put_header(out, h, RBL_POSE);
    rbli_put32(out + RBL_HEADER_LEN, (uint32_t)count);
    rbli_put32(out + RBL_HEADER_LEN + 4, 0);
    p = out + RBL_HEADER_LEN + 8;
    for (i = 0; i < count; i++, p += RBL_POSE_LEN) {
        const rbl_pose* q = &poses[i];
        rbli_put32(p, q->entity);
        rbli_put32(p + 4, q->flags);
        rbli_put64(p + 8, q->time_us);
        rbli_putfs(p + 16, q->translation, 3);
        rbli_putfs(p + 28, q->rotation, 4);
        rbli_putfs(p + 44, q->scale, 3);
        rbli_putfs(p + 56, q->velocity, 3);
        rbli_putfs(p + 68, q->angular_velocity, 3);
    }
    return (int)total;
}

RBLDEF int rbl_wire_encode_collision(const rbl_header* h, const rbl_collision* c, const rbl_triangle* tris,
                                     size_t count, uint8_t* out, size_t cap) {
    uint8_t* p;
    size_t i, total;
    if (!h || !c || !out || (count > 0 && !tris)) return RBL_ERR_ARGS;
    if (count > RBL_MAX_TRIANGLES) return RBL_ERR_MALFORMED;
    if (c->chunk_count < 1 || c->chunk_index >= c->chunk_count) return RBL_ERR_MALFORMED;
    total = RBL_HEADER_LEN + 32 + RBL_TRIANGLE_LEN * count;
    if (cap < total) return RBL_ERR_TOO_BIG;
    rbli_put_header(out, h, RBL_COLLISION);
    p = out + RBL_HEADER_LEN;
    rbli_put32(p, c->query);
    rbli_put16(p + 4, c->chunk_index);
    rbli_put16(p + 6, c->chunk_count);
    rbli_putfs(p + 8, c->center, 3);
    rbli_putf(p + 20, c->radius);
    rbli_put32(p + 24, (uint32_t)count);
    rbli_put32(p + 28, 0);
    p += 32;
    for (i = 0; i < count; i++, p += RBL_TRIANGLE_LEN) rbli_putfs(p, &tris[i].v[0][0], 9);
    return (int)total;
}

RBLDEF int rbl_wire_encode_frame(const rbl_header* h, const rbl_frame* f, uint8_t* out, size_t cap) {
    uint8_t* p;
    if (!h || !f || !out) return RBL_ERR_ARGS;
    if (f->frame == 0) return RBL_ERR_MALFORMED;
    if (cap < RBL_HEADER_LEN + RBL_FRAME_LEN) return RBL_ERR_TOO_BIG;
    rbli_put_header(out, h, RBL_FRAME);
    p = out + RBL_HEADER_LEN;
    rbli_put64(p, f->frame);
    rbli_put32(p + 8, f->slot);
    rbli_put32(p + 12, f->camera_seq);
    rbli_put32(p + 16, f->width);
    rbli_put32(p + 20, f->height);
    return RBL_HEADER_LEN + RBL_FRAME_LEN;
}

RBLDEF size_t rbl_wire_chunk_count(size_t n) {
    size_t c = (n + RBL_MAX_TRIANGLES - 1) / RBL_MAX_TRIANGLES;
    return c < 1 ? 1 : c;
}

RBLDEF int rbl_wire_decode(const uint8_t* buf, size_t len, rbl_decoded* d) {
    const uint8_t* p;
    size_t payload;
    uint32_t kind;
    if (!buf || !d) return RBL_ERR_ARGS;
    memset(d, 0, sizeof *d);
    if (len < RBL_HEADER_LEN) return RBL_ERR_MALFORMED;
    if (rbli_get32(buf) != RBL_MAGIC) return RBL_ERR_MALFORMED;
    if (rbli_get16(buf + 4) != RBL_VERSION) return RBL_ERR_MALFORMED;
    d->header.magic = RBL_MAGIC;
    d->header.version = (uint16_t)rbli_get16(buf + 4);
    d->header.kind = buf[6];
    d->header.flags = buf[7];
    d->header.session = rbli_get32(buf + 8);
    d->header.seq = rbli_get32(buf + 12);
    d->header.time_us = rbli_get64(buf + 16);
    kind = buf[6];
    payload = len - RBL_HEADER_LEN;
    p = buf + RBL_HEADER_LEN;
    switch (kind) {
    case RBL_HEARTBEAT:
        if (payload != 0) return RBL_ERR_MALFORMED;
        break;
    case RBL_CAMERA: {
        rbl_camera* c = &d->u.camera;
        if (payload != RBL_CAMERA_LEN) return RBL_ERR_MALFORMED;
        if (rbli_get32(p + 148) > 2) return RBL_ERR_MALFORMED;
        rbli_getfs(p, c->view, 16);
        rbli_getfs(p + 64, c->proj, 16);
        c->fov_y = rbli_getf(p + 128);
        c->near_m = rbli_getf(p + 132);
        c->far_m = rbli_getf(p + 136);
        c->viewport_width = rbli_get32(p + 140);
        c->viewport_height = rbli_get32(p + 144);
        c->depth_range = rbli_get32(p + 148);
        break;
    }
    case RBL_POSE: {
        uint32_t count;
        if (payload < 4) return RBL_ERR_MALFORMED;
        count = rbli_get32(p);
        if (count == 0 || count > RBL_MAX_POSES) return RBL_ERR_MALFORMED;
        if (payload != 8 + (size_t)RBL_POSE_LEN * count) return RBL_ERR_MALFORMED;
        d->u.poses.count = count;
        d->records = p + 8;
        break;
    }
    case RBL_COLLISION: {
        rbl_collision* c = &d->u.collision;
        uint32_t n, index, count;
        if (payload < 32) return RBL_ERR_MALFORMED;
        index = rbli_get16(p + 4);
        count = rbli_get16(p + 6);
        n = rbli_get32(p + 24);
        if (count == 0 || index >= count) return RBL_ERR_MALFORMED;
        if (n > RBL_MAX_TRIANGLES) return RBL_ERR_MALFORMED;
        if (payload != 32 + (size_t)RBL_TRIANGLE_LEN * n) return RBL_ERR_MALFORMED;
        c->query = rbli_get32(p);
        c->chunk_index = (uint16_t)index;
        c->chunk_count = (uint16_t)count;
        rbli_getfs(p + 8, c->center, 3);
        c->radius = rbli_getf(p + 20);
        c->triangle_count = n;
        c->reserved = rbli_get32(p + 28);
        d->records = p + 32;
        break;
    }
    case RBL_FRAME: {
        rbl_frame* f = &d->u.frame;
        if (payload != RBL_FRAME_LEN) return RBL_ERR_MALFORMED;
        f->frame = rbli_get64(p);
        if (f->frame == 0) return RBL_ERR_MALFORMED;
        f->slot = rbli_get32(p + 8);
        f->camera_seq = rbli_get32(p + 12);
        f->width = rbli_get32(p + 16);
        f->height = rbli_get32(p + 20);
        break;
    }
    default:
        d->u.unknown.payload_len = payload;
        break;
    }
    return RBL_OK;
}

RBLDEF void rbl_wire_pose(const rbl_decoded* d, uint32_t i, rbl_pose* out) {
    const uint8_t* p = d->records + (size_t)RBL_POSE_LEN * i;
    out->entity = rbli_get32(p);
    out->flags = rbli_get32(p + 4);
    out->time_us = rbli_get64(p + 8);
    rbli_getfs(p + 16, out->translation, 3);
    rbli_getfs(p + 28, out->rotation, 4);
    rbli_getfs(p + 44, out->scale, 3);
    rbli_getfs(p + 56, out->velocity, 3);
    rbli_getfs(p + 68, out->angular_velocity, 3);
}

RBLDEF void rbl_wire_triangle(const rbl_decoded* d, uint32_t i, rbl_triangle* out) {
    rbli_getfs(d->records + (size_t)RBL_TRIANGLE_LEN * i, &out->v[0][0], 9);
}

/* ------------------------------------------------------------------------------------------
 * UTF-8
 * ---------------------------------------------------------------------------------------- */

/* Length (1..4) of the valid UTF-8 sequence at s, or 0 if there is none (overlong forms,
 * surrogates and code points above U+10FFFF are invalid, as in Rust's str). */
static size_t rbli_utf8_seq(const uint8_t* s, size_t n) {
    uint8_t a;
    if (n == 0) return 0;
    a = s[0];
    if (a < 0x80) return 1;
    if (a >= 0xC2 && a <= 0xDF) return (n >= 2 && (s[1] & 0xC0) == 0x80) ? 2 : 0;
    if (a >= 0xE0 && a <= 0xEF) {
        uint8_t lo = a == 0xE0 ? 0xA0 : 0x80, hi = a == 0xED ? 0x9F : 0xBF;
        return (n >= 3 && s[1] >= lo && s[1] <= hi && (s[2] & 0xC0) == 0x80) ? 3 : 0;
    }
    if (a >= 0xF0 && a <= 0xF4) {
        uint8_t lo = a == 0xF0 ? 0x90 : 0x80, hi = a == 0xF4 ? 0x8F : 0xBF;
        return (n >= 4 && s[1] >= lo && s[1] <= hi && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) ? 4 : 0;
    }
    return 0;
}

static int rbli_utf8_valid(const uint8_t* s, size_t n) {
    size_t i = 0;
    while (i < n) {
        size_t k;
        if (s[i] < 0x80) { i++; continue; }
        k = rbli_utf8_seq(s + i, n - i);
        if (k == 0) return 0;
        i += k;
    }
    return 1;
}

/* ------------------------------------------------------------------------------------------
 * JSON: a strict reader for the control lines. It accepts exactly what serde_json accepts (the
 * Rust side): numbers follow the JSON grammar, -0 is a float, a number beyond the range of a
 * double is an error, \u escapes take four hex digits and surrogates must come as a pair, and
 * arrays and objects nest at most 127 deep. Strings are decoded in place.
 * ---------------------------------------------------------------------------------------- */

#define RBLI_JSON_MAX_NESTING 127

typedef struct rbli_jf { /* a captured top-level value */
    int kind;            /* 0 absent, 1 string, 2 unsigned integer, 3 null, 4 anything else */
    char* s;
    size_t slen;
    uint64_t u;
} rbli_jf;

typedef struct rbli_jp {
    char* s;
    size_t n, i;
    const char* why;
} rbli_jp;

static int rbli_jfail(rbli_jp* p, const char* why) { p->why = why; return -1; }

static void rbli_jws(rbli_jp* p) {
    while (p->i < p->n) {
        char c = p->s[p->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->i++;
        else break;
    }
}

static int rbli_jdigit(const rbli_jp* p) { return p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9'; }

/* number = [-] (0 | 1-9 digits) [. digits] [(e|E) [+|-] digits]  (RFC 8259) */
static int rbli_jnumber(rbli_jp* p, rbli_jf* cap) {
    size_t start = p->i, int_start, int_end, frac_start = 0, frac_end = 0;
    int neg = 0, integer = 1;
    long exp10 = 0;
    int exp_neg = 0;
    if (p->s[p->i] == '-') { neg = 1; p->i++; }
    if (!rbli_jdigit(p)) return rbli_jfail(p, "bad number");
    int_start = p->i;
    if (p->s[p->i] == '0') p->i++; /* a leading zero ends the integer part: "01" leaves the 1 behind, which no caller accepts */
    else while (rbli_jdigit(p)) p->i++;
    int_end = p->i;
    if (p->i < p->n && p->s[p->i] == '.') {
        integer = 0;
        p->i++;
        if (!rbli_jdigit(p)) return rbli_jfail(p, "bad number");
        frac_start = p->i;
        while (rbli_jdigit(p)) p->i++;
        frac_end = p->i;
    }
    if (p->i < p->n && (p->s[p->i] == 'e' || p->s[p->i] == 'E')) {
        integer = 0;
        p->i++;
        if (p->i < p->n && (p->s[p->i] == '+' || p->s[p->i] == '-')) { exp_neg = p->s[p->i] == '-'; p->i++; }
        if (!rbli_jdigit(p)) return rbli_jfail(p, "bad number");
        while (rbli_jdigit(p)) {
            if (exp10 < 100000) exp10 = exp10 * 10 + (p->s[p->i] - '0');
            p->i++;
        }
        if (exp_neg) exp10 = -exp10;
    }
    {   /* a number beyond the range of a double is an error (it would be infinity) */
        size_t k, lead_zero = 0, sig_int;
        long dexp;
        int have_sig = 0;
        char first[17];
        size_t nfirst = 0;
        /* significant digits: integer part without leading zeros, then the fraction */
        sig_int = 0;
        for (k = int_start; k < int_end; k++) {
            if (!have_sig && p->s[k] == '0') continue;
            have_sig = 1;
            if (nfirst < 17) first[nfirst++] = p->s[k];
            sig_int++;
        }
        if (have_sig) {
            for (k = frac_start; k < frac_end && nfirst < 17; k++) first[nfirst++] = p->s[k];
            dexp = (long)sig_int - 1 + exp10;
        } else {
            for (k = frac_start; k < frac_end; k++) {
                if (p->s[k] == '0') { lead_zero++; continue; }
                have_sig = 1;
                break;
            }
            if (have_sig) {
                for (; k < frac_end && nfirst < 17; k++) first[nfirst++] = p->s[k];
                dexp = -(long)(lead_zero + 1) + exp10;
            } else {
                dexp = -100000;
            }
        }
        if (have_sig) {
            if (dexp > 308) return rbli_jfail(p, "number out of range");
            if (dexp == 308) {
                static const char max17[] = "17976931348623158";
                char pad[17];
                size_t m;
                for (m = 0; m < 17; m++) pad[m] = m < nfirst ? first[m] : '0';
                if (memcmp(pad, max17, 17) >= 0) return rbli_jfail(p, "number out of range");
            }
        }
    }
    if (cap) {
        size_t dl = int_end - int_start;
        cap->kind = 4;
        if (integer && !neg && dl <= 20) {
            uint64_t v = 0;
            size_t k;
            int over = 0;
            for (k = int_start; k < int_end; k++) {
                uint64_t dgt = (uint64_t)(p->s[k] - '0');
                if (v > (UINT64_MAX - dgt) / 10u) { over = 1; break; }
                v = v * 10u + dgt;
            }
            if (!over) { cap->kind = 2; cap->u = v; }
        }
    }
    (void)start;
    return 0;
}

static int rbli_jhex4(rbli_jp* p, uint32_t* out) {
    uint32_t v = 0;
    int k;
    if (p->i + 4 > p->n) return rbli_jfail(p, "bad \\u escape");
    for (k = 0; k < 4; k++) {
        char c = p->s[p->i + (size_t)k];
        int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) return rbli_jfail(p, "bad \\u escape");
        v = v * 16 + (uint32_t)d;
    }
    p->i += 4;
    *out = v;
    return 0;
}

static char* rbli_putcp(char* w, uint32_t cp) {
    if (cp < 0x80) { *w++ = (char)cp; }
    else if (cp < 0x800) { *w++ = (char)(0xC0 | (cp >> 6)); *w++ = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { *w++ = (char)(0xE0 | (cp >> 12)); *w++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *w++ = (char)(0x80 | (cp & 0x3F)); }
    else { *w++ = (char)(0xF0 | (cp >> 18)); *w++ = (char)(0x80 | ((cp >> 12) & 0x3F)); *w++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *w++ = (char)(0x80 | (cp & 0x3F)); }
    return w;
}

/* p->i is at the opening quote. Decodes in place: the output never outruns the input. */
static int rbli_jstring(rbli_jp* p, char** out, size_t* outlen) {
    char* start;
    char* w;
    p->i++;
    start = p->s + p->i;
    w = start;
    for (;;) {
        unsigned char c;
        if (p->i >= p->n) return rbli_jfail(p, "unterminated string");
        c = (unsigned char)p->s[p->i++];
        if (c == '"') { *w = 0; *out = start; *outlen = (size_t)(w - start); return 0; }
        if (c < 0x20) return rbli_jfail(p, "control character in string");
        if (c != '\\') { *w++ = (char)c; continue; }
        if (p->i >= p->n) return rbli_jfail(p, "unterminated escape");
        c = (unsigned char)p->s[p->i++];
        switch (c) {
        case '"': *w++ = '"'; break;
        case '\\': *w++ = '\\'; break;
        case '/': *w++ = '/'; break;
        case 'b': *w++ = '\b'; break;
        case 'f': *w++ = '\f'; break;
        case 'n': *w++ = '\n'; break;
        case 'r': *w++ = '\r'; break;
        case 't': *w++ = '\t'; break;
        case 'u': {
            uint32_t u, low;
            if (rbli_jhex4(p, &u) != 0) return -1;
            if (u >= 0xD800 && u <= 0xDBFF) {
                if (p->i + 1 >= p->n || p->s[p->i] != '\\' || p->s[p->i + 1] != 'u') return rbli_jfail(p, "lone leading surrogate in hex escape");
                p->i += 2;
                if (rbli_jhex4(p, &low) != 0) return -1;
                if (low < 0xDC00 || low > 0xDFFF) return rbli_jfail(p, "lone leading surrogate in hex escape");
                u = 0x10000 + ((u - 0xD800) << 10) + (low - 0xDC00);
            } else if (u >= 0xDC00 && u <= 0xDFFF) {
                return rbli_jfail(p, "lone trailing surrogate in hex escape");
            }
            w = rbli_putcp(w, u);
            break;
        }
        default: return rbli_jfail(p, "bad escape");
        }
    }
}

static int rbli_jvalue(rbli_jp* p, int depth, rbli_jf* cap, const char* const* names, rbli_jf* fields, size_t nfields);

static int rbli_jliteral(rbli_jp* p, const char* lit) {
    size_t l = strlen(lit);
    if (p->i + l > p->n || memcmp(p->s + p->i, lit, l) != 0) return rbli_jfail(p, "bad literal");
    p->i += l;
    return 0;
}

static int rbli_jarray(rbli_jp* p, int depth) {
    p->i++;
    rbli_jws(p);
    if (p->i < p->n && p->s[p->i] == ']') { p->i++; return 0; }
    for (;;) {
        char c;
        rbli_jws(p);
        if (rbli_jvalue(p, depth + 1, NULL, NULL, NULL, 0) != 0) return -1;
        rbli_jws(p);
        if (p->i >= p->n) return rbli_jfail(p, "unterminated array");
        c = p->s[p->i++];
        if (c == ']') return 0;
        if (c != ',') return rbli_jfail(p, "expected , or ]");
    }
}

/* names/fields: when non-NULL, the object's own keys are looked up in names[0..nfields) and the
 * matching values captured (the last of a repeated key wins, as in serde_json's map). */
static int rbli_jobject(rbli_jp* p, int depth, const char* const* names, rbli_jf* fields, size_t nfields) {
    p->i++;
    rbli_jws(p);
    if (p->i < p->n && p->s[p->i] == '}') { p->i++; return 0; }
    for (;;) {
        char c, *key;
        size_t klen, f;
        rbli_jf* cap = NULL;
        rbli_jws(p);
        if (p->i >= p->n || p->s[p->i] != '"') return rbli_jfail(p, "expected key");
        if (rbli_jstring(p, &key, &klen) != 0) return -1;
        rbli_jws(p);
        if (p->i >= p->n || p->s[p->i++] != ':') return rbli_jfail(p, "expected :");
        rbli_jws(p);
        if (names) {
            for (f = 0; f < nfields; f++) {
                if (strlen(names[f]) == klen && memcmp(names[f], key, klen) == 0) { cap = &fields[f]; break; }
            }
        }
        if (rbli_jvalue(p, depth + 1, cap, NULL, NULL, 0) != 0) return -1;
        rbli_jws(p);
        if (p->i >= p->n) return rbli_jfail(p, "unterminated object");
        c = p->s[p->i++];
        if (c == '}') return 0;
        if (c != ',') return rbli_jfail(p, "expected , or }");
    }
}

static int rbli_jvalue(rbli_jp* p, int depth, rbli_jf* cap, const char* const* names, rbli_jf* fields, size_t nfields) {
    char c;
    if (p->i >= p->n) return rbli_jfail(p, "unexpected end");
    c = p->s[p->i];
    if ((c == '{' || c == '[') && depth >= RBLI_JSON_MAX_NESTING) return rbli_jfail(p, "nested too deeply");
    if (cap) { cap->kind = 4; cap->s = NULL; cap->slen = 0; cap->u = 0; }
    switch (c) {
    case '{': return rbli_jobject(p, depth, names, fields, nfields);
    case '[': return rbli_jarray(p, depth);
    case '"': {
        char* s;
        size_t l;
        if (rbli_jstring(p, &s, &l) != 0) return -1;
        if (cap) { cap->kind = 1; cap->s = s; cap->slen = l; }
        return 0;
    }
    case 't': return rbli_jliteral(p, "true");
    case 'f': return rbli_jliteral(p, "false");
    case 'n':
        if (rbli_jliteral(p, "null") != 0) return -1;
        if (cap) cap->kind = 3;
        return 0;
    default:
        if (c == '-' || (c >= '0' && c <= '9')) return rbli_jnumber(p, cap);
        return rbli_jfail(p, "unexpected character");
    }
}

/* Parses a whole document that must be one object; captures the named top-level keys. */
static int rbli_json_object(char* s, size_t n, const char* const* names, rbli_jf* fields, size_t nfields, const char** why) {
    rbli_jp p;
    size_t k;
    p.s = s; p.n = n; p.i = 0; p.why = "bad json";
    for (k = 0; k < nfields; k++) { fields[k].kind = 0; fields[k].s = NULL; fields[k].slen = 0; fields[k].u = 0; }
    rbli_jws(&p);
    if (p.i >= p.n) { *why = "unexpected end"; return -1; }
    if (p.s[p.i] != '{') {
        /* not an object: still validate it as JSON first, so the error is the same one serde gives */
        if (rbli_jvalue(&p, 0, NULL, NULL, NULL, 0) != 0) { *why = p.why; return -1; }
        rbli_jws(&p);
        if (p.i != p.n) { *why = "trailing characters"; return -1; }
        *why = "not an object";
        return -1;
    }
    if (rbli_jvalue(&p, 0, NULL, names, fields, nfields) != 0) { *why = p.why; return -1; }
    rbli_jws(&p);
    if (p.i != p.n) { *why = "trailing characters"; return -1; }
    return 0;
}

/* ------------------------------------------------------------------------------------------
 * Control messages
 * ---------------------------------------------------------------------------------------- */

enum {
    RBLI_K_TYPE, RBLI_K_ROLE, RBLI_K_GAME, RBLI_K_PROTOCOL, RBLI_K_MIN_PROTOCOL, RBLI_K_UDP_PORT,
    RBLI_K_HEARTBEAT_MS, RBLI_K_TIMEOUT_MS, RBLI_K_SESSION, RBLI_K_ENTITY, RBLI_K_PROVIDER, RBLI_K_ASSET,
    RBLI_K_NAME, RBLI_K_FOCUS, RBLI_K_REASON, RBLI_K_PATH, RBLI_K_WIDTH, RBLI_K_HEIGHT, RBLI_K_SLOTS,
    RBLI_K_COUNT
};

static const char* const rbli_key_names[RBLI_K_COUNT] = {
    "type", "role", "game", "protocol", "min_protocol", "udp_port", "heartbeat_ms", "timeout_ms", "session",
    "entity", "provider", "asset", "name", "focus", "reason", "path", "width", "height", "slots"
};

static int rbli_str_is(const rbli_jf* f, const char* lit) {
    size_t l = strlen(lit);
    return f->kind == 1 && f->slen == l && memcmp(f->s, lit, l) == 0;
}

static int rbli_need_str(const rbli_jf* f, rbl_str* out, const char** why) {
    if (f->kind != 1) { *why = "bad control line: missing string field"; return -1; }
    out->ptr = f->s; out->len = f->slen;
    return 0;
}
static int rbli_opt_str(const rbli_jf* f, rbl_str* out, int* has, const char** why) {
    if (f->kind == 0 || f->kind == 3) { out->ptr = ""; out->len = 0; *has = 0; return 0; }
    *has = 1;
    return rbli_need_str(f, out, why);
}
static int rbli_need_uint(const rbli_jf* f, uint64_t max, uint32_t* out, const char** why) {
    if (f->kind != 2 || f->u > max) { *why = "bad control line: missing or out-of-range integer field"; return -1; }
    *out = (uint32_t)f->u;
    return 0;
}
static int rbli_role_of(const rbli_jf* f, rbl_role* out, const char** why) {
    if (rbli_str_is(f, "host")) { *out = RBL_ROLE_HOST; return 0; }
    if (rbli_str_is(f, "guest")) { *out = RBL_ROLE_GUEST; return 0; }
    *why = "bad control line: role";
    return -1;
}

RBLDEF int rbl_control_parse(char* line, size_t len, rbl_control* out, const char** why) {
    rbli_jf f[RBLI_K_COUNT];
    const char* w = "bad control line";
    if (!line || !out) return RBL_ERR_ARGS;
    if (!why) why = &w;
    memset(out, 0, sizeof *out);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
    if (len >= RBL_MAX_LINE) { *why = "bad control line: line too long"; return RBL_ERR_MALFORMED; }
    if (rbli_json_object(line, len, rbli_key_names, f, RBLI_K_COUNT, why) != 0) return RBL_ERR_MALFORMED;
    if (f[RBLI_K_TYPE].kind != 1) { *why = "bad control line: no string \"type\""; return RBL_ERR_MALFORMED; }
    if (rbli_str_is(&f[RBLI_K_TYPE], "hello")) {
        out->type = RBL_CTL_HELLO;
        if (rbli_role_of(&f[RBLI_K_ROLE], &out->u.hello.role, why) != 0 ||
            rbli_need_str(&f[RBLI_K_GAME], &out->u.hello.game, why) != 0 ||
            rbli_need_uint(&f[RBLI_K_PROTOCOL], 0xFFFF, &out->u.hello.protocol, why) != 0 ||
            rbli_need_uint(&f[RBLI_K_MIN_PROTOCOL], 0xFFFF, &out->u.hello.min_protocol, why) != 0 ||
            rbli_need_uint(&f[RBLI_K_UDP_PORT], 0xFFFF, &out->u.hello.udp_port, why) != 0 ||
            rbli_need_uint(&f[RBLI_K_HEARTBEAT_MS], RBLI_U32_MAX, &out->u.hello.heartbeat_ms, why) != 0 ||
            rbli_need_uint(&f[RBLI_K_TIMEOUT_MS], RBLI_U32_MAX, &out->u.hello.timeout_ms, why) != 0)
            return RBL_ERR_MALFORMED;
        if (f[RBLI_K_SESSION].kind != 0 && f[RBLI_K_SESSION].kind != 3) {
            if (rbli_need_uint(&f[RBLI_K_SESSION], RBLI_U32_MAX, &out->u.hello.session, why) != 0) return RBL_ERR_MALFORMED;
            out->u.hello.has_session = 1;
        }
    } else if (rbli_str_is(&f[RBLI_K_TYPE], "spawn")) {
        out->type = RBL_CTL_SPAWN;
        if (rbli_need_uint(&f[RBLI_K_ENTITY], RBLI_U32_MAX, &out->u.spawn.entity, why) != 0 ||
            rbli_need_str(&f[RBLI_K_PROVIDER], &out->u.spawn.provider, why) != 0 ||
            rbli_need_str(&f[RBLI_K_ASSET], &out->u.spawn.asset, why) != 0 ||
            rbli_opt_str(&f[RBLI_K_NAME], &out->u.spawn.name, &out->u.spawn.has_name, why) != 0)
            return RBL_ERR_MALFORMED;
    } else if (rbli_str_is(&f[RBLI_K_TYPE], "despawn")) {
        out->type = RBL_CTL_DESPAWN;
        if (rbli_need_uint(&f[RBLI_K_ENTITY], RBLI_U32_MAX, &out->u.despawn.entity, why) != 0) return RBL_ERR_MALFORMED;
    } else if (rbli_str_is(&f[RBLI_K_TYPE], "input")) {
        out->type = RBL_CTL_INPUT;
        if (rbli_role_of(&f[RBLI_K_FOCUS], &out->u.input.focus, why) != 0) {
            *why = "bad control line: focus";
            return RBL_ERR_MALFORMED;
        }
        if (rbli_opt_str(&f[RBLI_K_REASON], &out->u.input.reason, &out->u.input.has_reason, why) != 0) return RBL_ERR_MALFORMED;
    } else if (rbli_str_is(&f[RBLI_K_TYPE], "frame_channel")) {
        out->type = RBL_CTL_FRAME_CHANNEL;
        if (rbli_need_str(&f[RBLI_K_PATH], &out->u.frame_channel.path, why) != 0 ||
            rbli_need_uint(&f[RBLI_K_WIDTH], RBLI_U32_MAX, &out->u.frame_channel.width, why) != 0 ||
            rbli_need_uint(&f[RBLI_K_HEIGHT], RBLI_U32_MAX, &out->u.frame_channel.height, why) != 0 ||
            rbli_need_uint(&f[RBLI_K_SLOTS], RBLI_U32_MAX, &out->u.frame_channel.slots, why) != 0)
            return RBL_ERR_MALFORMED;
    } else if (rbli_str_is(&f[RBLI_K_TYPE], "bye")) {
        int has;
        out->type = RBL_CTL_BYE;
        if (rbli_opt_str(&f[RBLI_K_REASON], &out->u.bye.reason, &has, why) != 0) return RBL_ERR_MALFORMED;
    } else {
        out->type = RBL_CTL_UNKNOWN;
        out->unknown_type.ptr = f[RBLI_K_TYPE].s;
        out->unknown_type.len = f[RBLI_K_TYPE].slen;
    }
    return RBL_OK;
}

/* ---- output sink: a fixed buffer, or one that grows (the control channel's write buffer) ---- */

typedef struct rbli_out {
    char* buf;
    size_t len, cap;
    int growable;
    int fail; /* 0 ok, RBL_ERR_TOO_BIG or RBL_ERR_NOMEM */
} rbli_out;

static int rbli_out_room(rbli_out* o, size_t extra) {
    size_t need = o->len + extra;
    if (o->fail) return -1;
    if (need < o->len) { o->fail = RBL_ERR_TOO_BIG; return -1; }
    if (need > o->cap) {
        size_t nc;
        char* nb;
        if (!o->growable) { o->fail = RBL_ERR_TOO_BIG; return -1; }
        nc = o->cap ? o->cap * 2 : 512;
        while (nc < need) nc *= 2;
        nb = (char*)REWRITE_LINK_REALLOC(o->buf, nc);
        if (!nb) { o->fail = RBL_ERR_NOMEM; return -1; }
        o->buf = nb;
        o->cap = nc;
    }
    return 0;
}
static void rbli_out_bytes(rbli_out* o, const char* s, size_t n) {
    if (rbli_out_room(o, n) != 0) return;
    memcpy(o->buf + o->len, s, n);
    o->len += n;
}
static void rbli_out_lit(rbli_out* o, const char* s) { rbli_out_bytes(o, s, strlen(s)); }
static void rbli_out_u64(rbli_out* o, uint64_t v) {
    char t[24];
    int n = 0;
    do { t[n++] = (char)('0' + (int)(v % 10u)); v /= 10u; } while (v);
    if (rbli_out_room(o, (size_t)n) != 0) return;
    while (n) o->buf[o->len++] = t[--n];
}
/* A JSON string literal; invalid UTF-8 is replaced by U+FFFD so the peer always gets valid UTF-8. */
static void rbli_out_json(rbli_out* o, const char* s, size_t n) {
    static const char hex[] = "0123456789abcdef";
    size_t i = 0;
    rbli_out_lit(o, "\"");
    while (i < n && !o->fail) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x80) {
            size_t k = rbli_utf8_seq((const uint8_t*)s + i, n - i);
            if (k == 0) { rbli_out_bytes(o, "\xEF\xBF\xBD", 3); i++; }
            else { rbli_out_bytes(o, s + i, k); i += k; }
            continue;
        }
        i++;
        switch (c) {
        case '"': rbli_out_lit(o, "\\\""); break;
        case '\\': rbli_out_lit(o, "\\\\"); break;
        case '\n': rbli_out_lit(o, "\\n"); break;
        case '\r': rbli_out_lit(o, "\\r"); break;
        case '\t': rbli_out_lit(o, "\\t"); break;
        default:
            if (c < 0x20) {
                char t[6];
                t[0] = '\\'; t[1] = 'u'; t[2] = '0'; t[3] = '0'; t[4] = hex[c >> 4]; t[5] = hex[c & 15];
                rbli_out_bytes(o, t, 6);
            } else {
                char ch = (char)c;
                rbli_out_bytes(o, &ch, 1);
            }
        }
    }
    rbli_out_lit(o, "\"");
}
static void rbli_out_jstr(rbli_out* o, rbl_str s) { rbli_out_json(o, s.ptr ? s.ptr : "", s.ptr ? s.len : 0); }

static void rbli_out_key_u(rbli_out* o, const char* key, uint64_t v) {
    rbli_out_lit(o, ",\"");
    rbli_out_lit(o, key);
    rbli_out_lit(o, "\":");
    rbli_out_u64(o, v);
}
static void rbli_out_key_s(rbli_out* o, const char* key, rbl_str v) {
    rbli_out_lit(o, ",\"");
    rbli_out_lit(o, key);
    rbli_out_lit(o, "\":");
    rbli_out_jstr(o, v);
}

/* Appends the message and its '\n' to o. */
static void rbli_format_control(rbli_out* o, const rbl_control* c) {
    switch (c->type) {
    case RBL_CTL_HELLO:
        rbli_out_lit(o, "{\"type\":\"hello\",\"role\":\"");
        rbli_out_lit(o, c->u.hello.role == RBL_ROLE_HOST ? "host" : "guest");
        rbli_out_lit(o, "\"");
        rbli_out_key_s(o, "game", c->u.hello.game);
        rbli_out_key_u(o, "protocol", c->u.hello.protocol);
        rbli_out_key_u(o, "min_protocol", c->u.hello.min_protocol);
        rbli_out_key_u(o, "udp_port", c->u.hello.udp_port);
        rbli_out_key_u(o, "heartbeat_ms", c->u.hello.heartbeat_ms);
        rbli_out_key_u(o, "timeout_ms", c->u.hello.timeout_ms);
        if (c->u.hello.has_session) rbli_out_key_u(o, "session", c->u.hello.session);
        break;
    case RBL_CTL_SPAWN:
        rbli_out_lit(o, "{\"type\":\"spawn\"");
        rbli_out_key_u(o, "entity", c->u.spawn.entity);
        rbli_out_key_s(o, "provider", c->u.spawn.provider);
        rbli_out_key_s(o, "asset", c->u.spawn.asset);
        if (c->u.spawn.has_name) rbli_out_key_s(o, "name", c->u.spawn.name);
        break;
    case RBL_CTL_DESPAWN:
        rbli_out_lit(o, "{\"type\":\"despawn\"");
        rbli_out_key_u(o, "entity", c->u.despawn.entity);
        break;
    case RBL_CTL_INPUT:
        rbli_out_lit(o, "{\"type\":\"input\",\"focus\":\"");
        rbli_out_lit(o, c->u.input.focus == RBL_ROLE_HOST ? "host" : "guest");
        rbli_out_lit(o, "\"");
        if (c->u.input.has_reason) rbli_out_key_s(o, "reason", c->u.input.reason);
        break;
    case RBL_CTL_FRAME_CHANNEL:
        rbli_out_lit(o, "{\"type\":\"frame_channel\"");
        rbli_out_key_s(o, "path", c->u.frame_channel.path);
        rbli_out_key_u(o, "width", c->u.frame_channel.width);
        rbli_out_key_u(o, "height", c->u.frame_channel.height);
        rbli_out_key_u(o, "slots", c->u.frame_channel.slots);
        break;
    case RBL_CTL_BYE:
        rbli_out_lit(o, "{\"type\":\"bye\"");
        rbli_out_key_s(o, "reason", c->u.bye.reason);
        break;
    default:
        rbli_out_lit(o, "{\"type\":");
        rbli_out_jstr(o, c->unknown_type);
        break;
    }
    rbli_out_lit(o, "}\n");
}

RBLDEF int rbl_control_format(const rbl_control* c, char* out, size_t cap) {
    rbli_out o;
    if (!c || !out) return RBL_ERR_ARGS;
    o.buf = out; o.len = 0; o.cap = cap; o.growable = 0; o.fail = 0;
    rbli_format_control(&o, c);
    if (o.fail) return o.fail;
    if (o.len > RBL_MAX_LINE) return RBL_ERR_TOO_BIG;
    return (int)o.len;
}

/* ------------------------------------------------------------------------------------------
 * The link: control channel, closing connections, the arena, the entity table
 * ---------------------------------------------------------------------------------------- */

#define RBLI_MAX_CLOSING 8
#define RBLI_ENT_CAP 2048 /* a power of two, twice RBL_MAX_ENTITIES */
#define RBLI_BLK_HDR 32
#define RBLI_BLK_SIZE 65536

typedef struct rbli_blk {
    struct rbli_blk* next;
    size_t cap, used;
} rbli_blk;

/* A control connection: socket, line reader and write queue. */
typedef struct rbli_ctl {
    rbli_sock sock;
    uint8_t* rbuf;
    size_t rstart, rlen, rcap;
    char* wbuf;
    size_t wlen, wcap;
    const char* fatal; /* a static reason once the stream is unusable (line too long, not UTF-8) */
    const char* end;   /* a static reason once the peer closed or the read failed; applies after the last line */
} rbli_ctl;

/* A control connection being closed gracefully (CONTRACT.md 6.1): flush the last bytes (usually a
 * bye), half-close the write side, then read and discard what the peer still sends until it closes
 * too or 200 ms pass, and only then close the socket. */
typedef struct rbli_closing {
    rbli_sock sock;
    char* wbuf;
    size_t wlen, wcap;
    uint64_t deadline;
    int shut;
} rbli_closing;

typedef struct rbli_chunk { rbl_triangle* t; uint32_t n; uint32_t got; } rbli_chunk;

typedef struct rbli_ent { uint64_t time_us; uint32_t id; uint32_t used; } rbli_ent;

typedef enum rbli_state { RBLI_IDLE = 0, RBLI_CONNECTING, RBLI_HANDSHAKE, RBLI_LIVE } rbli_state;

struct rbl_link {
    rbl_role role;
    rbl_config cfg;
    char game[256];
    uint32_t heartbeat_ms;
    rbli_sock udp, listener;
    uint16_t udp_port, control_port;
    int net_up;

    rbli_state state;
    uint64_t next_try;   /* IDLE: a guest connects again at this time */
    uint64_t since;      /* CONNECTING, HANDSHAKE: when it began */
    rbli_sock connecting;
    rbli_ctl ctl;        /* HANDSHAKE, LIVE */
    rbl_peer_info peer;  /* LIVE */
    uint64_t timeout_ms, last_heard, last_beat;

    rbli_closing closing[RBLI_MAX_CLOSING];
    int nclosing;

    uint32_t seq, next_query;
    rbl_role focus;

    /* collision snapshot being sent: a copy of the caller's triangles, chunked as it goes out */
    rbl_triangle* cq_tris;
    size_t cq_n;
    uint32_t cq_query, cq_chunks, cq_next;
    float cq_center[3], cq_radius;

    /* per-session receive state */
    int has_last_camera;
    uint32_t last_camera;
    rbli_ent ent[RBLI_ENT_CAP];
    uint32_t nent;
    int spawn_ignored_logged;
    int has_last_collision;
    uint32_t last_collision;
    int asm_active;
    uint32_t asm_query, asm_count, asm_received;
    float asm_center[3], asm_radius;
    uint64_t asm_tris;
    rbli_chunk* asm_chunks;
    uint64_t last_frame;

    rbl_stats stats;

    rbli_blk* arena;

    rbl_event* ev; /* the caller's array during rbl_link_poll, NULL otherwise */
    size_t ev_n, ev_cap;
    int pending_type; /* an end-of-session event that had no room: delivered by the next poll */
    rbl_reason pending;

    uint8_t rx[2 * RBL_MAX_DATAGRAM];
    uint8_t tx[RBL_MAX_DATAGRAM];
};

/* ---- arena: memory for the strings, poses and triangles of one poll's events ---- */

static void* rbli_arena_alloc(rbl_link* l, size_t n) {
    rbli_blk* b = l->arena;
    if (n > ((size_t)-1) - 64) return NULL;
    n = (n + 15u) & ~(size_t)15u;
    if (!b || b->cap - b->used < n) {
        size_t cap = n > RBLI_BLK_SIZE ? n : RBLI_BLK_SIZE;
        b = (rbli_blk*)REWRITE_LINK_MALLOC(RBLI_BLK_HDR + cap);
        if (!b) return NULL;
        b->next = l->arena;
        b->cap = cap;
        b->used = 0;
        l->arena = b;
    }
    b->used += n;
    return (char*)b + RBLI_BLK_HDR + (b->used - n);
}

static void rbli_arena_reset(rbl_link* l, int keep_one) {
    rbli_blk* b = l->arena;
    rbli_blk* kept = NULL;
    while (b) {
        rbli_blk* next = b->next;
        if (keep_one && !kept && b->cap == RBLI_BLK_SIZE) { kept = b; b->used = 0; b->next = NULL; }
        else REWRITE_LINK_FREE(b);
        b = next;
    }
    l->arena = kept;
}

static char* rbli_arena_str(rbl_link* l, rbl_str s) {
    char* p = (char*)rbli_arena_alloc(l, s.len + 1);
    if (!p) return NULL;
    if (s.len) memcpy(p, s.ptr, s.len);
    p[s.len] = 0;
    return p;
}

/* Copies s into dst[cap] as a C string, cut at a UTF-8 boundary. */
static void rbli_copy_text(char* dst, size_t cap, const char* s, size_t n) {
    size_t k;
    if (cap == 0) return;
    if (n > cap - 1) {
        n = cap - 1;
        /* do not cut a multi-byte sequence in half: back up to its first byte */
        k = n;
        while (k > 0 && ((unsigned char)s[k] & 0xC0) == 0x80) k--;
        if ((unsigned char)s[k] >= 0xC0) n = k;
    }
    memcpy(dst, s, n);
    dst[n] = 0;
}

/* ---- events ---- */

static rbl_event* rbli_ev_push(rbl_link* l, rbl_event_type t) {
    rbl_event* e;
    if (!l->ev || l->ev_n >= l->ev_cap) return NULL;
    e = &l->ev[l->ev_n++];
    memset(e, 0, sizeof *e);
    e->type = t;
    return e;
}

/* The input loops stop one short of the array, so the one event that can end a session in the same
 * poll always has its place. */
static int rbli_room(const rbl_link* l) { return l->ev && l->ev_n + 1 < l->ev_cap; }

static void rbli_report_end(rbl_link* l, int was_live, const char* reason) {
    rbl_event_type t = was_live ? RBL_EV_LOST : RBL_EV_HANDSHAKE_FAILED;
    rbl_event* e = rbli_ev_push(l, t);
    if (e) {
        rbli_copy_text(e->u.lost.text, sizeof e->u.lost.text, reason, strlen(reason));
    } else {
        l->pending_type = (int)t;
        rbli_copy_text(l->pending.text, sizeof l->pending.text, reason, strlen(reason));
    }
}

/* ---- entity table (open addressing, backward-shift deletion) ---- */

static unsigned rbli_ent_hash(uint32_t id) { return (unsigned)((id * 2654435761u) >> 21) & (RBLI_ENT_CAP - 1); }

static int rbli_ent_find(const rbl_link* l, uint32_t id) {
    unsigned i = rbli_ent_hash(id);
    while (l->ent[i].used) {
        if (l->ent[i].id == id) return (int)i;
        i = (i + 1) & (RBLI_ENT_CAP - 1);
    }
    return -1;
}
static void rbli_ent_insert(rbl_link* l, uint32_t id) {
    unsigned i = rbli_ent_hash(id);
    while (l->ent[i].used) i = (i + 1) & (RBLI_ENT_CAP - 1);
    l->ent[i].used = 1;
    l->ent[i].id = id;
    l->ent[i].time_us = 0;
    l->nent++;
}
static void rbli_ent_remove(rbl_link* l, int idx) {
    unsigned i = (unsigned)idx, j = i;
    for (;;) {
        unsigned k;
        j = (j + 1) & (RBLI_ENT_CAP - 1);
        if (!l->ent[j].used) break;
        k = rbli_ent_hash(l->ent[j].id);
        if (i <= j ? (i < k && k <= j) : (i < k || k <= j)) continue;
        l->ent[i] = l->ent[j];
        i = j;
    }
    l->ent[i].used = 0;
    l->nent--;
}

/* ---- control channel ---- */

static void rbli_ctl_init(rbli_ctl* c, rbli_sock s) {
    memset(c, 0, sizeof *c);
    c->sock = s;
}

static void rbli_ctl_free(rbli_ctl* c) {
    if (c->rbuf) REWRITE_LINK_FREE(c->rbuf);
    if (c->wbuf) REWRITE_LINK_FREE(c->wbuf);
    memset(c, 0, sizeof *c);
    c->sock = RBLI_BAD_SOCK;
}

/* Writes as much as the socket takes without blocking; returns an error reason or NULL. */
static const char* rbli_flush(rbli_sock s, char* wbuf, size_t* wlen) {
    size_t off = 0;
    while (off < *wlen) {
        int n = rbli_tcp_send(s, wbuf + off, *wlen - off);
        if (n == -1) break;
        if (n == -2) return "control write failed";
        if (n == 0) break;
        off += (size_t)n;
    }
    if (off > 0) {
        memmove(wbuf, wbuf + off, *wlen - off);
        *wlen -= off;
    }
    if (*wlen > RBL_MAX_PENDING_CONTROL) return "peer stopped reading the control channel";
    return NULL;
}

static const char* rbli_ctl_flush(rbli_ctl* c) { return rbli_flush(c->sock, c->wbuf, &c->wlen); }

/* Reads what is there. A closed or failed stream is remembered in c->end and applies after the
 * last complete line has been handled. */
static void rbli_ctl_fill(rbli_ctl* c) {
    if (c->end || c->fatal) return;
    if (c->rstart > 0) {
        memmove(c->rbuf, c->rbuf + c->rstart, c->rlen - c->rstart);
        c->rlen -= c->rstart;
        c->rstart = 0;
    }
    for (;;) {
        int n;
        if (c->rlen > 4 * (size_t)RBL_MAX_LINE) break; /* let the caller process what it has first */
        if (c->rcap - c->rlen < 8192) {
            size_t nc = c->rcap ? c->rcap * 2 : 16384;
            uint8_t* nb = (uint8_t*)REWRITE_LINK_REALLOC(c->rbuf, nc);
            if (!nb) { c->end = "out of memory"; break; }
            c->rbuf = nb;
            c->rcap = nc;
        }
        n = rbli_tcp_recv(c->sock, c->rbuf + c->rlen, 8192);
        if (n > 0) { c->rlen += (size_t)n; continue; }
        if (n == 0) { c->end = "connection closed"; break; }
        if (n == -1) break;
        c->end = "control read failed";
        break;
    }
}

/* 1: *line and *len are the next complete line (with its '\n'), editable in place; 0: no complete line;
 * -1: the stream is unusable, c->fatal says why. */
static int rbli_ctl_next_line(rbli_ctl* c, char** line, size_t* len) {
    uint8_t* start = c->rbuf ? c->rbuf + c->rstart : NULL;
    size_t avail = c->rlen - c->rstart, n;
    uint8_t* nl = avail ? (uint8_t*)memchr(start, '\n', avail) : NULL;
    if (!nl) {
        if (avail >= RBL_MAX_LINE) { c->fatal = "control line too long"; return -1; }
        return 0;
    }
    n = (size_t)(nl + 1 - start);
    if (n > RBL_MAX_LINE) { c->fatal = "control line too long"; return -1; }
    if (!rbli_utf8_valid(start, n)) { c->fatal = "control line is not UTF-8"; return -1; }
    *line = (char*)start;
    *len = n;
    c->rstart += n;
    return 1;
}

/* Appends a control message to the queue. Rolls back and fails if it would exceed one line. */
static int rbli_ctl_queue(rbli_ctl* c, const rbl_control* m) {
    rbli_out o;
    size_t before = c->wlen;
    o.buf = c->wbuf; o.len = c->wlen; o.cap = c->wcap; o.growable = 1; o.fail = 0;
    rbli_format_control(&o, m);
    c->wbuf = o.buf; c->wcap = o.cap;
    if (o.fail) { c->wlen = before; return o.fail; }
    if (o.len - before > RBL_MAX_LINE) { c->wlen = before; return RBL_ERR_TOO_BIG; }
    c->wlen = o.len;
    return RBL_OK;
}

/* ---- closing connections ---- */

static void rbli_closing_free(rbli_closing* c) {
    rbli_sock_close(c->sock);
    if (c->wbuf) REWRITE_LINK_FREE(c->wbuf);
    c->wbuf = NULL;
    c->sock = RBLI_BAD_SOCK;
}

/* Makes progress without blocking; 0 once the socket is closed (and freed). */
static int rbli_closing_pump(rbli_closing* c, uint64_t now) {
    int i;
    if (now >= c->deadline || rbli_flush(c->sock, c->wbuf, &c->wlen) != NULL) { rbli_closing_free(c); return 0; }
    if (!c->shut) {
        if (c->wlen > 0) return 1;
        rbli_shutdown_write(c->sock);
        c->shut = 1;
    }
    for (i = 0; i < 64; i++) {
        uint8_t junk[4096];
        int n = rbli_tcp_recv(c->sock, junk, sizeof junk);
        if (n == 0 || n == -2) { rbli_closing_free(c); return 0; }
        if (n == -1) return 1;
    }
    return 1;
}

/* Takes over a socket and its unsent bytes, optionally adds a bye, and closes it gracefully. */
static void rbli_retire(rbl_link* l, rbli_sock sock, char* wbuf, size_t wlen, size_t wcap, const char* bye, uint64_t now) {
    rbli_closing c;
    c.sock = sock; c.wbuf = wbuf; c.wlen = wlen; c.wcap = wcap; c.deadline = now + RBL_LINGER_MS; c.shut = 0;
    if (bye) {
        rbl_control m;
        rbli_ctl tmp;
        memset(&m, 0, sizeof m);
        m.type = RBL_CTL_BYE;
        m.u.bye.reason.ptr = bye;
        m.u.bye.reason.len = strlen(bye);
        memset(&tmp, 0, sizeof tmp);
        tmp.wbuf = c.wbuf; tmp.wlen = c.wlen; tmp.wcap = c.wcap;
        rbli_ctl_queue(&tmp, &m); /* if it cannot be queued (memory), the connection just closes */
        c.wbuf = tmp.wbuf; c.wlen = tmp.wlen; c.wcap = tmp.wcap;
        l->stats.lines_sent++;
    }
    if (!rbli_closing_pump(&c, now)) return;
    if (l->nclosing == RBLI_MAX_CLOSING) { /* too many at once: the oldest is closed the hard way */
        rbli_closing_free(&l->closing[0]);
        memmove(&l->closing[0], &l->closing[1], sizeof(rbli_closing) * (RBLI_MAX_CLOSING - 1));
        l->nclosing--;
    }
    l->closing[l->nclosing++] = c;
}

static void rbli_retire_ctl(rbl_link* l, const char* bye, uint64_t now) {
    rbli_ctl* c = &l->ctl;
    rbli_sock s = c->sock;
    char* wbuf = c->wbuf;
    size_t wlen = c->wlen, wcap = c->wcap;
    c->wbuf = NULL;
    if (c->rbuf) REWRITE_LINK_FREE(c->rbuf);
    memset(c, 0, sizeof *c);
    c->sock = RBLI_BAD_SOCK;
    rbli_retire(l, s, wbuf, wlen, wcap, bye, now);
}

/* ---- session state ---- */

static void rbli_cq_clear(rbl_link* l) {
    if (l->cq_tris) REWRITE_LINK_FREE(l->cq_tris);
    l->cq_tris = NULL;
    l->cq_n = 0;
    l->cq_chunks = l->cq_next = 0;
}

static void rbli_asm_clear(rbl_link* l) {
    if (l->asm_chunks) {
        uint32_t i;
        for (i = 0; i < l->asm_count; i++) if (l->asm_chunks[i].t) REWRITE_LINK_FREE(l->asm_chunks[i].t);
        REWRITE_LINK_FREE(l->asm_chunks);
    }
    l->asm_chunks = NULL;
    l->asm_active = 0;
    l->asm_count = l->asm_received = 0;
    l->asm_tris = 0;
}

static void rbli_reset_session(rbl_link* l) {
    l->focus = RBL_ROLE_HOST;
    l->has_last_camera = 0;
    memset(l->ent, 0, sizeof l->ent);
    l->nent = 0;
    l->spawn_ignored_logged = 0;
    rbli_cq_clear(l);
    l->has_last_collision = 0;
    rbli_asm_clear(l);
    l->last_frame = 0;
}

/* Ends the current connection, reporting it as lost (or as a failed handshake). */
static void rbli_end(rbl_link* l, const char* reason, int bye) {
    uint64_t now = rbli_mono_ms();
    int was_live = l->state == RBLI_LIVE;
    int had_ctl = l->state == RBLI_HANDSHAKE || l->state == RBLI_LIVE;
    char text[200];
    rbli_copy_text(text, sizeof text, reason, strlen(reason));
    l->state = RBLI_IDLE;
    l->next_try = l->role == RBL_ROLE_GUEST ? now + l->cfg.reconnect_ms : now;
    if (had_ctl) rbli_retire_ctl(l, bye ? text : NULL, now);
    rbli_reset_session(l);
    rbli_report_end(l, was_live, text);
}

/* ---- sending ---- */

static int rbli_live_header(rbl_link* l, rbl_header* h) {
    if (l->state != RBLI_LIVE) return RBL_ERR_NOT_CONNECTED;
    memset(h, 0, sizeof *h);
    h->magic = RBL_MAGIC;
    h->version = (uint16_t)l->peer.protocol;
    h->session = l->peer.session;
    h->seq = l->seq;
    h->time_us = rbl_now_us();
    return RBL_OK;
}

/* Sends l->tx[0..len) to the peer and takes the next seq. Returns the seq used. */
static uint32_t rbli_emit(rbl_link* l, int len) {
    uint32_t s = l->seq++;
    if (rbli_udp_send(l->udp, l->tx, (size_t)len, l->peer.udp_port)) l->stats.datagrams_sent++;
    else l->stats.send_dropped++; /* refused or would block: the next tick replaces it */
    return s;
}

static void rbli_send_heartbeat(rbl_link* l) {
    rbl_header h;
    int n;
    if (rbli_live_header(l, &h) != RBL_OK) return;
    n = rbl_wire_encode_heartbeat(&h, l->tx, sizeof l->tx);
    if (n > 0) rbli_emit(l, n);
}

static int rbli_send_control(rbl_link* l, const rbl_control* m) {
    int r;
    const char* err;
    if (l->state != RBLI_LIVE) return RBL_ERR_NOT_CONNECTED;
    r = rbli_ctl_queue(&l->ctl, m);
    if (r != RBL_OK) return r;
    l->stats.lines_sent++;
    err = rbli_ctl_flush(&l->ctl);
    if (err) {
        rbli_end(l, err, 0);
        return RBL_ERR_NOT_CONNECTED;
    }
    return RBL_OK;
}

static rbl_str rbli_cstr(const char* s) {
    rbl_str r;
    r.ptr = s ? s : "";
    r.len = s ? strlen(s) : 0;
    return r;
}

static void rbli_hello_for(rbl_link* l, rbl_control* m, uint32_t protocol, int has_session, uint32_t session) {
    memset(m, 0, sizeof *m);
    m->type = RBL_CTL_HELLO;
    m->u.hello.role = l->role;
    m->u.hello.game = rbli_cstr(l->game);
    m->u.hello.protocol = protocol;
    m->u.hello.min_protocol = RBL_MIN_VERSION;
    m->u.hello.udp_port = l->udp_port;
    m->u.hello.heartbeat_ms = l->heartbeat_ms;
    m->u.hello.timeout_ms = l->cfg.timeout_ms;
    m->u.hello.has_session = has_session;
    m->u.hello.session = session;
}

/* ---- polling: connect ---- */

static void rbli_abort_connect(rbl_link* l, uint64_t now) {
    if (l->connecting != RBLI_BAD_SOCK) rbli_sock_close(l->connecting);
    l->connecting = RBLI_BAD_SOCK;
    l->state = RBLI_IDLE;
    l->next_try = now + l->cfg.reconnect_ms;
}

static void rbli_poll_connect(rbl_link* l, uint64_t now) {
    if (l->listener != RBLI_BAD_SOCK) {
        for (;;) {
            rbli_sock s = rbli_tcp_accept(l->listener);
            if (s == RBLI_BAD_SOCK) break;
            if (l->state == RBLI_IDLE) {
                rbli_ctl_init(&l->ctl, s);
                l->state = RBLI_HANDSHAKE;
                l->since = now;
            } else {
                rbli_retire(l, s, NULL, 0, 0, "busy", now); /* one guest per host */
            }
        }
        return;
    }
    if (l->state == RBLI_IDLE && now >= l->next_try) {
        rbli_sock s = rbli_tcp_connect_start(l->cfg.control_port);
        if (s == RBLI_BAD_SOCK) { l->next_try = now + l->cfg.reconnect_ms; return; }
        l->connecting = s;
        l->state = RBLI_CONNECTING;
        l->since = now;
    } else if (l->state == RBLI_CONNECTING) {
        int r = rbli_tcp_connect_check(l->connecting);
        if (r == 1) {
            rbl_control m;
            rbli_ctl_init(&l->ctl, l->connecting);
            l->connecting = RBLI_BAD_SOCK;
            rbli_nodelay(l->ctl.sock);
            rbli_hello_for(l, &m, RBL_VERSION, 0, 0);
            rbli_ctl_queue(&l->ctl, &m);
            l->stats.lines_sent++;
            rbli_ctl_flush(&l->ctl);
            l->state = RBLI_HANDSHAKE;
            l->since = now;
        } else if (r < 0 || now - l->since > 1000) {
            rbli_abort_connect(l, now);
        }
    }
}

/* ---- polling: control lines ---- */

static int rbli_on_hello(rbl_link* l, const rbl_control* c, uint64_t now, char* reason, size_t rcap) {
    const rbl_str* g = &c->u.hello.game;
    rbl_role expected = l->role == RBL_ROLE_HOST ? RBL_ROLE_GUEST : RBL_ROLE_HOST;
    uint32_t protocol, session;
    uint64_t timeout;
    rbl_control reply;
    if (c->u.hello.role != expected) {
        snprintf(reason, rcap, "peer claims role %s", c->u.hello.role == RBL_ROLE_HOST ? "host" : "guest");
        return 1;
    }
    if (c->u.hello.udp_port == 0) { snprintf(reason, rcap, "peer announced udp_port 0"); return 1; }
    if (c->u.hello.heartbeat_ms < RBL_MIN_HEARTBEAT_MS || c->u.hello.heartbeat_ms > RBL_MAX_HEARTBEAT_MS) {
        snprintf(reason, rcap, "peer announced heartbeat_ms %lu, outside %d..%d", (unsigned long)c->u.hello.heartbeat_ms,
                 RBL_MIN_HEARTBEAT_MS, RBL_MAX_HEARTBEAT_MS);
        return 1;
    }
    if (l->role == RBL_ROLE_HOST) {
        int v = rbl_negotiate(RBL_MIN_VERSION, RBL_VERSION, (int)c->u.hello.min_protocol, (int)c->u.hello.protocol);
        uint8_t r[4];
        if (v < 0) {
            snprintf(reason, rcap, "unsupported protocol: peer %lu..%lu, ours %d..%d", (unsigned long)c->u.hello.min_protocol,
                     (unsigned long)c->u.hello.protocol, RBL_MIN_VERSION, RBL_VERSION);
            return 1;
        }
        protocol = (uint32_t)v;
        do { rbli_random(r, 4); session = rbli_get32(r); } while (session == 0);
    } else {
        if (c->u.hello.protocol < RBL_MIN_VERSION || c->u.hello.protocol > RBL_VERSION) {
            snprintf(reason, rcap, "host chose unsupported protocol %lu", (unsigned long)c->u.hello.protocol);
            return 1;
        }
        if (!c->u.hello.has_session || c->u.hello.session == 0) {
            snprintf(reason, rcap, "host hello without a nonzero session");
            return 1;
        }
        protocol = c->u.hello.protocol;
        session = c->u.hello.session;
    }
    timeout = l->cfg.timeout_ms;
    if ((uint64_t)4 * c->u.hello.heartbeat_ms > timeout) timeout = (uint64_t)4 * c->u.hello.heartbeat_ms;
    if (l->role == RBL_ROLE_HOST) {
        rbli_hello_for(l, &reply, protocol, 1, session);
        rbli_ctl_queue(&l->ctl, &reply);
        l->stats.lines_sent++;
        rbli_ctl_flush(&l->ctl);
    }
    rbli_reset_session(l);
    memset(&l->peer, 0, sizeof l->peer);
    rbli_copy_text(l->peer.game, sizeof l->peer.game, g->ptr, g->len);
    l->peer.role = c->u.hello.role;
    l->peer.protocol = protocol;
    l->peer.session = session;
    l->peer.udp_port = (uint16_t)c->u.hello.udp_port;
    l->peer.heartbeat_ms = c->u.hello.heartbeat_ms;
    l->peer.timeout_ms = (uint32_t)(timeout > RBLI_U32_MAX ? RBLI_U32_MAX : timeout);
    l->timeout_ms = timeout;
    l->state = RBLI_LIVE;
    l->last_heard = now;
    l->last_beat = now;
    {
        rbl_event* e = rbli_ev_push(l, RBL_EV_CONNECTED);
        if (e) e->u.connected = l->peer;
    }
    rbli_send_heartbeat(l);
    return 0;
}

/* Handles one control line. Returns 0, or 1 to end the session (reason filled in, *bye set if a
 * bye should be sent). */
static int rbli_on_line(rbl_link* l, char* line, size_t len, uint64_t now, char* reason, size_t rcap, int* bye) {
    rbl_control m;
    const char* why = "bad control line";
    *bye = 0;
    if (rbl_control_parse(line, len, &m, &why) != RBL_OK) {
        snprintf(reason, rcap, "protocol error: %s", why);
        *bye = 1;
        return 1;
    }
    if (l->state == RBLI_LIVE) l->last_heard = now;
    if (m.type == RBL_CTL_BYE) {
        char t[160];
        rbli_copy_text(t, sizeof t, m.u.bye.reason.ptr, m.u.bye.reason.len);
        snprintf(reason, rcap, "peer said bye: %s", t);
        return 1;
    }
    if (l->state == RBLI_HANDSHAKE) {
        if (m.type != RBL_CTL_HELLO) { snprintf(reason, rcap, "expected hello"); *bye = 1; return 1; }
        if (rbli_on_hello(l, &m, now, reason, rcap) != 0) { *bye = 1; return 1; }
        return 0;
    }
    switch (m.type) {
    case RBL_CTL_HELLO:
        snprintf(reason, rcap, "second hello");
        *bye = 1;
        return 1;
    case RBL_CTL_SPAWN: {
        /* A second spawn of a live id replaces the first; a new id needs a free place. */
        if (rbli_ent_find(l, m.u.spawn.entity) < 0) {
            if (l->nent >= RBL_MAX_ENTITIES) {
                l->stats.spawn_ignored++;
                if (!l->spawn_ignored_logged) {
                    l->spawn_ignored_logged = 1;
                    fprintf(stderr, "rewrite_link: the peer has %d live entities; further spawns are ignored (entity %lu and later ones)\n",
                            RBL_MAX_ENTITIES, (unsigned long)m.u.spawn.entity);
                }
                return 0;
            }
            rbli_ent_insert(l, m.u.spawn.entity);
        }
        {
            char* provider = rbli_arena_str(l, m.u.spawn.provider);
            char* asset = rbli_arena_str(l, m.u.spawn.asset);
            char* name = m.u.spawn.has_name ? rbli_arena_str(l, m.u.spawn.name) : NULL;
            rbl_event* e;
            if (!provider || !asset || (m.u.spawn.has_name && !name)) { l->stats.dropped++; return 0; }
            e = rbli_ev_push(l, RBL_EV_SPAWN);
            if (e) {
                e->u.spawn.entity = m.u.spawn.entity;
                e->u.spawn.provider = provider;
                e->u.spawn.asset = asset;
                e->u.spawn.name = name;
            }
        }
        return 0;
    }
    case RBL_CTL_DESPAWN: {
        int idx = rbli_ent_find(l, m.u.despawn.entity);
        if (idx >= 0) {
            rbl_event* e;
            rbli_ent_remove(l, idx);
            e = rbli_ev_push(l, RBL_EV_DESPAWN);
            if (e) e->u.despawn.entity = m.u.despawn.entity;
        }
        return 0;
    }
    case RBL_CTL_INPUT: {
        int requested = l->role == RBL_ROLE_HOST;
        rbl_event* e;
        if (!requested) l->focus = m.u.input.focus;
        e = rbli_ev_push(l, RBL_EV_FOCUS);
        if (e) { e->u.focus.focus = m.u.input.focus; e->u.focus.requested = requested; }
        return 0;
    }
    case RBL_CTL_FRAME_CHANNEL: {
        /* A new pixel file numbers its frames from 1 again (CONTRACT.md 8). */
        char* path = rbli_arena_str(l, m.u.frame_channel.path);
        rbl_event* e;
        l->last_frame = 0;
        if (!path) { l->stats.dropped++; return 0; }
        e = rbli_ev_push(l, RBL_EV_FRAME_CHANNEL);
        if (e) {
            e->u.frame_channel.path = strlen(path) == m.u.frame_channel.path.len ? path : NULL;
            e->u.frame_channel.width = m.u.frame_channel.width;
            e->u.frame_channel.height = m.u.frame_channel.height;
            e->u.frame_channel.slots = m.u.frame_channel.slots;
        }
        return 0;
    }
    default:
        return 0; /* an unknown type is ignored; bye was handled above */
    }
}

static void rbli_poll_control(rbl_link* l, uint64_t now) {
    rbli_ctl* c;
    const char* err;
    if (l->state != RBLI_HANDSHAKE && l->state != RBLI_LIVE) return;
    c = &l->ctl;
    err = rbli_ctl_flush(c);
    if (err) { rbli_end(l, err, 0); return; }
    rbli_ctl_fill(c);
    for (;;) {
        char* line;
        size_t len;
        char reason[200];
        int bye, r;
        if (!rbli_room(l)) return; /* the caller's array is full: the rest waits for the next poll */
        r = rbli_ctl_next_line(c, &line, &len);
        if (r < 0) { rbli_end(l, c->fatal, 0); return; }
        if (r == 0) break;
        l->stats.lines_received++;
        if (rbli_on_line(l, line, len, now, reason, sizeof reason, &bye) != 0) {
            rbli_end(l, reason, bye);
            return;
        }
        if (l->state == RBLI_IDLE) return;
    }
    /* A bye was handled above (it ends the session first), so an end of stream or a reset that
     * follows a bye never reaches this point as a failure. */
    if (c->end) rbli_end(l, c->end, 0);
}

/* ---- polling: datagrams ---- */

static void rbli_on_collision(rbl_link* l, const rbl_decoded* d) {
    const rbl_collision* c = &d->u.collision;
    rbli_chunk* ch;
    uint32_t i;
    if (l->has_last_collision && !rbl_seq_newer(c->query, l->last_collision)) { l->stats.stale++; return; }
    if (l->asm_active && l->asm_query == c->query) {
        if (l->asm_count != c->chunk_count) { l->stats.malformed++; return; }
    } else if (l->asm_active && rbl_seq_newer(l->asm_query, c->query)) {
        l->stats.stale++;
        return;
    } else {
        rbli_asm_clear(l); /* a newer query starts: the incomplete older one is discarded */
        l->asm_chunks = (rbli_chunk*)REWRITE_LINK_MALLOC(sizeof(rbli_chunk) * c->chunk_count);
        if (!l->asm_chunks) { l->stats.dropped++; return; }
        memset(l->asm_chunks, 0, sizeof(rbli_chunk) * c->chunk_count);
        l->asm_active = 1;
        l->asm_query = c->query;
        l->asm_count = c->chunk_count;
        l->asm_received = 0;
        l->asm_tris = 0;
        l->asm_center[0] = c->center[0]; l->asm_center[1] = c->center[1]; l->asm_center[2] = c->center[2];
        l->asm_radius = c->radius;
    }
    ch = &l->asm_chunks[c->chunk_index];
    if (!ch->got) {
        if (l->asm_tris + c->triangle_count > l->cfg.max_collision_triangles) {
            l->stats.malformed++; /* more than this receiver accepts: the snapshot is dropped, */
            rbli_asm_clear(l);
            l->has_last_collision = 1; /* and so are the rest of its chunks (they now look stale) */
            l->last_collision = c->query;
            return;
        }
        if (c->triangle_count) {
            ch->t = (rbl_triangle*)REWRITE_LINK_MALLOC(sizeof(rbl_triangle) * c->triangle_count);
            if (!ch->t) { l->stats.dropped++; rbli_asm_clear(l); return; }
            for (i = 0; i < c->triangle_count; i++) rbl_wire_triangle(d, i, &ch->t[i]);
        }
        ch->n = c->triangle_count;
        ch->got = 1;
        l->asm_tris += c->triangle_count;
        l->asm_received++;
    }
    if (l->asm_received == l->asm_count) {
        rbl_triangle* all = NULL;
        size_t at = 0;
        if (l->asm_tris) {
            all = (rbl_triangle*)rbli_arena_alloc(l, sizeof(rbl_triangle) * (size_t)l->asm_tris);
            if (!all) { l->stats.dropped++; rbli_asm_clear(l); return; }
            for (i = 0; i < l->asm_count; i++) {
                if (l->asm_chunks[i].n) memcpy(all + at, l->asm_chunks[i].t, sizeof(rbl_triangle) * l->asm_chunks[i].n);
                at += l->asm_chunks[i].n;
            }
        }
        {
            rbl_event* e = rbli_ev_push(l, RBL_EV_COLLISION);
            if (e) {
                e->u.collision.query = l->asm_query;
                e->u.collision.center[0] = l->asm_center[0]; e->u.collision.center[1] = l->asm_center[1]; e->u.collision.center[2] = l->asm_center[2];
                e->u.collision.radius = l->asm_radius;
                e->u.collision.triangles = all;
                e->u.collision.triangle_count = (uint32_t)l->asm_tris;
            }
        }
        l->has_last_collision = 1;
        l->last_collision = l->asm_query;
        rbli_asm_clear(l);
    }
}

static void rbli_on_datagram(rbl_link* l, const rbl_decoded* d) {
    switch (d->header.kind) {
    case RBL_CAMERA: {
        rbl_event* e;
        if (l->has_last_camera && !rbl_seq_newer(d->header.seq, l->last_camera)) { l->stats.stale++; return; }
        l->has_last_camera = 1;
        l->last_camera = d->header.seq;
        e = rbli_ev_push(l, RBL_EV_CAMERA);
        if (e) { e->u.camera.header = d->header; e->u.camera.camera = d->u.camera; }
        break;
    }
    case RBL_POSE: {
        uint32_t i, nf = 0;
        rbl_pose* fresh = (rbl_pose*)rbli_arena_alloc(l, sizeof(rbl_pose) * d->u.poses.count);
        if (!fresh) { l->stats.dropped++; return; }
        for (i = 0; i < d->u.poses.count; i++) {
            rbl_pose p;
            int idx;
            rbl_wire_pose(d, i, &p);
            idx = rbli_ent_find(l, p.entity);
            if (idx < 0) { l->stats.unknown_entity++; continue; } /* never spawned (or despawned): no state is kept */
            if (p.time_us >= l->ent[idx].time_us) {
                l->ent[idx].time_us = p.time_us;
                fresh[nf++] = p;
            } else {
                l->stats.stale++;
            }
        }
        if (nf) {
            rbl_event* e = rbli_ev_push(l, RBL_EV_POSES);
            if (e) { e->u.poses.header = d->header; e->u.poses.poses = fresh; e->u.poses.count = nf; }
        }
        break;
    }
    case RBL_COLLISION:
        rbli_on_collision(l, d);
        break;
    case RBL_FRAME: {
        rbl_event* e;
        if (d->u.frame.frame <= l->last_frame) { l->stats.stale++; return; }
        l->last_frame = d->u.frame.frame;
        e = rbli_ev_push(l, RBL_EV_FRAME);
        if (e) { e->u.frame.header = d->header; e->u.frame.frame = d->u.frame; }
        break;
    }
    default:
        break; /* a Heartbeat or an unknown kind only counts as the peer being alive (done by the caller) */
    }
}

static void rbli_poll_udp(rbl_link* l, uint64_t now) {
    int n;
    for (n = 0; n < 4096; n++) {
        uint32_t addr = 0;
        uint16_t port = 0;
        int len;
        rbl_decoded d;
        if (!rbli_room(l)) break; /* leave the rest in the socket buffer */
        len = rbli_udp_recv(l->udp, l->rx, sizeof l->rx, &addr, &port);
        if (len == -1) break;
        if (len == -2) continue; /* e.g. ICMP port-unreachable from an earlier send */
        if (l->state != RBLI_LIVE) { l->stats.foreign++; continue; }
        if (!rbli_is_peer_addr(addr, port, l->peer.udp_port)) { l->stats.foreign++; continue; }
        if ((size_t)len < RBL_HEADER_LEN || rbli_get32(l->rx) != RBL_MAGIC || rbli_get16(l->rx + 4) != RBL_VERSION) {
            l->stats.malformed++;
            continue;
        }
        if (rbli_get32(l->rx + 8) != l->peer.session || rbli_get16(l->rx + 4) != l->peer.protocol) {
            l->stats.foreign++;
            continue;
        }
        if (rbl_wire_decode(l->rx, (size_t)len, &d) != RBL_OK) { l->stats.malformed++; continue; }
        l->last_heard = now;
        l->stats.datagrams_received++;
        rbli_on_datagram(l, &d);
    }
}

/* Sends up to 32 queued collision chunks. */
static void rbli_pump_collision(rbl_link* l) {
    int i;
    for (i = 0; i < RBL_COLLISION_PER_POLL && l->cq_next < l->cq_chunks; i++) {
        rbl_header h;
        rbl_collision c;
        size_t from = (size_t)l->cq_next * RBL_MAX_TRIANGLES, to = from + RBL_MAX_TRIANGLES, n;
        int len;
        if (to > l->cq_n) to = l->cq_n;
        n = to > from ? to - from : 0;
        if (rbli_live_header(l, &h) != RBL_OK) { rbli_cq_clear(l); return; }
        memset(&c, 0, sizeof c);
        c.query = l->cq_query;
        c.chunk_index = (uint16_t)l->cq_next;
        c.chunk_count = (uint16_t)l->cq_chunks;
        c.center[0] = l->cq_center[0]; c.center[1] = l->cq_center[1]; c.center[2] = l->cq_center[2];
        c.radius = l->cq_radius;
        len = rbl_wire_encode_collision(&h, &c, n ? l->cq_tris + from : NULL, n, l->tx, sizeof l->tx);
        if (len <= 0) { rbli_cq_clear(l); return; }
        rbli_emit(l, len);
        l->cq_next++;
    }
    if (l->cq_chunks && l->cq_next >= l->cq_chunks) rbli_cq_clear(l);
}

static void rbli_poll_timers(rbl_link* l, uint64_t now) {
    if (l->state == RBLI_HANDSHAKE && now - l->since > l->cfg.timeout_ms) {
        rbli_end(l, "no hello before the timeout", 1);
    } else if (l->state == RBLI_LIVE) {
        uint64_t silent = now - l->last_heard;
        if (silent > l->timeout_ms) {
            char t[64];
            snprintf(t, sizeof t, "watchdog: nothing heard for %lu ms", (unsigned long)silent);
            rbli_end(l, t, 1);
            return;
        }
        if (now - l->last_beat >= l->heartbeat_ms) {
            l->last_beat = now;
            rbli_send_heartbeat(l);
        }
    }
}

/* ------------------------------------------------------------------------------------------
 * The link: public functions
 * ---------------------------------------------------------------------------------------- */

RBLDEF void rbl_config_init(rbl_config* cfg, const char* game) {
    memset(cfg, 0, sizeof *cfg);
    cfg->game = game;
    cfg->control_port = RBL_DEFAULT_CONTROL_PORT;
    cfg->udp_port = 0;
    cfg->heartbeat_ms = 250;
    cfg->timeout_ms = 2000;
    cfg->reconnect_ms = 500;
    cfg->max_collision_triangles = 2097152;
}

RBLDEF int rbl_os_error(void) { return rbli_os_errno(); }

static void rbli_link_destroy(rbl_link* l) {
    int i;
    if (l->connecting != RBLI_BAD_SOCK) rbli_sock_close(l->connecting);
    if (l->ctl.sock != RBLI_BAD_SOCK) rbli_sock_close(l->ctl.sock);
    rbli_ctl_free(&l->ctl);
    for (i = 0; i < l->nclosing; i++) rbli_closing_free(&l->closing[i]);
    l->nclosing = 0;
    rbli_cq_clear(l);
    rbli_asm_clear(l);
    rbli_arena_reset(l, 0);
    if (l->udp != RBLI_BAD_SOCK) rbli_sock_close(l->udp);
    if (l->listener != RBLI_BAD_SOCK) rbli_sock_close(l->listener);
    if (l->net_up) rbli_net_down();
    REWRITE_LINK_FREE(l);
}

static int rbli_link_create(const rbl_config* cfg, rbl_role role, rbl_link** out) {
    rbl_link* l;
    int saved;
    if (!out) return RBL_ERR_ARGS;
    *out = NULL;
    if (!cfg) return RBL_ERR_ARGS;
    l = (rbl_link*)REWRITE_LINK_MALLOC(sizeof *l);
    if (!l) return RBL_ERR_NOMEM;
    memset(l, 0, sizeof *l);
    l->role = role;
    l->cfg = *cfg;
    l->udp = l->listener = l->connecting = RBLI_BAD_SOCK;
    l->ctl.sock = RBLI_BAD_SOCK;
    l->state = RBLI_IDLE;
    l->next_try = rbli_mono_ms();
    l->next_query = 1;
    l->focus = RBL_ROLE_HOST;
    if (rbli_net_up() != 0) { REWRITE_LINK_FREE(l); return RBL_ERR_IO; }
    l->net_up = 1;
    {   const char* g = cfg->game ? cfg->game : "";
        rbli_copy_text(l->game, sizeof l->game, g, strlen(g));
        l->cfg.game = l->game;
    }
    if (l->cfg.heartbeat_ms == 0) l->cfg.heartbeat_ms = 250;
    if (l->cfg.timeout_ms == 0) l->cfg.timeout_ms = 2000;
    if (l->cfg.reconnect_ms == 0) l->cfg.reconnect_ms = 500;
    if (l->cfg.max_collision_triangles == 0) l->cfg.max_collision_triangles = 2097152;
    l->heartbeat_ms = l->cfg.heartbeat_ms < RBL_MIN_HEARTBEAT_MS ? RBL_MIN_HEARTBEAT_MS
                      : l->cfg.heartbeat_ms > RBL_MAX_HEARTBEAT_MS ? RBL_MAX_HEARTBEAT_MS : l->cfg.heartbeat_ms;
    l->udp = rbli_udp_open(cfg->udp_port, &l->udp_port);
    if (l->udp != RBLI_BAD_SOCK && role == RBL_ROLE_HOST) {
        l->listener = rbli_tcp_listen(cfg->control_port, &l->control_port);
    } else {
        l->control_port = cfg->control_port;
    }
    if (l->udp == RBLI_BAD_SOCK || (role == RBL_ROLE_HOST && l->listener == RBLI_BAD_SOCK)) {
        saved = rbli_os_errno();
        rbli_link_destroy(l);
#if RBLI_WINDOWS
        WSASetLastError(saved);
#else
        errno = saved;
#endif
        return RBL_ERR_IO;
    }
    *out = l;
    return RBL_OK;
}

RBLDEF int rbl_link_host(const rbl_config* cfg, rbl_link** out) { return rbli_link_create(cfg, RBL_ROLE_HOST, out); }
RBLDEF int rbl_link_guest(const rbl_config* cfg, rbl_link** out) { return rbli_link_create(cfg, RBL_ROLE_GUEST, out); }

RBLDEF int rbl_link_poll(rbl_link* l, rbl_event* events, size_t cap) {
    uint64_t now;
    int i;
    size_t n;
    if (!l || !events || cap < RBL_MIN_EVENTS) return RBL_ERR_ARGS;
    rbli_arena_reset(l, 1);
    l->ev = events;
    l->ev_n = 0;
    l->ev_cap = cap;
    if (l->pending_type) {
        rbl_event* e = rbli_ev_push(l, (rbl_event_type)l->pending_type);
        if (e) e->u.lost = l->pending;
        l->pending_type = 0;
    }
    now = rbli_mono_ms();
    for (i = 0; i < l->nclosing;) {
        if (rbli_closing_pump(&l->closing[i], now)) {
            i++;
        } else {
            memmove(&l->closing[i], &l->closing[i + 1], sizeof(rbli_closing) * (size_t)(l->nclosing - i - 1));
            l->nclosing--;
        }
    }
    rbli_poll_connect(l, now);
    rbli_poll_control(l, now);
    rbli_poll_udp(l, now);
    rbli_pump_collision(l);
    rbli_poll_timers(l, now);
    n = l->ev_n;
    l->ev = NULL;
    l->ev_n = l->ev_cap = 0;
    return (int)n;
}

RBLDEF int rbl_link_send_camera(rbl_link* l, const rbl_camera* c, uint32_t* seq_out) {
    rbl_header h;
    int r, n;
    uint32_t s;
    if (!l || !c) return RBL_ERR_ARGS;
    r = rbli_live_header(l, &h);
    if (r != RBL_OK) return r;
    n = rbl_wire_encode_camera(&h, c, l->tx, sizeof l->tx);
    if (n < 0) return n;
    s = rbli_emit(l, n);
    if (seq_out) *seq_out = s;
    return RBL_OK;
}

RBLDEF int rbl_link_send_poses(rbl_link* l, const rbl_pose* poses, size_t count) {
    size_t i;
    if (!l || (count && !poses)) return RBL_ERR_ARGS;
    if (l->state != RBLI_LIVE) return RBL_ERR_NOT_CONNECTED;
    for (i = 0; i < count; i += RBL_MAX_POSES) {
        rbl_header h;
        size_t n = count - i < RBL_MAX_POSES ? count - i : RBL_MAX_POSES;
        int len;
        rbli_live_header(l, &h);
        len = rbl_wire_encode_poses(&h, poses + i, n, l->tx, sizeof l->tx);
        if (len < 0) return len;
        rbli_emit(l, len);
    }
    return RBL_OK;
}

RBLDEF int rbl_link_send_collision(rbl_link* l, const float center[3], float radius, const rbl_triangle* tris,
                                   size_t count, uint32_t* query_out) {
    rbl_triangle* copy = NULL;
    size_t chunks;
    if (!l || !center || (count && !tris)) return RBL_ERR_ARGS;
    if (l->state != RBLI_LIVE) return RBL_ERR_NOT_CONNECTED;
    chunks = rbl_wire_chunk_count(count);
    if (chunks > 0xFFFF || count > ((size_t)-1) / sizeof(rbl_triangle)) return RBL_ERR_TOO_BIG;
    if (count) {
        copy = (rbl_triangle*)REWRITE_LINK_MALLOC(sizeof(rbl_triangle) * count);
        if (!copy) return RBL_ERR_NOMEM;
        memcpy(copy, tris, sizeof(rbl_triangle) * count);
    }
    rbli_cq_clear(l); /* a newer snapshot replaces the chunks still waiting: the receiver would drop the old query anyway */
    l->cq_tris = copy;
    l->cq_n = count;
    l->cq_query = l->next_query++;
    l->cq_chunks = (uint32_t)chunks;
    l->cq_next = 0;
    l->cq_center[0] = center[0]; l->cq_center[1] = center[1]; l->cq_center[2] = center[2];
    l->cq_radius = radius;
    if (query_out) *query_out = l->cq_query;
    return RBL_OK;
}

RBLDEF int rbl_link_send_frame(rbl_link* l, const rbl_frame* f) {
    rbl_header h;
    int r, n;
    if (!l || !f) return RBL_ERR_ARGS;
    r = rbli_live_header(l, &h);
    if (r != RBL_OK) return r;
    n = rbl_wire_encode_frame(&h, f, l->tx, sizeof l->tx);
    if (n < 0) return n;
    rbli_emit(l, n);
    return RBL_OK;
}

RBLDEF int rbl_link_spawn(rbl_link* l, uint32_t entity, const char* provider, const char* asset, const char* name) {
    rbl_control m;
    if (!l || !provider || !asset) return RBL_ERR_ARGS;
    memset(&m, 0, sizeof m);
    m.type = RBL_CTL_SPAWN;
    m.u.spawn.entity = entity;
    m.u.spawn.provider = rbli_cstr(provider);
    m.u.spawn.asset = rbli_cstr(asset);
    m.u.spawn.has_name = name != NULL;
    m.u.spawn.name = rbli_cstr(name);
    return rbli_send_control(l, &m);
}

RBLDEF int rbl_link_despawn(rbl_link* l, uint32_t entity) {
    rbl_control m;
    if (!l) return RBL_ERR_ARGS;
    memset(&m, 0, sizeof m);
    m.type = RBL_CTL_DESPAWN;
    m.u.despawn.entity = entity;
    return rbli_send_control(l, &m);
}

RBLDEF int rbl_link_set_focus(rbl_link* l, rbl_role focus) {
    rbl_control m;
    int r;
    if (!l) return RBL_ERR_ARGS;
    memset(&m, 0, sizeof m);
    m.type = RBL_CTL_INPUT;
    m.u.input.focus = focus;
    r = rbli_send_control(l, &m);
    if (r == RBL_OK && l->role == RBL_ROLE_HOST) l->focus = focus;
    return r;
}

RBLDEF int rbl_link_announce_frames(rbl_link* l, const char* path, uint32_t width, uint32_t height, uint32_t slots) {
    rbl_control m;
    if (!l || !path) return RBL_ERR_ARGS;
    memset(&m, 0, sizeof m);
    m.type = RBL_CTL_FRAME_CHANNEL;
    m.u.frame_channel.path = rbli_cstr(path);
    m.u.frame_channel.width = width;
    m.u.frame_channel.height = height;
    m.u.frame_channel.slots = slots;
    return rbli_send_control(l, &m);
}

RBLDEF void rbl_link_disconnect(rbl_link* l, const char* reason) {
    if (!l) return;
    if (l->state == RBLI_HANDSHAKE || l->state == RBLI_LIVE) {
        rbli_end(l, reason ? reason : "", 1);
        l->pending_type = 0; /* the caller knows; do not report its own disconnect back */
    }
}

RBLDEF void rbl_link_close(rbl_link* l) {
    uint64_t now, stop;
    int i;
    if (!l) return;
    now = rbli_mono_ms();
    if (l->state == RBLI_CONNECTING && l->connecting != RBLI_BAD_SOCK) {
        rbli_sock_close(l->connecting);
        l->connecting = RBLI_BAD_SOCK;
    }
    if (l->state == RBLI_HANDSHAKE || l->state == RBLI_LIVE) rbli_retire_ctl(l, "closing", now);
    l->state = RBLI_IDLE;
    stop = now + RBL_LINGER_MS + 50;
    while (l->nclosing > 0 && rbli_mono_ms() < stop) {
        uint64_t t;
        rbli_sleep_ms(2);
        t = rbli_mono_ms();
        for (i = 0; i < l->nclosing;) {
            if (rbli_closing_pump(&l->closing[i], t)) {
                i++;
            } else {
                memmove(&l->closing[i], &l->closing[i + 1], sizeof(rbli_closing) * (size_t)(l->nclosing - i - 1));
                l->nclosing--;
            }
        }
    }
    rbli_link_destroy(l);
}

RBLDEF rbl_role rbl_link_role(const rbl_link* l) { return l->role; }
RBLDEF uint16_t rbl_link_control_port(const rbl_link* l) { return l->control_port; }
RBLDEF uint16_t rbl_link_udp_port(const rbl_link* l) { return l->udp_port; }
RBLDEF int rbl_link_is_connected(const rbl_link* l) { return l->state == RBLI_LIVE; }
RBLDEF int rbl_link_peer(const rbl_link* l, rbl_peer_info* out) {
    if (l->state != RBLI_LIVE) return 0;
    *out = l->peer;
    return 1;
}
RBLDEF rbl_role rbl_link_focus(const rbl_link* l) { return l->focus; }
RBLDEF void rbl_link_stats(const rbl_link* l, rbl_stats* out) { *out = l->stats; }

/* ------------------------------------------------------------------------------------------
 * The pixel file
 *
 * The seqlock (CONTRACT.md 9.6). The writer makes a slot's 64-bit `lock` odd, writes the slot,
 * and makes it even again; the reader copies and checks that the lock did not change:
 *
 *   writer: lock+1 (relaxed); release fence; header, pixels, depth; lock+2 (release store)
 *   reader: l1 = lock (acquire), odd -> skip; copy; acquire fence; l2 = lock (relaxed)
 *           l2 != l1 or the copied frame number is not the one announced -> discard
 *
 * With GCC and Clang these are __atomic builtins, which work in C99 and C++. With MSVC they are
 * full barriers (MemoryBarrier) around plain 64-bit accesses (__iso_volatile_load64/store64), a
 * stronger ordering than the contract asks for; a plain access is also all a read-only mapping
 * allows (a locked read-modify-write would fault there). The lock sits at a multiple of 64 bytes
 * from the start of the (page aligned) mapping, so it is 8-byte aligned. 32-bit x86 builds are not
 * a target: a 64-bit atomic load there may need cmpxchg8b, which faults on a read-only page.
 * ---------------------------------------------------------------------------------------- */

#if RBLI_MSVC
static uint64_t rbli_ld_acquire(const void* p) {
    uint64_t v = (uint64_t)__iso_volatile_load64((const volatile __int64*)p);
    MemoryBarrier();
    return v;
}
static uint64_t rbli_ld_relaxed(const void* p) { return (uint64_t)__iso_volatile_load64((const volatile __int64*)p); }
static void rbli_st_relaxed(void* p, uint64_t v) { __iso_volatile_store64((volatile __int64*)p, (__int64)v); }
static void rbli_st_release(void* p, uint64_t v) { MemoryBarrier(); __iso_volatile_store64((volatile __int64*)p, (__int64)v); }
static void rbli_fence_acquire(void) { MemoryBarrier(); }
static void rbli_fence_release(void) { MemoryBarrier(); }
static void rbli_fence_full(void) { MemoryBarrier(); }
#else
/* The file is little-endian: on a big-endian host the lock word is swapped on the way in and out
 * (this path is not tested; no big-endian machine is a target of this library). */
#if RBLI_BIG_ENDIAN
#define RBLI_LE64(v) __builtin_bswap64(v)
#else
#define RBLI_LE64(v) (v)
#endif
static uint64_t rbli_ld_acquire(const void* p) { return RBLI_LE64(__atomic_load_n((const uint64_t*)p, __ATOMIC_ACQUIRE)); }
static uint64_t rbli_ld_relaxed(const void* p) { return RBLI_LE64(__atomic_load_n((const uint64_t*)p, __ATOMIC_RELAXED)); }
static void rbli_st_relaxed(void* p, uint64_t v) { __atomic_store_n((uint64_t*)p, RBLI_LE64(v), __ATOMIC_RELAXED); }
static void rbli_st_release(void* p, uint64_t v) { __atomic_store_n((uint64_t*)p, RBLI_LE64(v), __ATOMIC_RELEASE); }
static void rbli_fence_acquire(void) { __atomic_thread_fence(__ATOMIC_ACQUIRE); }
static void rbli_fence_release(void) { __atomic_thread_fence(__ATOMIC_RELEASE); }
static void rbli_fence_full(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
#endif

#define RBLI_FILE_HDR 64u
#define RBLI_SLOT_HDR 64u

/* Plane copies. The wire is little-endian; on a big-endian host the depth floats are swapped. */
static void rbli_copy_f32_le(float* dst, const uint8_t* src, size_t n) {
#if RBLI_BIG_ENDIAN
    size_t i;
    for (i = 0; i < n; i++) dst[i] = rbli_getf(src + 4 * i);
#else
    memcpy(dst, src, 4 * n);
#endif
}
static void rbli_store_f32_le(uint8_t* dst, const float* src, size_t n) {
#if RBLI_BIG_ENDIAN
    size_t i;
    for (i = 0; i < n; i++) rbli_putf(dst + 4 * i, src[i]);
#else
    memcpy(dst, src, 4 * n);
#endif
}

RBLDEF int rbl_path_is_absolute(const char* path, int windows_rules) {
    if (!path) return 0;
    if (!windows_rules) return path[0] == '/';
    if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':' &&
        (path[2] == '\\' || path[2] == '/'))
        return 1;
    return (path[0] == '\\' || path[0] == '/') && (path[1] == '\\' || path[1] == '/');
}

static int rbli_path_abs_here(const char* path) { return rbl_path_is_absolute(path, RBLI_WINDOWS); }

/* Checks a pixel file header (revision 4) against the file's length. size is the file length
 * in bytes. Returns RBL_OK or RBL_ERR_REFUSED. Pure: it reads only hdr[0..64). */
static int rbli_pf_validate(const uint8_t* hdr, uint64_t size, rbl_pixel_info* info) {
    uint32_t slots, w, h, stride, format, depth_format, depth_stride;
    uint64_t slot_size, depth_offset, colour_end, need, t, plane_end;
    if (size < RBLI_FILE_HDR) return RBL_ERR_REFUSED;
    if (memcmp(hdr, "RBFRAME\0", 8) != 0) return RBL_ERR_REFUSED;
    if (rbli_get32(hdr + 8) != 1) return RBL_ERR_REFUSED;
    slots = rbli_get32(hdr + 12);
    w = rbli_get32(hdr + 16);
    h = rbli_get32(hdr + 20);
    stride = rbli_get32(hdr + 24);
    format = rbli_get32(hdr + 28);
    slot_size = rbli_get64(hdr + 32);
    depth_format = rbli_get32(hdr + 40);
    depth_stride = rbli_get32(hdr + 44);
    depth_offset = rbli_get64(hdr + 48);
    if (slots < 1 || slots > RBL_MAX_SLOTS) return RBL_ERR_REFUSED;
    /* the limits come first: no size is computed from numbers that have not passed them */
    if (w < 1 || h < 1 || w > RBL_MAX_FRAME_DIM || h > RBL_MAX_FRAME_DIM) return RBL_ERR_REFUSED;
    if ((uint64_t)w * h > RBL_MAX_FRAME_PIXELS) return RBL_ERR_REFUSED;
    if (format != 1) return RBL_ERR_REFUSED;
    if (stride != w * 4u) return RBL_ERR_REFUSED;
    if (rbli_mul_u64(stride, h, &t) || rbli_add_u64(RBLI_SLOT_HDR, t, &colour_end)) return RBL_ERR_REFUSED;
    if (slot_size % 64 != 0 || slot_size < colour_end) return RBL_ERR_REFUSED;
    if (rbli_mul_u64(slots, slot_size, &t) || rbli_add_u64(RBLI_FILE_HDR, t, &need)) return RBL_ERR_REFUSED;
    if (need > size) return RBL_ERR_REFUSED;
    if (depth_format == RBL_DEPTH_LINEAR_F32) {
        if (depth_stride != w * 4u) return RBL_ERR_REFUSED;
        if (depth_offset % 64 != 0 || depth_offset < colour_end) return RBL_ERR_REFUSED;
        if (rbli_mul_u64(depth_stride, h, &t) || rbli_add_u64(depth_offset, t, &plane_end)) return RBL_ERR_REFUSED;
        if (plane_end > slot_size) return RBL_ERR_REFUSED;
    } else {
        depth_format = RBL_DEPTH_NONE; /* an unknown format is not an error: colour only */
        depth_stride = 0;
        depth_offset = 0;
    }
    if (info) {
        info->slots = slots; info->width = w; info->height = h; info->stride = stride;
        info->slot_size = slot_size; info->depth_format = depth_format;
        info->depth_stride = depth_stride; info->depth_offset = depth_offset;
    }
    return RBL_OK;
}

#if RBLI_WINDOWS
static wchar_t* rbli_widen(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    wchar_t* w;
    if (n <= 0) return NULL;
    w = (wchar_t*)REWRITE_LINK_MALLOC(sizeof(wchar_t) * (size_t)n);
    if (!w) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) <= 0) { REWRITE_LINK_FREE(w); return NULL; }
    return w;
}
#endif

struct rbl_pixel_reader {
    const uint8_t* map;
    size_t mapped;
#if RBLI_WINDOWS
    HANDLE file, mapping;
#else
    int fd;
#endif
    rbl_pixel_info info;
    rbl_pixel_stats st;
};

struct rbl_pixel_writer {
    uint8_t* map;
    size_t mapped;
#if RBLI_WINDOWS
    HANDLE file, mapping;
    wchar_t* wpath;
#else
    int fd;
#endif
    char* path;
    rbl_pixel_info info;
    uint64_t frame_no;
};

RBLDEF int rbl_pixel_reader_open(const char* path, rbl_pixel_reader** out) {
    rbl_pixel_reader* r;
    uint64_t size;
    int rc;
    if (!out) return RBL_ERR_ARGS;
    *out = NULL;
    if (!path) return RBL_ERR_ARGS;
    if (!rbli_path_abs_here(path)) return RBL_ERR_REFUSED; /* the path comes from the peer: it must be absolute */
    r = (rbl_pixel_reader*)REWRITE_LINK_MALLOC(sizeof *r);
    if (!r) return RBL_ERR_NOMEM;
    memset(r, 0, sizeof *r);
#if RBLI_WINDOWS
    {
        wchar_t* wp = rbli_widen(path);
        WIN32_FILE_ATTRIBUTE_DATA fad;
        BY_HANDLE_FILE_INFORMATION bi;
        LARGE_INTEGER sz;
        void* view;
        if (!wp) { REWRITE_LINK_FREE(r); return RBL_ERR_REFUSED; }
        if (!GetFileAttributesExW(wp, GetFileExInfoStandard, &fad)) { REWRITE_LINK_FREE(wp); REWRITE_LINK_FREE(r); return RBL_ERR_IO; }
        if (fad.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) {
            REWRITE_LINK_FREE(wp); REWRITE_LINK_FREE(r); return RBL_ERR_REFUSED;
        }
        r->file = CreateFileW(wp, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        REWRITE_LINK_FREE(wp);
        if (r->file == INVALID_HANDLE_VALUE) { REWRITE_LINK_FREE(r); return RBL_ERR_IO; }
        if (GetFileType(r->file) != FILE_TYPE_DISK || !GetFileInformationByHandle(r->file, &bi) ||
            (bi.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) ||
            !GetFileSizeEx(r->file, &sz)) {
            CloseHandle(r->file); REWRITE_LINK_FREE(r); return RBL_ERR_REFUSED;
        }
        size = (uint64_t)sz.QuadPart;
        if (size < RBLI_FILE_HDR || size > (uint64_t)((size_t)-1)) { CloseHandle(r->file); REWRITE_LINK_FREE(r); return RBL_ERR_REFUSED; }
        r->mapping = CreateFileMappingW(r->file, NULL, PAGE_READONLY, 0, 0, NULL);
        if (!r->mapping) { CloseHandle(r->file); REWRITE_LINK_FREE(r); return RBL_ERR_IO; }
        view = MapViewOfFile(r->mapping, FILE_MAP_READ, 0, 0, 0);
        if (!view) { CloseHandle(r->mapping); CloseHandle(r->file); REWRITE_LINK_FREE(r); return RBL_ERR_IO; }
        r->map = (const uint8_t*)view;
        r->mapped = (size_t)size;
    }
#else
    {
        struct stat before, after;
        void* view;
        if (lstat(path, &before) != 0) { REWRITE_LINK_FREE(r); return RBL_ERR_IO; }
        if (!S_ISREG(before.st_mode)) { REWRITE_LINK_FREE(r); return RBL_ERR_REFUSED; } /* symlink, FIFO, device, directory, socket */
        /* O_NONBLOCK: opening a FIFO a peer swapped in after the check above must not wait for a writer */
        r->fd = open(path, O_RDONLY | O_NONBLOCK
#ifdef O_CLOEXEC
                     | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
                     | O_NOFOLLOW
#endif
        );
        if (r->fd < 0) {
            int e = errno;
            REWRITE_LINK_FREE(r);
#ifdef ELOOP
            if (e == ELOOP) return RBL_ERR_REFUSED;
#endif
            (void)e;
            return RBL_ERR_IO;
        }
        if (fstat(r->fd, &after) != 0 || !S_ISREG(after.st_mode) || before.st_dev != after.st_dev || before.st_ino != after.st_ino) {
            close(r->fd); REWRITE_LINK_FREE(r); return RBL_ERR_REFUSED;
        }
        size = (uint64_t)after.st_size;
        if (size < RBLI_FILE_HDR || size > (uint64_t)((size_t)-1)) { close(r->fd); REWRITE_LINK_FREE(r); return RBL_ERR_REFUSED; }
        view = mmap(NULL, (size_t)size, PROT_READ, MAP_SHARED, r->fd, 0);
        if (view == MAP_FAILED) { close(r->fd); REWRITE_LINK_FREE(r); return RBL_ERR_IO; }
        r->map = (const uint8_t*)view;
        r->mapped = (size_t)size;
    }
#endif
    /* The sizes in the header come from the peer: none is trusted (see rbli_pf_validate). */
    rc = rbli_pf_validate(r->map, size, &r->info);
    if (rc != RBL_OK) {
        rbl_pixel_reader_close(r);
        return rc;
    }
    *out = r;
    return RBL_OK;
}

RBLDEF void rbl_pixel_reader_info(const rbl_pixel_reader* r, rbl_pixel_info* out) { *out = r->info; }
RBLDEF int rbl_pixel_reader_has_depth(const rbl_pixel_reader* r) { return r->info.depth_format == RBL_DEPTH_LINEAR_F32; }
RBLDEF void rbl_pixel_reader_stats(const rbl_pixel_reader* r, rbl_pixel_stats* out) { *out = r->st; }

RBLDEF int rbl_pixel_reader_is_gone(const rbl_pixel_reader* r) {
#if RBLI_WINDOWS
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(r->file, &sz)) return 1;
    return (uint64_t)sz.QuadPart < (uint64_t)r->mapped;
#else
    struct stat st;
    if (fstat(r->fd, &st) != 0) return 1;
    return (uint64_t)st.st_size < (uint64_t)r->mapped;
#endif
}

RBLDEF int rbl_pixel_reader_read(rbl_pixel_reader* r, uint64_t frame, uint32_t slot, uint8_t* rgba, size_t rgba_cap,
                                 float* depth, size_t depth_cap, rbl_pixel_frame* info) {
    const uint8_t* p;
    uint64_t l1, l2, f, time_us;
    uint32_t cam, w, h, y;
    size_t row, need_c, need_d;
    int want_depth;
    if (!r || !rgba) return RBL_ERR_ARGS;
    if (slot >= r->info.slots) return RBL_MISS_OTHER_FRAME;
    if (rbl_pixel_reader_is_gone(r)) { r->st.gone++; return RBL_MISS_GONE; } /* the next access to a shrunk file would fault */
    p = r->map + RBLI_FILE_HDR + (size_t)slot * (size_t)r->info.slot_size; /* checked at open: inside the file */
    l1 = rbli_ld_acquire(p); /* step 1 */
    if (l1 & 1) { r->st.busy++; return RBL_MISS_BUSY; }
    f = rbli_get64(p + 8);
    time_us = rbli_get64(p + 16);
    cam = rbli_get32(p + 24);
    w = rbli_get32(p + 28);
    h = rbli_get32(p + 32);
    if (f != frame || w < 1 || h < 1 || w > r->info.width || h > r->info.height) { r->st.other_frame++; return RBL_MISS_OTHER_FRAME; }
    want_depth = depth != NULL && r->info.depth_format == RBL_DEPTH_LINEAR_F32;
    need_c = (size_t)w * h * 4u;
    need_d = (size_t)w * h;
    if (rgba_cap < need_c || (want_depth && depth_cap < need_d)) {
        /* too small for this frame, unless the header was torn by a writer in the middle of a write */
        rbli_fence_acquire();
        if (rbli_ld_relaxed(p) != l1) { r->st.overwritten++; return RBL_MISS_OVERWRITTEN; }
        return RBL_ERR_TOO_BIG;
    }
    row = (size_t)w * 4u;
    for (y = 0; y < h; y++) memcpy(rgba + (size_t)y * row, p + RBLI_SLOT_HDR + (size_t)y * r->info.stride, row);
    if (want_depth) {
        for (y = 0; y < h; y++)
            rbli_copy_f32_le(depth + (size_t)y * w, p + (size_t)r->info.depth_offset + (size_t)y * r->info.depth_stride, w);
    }
    rbli_fence_acquire(); /* step 3: the acquire fence, then l2 */
    l2 = rbli_ld_relaxed(p);
    if (l2 != l1 || rbli_get64(p + 8) != frame) { r->st.overwritten++; return RBL_MISS_OVERWRITTEN; }
    if (info) {
        info->frame = f; info->time_us = time_us; info->camera_seq = cam; info->width = w; info->height = h;
        info->has_depth = want_depth;
    }
    return RBL_OK;
}

RBLDEF void rbl_pixel_reader_close(rbl_pixel_reader* r) {
    if (!r) return;
#if RBLI_WINDOWS
    if (r->map) UnmapViewOfFile(r->map);
    if (r->mapping) CloseHandle(r->mapping);
    if (r->file && r->file != INVALID_HANDLE_VALUE) CloseHandle(r->file);
#else
    if (r->map) munmap((void*)(uintptr_t)r->map, r->mapped);
    if (r->fd >= 0) close(r->fd);
#endif
    REWRITE_LINK_FREE(r);
}

/* ---- writer ---- */

RBLDEF int rbl_temp_dir(char* out, size_t cap) {
#if RBLI_WINDOWS
    DWORD n = GetTempPathA((DWORD)(cap > 0xFFFF ? 0xFFFF : cap), out);
    if (n == 0 || n >= cap) return RBL_ERR_TOO_BIG;
    return (int)n;
#else
    const char* t = getenv("TMPDIR");
    size_t n;
    if (!t || t[0] != '/') t = "/tmp";
    n = strlen(t);
    if (n + 2 > cap) return RBL_ERR_TOO_BIG;
    memcpy(out, t, n);
    if (n == 0 || out[n - 1] != '/') out[n++] = '/';
    out[n] = 0;
    return (int)n;
#endif
}

static uint64_t rbli_round64(uint64_t v) { return (v + 63u) & ~(uint64_t)63u; }

RBLDEF int rbl_pixel_writer_create(const char* path, uint32_t width, uint32_t height, uint32_t slots, int with_depth,
                                   rbl_pixel_writer** out) {
    rbl_pixel_writer* w;
    uint64_t stride, colour_end, depth_offset, slot_size, total, t;
    size_t plen;
    if (!out) return RBL_ERR_ARGS;
    *out = NULL;
    if (!path || !path[0]) return RBL_ERR_ARGS;
    if (slots < 1 || slots > RBL_MAX_SLOTS || width < 1 || height < 1) return RBL_ERR_ARGS;
    if (width > RBL_MAX_FRAME_DIM || height > RBL_MAX_FRAME_DIM) return RBL_ERR_ARGS;
    if ((uint64_t)width * height > RBL_MAX_FRAME_PIXELS) return RBL_ERR_ARGS;
    stride = (uint64_t)width * 4u;
    colour_end = RBLI_SLOT_HDR + stride * height; /* at most 64 + 2^28: no overflow */
    depth_offset = with_depth ? rbli_round64(colour_end) : 0;
    slot_size = rbli_round64(with_depth ? depth_offset + stride * height : colour_end);
    if (rbli_mul_u64(slots, slot_size, &t) || rbli_add_u64(RBLI_FILE_HDR, t, &total) || total > (uint64_t)((size_t)-1))
        return RBL_ERR_TOO_BIG;
    w = (rbl_pixel_writer*)REWRITE_LINK_MALLOC(sizeof *w);
    if (!w) return RBL_ERR_NOMEM;
    memset(w, 0, sizeof *w);
    /* an absolute path to announce: a relative one is resolved against the current directory now */
    plen = strlen(path);
    if (rbli_path_abs_here(path)) {
        w->path = (char*)REWRITE_LINK_MALLOC(plen + 1);
        if (w->path) memcpy(w->path, path, plen + 1);
    } else {
#if RBLI_WINDOWS
        char cwd[1024];
        DWORD n = GetCurrentDirectoryA(sizeof cwd, cwd);
        if (n > 0 && n < sizeof cwd) {
            w->path = (char*)REWRITE_LINK_MALLOC(n + plen + 2);
            if (w->path) { memcpy(w->path, cwd, n); w->path[n] = '\\'; memcpy(w->path + n + 1, path, plen + 1); }
        }
#else
        char cwd[4096];
        if (getcwd(cwd, sizeof cwd)) {
            size_t n = strlen(cwd);
            w->path = (char*)REWRITE_LINK_MALLOC(n + plen + 2);
            if (w->path) { memcpy(w->path, cwd, n); w->path[n] = '/'; memcpy(w->path + n + 1, path, plen + 1); }
        }
#endif
    }
    if (!w->path) { REWRITE_LINK_FREE(w); return RBL_ERR_IO; }
#if RBLI_WINDOWS
    w->wpath = rbli_widen(w->path);
    if (!w->wpath) { REWRITE_LINK_FREE(w->path); REWRITE_LINK_FREE(w); return RBL_ERR_ARGS; }
    /* CREATE_NEW: creation fails if the path exists, so it never opens over another file. Both sides
     * share read, write and delete, so the receiver can open the file and this side can delete it. */
    w->file = CreateFileW(w->wpath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                          CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (w->file == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        REWRITE_LINK_FREE(w->wpath); REWRITE_LINK_FREE(w->path); REWRITE_LINK_FREE(w);
        return (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS) ? RBL_ERR_EXISTS : RBL_ERR_IO;
    }
    w->mapping = CreateFileMappingW(w->file, NULL, PAGE_READWRITE, (DWORD)(total >> 32), (DWORD)(total & 0xFFFFFFFFu), NULL);
    if (w->mapping) w->map = (uint8_t*)MapViewOfFile(w->mapping, FILE_MAP_WRITE, 0, 0, (size_t)total);
    if (!w->map) {
        if (w->mapping) CloseHandle(w->mapping);
        CloseHandle(w->file);
        DeleteFileW(w->wpath);
        REWRITE_LINK_FREE(w->wpath); REWRITE_LINK_FREE(w->path); REWRITE_LINK_FREE(w);
        return RBL_ERR_IO;
    }
#else
    w->fd = open(w->path, O_RDWR | O_CREAT | O_EXCL
#ifdef O_CLOEXEC
                 | O_CLOEXEC
#endif
                 , 0600);
    if (w->fd < 0) {
        int e = errno;
        REWRITE_LINK_FREE(w->path); REWRITE_LINK_FREE(w);
        return e == EEXIST ? RBL_ERR_EXISTS : RBL_ERR_IO;
    }
    {   /* sized once, here, before the path is announced; never shrunk or resized afterwards */
        void* view = MAP_FAILED;
        if (ftruncate(w->fd, (off_t)total) == 0) view = mmap(NULL, (size_t)total, PROT_READ | PROT_WRITE, MAP_SHARED, w->fd, 0);
        if (view == MAP_FAILED) {
            close(w->fd);
            unlink(w->path);
            REWRITE_LINK_FREE(w->path); REWRITE_LINK_FREE(w);
            return RBL_ERR_IO;
        }
        w->map = (uint8_t*)view;
    }
#endif
    w->mapped = (size_t)total;
    w->info.slots = slots; w->info.width = width; w->info.height = height; w->info.stride = (uint32_t)stride;
    w->info.slot_size = slot_size;
    w->info.depth_format = with_depth ? RBL_DEPTH_LINEAR_F32 : RBL_DEPTH_NONE;
    w->info.depth_stride = with_depth ? (uint32_t)stride : 0;
    w->info.depth_offset = depth_offset;
    /* the file is new and zero filled: write the header, then nobody has seen the file yet */
    memcpy(w->map, "RBFRAME\0", 8);
    rbli_put32(w->map + 8, 1);
    rbli_put32(w->map + 12, slots);
    rbli_put32(w->map + 16, width);
    rbli_put32(w->map + 20, height);
    rbli_put32(w->map + 24, (uint32_t)stride);
    rbli_put32(w->map + 28, 1);
    rbli_put64(w->map + 32, slot_size);
    rbli_put32(w->map + 40, w->info.depth_format);
    rbli_put32(w->map + 44, w->info.depth_stride);
    rbli_put64(w->map + 48, depth_offset);
    *out = w;
    return RBL_OK;
}

RBLDEF int rbl_pixel_writer_create_temp(const char* tag, uint32_t width, uint32_t height, uint32_t slots, int with_depth,
                                        rbl_pixel_writer** out) {
    char dir[1024], path[1280], clean[40];
    int n, attempt;
    size_t i;
    if (!out) return RBL_ERR_ARGS;
    *out = NULL;
    n = rbl_temp_dir(dir, sizeof dir);
    if (n < 0) return n;
    for (i = 0; tag && tag[i] && i < sizeof clean - 1; i++) {
        char c = tag[i];
        clean[i] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') ? c : '_';
    }
    clean[i] = 0;
    for (attempt = 0; attempt < 64; attempt++) {
        uint8_t rnd[4];
        int r;
        rbli_random(rnd, 4);
        snprintf(path, sizeof path, "%srewrite-bridge-%u-%s-%08lx.frames", dir, rbli_pid(), clean, (unsigned long)rbli_get32(rnd));
        r = rbl_pixel_writer_create(path, width, height, slots, with_depth, out);
        if (r != RBL_ERR_EXISTS) return r;
    }
    return RBL_ERR_EXISTS;
}

RBLDEF int rbl_pixel_writer_write(rbl_pixel_writer* wr, const uint8_t* rgba, size_t rgba_len, const float* depth,
                                  size_t depth_len, uint32_t w, uint32_t h, uint32_t camera_seq, uint64_t time_us,
                                  uint64_t* frame_out) {
    uint64_t frame, lock;
    uint32_t slot, y;
    uint8_t* p;
    size_t row;
    if (!wr || !rgba) return RBL_ERR_ARGS;
    if (w < 1 || h < 1 || w > wr->info.width || h > wr->info.height) return RBL_ERR_ARGS;
    if (rgba_len != (size_t)w * h * 4u) return RBL_ERR_ARGS;
    if (depth) {
        if (wr->info.depth_format != RBL_DEPTH_LINEAR_F32) return RBL_ERR_ARGS;
        if (depth_len != (size_t)w * h) return RBL_ERR_ARGS;
    }
    frame = ++wr->frame_no;
    slot = (uint32_t)(frame % wr->info.slots);
    p = wr->map + RBLI_FILE_HDR + (size_t)slot * (size_t)wr->info.slot_size;
    lock = rbli_ld_relaxed(p); /* only this writer changes it */
    rbli_st_relaxed(p, lock + 1); /* step 1: odd, then the release fence */
    rbli_fence_release();
    rbli_put64(p + 8, frame);
    rbli_put64(p + 16, time_us);
    rbli_put32(p + 24, camera_seq);
    rbli_put32(p + 28, w);
    rbli_put32(p + 32, h);
    row = (size_t)w * 4u;
    for (y = 0; y < h; y++) memcpy(p + RBLI_SLOT_HDR + (size_t)y * wr->info.stride, rgba + (size_t)y * row, row);
    if (wr->info.depth_format == RBL_DEPTH_LINEAR_F32) {
        for (y = 0; y < h; y++) {
            uint8_t* drow = p + (size_t)wr->info.depth_offset + (size_t)y * wr->info.depth_stride;
            if (depth) {
                rbli_store_f32_le(drow, depth + (size_t)y * w, w);
            } else {
                uint32_t x;
                for (x = 0; x < w; x++) rbli_putf(drow + 4u * x, (float)HUGE_VAL); /* nothing here: +inf */
            }
        }
    }
    rbli_st_release(p, lock + 2); /* step 3: even again */
    rbli_fence_full();            /* out before the caller sends the Frame datagram */
    if (frame_out) *frame_out = frame;
    return RBL_OK;
}

RBLDEF const char* rbl_pixel_writer_path(const rbl_pixel_writer* w) { return w->path; }
RBLDEF void rbl_pixel_writer_info(const rbl_pixel_writer* w, rbl_pixel_info* out) { *out = w->info; }

RBLDEF int rbl_link_announce_writer(rbl_link* l, const rbl_pixel_writer* w) {
    if (!l || !w) return RBL_ERR_ARGS;
    return rbl_link_announce_frames(l, w->path, w->info.width, w->info.height, w->info.slots);
}

RBLDEF void rbl_pixel_writer_close(rbl_pixel_writer* w) {
    if (!w) return;
#if RBLI_WINDOWS
    /* Windows refuses to delete a file its own process maps: unmap and close first */
    if (w->map) UnmapViewOfFile(w->map);
    if (w->mapping) CloseHandle(w->mapping);
    if (w->file && w->file != INVALID_HANDLE_VALUE) CloseHandle(w->file);
    if (w->wpath) { DeleteFileW(w->wpath); REWRITE_LINK_FREE(w->wpath); }
#else
    if (w->map) munmap(w->map, w->mapped);
    if (w->fd >= 0) close(w->fd);
    if (w->path) unlink(w->path);
#endif
    if (w->path) REWRITE_LINK_FREE(w->path);
    REWRITE_LINK_FREE(w);
}

#ifdef __cplusplus
}
#endif

#endif /* REWRITE_LINK_IMPLEMENTATION_DONE */
#endif /* REWRITE_LINK_IMPLEMENTATION */
