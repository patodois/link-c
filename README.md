# link-c

A single-header **C99** implementation of the **rewrite-bridge passthrough link protocol v1** (contract
revision 4), so that a game mod written in C or C++ (an SKSE or F4SE plugin, a ScriptHookV script, an Unreal
or other native plugin) can be a **host** or a **guest** next to any other game or rewrite that speaks it.
The protocol is defined by `docs/CONTRACT.md` in the rewrite-bridge repository; this library is one
implementation of it. The Rust crate, [link-java](https://github.com/patodois/link-java) and this library
are the implementations that exist so far, and they talk to each other (see "Interop").

- One file, `rewrite_link.h`, in the style of the stb libraries: `#define REWRITE_LINK_IMPLEMENTATION` in
  exactly one `.c` or `.cpp` file, plain `#include` everywhere else.
- C99, and it compiles as C++ (`extern "C"`). Nothing but the C library and the platform: POSIX sockets and
  `mmap` on macOS and Linux, Winsock and `MapViewOfFile` on Windows. No threads are created.
- Everything binds and connects to `127.0.0.1` only.
- No global state. The few allocations are listed at the top of the header; `REWRITE_LINK_MALLOC`,
  `REWRITE_LINK_REALLOC` and `REWRITE_LINK_FREE` replace the allocator.
- Licence: MIT OR Apache-2.0.

The functions are prefixed `rbl_` (the prefix the contract uses for its C layouts in section 10, which this
header defines with the same names: `rbl_header`, `rbl_camera`, `rbl_pose`, ...), so they do not collide with
anything in a large plugin project.

## A host in 20 lines

```c
#define REWRITE_LINK_IMPLEMENTATION
#include "../rewrite_link.h"
#include <stdio.h>
extern int game_running(void); /* your frame loop; the game paces it */
int main(void) {
    rbl_config cfg; rbl_link* link = NULL; rbl_event ev[64];
    rbl_config_init(&cfg, "My Game");                  /* listens on 127.0.0.1:47800 */
    if (rbl_link_host(&cfg, &link) != RBL_OK) return 1;
    while (game_running()) {
        int i, n = rbl_link_poll(link, ev, 64);         /* never blocks; sends the heartbeats */
        for (i = 0; i < n; i++)
            if (ev[i].type == RBL_EV_POSES) printf("%u poses\n", ev[i].u.poses.count);
        if (rbl_link_is_connected(link)) {              /* once per rendered frame */
            rbl_camera cam; const float eye[3] = {0, 2, 8}, at[3] = {0, 1, 0};
            rbl_camera_look_at(&cam, eye, at, 1.0f, 0.1f, 1000.0f, 1280, 720);
            rbl_link_send_camera(link, &cam, NULL);
        }
    }
    rbl_link_close(link);
}
```

## A guest in 20 lines

```c
#define REWRITE_LINK_IMPLEMENTATION
#include "../rewrite_link.h"
extern int game_running(void); /* your frame loop; the game paces it */
int main(void) {
    rbl_config cfg; rbl_link* link = NULL; rbl_event ev[64];
    rbl_config_init(&cfg, "My Rewrite");               /* connects to 127.0.0.1:47800, retrying */
    if (rbl_link_guest(&cfg, &link) != RBL_OK) return 1;
    while (game_running()) {
        int i, n = rbl_link_poll(link, ev, 64);
        for (i = 0; i < n; i++) {
            if (ev[i].type == RBL_EV_CONNECTED) rbl_link_spawn(link, 1, "My Rewrite", "demo/cube", "Cube");
            if (ev[i].type == RBL_EV_CAMERA) {          /* the host's view, once per frame */
                rbl_pose pose; rbl_pose_identity(&pose, 1, rbl_now_us());
                pose.translation[1] = 1.0f;             /* metres, +Y up, in the host's world */
                rbl_link_send_poses(link, &pose, 1);
            }
        }
    }
    rbl_link_close(link);
}
```

Both run: `make` builds them as `build/host_example` and `build/guest_example`, with `examples/game_stub.c`
standing in for the game's frame loop. `examples/demo.c` is the longer one, the C twin of link-java's
`LinkDemo` (host or guest, `--frames`, `--orbit`, `--big-collision N`, `--seconds S`, `--port N`).

## The API

| | |
|---|---|
| `rbl_link_host(&cfg, &link)`, `rbl_link_guest(&cfg, &link)` | Create one side. `rbl_config_init(&cfg, "My Game")` fills in the defaults (port 47800, heartbeat 250 ms, timeout 2000 ms). |
| `rbl_link_poll(link, events, cap)` | **Never blocks.** Does the handshake, reads both channels, sends heartbeats and at most 32 collision chunks, checks the watchdog, and writes what happened into **your** array of `rbl_event`. Returns the count. Call it every frame. |
| `rbl_link_send_camera`, `rbl_link_send_poses`, `rbl_link_send_collision`, `rbl_link_send_frame` | State datagrams. `send_collision` copies the triangles and queues the chunks (paced, a newer snapshot replaces the queued one); the others send at once. `send_camera` returns the `seq` a guest quotes in `rbl_frame.camera_seq`. |
| `rbl_link_spawn`, `rbl_link_despawn`, `rbl_link_set_focus`, `rbl_link_announce_frames` | Control messages. |
| `rbl_link_disconnect(link, reason)`, `rbl_link_close(link)` | `bye`, half-close, drain (CONTRACT 6.1). `disconnect` does not block; `close` may block for up to about 250 ms, so call it at shutdown. |
| `rbl_link_is_connected`, `rbl_link_peer`, `rbl_link_focus`, `rbl_link_stats`, `rbl_link_control_port`, `rbl_link_udp_port` | Queries. `rbl_stats` has `malformed`, `stale`, `foreign`, `unknown_entity`, `spawn_ignored`, `send_dropped`, `datagrams_sent/received`, `lines_sent/received`, `dropped`. |
| `rbl_pixel_writer_create[_temp]`, `rbl_pixel_writer_write`, `rbl_link_announce_writer`, `rbl_pixel_writer_close` | The sending side of the pixel file: created exclusively, colour and optional linear depth, seqlock. |
| `rbl_pixel_reader_open`, `rbl_pixel_reader_read`, `rbl_pixel_reader_close` | The receiving side: every revision 4 check, copies a slot into your buffers with the seqlock. |
| `rbl_wire_*`, `rbl_control_parse`, `rbl_control_format` | The codecs, public for tools and tests. |
| `rbl_camera_look_at`, `rbl_pose_identity`, `rbl_quat_*` | Small helpers in the link's conventions (right-handed, +Y up, +Z front). |

All calls return `RBL_OK` (0) or a negative `rbl_status`; `rbl_strerror()` describes it.

### Events

`rbl_link_poll` fills `rbl_event`s: `RBL_EV_CONNECTED`, `LOST`, `HANDSHAKE_FAILED`, `CAMERA`, `POSES`, `COLLISION`
(a complete snapshot), `FRAME`, `SPAWN`, `DESPAWN`, `FOCUS`, `FRAME_CHANNEL`. Pointers inside an event (poses,
triangles, spawn and path strings) are valid **until the next `rbl_link_poll` or `rbl_link_close`**; copy what you
keep. If more is waiting than your array holds, the rest waits in the socket buffers for the next call, so pass
`RBL_MIN_EVENTS` (8) at least and preferably 64 or more.

### The pixel file

A guest renders, reads the image back, `rbl_pixel_writer_write()`s it (RGBA8 and, if the file was created with
depth, linear view depth in metres) and then sends the `Frame` datagram with `rbl_link_send_frame()`. A host
that gets `RBL_EV_FRAME_CHANNEL` opens the path with `rbl_pixel_reader_open()` and, on every `RBL_EV_FRAME`,
calls `rbl_pixel_reader_read()` with the frame number and slot from the event. The reader returns
`RBL_MISS_BUSY`, `OTHER_FRAME` or `OVERWRITTEN` when the writer got there first (skip the frame), and
`RBL_MISS_GONE` when the file shrank under the mapping (drop the reader). Reprojection and compositing with
depth (CONTRACT 9.7) are the host engine's job; this library does not do them.

## Contract revision 4

All of it is implemented and each rule has a test that fails without it (the tests were also run against
deliberately broken copies of the header to check that they notice):

- paced collision, at most 32 chunks per `rbl_link_poll`, a newer snapshot replaces queued chunks; a 4 MiB
  UDP receive buffer is requested;
- a new `frame_channel` resets the frame staleness; datagrams are accepted only from `127.0.0.1:<peer port>`;
- poses only for spawned entities, at most 1024 live ones (`unknown_entity`, `spawn_ignored`);
- `heartbeat_ms` 10 to 10000: out of range in a `hello` is a handshake error, in your own config it is clamped;
- the pixel file reader: an absolute path to a regular file (not a symlink, FIFO, device or directory; checked
  before and after opening with `O_NONBLOCK`, same device and inode), width and height at most 16384, at most 2^26
  pixels, `stride == width * 4`, checked 64-bit arithmetic for every size, a length check against the mapping
  **before every read**; the writer: created with `O_CREAT | O_EXCL` (`CREATE_NEW`), never resized or truncated,
  the same limits, deleted at the end;
- the control channel reads lines of at most 65536 bytes, strict UTF-8, and a JSON reader that accepts exactly
  what Rust's `serde_json` accepts (the corpus of link-java's tests is in `tests/test_link.c`);
