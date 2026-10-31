# CLOUD-REPORT: fuzzing the link parsers of link-c

Branch `cloud/fuzz-link-c`, built on `main` (contract revision 6). Nothing was merged, no pull request opened, no other repository touched.

## What was done

Six libFuzzer harnesses (clang 18, `-fsanitize=fuzzer,address,undefined`, no recovery from UB) in `fuzz/`, seeded only from `tests/vectors/link`:

| Target | Entry points | Oracle |
|---|---|---|
| `fuzz_wire` | `rbl_wire_decode`, `rbl_wire_pose/triangle`, `rbl_wire_encode_*` | independent reference of the length/range rules; field-by-field equality with the bytes; re-encode round trip |
| `fuzz_control` | `rbl_control_parse`, `rbl_control_format` | string/integer invariants; format-then-parse round trip |
| `fuzz_session` | line framing, read buffer, handshake (token, role, ports, heartbeat), live commands | same events whatever the read chunking; no connection without the token and `unauthorized` first; nothing after end of session; entity table consistent |
| `fuzz_udp` | UDP receive path over a real loopback socket, collision assembly | ordering (RFC 1982) of camera/frame/query, pose entity filter and time, snapshot triangles come from its query, foreign port yields nothing |
| `fuzz_pixel` | `rbl_pixel_reader_open`, `_read_frame`, `_read`, `_is_gone` | independent reference of the header rules (128-bit arithmetic); every slot read back and compared byte for byte with the file; near misses must be skips; truncated file is always a skip |
| `fuzz_endpoint` | `rbl_endpoint_read`, `rbl_endpoint_write` | mode & 077 refused; port/token ranges; write-then-read round trip |

`fuzz/check_corpus.py` + `fuzz/verdict.c` compare accept/reject and decoded values of the control parser and the endpoint reader with a reference built on Python's `json` (validated against all vectors first), over the vectors, the grown corpora and about 150 000 generated cases (mutations, number grammar, nesting, duplicate keys, escapes). `make -C fuzz all corpus smoke`, `fuzz/run.sh SECONDS`, `make fuzz` at the top level. See `fuzz/README.md`.

## Fuzzing time

All six ran at the same time on 4 cores, 1861 s (31 min) each, with `fuzz/json.dict`:

| Target | Executions |
|---|---|
| fuzz_wire | 171 867 243 |
| fuzz_control | 42 172 731 |
| fuzz_endpoint | 46 689 601 |
| fuzz_pixel | 8 836 127 |
| fuzz_session | 1 423 934 |
| fuzz_udp | 8 196 527 |

Plus short earlier runs (2 to 2.5 min each) of the first versions of the harnesses. These long runs were done with the final harnesses and the fixed header. No crash, sanitizer report, hang or oracle failure came out of any of them. After the runs, `check_corpus.py` over the grown corpora: 0 disagreements.

## Bugs

1. **Number out of range, boundary (contract violation).**
   - Input shape: any JSON number in a control line or endpoint file whose value is in [1.7976931348623158e308, 2^1024 − 2^970), for example `{"type":"bye","x":1.7976931348623158e308}`.
   - Failure: refused as "number out of range" (the code compared only the first 17 digits with 17976931348623158). Those values round to the largest finite double, so they are in range; a number is out of range only when it rounds to infinity (IEEE 754-2019 4.3.1, roundTiesToEven: at or above 2^1024 − 2^970).
   - Fix: compare the significant digits with the 309 digits of 2^1024 − 2^970 (`rbli_jnumber`, `rewrite_link.h`).
   - Test: `a_number_is_out_of_range_exactly_when_it_rounds_to_infinity` in `tests/test_link.c` (fails without the fix, passes with it).
   - Found by: `check_corpus.py` (86 disagreements on the old code, 0 after).
2. **Test bug, not library (CI).** `ep_path()` in `tests/test_link.c` was called with the same buffer as destination and source of `snprintf` (undefined). With glibc 2.39 three endpoint tests failed on the unmodified `main`. `ep_path` now builds the path in a copy. No library change.

## Left out, and why

- **`docs/CONTRACT.md` was not read.** This session's GitHub access was limited to `link-c`, so I did not clone `rewrite-bridge`. Rules come from the vectors' README, `GAPS.md`, the vectors and the header comments. The reference oracles apply only those; where they are silent nothing is checked.
- **Bug 1's premise** (that the contract's "beyond the range of a double" means "rounds to infinity") is taken from the code comment and from IEEE 754, not from the contract text. I believe it is right but could not confirm it.
- `rbl_path_is_absolute`: a six-line pure function fully described by its header comment; exercised through `fuzz_pixel`/the tests, no harness of its own.
- The pixel *writer*, the senders (`rbl_link_send_*`), and the camera/quaternion math do not parse untrusted input; not fuzzed.
- Real TCP accept/timeouts/`busy` handling and the watchdog are not fuzzed (they depend on time and real connections); `sessions.json` covers them. Note for the maintainers, not changed: any local process can hold a host's handshake slot until the timeout, and a second connection gets `busy` before authenticating.
- Collision assembly has property checks (ordering, provenance, limits), not a full model of section 9.5, so a snapshot that is wrongly *not* delivered would go unseen.
- No Windows/MSVC or macOS run; the harnesses are Linux-only. The header change uses only C99 constructs and was built as C99 and C++11 (`make`, `make cxx`).

## Uncertain

- fuzz_session ran only 1.4 M executions (about 760/s: two runs of every input, a socket pair each); coverage had plateaued, but this is the least-exercised target.
- Throughput was shared by six processes on 4 cores.
- The three clean sessions of 31 minutes say little about deeper states; more time or a structure-aware mutator for sessions would be the next step.

## Checks run

`make`, `make test` (69 tests and all vector files), `make asan`, `make cxx`, `make -C fuzz smoke` and `python3 tools/check_public_tree.py --range origin/main..HEAD` all pass.
