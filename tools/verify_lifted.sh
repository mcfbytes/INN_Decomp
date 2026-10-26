#!/usr/bin/env bash
# Runs the client with every lifted function shadow-verified: boot, Play, log on, reach the map.
# Uses a per-port copy of work/inn-c, so concurrent runs never share swap or player files.
# usage: verify_lifted.sh <build_dir> <server_port> [run_ms] [calls_per_function] [script]
# The script (input_script format) replaces the logon one; @OUT@ in it becomes the output directory.
# INN_ARGS adds frontend options, e.g. INN_ARGS='--conv-kb 520'. INN_DRIVE picks the C: drive to copy
# (default work/inn-c; the Yserbius scenarios need one with \INN\YSERBIUS, docs/yserbius.md).
# INN_TSN_HOST=<rules> runs without a server: dosrt's TSN host stand-in plays the host
# (docs/runtime.md), and INN_PROGRAM names the program and its arguments (default
# 'INN\TSNEXEC.EXE DEFAULT'); for Red Baron see docs/redbaron.md. The port then only names the run.
# INN_COVERAGE=1 writes the call coverage (docs/lifting.md, "Coverage") to <output dir>/coverage.txt;
# any other value is used as the file name.
# VERIFY_TIMEOUT=<seconds> limits the client's wall time (default 900; long scenarios at 100000 calls
# need more).
# INN_SYNTH=<call lists> adds synthetic calls (docs/lifting.md, "Synthetic calls"); the environment passes it.
# Runs repeat: the emulated clock starts at INN_EPOCH (default 750000000, 8 Oct 1993) in UTC, unless
# INN_EPOCH= (empty) asks for the host clock.
set -euo pipefail
export INN_EPOCH=${INN_EPOCH-750000000} TZ=UTC
BUILD=$1
PORT=$2
RUN_MS=${3:-95000}
CALLS=${4:-50}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SERVER=${INNKEEPERD:-$ROOT/../TSN-INN-Decomp/target/debug/innkeeperd}
OUT=$ROOT/work/verify/$PORT
mkdir -p "$OUT"
exec 9> "$OUT/.lock"
flock -n 9 || { echo "port $PORT is in use by another verify run" >&2; exit 2; }
if [ -n "${5:-}" ]; then
    sed "s#@OUT@#$OUT#g" "$5" > "$OUT/play.txt"
else
    cat > "$OUT/play.txt" <<SCRIPT
11000 click 285 59
60000 shot $OUT/map.ppm
SCRIPT
fi
# Each run gets its own C: drive: the client writes swap and player files there.
DRIVE=$ROOT/work/verify-drive/$PORT
mkdir -p "$DRIVE"
rsync -a --delete --delete-excluded --exclude 'VMF/*' "${INN_DRIVE:-$ROOT/work/inn-c}/" "$DRIVE/"
read -r -a PROGRAM <<< "${INN_PROGRAM:-INN\\TSNEXEC.EXE DEFAULT}"
if [ -n "${INN_TSN_HOST:-}" ]; then
    HOST_ARGS=(--tsn-host "$INN_TSN_HOST")
else
    "$SERVER" --bind 127.0.0.1:"$PORT" --no-int14h --no-capture > "$OUT/server.log" 2>&1 &
    SERVER_PID=$!
    trap 'kill $SERVER_PID 2>/dev/null || true' EXIT
    sleep 1
    HOST_ARGS=(--com1 127.0.0.1:"$PORT")
fi
COVERAGE=${INN_COVERAGE:-}
[ "$COVERAGE" = 1 ] && COVERAGE=$OUT/coverage.txt
INN_COVERAGE=$COVERAGE INN_VERIFY_LIFTED=$CALLS timeout "${VERIFY_TIMEOUT:-900}" "$BUILD/inn" ${INN_ARGS:-} --headless \
    --run-ms "$RUN_MS" --script "$OUT/play.txt" "${HOST_ARGS[@]}" "$DRIVE" "${PROGRAM[@]}" \
    2>&1 | { head -c 256M > "$OUT/client.log"; tail -c 64M > "$OUT/client.tail"; } || true
# A run that logs without end (a mismatch on every call) keeps its first 256 MiB and last 64 MiB only.
if [ -s "$OUT/client.tail" ]; then
    echo "[client.log cut: the first 256 MiB, then the last 64 MiB]" >> "$OUT/client.log"
    cat "$OUT/client.tail" >> "$OUT/client.log"
fi
rm -f "$OUT/client.tail"
python3 -I "$ROOT/tools/ppm2png.py" "$OUT/map.ppm" "$OUT/map.png" 2>/dev/null || true
grep -E '^verify:' "$OUT/client.log" | sort | uniq -c | sort -rn | head -200 || true
[ -n "$COVERAGE" ] && echo "coverage: $COVERAGE"
echo "logs and the map screenshot: $OUT"
