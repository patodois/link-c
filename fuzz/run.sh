#!/bin/sh
# Runs the harnesses at the same time for SECONDS each (default 1800) and prints a summary.
#   fuzz/run.sh [SECONDS [target ...]]      targets default to all of them; run `make -C fuzz all corpus` first
# Corpora grow in fuzz/build/work/<target>/, logs in fuzz/build/work/<target>.log, crashing inputs in fuzz/build/crash/.
set -u
here=$(cd "$(dirname "$0")" && pwd)
secs=${1:-1800}
[ $# -gt 0 ] && shift
targets=${*:-"fuzz_wire fuzz_control fuzz_endpoint fuzz_pixel fuzz_session fuzz_udp"}
mkdir -p "$here/build/work" "$here/build/crash"
pids=""
for t in $targets; do
    case $t in
    fuzz_wire) len=9300 ;;
    fuzz_endpoint) len=4300 ;;
    fuzz_udp) len=20000 ;;
    *) len=70000 ;;
    esac
    mkdir -p "$here/build/work/$t"
    "$here/build/$t" -max_len=$len -max_total_time="$secs" -timeout=60 -dict="$here/json.dict" \
        -artifact_prefix="$here/build/crash/$t-" "$here/build/work/$t" "$here/build/corpus/$t" >"$here/build/work/$t.log" 2>&1 &
    pids="$pids $!"
done
status=0
for p in $pids; do wait "$p" || status=1; done
for t in $targets; do
    printf '%-14s %s\n' "$t" "$(grep -E '^Done [0-9]+ runs' "$here/build/work/$t.log" | tail -1)"
done
ls "$here/build/crash" 2>/dev/null | sed 's/^/crash: /'
exit $status