- after `bye`: half-close, drain for at most 200 ms, close; a received `bye` wins over the reset that follows it.

Choices that the contract leaves open, made here:

- a receiver drops a collision snapshot of more than `cfg.max_collision_triangles` (default 2097152, 72 MiB) and
  counts it in `malformed`; the Rust and Java receivers keep whatever arrives;
- a `frame_channel` path with an embedded NUL (`\u0000`) is delivered as `NULL` and so refused by the reader;
- a `hello` with `udp_port` 0 is rejected, as the Rust and Java implementations do;
- one line goes to `stderr` per session when a peer overflows the entity table (the Rust and Java ones do too).

## Embedding it in a game plugin

SKSE (Skyrim) and F4SE (Fallout 4) plugins are native DLLs that the script extender's loader brings into the game
process, and they are normally written in C++; the same goes for other native plugins (ScriptHookV scripts,
Unreal modules). This is what I know of that and what follows from it for this library; **I have not built or
run it inside any of them.**

1. Put `#define REWRITE_LINK_IMPLEMENTATION` before `#include "rewrite_link.h"` in exactly one `.cpp` file of the
   plugin (the header compiles as C++; the checks here used clang++ on macOS). Everywhere else include it plainly.
2. Link `ws2_32` and `bcrypt` (MSVC picks them up from the `#pragma comment` in the header; MinGW needs
   `-lws2_32 -lbcrypt`). The library calls `WSAStartup` and `WSACleanup` itself for each link, which Windows
   counts, so it does not disturb a game that uses Winsock too.
