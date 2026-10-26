#!/usr/bin/env bash
# Two clients against one innkeeperd, both with every lifted function shadow-verified: the sibling of
# verify_lifted.sh for multiplayer paths (docs/runtime.md, "Input scripts").
# usage: verify_two.sh <build_dir> <server_port> <run_ms> <calls_per_function> <scriptA> <scriptB>
# Client A copies INN_DRIVE (default work/inn-c), client B copies INN_DRIVE_B (default work/inn-c-p2, a
# second account and persona); each gets its own drive copy work/verify-drive/<port>a and <port>b, and its
# output directory work/verify/<port>/a and /b (client.log, play.txt, screenshots). @OUT@ in a script
# becomes that client's directory. The scripts meet through "signal <name>" and "wait <name> [timeout_ms]"
# (docs/runtime.md, "Input scripts"); their sync directory is work/verify/<port>/sync, emptied first.
# INN_ARGS and INN_ARGS_B add frontend options to A and B; INN_SYNTH and INN_EPOCH work as in
# verify_lifted.sh; INN_COVERAGE=1 writes coverage.txt into each client's directory, any other value is a
# file name that gets .a and .b appended. INN_PROGRAM names the program as
# there. The exit status is non-zero when either client hits a fatal error, a script failure (a wait that
# timed out) or the wall-clock timeout.
set -euo pipefail
export INN_EPOCH=${INN_EPOCH-750000000} TZ=UTC
[ $# -eq 6 ] || { sed -n '4p' "$0" >&2; exit 2; }
BUILD=$1
PORT=$2
RUN_MS=$3
CALLS=$4
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SERVER=${INNKEEPERD:-$ROOT/../TSN-INN-Decomp/target/debug/innkeeperd}
OUT=$ROOT/work/verify/$PORT
mkdir -p "$OUT"
exec 9> "$OUT/.lock"
flock -n 9 || { echo "port $PORT is in use by another verify run" >&2; exit 2; }
SYNC=$OUT/sync
rm -rf "$SYNC"
mkdir -p "$SYNC"
read -r -a PROGRAM <<< "${INN_PROGRAM:-INN\\TSNEXEC.EXE DEFAULT}"

prepare() { # <side> <script> <drive>
    local dir=$OUT/$1 drive=$ROOT/work/verify-drive/$PORT$1
    rm -rf "$dir"
    mkdir -p "$dir" "$drive"
    sed "s#@OUT@#$dir#g" "$2" > "$dir/play.txt"
    rsync -a --delete --delete-excluded --exclude 'VMF/*' "$3/" "$drive/"
}
prepare a "$5" "${INN_DRIVE:-$ROOT/work/inn-c}"
prepare b "$6" "${INN_DRIVE_B:-$ROOT/work/inn-c-p2}"

"$SERVER" --bind 127.0.0.1:"$PORT" --no-int14h --no-capture > "$OUT/server.log" 2>&1 &
SERVER_PID=$!
PIDS=()
trap 'kill $SERVER_PID "${PIDS[@]}" 2>/dev/null || true' EXIT
sleep 1

coverage_file() { # <side>
    case ${INN_COVERAGE:-} in
    "") ;;
    1) echo "$OUT/$1/coverage.txt" ;;
    *) echo "$INN_COVERAGE.$1" ;;
    esac
}

run_client() { # <side> <extra args>
    local dir=$OUT/$1 coverage=
    coverage=$(coverage_file "$1")
    local rc=0
    # shellcheck disable=SC2086
    INN_COVERAGE=$coverage INN_VERIFY_LIFTED=$CALLS timeout 900 "$BUILD/inn" $2 --headless \
        --run-ms "$RUN_MS" --script "$dir/play.txt" --sync-dir "$SYNC" --com1 127.0.0.1:"$PORT" \
        "$ROOT/work/verify-drive/$PORT$1" "${PROGRAM[@]}" > "$dir/client.log" 2>&1 || rc=$?
    echo "$rc" > "$dir/exit_status"
}
run_client a "${INN_ARGS:-}" &
PIDS+=($!)
run_client b "${INN_ARGS_B:-}" &
PIDS+=($!)
wait "${PIDS[@]}" || true
PIDS=()

STATUS=0
for side in a b; do
    dir=$OUT/$side
    for shot in "$dir"/*.ppm; do
        [ -e "$shot" ] && python3 -I "$ROOT/tools/ppm2png.py" "$shot" "${shot%.ppm}.png" 2>/dev/null || true
    done
    echo "== client $side"
    grep -E '^verify:' "$dir/client.log" | sort | uniq -c | sort -rn | head -200 || true
    rc=$(cat "$dir/exit_status")
    problems=$(grep -E '^fatal:|^script: (wait .* timed out|.* needs --sync-dir|signal .* cannot create)' "$dir/client.log" || true)
    if [ "$rc" = 124 ] || [ -n "$problems" ]; then
        STATUS=1
        echo "client $side FAILED (exit $rc)"
        [ "$rc" = 124 ] && echo "  timed out after 900 s of wall time"
        [ -n "$problems" ] && sed 's/^/  /' <<< "$problems"
    fi
    if [ -n "${INN_COVERAGE:-}" ]; then echo "coverage: $(coverage_file $side)"; fi
done
echo "logs and screenshots: $OUT (a, b, server.log, sync)"
exit $STATUS
