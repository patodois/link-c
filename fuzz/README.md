# Fuzzing `rewrite_link.h`

libFuzzer harnesses for every place where the library parses bytes that another local process wrote. They are **not**
part of the default build, the tests or CI: `make` and `make test` never touch this directory.

| Harness | Entry point | Input | What it checks besides memory safety |
|---|---|---|---|
| `fuzz_wire` | `rbl_wire_decode`, `rbl_wire_pose`, `rbl_wire_triangle`, the `rbl_wire_encode_*` | one UDP datagram | accepts exactly what an independent reference of the length and range rules accepts; every decoded field equals the bytes; re-encoding gives the input back (reserved words aside) |
| `fuzz_control` | `rbl_control_parse`, `rbl_control_format` | one control line | strings lie inside the line, are terminated, hold no NUL; integers are in range; `format` then `parse` gives the same message |
| `fuzz_session` | `rbli_ctl_next_line`, `rbli_ctl_fill`, the handshake and the live commands (`rbli_poll_control`) | byte 0 set-up, byte 1 chunk size, then the TCP stream | the events must not depend on how the stream is cut into reads; a token-protected host never connects without the token and answers `unauthorized` first; nothing follows the end of a session; the entity table stays consistent |
| `fuzz_udp` | `rbli_poll_udp`: source and session checks, decode, ordering, entity filter, collision assembly | byte 0 set-up, then records `flags, len16, bytes` sent over a real loopback UDP socket | camera/frame/collision numbers delivered are strictly newer; poses only for spawned entities and never back in time; a snapshot's triangles all come from datagrams of its query; a datagram from an unannounced port yields nothing |
| `fuzz_pixel` | `rbl_pixel_reader_open`, `rbl_pixel_reader_read_frame`, `rbl_pixel_reader_read`, `rbl_pixel_reader_is_gone` | the pixel file, byte for byte | accepts exactly what an independent reference of the header rules accepts; every slot is read back and must equal the file's bytes (colour and depth) or be skipped for the reasons of section 9.6; a truncated file is always a skip |
| `fuzz_endpoint` | `rbl_endpoint_read`, `rbl_endpoint_write` | byte 0 picks the file mode, the rest is the file | a file readable by group or others is refused; an accepted file gives port 1..65535 and a token of 1..256 bytes; write then read gives the same values |

`check_corpus.py` is the other half. A fuzzer finds crashes; it cannot know whether a line *should* have been accepted.
The script re-implements the rules for control lines and endpoint files (`CONTRACT.md` sections 6.2 and 8, as stated by the
vectors and `GAPS.md`) on top of Python's `json` module and compares the library's verdict (through `build/verdict`) over the
vectors, the corpora the fuzzers grew, and a few ten thousand generated lines (mutations of the vectors, number grammar,
nesting, duplicate keys, escapes). It first checks the reference against the vectors, so a wrong reference is caught before it
blames the library.

## Build and run

Needs `clang` with the libFuzzer and sanitizer runtimes (Debian/Ubuntu: `clang` and `libclang-rt-<n>-dev`) and Python 3.

```sh
make -C fuzz all corpus            # harnesses + verdict helper in fuzz/build/, seeds from tests/vectors/link in fuzz/build/corpus/
make -C fuzz smoke                 # every harness over its seeds once, no mutation (a quick regression check)
fuzz/run.sh 1800                   # every harness for 30 minutes, at the same time (or: fuzz/run.sh 600 fuzz_wire fuzz_pixel)
python3 fuzz/check_corpus.py --cases 200000 fuzz/build/work/fuzz_control fuzz/build/work/fuzz_endpoint
```

`make fuzz` at the top of the repository is the same as `make -C fuzz all`. Corpora grow in `fuzz/build/work/<target>/`
(`run.sh` mutates from the seeds and writes the interesting inputs there); a crash is saved in `fuzz/build/crash/`. To look at one:

```sh
fuzz/build/fuzz_wire fuzz/build/crash/fuzz_wire-crash-<hash>
```

`json.dict` is a libFuzzer dictionary for the two JSON harnesses (`run.sh` passes it to all of them; the others ignore what
they cannot use).

## Seeds

`make_corpus.py` writes them from the shared vectors only (`datagrams.json`, `control.json`, `sessions.json`, `pixelfiles.json`
and `pixelfiles/`, `endpoints.json`). Nothing else goes in: no game files and no game data, only the synthetic inputs of the
vectors and what the fuzzers derive from them.

## Notes for adding a harness

* The harnesses `#include "../rewrite_link.h"` with `REWRITE_LINK_IMPLEMENTATION`, so they can reach the `static` functions
  (`rbli_*`) as `tests/test_link.c` does. They need Linux (`/dev/shm`, `socketpair`, `__int128`).
* The oracle must not share code with the library: `fuzz_common.h` has its own little-endian readers and UTF-8 validator.
* Copy the input into an exactly sized heap block (`fuzz_exact_copy`) before handing it over, so that AddressSanitizer sees any read
  past the end.
* A bug found this way gets a regular test in `tests/test_link.c` that fails without the fix, and the fix. The reports are in
  `CLOUD-REPORT.md` at the root of the branch.