3. Create the link where your plugin has finished loading, not in `DllMain` or while the loader lock is held, and
   call `rbl_link_poll` from the code that runs once per frame (a hook on the game's main loop or an update
   callback, whatever the framework gives you). The library starts no thread of its own; `rbl_link_poll` never
   blocks, and a game that stops calling it looks frozen to its peer, which is what the heartbeat is for. If you
   poll from a thread of your own instead, lock around every call: a link is not thread safe.
4. Send `rbl_link_send_camera` once per rendered frame, with the camera the game will actually present, and
   `rbl_link_send_poses` for entities you have `rbl_link_spawn`ed. Convert the game's coordinates to the link's
   (right-handed, +Y up, +Z front, metres) at that boundary, as CONTRACT.md section 4 describes, and write the
   mapping down per game.
5. Call `rbl_link_close` when the game shuts down (it may block for up to about 250 ms), not from a per-frame path.

A plugin is usually 64-bit; 32-bit x86 is not a target (a 64-bit atomic load on a read-only mapping may compile to
`cmpxchg8b` there, which faults).

## Build and test

```
make            # build/demo, build/host_example, build/guest_example, build/test_link, build/test_vectors, build/interop_peer   (macOS, Linux)
make test       # the unit tests and the shared test vectors
make asan       # the same under -fsanitize=address,undefined
make cxx        # the header, with its implementation, as C++11
cmake -S . -B build && cmake --build build && ctest --test-dir build      # also Windows (MSVC)
```

`tests/test_link.c` has the layout offsets, the worked example of CONTRACT.md section 11 byte for byte, the
datagram codecs, a fuzz of malformed datagrams (20000 random and 20000 mutated, plus every prefix of every valid
one; and 30000 junk datagrams at a live host), a fuzz of 100000 mutated control lines, the JSON corpus, live
sessions over loopback (handshake, state flow, bye, busy, rejected hellos, the watchdog, reconnecting, foreign
addresses, small event arrays, the allocation count), every revision 4 case, and the seqlock under contention
(a writer thread lapping a two-slot file against a reader thread; it also shows that the same copy without the
lock check does tear).

## Shared test vectors

