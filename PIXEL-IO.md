# Pixel readers and presentation migration

This branch proposes contract revision 7 without changing protocol version 1, datagrams,
file headers, slot offsets or depth layout. Writers retain their mapped publication path.

## Presenting frames

A Frame event is an unvalidated announcement. Use read_latest (Rust/C), ReadLatest (C#),
or readLatest (Java) for presentation. Only a successful current-file copy advances the
number. Raw read APIs remain unordered for inspection and re-reading. Rust accepts an
optional depth destination; the other APIs can omit depth using their existing argument.
Output buffers supplied by callers are valid only on success and may contain partial or
previous bytes after a miss. Clear the displayed image and discard the old reader on a new
channel, session loss or a gone reader, including when opening the replacement fails.
Async results must still belong to the active reader before they are published.

Reader stale counters count duplicate/older nonzero presentation reads: stale_reads() in
Rust, pixel stats.stale in C, SkippedStale in C#, staleReads() in Java. Null frame arguments
are caller misses: C returns RBL_ERR_ARGS, C# and Java return null, without counters or state
changes. Rust's borrowed FrameReady cannot be null. Frame number zero is invalid, not a
presentation-staleness event.

## Unix and Windows I/O

Unix readers use bounded positional I/O and never dereference peer-owned mapped pages.
They validate the header and dimensions, observe the lock, copy requested planes, then read
both lock and frame again. Short reads are completed; EOF and I/O errors reject the entire
copy. The metadata size check also rejects shrinking unused tail bytes. It is an early
rejection, not the protection against later truncation.

Normal cropped planes use up to eight rows per vectored read on macOS/Linux (preadv in
Rust/C/C#, scatter read after positioning the private single-threaded FileChannel in Java).
Data goes directly into owned output in Rust/C/C#; padding goes into reusable scratch and
is discarded. Java reuses packed direct buffers before copying into its array results.
This reduces syscall count without a full-plane scratch allocation. Grouping is limited to
strides up to 16 KiB and spans up to four times the useful output. Wider or very sparse files
keep per-row reads: measurements showed that reading their padding costs more than the
saved syscalls. Full-width frames retain contiguous copies. Other Unix targets and a missing
C# preadv binding use bounded 32 KiB packed spans under the same limits, then per-row reads.
Rust/C retry interruptions and continue partial vectored reads, including within a vector.

Java owned direct scratch is reclaimed by the JVM without invoking Unsafe on Unix reader
close. The writer's existing cleaner path is unchanged. C# caches SafeFileHandle and uses
native positional I/O when available; missing or unsupported bindings switch to safe
FileStream copies with explicit positions and buffering disabled. I/O/provider exceptions
inside CopyAt are contained as misses. The fallback is tested by fault injection, not by
running Unity or IL2CPP.

Windows keeps a read-only map and the existing atomic lock observations: an active section
prevents ordinary truncation. Creation also checks that the actual mapping/file extent did
not shrink before protection took effect. Rust and managed ports read the header using
bounded I/O before mapping; C rechecks the extent with the section active before touching
it. Windows was not run or benchmarked. Java mapped-reader cleanup may depend on GC if the
runtime cannot release its section. Readers are not safe for concurrent close/read; Rust
serializes scratch and presentation state but callers still own reader lifetime.

The revised protocol explicitly distinguishes observation checks from a formal acquire
load: POSIX does not specify pread as an atomic acquire from the writer's memory. Tests
reject detected overlapping writes, not every possible publication violation on every
filesystem. ABA and hostile reuse of lock/frame remain outside that guarantee. I/O can
block. Writers still assume nobody truncates their own mapping. Linux, Windows, WASI,
Mono, Unity and IL2CPP have not been run in this task.

## Benchmark method

The benchmark repeats raw reads of one valid slot, so it measures copies rather than the
ordered API rejecting duplicates. It does not include presentation-order locking or counters.
Synthetic local files, warm cache, two warm-up reads, 30 samples for normal/cropped frames
and 5 for maximum-sized outputs. Times include header validation, requested planes and final
identity checks; exclude writer, GPU, IPC and rendering. Rust/C reuse destination buffers;
C#/Java allocate results according to their public APIs, with GC requested outside timed
regions. Other processes may compete for CPU and memory. p95 with five samples is only the
largest observed value. Do not compare allocation policies across languages as if identical.

Measured on macOS arm64. All numbers are milliseconds per frame.

| File | Frame | Depth | Before p50 | After p10 | After p50 | After p95 |
|---|---|---|---:|---:|---:|---:|
| 1280x720 | 1280x720 | false | 0.2060 | 0.1800 | 0.1910 | 0.2360 |
| 1280x720 | 1280x720 | true | 0.5140 | 0.3350 | 0.3380 | 0.5710 |
| 1920x1080 | 1920x1080 | false | 0.5260 | 0.2960 | 0.2990 | 0.3490 |
| 1920x1080 | 1920x1080 | true | 1.5890 | 0.6570 | 0.6630 | 0.6950 |
| 3840x2160 | 3840x2160 | false | 3.4930 | 1.6980 | 1.7110 | 1.7870 |
| 3840x2160 | 3840x2160 | true | 7.0340 | 3.3110 | 3.3340 | 3.4890 |
| 8192x8192 | 8192x8192 | false | 27.4190 | 14.2830 | 14.4100 | 14.4930 |
| 8192x8192 | 8192x8192 | true | 58.6750 | 29.0660 | 32.1280 | 36.8160 |
| 16384x1 | 16384x1 | false | 0.0040 | 0.0020 | 0.0030 | 0.0030 |
| 16384x1 | 16384x1 | true | 0.0080 | 0.0040 | 0.0050 | 0.0050 |
| 16384x4096 | 16384x4096 | false | 36.0720 | 14.2380 | 14.2760 | 14.4090 |
| 16384x4096 | 16384x4096 | true | 80.9000 | 28.5400 | 28.6270 | 28.7380 |
| 1920x1080 | 1280x720 | false | 0.7650 | 0.1370 | 0.1380 | 0.1530 |
| 1920x1080 | 1280x720 | true | 1.9190 | 0.2910 | 0.2940 | 0.3080 |
| 3840x2160 | 1920x1080 | false | 1.9730 | 0.4030 | 0.4110 | 0.4380 |
| 3840x2160 | 1920x1080 | true | 4.5960 | 1.1530 | 1.1890 | 1.2860 |
| 16384x4096 | 8192x4096 | false | 22.0070 | 8.4820 | 8.5330 | 8.5620 |
| 16384x4096 | 8192x4096 | true | 41.4710 | 17.3300 | 17.4380 | 17.5190 |
| 16384x4096 | 1x4096 | false | 2.5180 | 1.1300 | 1.1500 | 1.1730 |
| 16384x4096 | 1x4096 | true | 5.2360 | 2.2680 | 2.2950 | 2.3310 |

## Matched cropped-frame comparison

The same final API was built with grouping disabled and enabled, one language at a time.
These sequential runs separate the optimization from other reader changes. Scheduling and
memory pressure still vary. Whole-plane scratch and unrestricted grouping were rejected
after measurements showed regressions on wide files. A one-column frame remains per-row.

| File | Frame | Depth | Row-wise p10 / p50 / p95 | Bounded p10 / p50 / p95 |
|---|---|---|---:|---:|
| 1920x1080 | 1280x720 | false | 0.4950 / 0.5710 / 0.7250 | 0.3320 / 0.3840 / 0.4830 |
| 1920x1080 | 1280x720 | true | 0.7640 / 0.8420 / 1.1420 | 0.5020 / 0.5700 / 0.6680 |
| 3840x2160 | 1920x1080 | false | 0.5550 / 0.5900 / 0.6360 | 0.5340 / 0.5580 / 0.6250 |
| 3840x2160 | 1920x1080 | true | 1.1780 / 1.2110 / 1.4840 | 1.2020 / 1.2280 / 1.3110 |
| 16384x4096 | 8192x4096 | false | 8.7400 / 8.7620 / 8.8070 | 8.6980 / 8.7260 / 8.7540 |
| 16384x4096 | 8192x4096 | true | 17.6700 / 17.8150 / 17.8920 | 17.6730 / 17.7810 / 17.8150 |
| 16384x4096 | 1x4096 | false | 1.1470 / 1.1640 / 1.1760 | 1.1280 / 1.1410 / 1.1690 |
| 16384x4096 | 1x4096 | true | 2.3340 / 2.3440 / 2.3630 | 2.2730 / 2.3050 / 2.3280 |
