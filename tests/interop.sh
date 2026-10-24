#!/usr/bin/env bash
# Interop of the C demo against the other implementations of the link protocol, both roles.
#
#   RBL_DEMO=build/demo \
#   RBL_FAKE_HOST=.../fake_host RBL_FAKE_GUEST=.../fake_guest \      (rewrite-bridge: cargo build --release --features link --examples)
#   RBL_JAVA_CP=.../link-java/build/classes/java/main \               (optional; RBL_JAVA is the java command, default "java")
#   RBL_LOG_DIR=/some/dir \                                           (default: a new temp directory)
#   tests/interop.sh [peer ...]                                       (peers to test against: rust java c; default: all that are configured)
#
# Each pair runs twice: "basic" (guest with a pixel file and depth, the host's ground collision patch,
# spawn, poses, the focus request) and "big" (the same with a 400-chunk collision snapshot every second
# from the host). Exit status 1 if any check fails.
set -u
DEMO="${RBL_DEMO:-build/demo}"
JAVA="${RBL_JAVA:-java}"
LOG_DIR="${RBL_LOG_DIR:-$(mktemp -d)}"
SECS="${RBL_SECONDS:-5}"
PORT="${RBL_BASE_PORT:-47900}"
mkdir -p "$LOG_DIR"

have_rust=0; [ -n "${RBL_FAKE_HOST:-}" ] && [ -n "${RBL_FAKE_GUEST:-}" ] && have_rust=1
have_java=0; [ -n "${RBL_JAVA_CP:-}" ] && have_java=1

# start_peer ROLE PEER PORT SECONDS LOG [extra args...]
start_peer() {
  local role=$1 peer=$2 port=$3 secs=$4 log=$5; shift 5
  case "$peer" in
    c)    "$DEMO" "$role" --port "$port" --seconds "$secs" "$@" > "$log" 2>&1 & ;;
    rust) if [ "$role" = host ]; then "$RBL_FAKE_HOST" --port "$port" --seconds "$secs" "$@" > "$log" 2>&1 &
          else "$RBL_FAKE_GUEST" --port "$port" --seconds "$secs" "$@" > "$log" 2>&1 & fi ;;
    java) "$JAVA" -cp "$RBL_JAVA_CP" dev.patodois.rewritelink.LinkDemo "$role" --port "$port" --seconds "$secs" "$@" > "$log" 2>&1 & ;;
  esac
}

fails=0
check() { # check NAME LOGFILE EXTENDED-REGEX
  if grep -Eq "$3" "$2"; then echo "    ok    $1"; else echo "    FAIL  $1   (no match for /$3/ in $2)"; fails=$((fails + 1)); fi
}
check_not() { # check_not NAME LOGFILE EXTENDED-REGEX
  if grep -Eq "$3" "$2"; then echo "    FAIL  $1   (matched /$3/ in $2)"; fails=$((fails + 1)); else echo "    ok    $1"; fi
}

run_case() { # run_case HOST_PEER GUEST_PEER SCENARIO
  local hp=$1 gp=$2 scenario=$3
  local name="$hp-host+$gp-guest-$scenario"
  local hl="$LOG_DIR/$name.host.log" gl="$LOG_DIR/$name.guest.log"
  local port=$PORT; PORT=$((PORT + 1))
  local hargs=() gargs=(--frames)
  local tris=800
  if [ "$scenario" = big ]; then hargs=(--big-collision 400); tris=90400; fi
  echo "== $hp host <-> $gp guest ($scenario)"
  start_peer host "$hp" "$port" $((SECS + 2)) "$hl" ${hargs[@]+"${hargs[@]}"}
  local hpid=$!
  sleep 0.8
  start_peer guest "$gp" "$port" "$SECS" "$gl" ${gargs[@]+"${gargs[@]}"}
  local gpid=$!
  wait "$gpid"; wait "$hpid"
  check "host sees the guest"                    "$hl" 'connected: guest'
  check "guest sees the host"                    "$gl" 'connected: host'
  check "guest spawn arrives"                    "$hl" 'guest spawned entity 1 '
  check "poses reach the host"                   "$hl" 'got [1-9][0-9]* poses'
  check "cameras reach the guest"                "$gl" 'got [1-9][0-9]* cameras'
  check "focus request, grant and echo"          "$hl" 'guest asks for input focus'
  check "the host's grant reaches the guest"     "$gl" 'host says input focus is now with the guest'
  check "the collision snapshot completes ($tris triangles)" "$gl" "collision query [0-9]+: $tris triangles"
  check "the guest sent frames"                  "$gl" 'sent [0-9]+ poses, [1-9][0-9]* frames'
  if [ "$hp" = rust ]; then
    check "the Rust host read the frames"        "$hl" 'poses in [0-9]+ datagrams, [1-9][0-9]* frames'
    check "the Rust host reprojected them"       "$hl" 'camera seq [0-9]+, reprojected'
  else
    check "the host read frames with depth"      "$hl" 'frames read [1-9][0-9]*, missed [0-9]+; last: frame [0-9]+ 320x240 camera_seq [0-9]+, [1-9][0-9]* drawn pixels, [1-9][0-9]* with finite depth \([0-9.]+\.\.[0-9.]+ m\)'
    # the Java and C demo guests draw a disc 3 m away, so the depth that arrives must be exactly that
    if [ "$gp" = c ] || [ "$gp" = java ]; then check "the depth plane carries 3 m"  "$hl" 'with finite depth \(3\.00\.\.3\.00 m\)'; fi
  fi
  # the guest ends first and says bye ("peer said bye"): that is a clean end, anything else is a failure
  check     "the guest's end arrives as a bye"   "$hl" 'LOST the guest: peer said bye'
  check_not "no failed session or handshake"     "$hl" 'LOST the guest: (watchdog|control|protocol|connection|no hello|peer claims)|handshake failed'
  check_not "no failed session (guest)"          "$gl" 'LOST the host|handshake failed'
  check_not "no malformed or foreign datagrams"  "$hl" 'malformed [1-9]|foreign [1-9]'
  check_not "no unknown entity poses"            "$hl" 'unknown_entity [1-9]'
}

peers=("$@")
if [ ${#peers[@]} -eq 0 ]; then
  peers=(c)
  [ $have_rust = 1 ] && peers+=(rust)
  [ $have_java = 1 ] && peers+=(java)
fi
for p in "${peers[@]}"; do
  case "$p" in
    c)    ;;
    rust) [ $have_rust = 1 ] || { echo "rust peers need RBL_FAKE_HOST and RBL_FAKE_GUEST"; exit 2; } ;;
    java) [ $have_java = 1 ] || { echo "java peers need RBL_JAVA_CP"; exit 2; } ;;
    *)    echo "unknown peer $p"; exit 2 ;;
  esac
  for scenario in basic big; do
    if [ "$p" = c ]; then
      run_case c c "$scenario"
    else
      run_case c "$p" "$scenario"
      run_case "$p" c "$scenario"
    fi
  done
done

echo
if [ "$fails" -eq 0 ]; then echo "interop: all checks passed (logs in $LOG_DIR)"; else echo "interop: $fails check(s) FAILED (logs in $LOG_DIR)"; exit 1; fi