`tests/vectors/link/` holds test vectors for the link protocol that all four implementations (Rust in
rewrite-bridge, `link-java`, this one, `link-cs`) run in their own test suites: control lines to decode, ignore,
reject or encode, UDP datagrams as hex, pixel files to open, read and write, and scripted sessions over real
sockets against one side of the link. They are derived from `docs/CONTRACT.md` alone, with an encoder of their own,
so a vector that this implementation fails is a place where it disagrees with the contract. The directory is a
copy of rewrite-bridge's `tests/vectors/link/` (without `gen.py`); `README.md` and `GAPS.md` in it describe the
formats and what the contract leaves open. Do not edit the copy.

`tests/test_vectors.c` runs them (`make test`, `make asan`, or `ctest -R vectors`; about 30 seconds, most of it
sessions that wait on timers). It checks every file against `SHA256SUMS`, and `SHA256SUMS` itself against the
constant `VECTORS_DIGEST` in the source, so the vectors can only change together with a deliberate edit of that
constant. Vectors this implementation is known to fail are listed with the reason in `KNOWN_DISAGREEMENTS` at the
top of the file: they are reported but do not fail the run, and one that starts passing does. The runner finds the
vectors in `$RBL_VECTORS_DIR`, else `tests/vectors/link` (also `../tests/vectors/link`) relative to the working
directory; `RBL_VECTORS_SKIP_LARGE=1` skips the cases that need a sparse file of up to 1 GiB.

To update the copy, run `scripts/sync-link-vectors.sh <path of this repository>` in the rewrite-bridge repository.
It copies the directory, keeps the `tests/vectors/link/** -text` line in `.gitattributes` (the files must stay byte
for byte), and prints `VECTORS_DIGEST = <hex>`: paste the hex into `tests/test_vectors.c`, run `make test`, and
revisit `KNOWN_DISAGREEMENTS`.

## Interop

`tests/interop_peer.c` is this implementation's side of rewrite-bridge's cross-language interop matrix
(`tests/interop/README.md` there): one process plays the host or the guest of a scripted session (handshake, spawn
and focus, five lock-step rounds of camera, poses, collision and frames through a real pixel file with depth,
despawn, bye) and prints a result line the driver there judges.

```
make build/interop_peer
build/interop_peer host  --port 47810 --label c &
build/interop_peer guest --port 47810 --label c
```

The last line on stdout is `RBL-INTEROP-RESULT {json}`; the exit status is 0 if every check passed, 1 if one failed,
2 for a bad command line. rewrite-bridge's `tests/interop/run.sh` starts it with `RBL_PEER_C="build/interop_peer"`
(or builds it with `make build/interop_peer` for you when you pass `--build`).

`tests/interop.sh` runs the demo against the Rust `fake_host` / `fake_guest` examples and link-java's `LinkDemo`,
in both roles, each pair twice: with the guest rendering a 320x240 frame with a depth plane for every camera, and
with the host sending a 400-chunk (90400 triangles) collision snapshot every second. It checks that the session
forms, spawn, poses, cameras, the focus request and grant, frames (the Rust host reprojects them with the depth
test; the C and Java hosts read them and check the depth), the whole snapshot, and that nothing was malformed,
foreign or from an unknown entity, and that the session ended with a `bye`.

```
RBL_DEMO=build/demo \
RBL_FAKE_HOST=path/to/rewrite-bridge/target/release/examples/fake_host \
RBL_FAKE_GUEST=path/to/rewrite-bridge/target/release/examples/fake_guest \
RBL_JAVA_CP=path/to/link-java/build/classes/java/main \
tests/interop.sh
```

## Notes and limits

- Developed and tested on macOS (arm64, clang). The Windows code (Winsock, `MapViewOfFile`, the MSVC atomics) was
  written against the documentation and syntax-checked against stand-in headers, **not compiled with MSVC or run
  on Windows**; the CI workflow builds and tests it there, but this repository has not been pushed, so it has not
  run yet. GCC on Linux has not been tried either.
- Winsock: each link calls `WSAStartup` and, when it is closed, `WSACleanup`; Windows counts them. If your own code uses sockets, hold your own `WSAStartup`
  reference (any game that uses Winsock does), or the last `rbl_link_close()` shuts Winsock down under your sockets.
- Big-endian hosts: the datagram and JSON codecs are byte-order clean; the pixel file path swaps the lock word and
  depth floats but has not been run.
- Strings that you pass in must be UTF-8 (invalid sequences are replaced by U+FFFD). `game` is cut at 255 bytes,
  a peer's game name at 127.
