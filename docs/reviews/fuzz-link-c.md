# Review of `cloud/fuzz-link-c` (against `main` 7570d05)

## Verdict: APPROVED (no blockers)

One library fix (`rbli_jnumber`, number range) and one test fix (`ep_path`). Both are correct. I found no new undefined behaviour and no CI regression. Three things could not be checked from this session; they are listed under "Not verified".

## Blockers

None.

## Non-blocking notes

1. `rewrite_link.h:1590` (branch): the branch `else if (dg != '0') return rbli_jfail(...)` is unreachable in effect. When digits remain after all 309 limit digits matched, `below` is 0 and `m >= nlimit`, so the final line rejects anyway. The mutation `dg != '1'` there survives (equivalent mutant). It is harmless; it can be deleted or kept for clarity.
2. `tests/test_link.c` `ep_path`: `snprintf(out, cap, "%s", tmp)` with `tmp[1400]` adds 4 new `-Wformat-truncation` warnings with GCC 13 (23 against 19 on `main`, `-Wall -Wextra -Wpedantic -Wshadow -O2`). CI has no `-Werror`, so it stays green. Clang 18 reports 0 warnings. Using `memmove` or a `size_t` length-checked copy would silence them.
3. The comment in the test and in the header cites IEEE 754-2019 4.3.1, not the contract. I could not read `docs/CONTRACT.md` (see below), so "out of range = rounds to infinity" is justified by the IEEE rule and by agreement with Python's correctly rounded `float()` (below), not by the contract text.
4. `CLOUD-REPORT.md` is accurate about the diff: the bug, its fix and its test match what the diff does. Two claims I could not reproduce: the 31-minute libFuzzer runs and the "86 disagreements on the old code". My own 30000-case `check_corpus.py` run gave 0 disagreements on the branch. The report says "`make asan` … all pass"; confirmed.
5. `fuzz/Makefile` needs the libFuzzer runtime (`libclang_rt.fuzzer`), which is not installed in every clang package (it was missing here). `make -C fuzz` fails with a linker error; it is not part of `make`, so CI is unaffected. Consider a clear message or a gcc/standalone-driver fallback.
6. My blind (non-coverage-guided) driver did not reach the `dexp == 308` branch from the control/endpoint harnesses (those lines uncovered). Coverage-guided libFuzzer with `json.dict` probably does; the number branch is also covered by `check_corpus.py` and the unit test.

## Findings per check

1. **Fix correctness.** Digit string compared with the 309 digits of 2^1024 - 2^970 (verified: `str(2**1024-2**970)` equals `limit`). Alignment is right: at `dexp == 308` the first significant digit is the 10^308 digit, over integer then fraction digits, leading zeros skipped. Tie rejects (roundTiesToEven goes to 2^1024 because the largest finite mantissa is odd). Shorter digit strings count as zeros, below the limit since its last digit is 2. Negative numbers, fractions, `0.xxxe309`, `xxx.0` and trailing zeros are handled. Differential check: 6000 generated numbers around the limit (several spellings, signs, digit counts up to 290 digits away) through the library's own control parser against Python's correctly rounded `float()` finite/infinite: 0 disagreements. Underflow stays accepted (unchanged). Exponent clamp at 100000 is pre-existing and not changed. CONTRACT.md: not readable, see below.
2. **Mutation check.** See table. The new test fails without the fix; the `ep_path` fix is covered by existing endpoint tests.
3. **Sanitizers.** `make asan` (ASan+UBSan unit tests and vectors): pass. Harnesses rebuilt with gcc + ASan + UBSan (`-fno-sanitize-recover`) and a replay/mutation driver of mine, 60 s each over the seed corpora: no crash, sanitizer report or oracle failure.
4. **Harness reach.** gcov of the harnesses (20 s each, blind driver): header lines executed wire 8%, control 18%, endpoint 16%, pixel 6%, session 43%, udp 18% (percent of the whole header including unrelated code). The harness files themselves have no signal handlers, `setjmp`, `_exit`, `fork` or `alarm` that would hide a crash; oracle failures `abort()`. Injecting an off-by-one in `rbl_wire_decode` (`len < RBL_HEADER_LEN - 1`) was caught by `fuzz_wire` as a heap-buffer-overflow within seconds; `payload < 31` for collision was not caught in 30 s by my blind driver (inconclusive; see "Not verified").
5. **Standard, compilers, CI.** `make`, `make test`, `make asan`, `make cxx`, `tests/interop.sh c` pass on the branch. `-std=c99 -Wall -Wextra -Wpedantic -Wshadow` is clean with clang 18; gcc 13 only the warnings of note 2. The header change uses only C99. Windows/MSVC and macOS jobs could not be run here.
6. **Report vs diff.** Consistent (note 4), except `docs/CONTRACT.md`, which the report itself says it did not read.

