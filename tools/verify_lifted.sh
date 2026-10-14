#!/usr/bin/env bash
# Runs the client with every lifted function shadow-verified: boot, Play, log on, reach the map.
# usage: verify_lifted.sh <build_dir> <server_port> [run_ms] [calls_per_function]
set -euo pipefail
BUILD=$1
PORT=$2
RUN_MS=${3:-95000}
CALLS=${4:-50}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SERVER=${INNKEEPERD:-$ROOT/../TSN-INN-Decomp/target/debug/innkeeperd}
OUT=$ROOT/work/verify/$PORT
mkdir -p "$OUT"
cat > "$OUT/play.txt" <<SCRIPT
11000 click 285 59
60000 shot $OUT/map.ppm
SCRIPT
"$SERVER" --bind 127.0.0.1:"$PORT" --no-int14h --no-capture > "$OUT/server.log" 2>&1 &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null || true' EXIT
sleep 1
INN_VERIFY_LIFTED=$CALLS timeout 900 "$BUILD/inn" --headless --run-ms "$RUN_MS" --script "$OUT/play.txt" \
    --com1 127.0.0.1:"$PORT" "$ROOT/work/inn-c" 'INN\TSNEXEC.EXE' DEFAULT > "$OUT/client.log" 2>&1 || true
python3 -I "$ROOT/tools/ppm2png.py" "$OUT/map.ppm" "$OUT/map.png" 2>/dev/null || true
grep -E '^verify:' "$OUT/client.log" | sort | uniq -c | sort -rn | head -200 || true
echo "logs and the map screenshot: $OUT"