## Mutations (scratch copies; unit test `a_number_is_out_of_range_exactly_when_it_rounds_to_infinity`, gcc -O1 c99)

| # | Mutation | Result |
|---|---|---|
| M0 | `main` header, branch test | test fails (all "finite" cases refused) |
| M1 | `dg < limit[m]` -> `<=` | killed (line 522) |
| M2 | `dg > limit[m]` -> `>=` | killed (line 519) |
| M3 | `m >= nlimit` -> `m > nlimit` (tie accepted) | killed (line 526) |
| M4 | tail check `dg != '0'` -> `dg != '1'` | survived: equivalent, see note 1 |
| M5 | `dexp == 308` -> `== 307` | killed |
| M6 | do not skip leading zeros | killed (`0.19e309`) |
| M7 | ignore fraction digits | killed |
| M8 | `dexp > 308` -> `> 309` | killed |
| M9 | last digit of the limit 2 -> 3 | killed |
| M10 | limit truncated by one digit | killed |
| M11 | `main` test file (old `ep_path`) with branch header | 3 endpoint tests fail on glibc 2.39 (fix of `ep_path` is needed) |
| H1 | harness: `len < RBL_HEADER_LEN - 1` in `rbl_wire_decode` | `fuzz_wire` ASan heap-buffer-overflow |
| H2 | harness: collision `payload < 31` | not detected in 30 s (blind driver; inconclusive) |

## Not verified

- `docs/CONTRACT.md` of `patodois/rewrite-bridge`: reading it was denied (this session's GitHub scope is `link-c` only). Whether "beyond the range of a double" is meant as "rounds to infinity" is not confirmed from the contract.
- libFuzzer itself: the runtime is not installed (`libclang_rt.fuzzer-x86_64.a` missing), so the real harness runs and the claimed 31-minute results were not reproduced; I used gcc + sanitizers + a blind driver. Coverage numbers are therefore lower bounds. H2 should be retried with libFuzzer.
- Windows/MSVC and macOS jobs.

## Commands run

```
git fetch origin cloud/fuzz-link-c main; git diff origin/main...origin/cloud/fuzz-link-c
make; make test; make asan; make cxx            # in a worktree of the branch
RBL_DEMO=build/demo tests/interop.sh c
python3 tools/check_public_tree.py --range origin/main..HEAD   # on the branch: passed
make -C fuzz all corpus smoke                   # fails: libFuzzer runtime missing
python3 fuzz/make_corpus.py <dir>
gcc -std=gnu99 -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -pthread fuzz/fuzz_*.c drv.c   # drv.c: own replay+mutation driver
gcc --coverage ... ; gcov -b                    # coverage
gcc -std=gnu99 ... fuzz/verdict.c; python3 fuzz/check_corpus.py --cases 30000 --seed 7   # 0 disagreements
python3 (own script): 6000 numbers around 2^1024-2^970 through fuzz/build/verdict vs float()   # 0 disagreements
clang -std=c99 -Wall -Wextra -Wpedantic -Wshadow tests/test_link.c; gcc same, counted warnings (branch 23, main 19)
mutation scripts (sed on scratch copies) M1..M11, H1, H2
```
