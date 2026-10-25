#!/usr/bin/env bash
# Runs several dosrt clients against one innkeeperd and checks the logs afterwards.
# usage: multi_client.sh <build_dir> <port> <manifest> [run_ms_default]
# Manifest format and an example: docs/runtime.md, "Several clients against one server".
# INNKEEPERD names the server binary. The environment passes through to the clients; INN_VERIFY_LIFTED
# (or 50 calls when only INN_VERIFY_TAGS is set) applies just to the tags listed in INN_VERIFY_TAGS
# (space or comma separated, "all" for every client). INN_ARGS adds frontend options, INN_PROGRAM
# names the program (default 'INN\TSNEXEC.EXE DEFAULT'), INN_EPOCH as in verify_lifted.sh.
set -uo pipefail
export INN_EPOCH=${INN_EPOCH-750000000} TZ=UTC
BUILD=$1
PORT=$2
MANIFEST=$3
RUN_MS_DEFAULT=${4:-95000}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SERVER=${INNKEEPERD:-$ROOT/../TSN-INN-Decomp/target/debug/innkeeperd}
MDIR=$(cd "$(dirname "$MANIFEST")" && pwd)
OUT=$ROOT/work/verify/$PORT
DEFAULT_DRIVE=$ROOT/work/srv-games-c
mkdir -p "$OUT"
exec 9> "$OUT/.lock"
flock -n 9 || { echo "port $PORT is in use by another run" >&2; exit 2; }
rm -rf "$OUT/data" "$OUT/captures" "$OUT/sync" "$OUT/server.log"
mkdir -p "$OUT/data" "$OUT/captures" "$OUT/sync"

abs() { case $1 in /*) echo "$1" ;; *) echo "$MDIR/$1" ;; esac; }
sub() { sed -e "s#@OUT@#$2#g" -e "s#@SYNC@#$OUT/sync#g" -e "s#@PORT@#$PORT#g" "$1"; }
strip_ansi() { sed -E 's/\x1b\[[0-9;]*[A-Za-z]//g' "$1"; }

SERVER_ARGS=()
CLIENTS=() COMMANDS=() CHECKS=()
while IFS= read -r line || [ -n "$line" ]; do
    line=${line%%#*}
    line=$(echo "$line" | sed -E 's/^[[:space:]]+|[[:space:]]+$//g')
    [ -z "$line" ] && continue
    kind=${line%% *}
    rest=${line#"$kind"}
    rest=${rest# }
    case $kind in
    server) read -r -a more <<< "$rest"; SERVER_ARGS+=("${more[@]}") ;;
    client) CLIENTS+=("$rest") ;;
    command) COMMANDS+=("$rest") ;;
    expect | reject) CHECKS+=("$kind $rest") ;;
    *) echo "manifest: unknown line '$line'" >&2; exit 2 ;;
    esac
done < "$MANIFEST"
[ ${#CLIENTS[@]} -gt 0 ] || { echo "manifest: no client lines" >&2; exit 2; }

"$SERVER" --bind 127.0.0.1:"$PORT" --no-int14h --data-dir "$OUT/data" --capture-dir "$OUT/captures" \
    "${SERVER_ARGS[@]}" > "$OUT/server.log" 2>&1 &
SERVER_PID=$!
PIDS=()
cleanup() { kill "$SERVER_PID" $(jobs -p) 2>/dev/null || true; }
trap cleanup EXIT
sleep 1
kill -0 "$SERVER_PID" 2>/dev/null || { echo "server did not start:" >&2; tail -5 "$OUT/server.log" >&2; exit 2; }

read -r -a PROGRAM <<< "${INN_PROGRAM:-INN\\TSNEXEC.EXE DEFAULT}"
VERIFY_TAGS=",$(echo "${INN_VERIFY_TAGS:-}" | tr ' ' ',' ),"
VERIFY_CALLS=${INN_VERIFY_LIFTED:-50}

run_client() {
    local tag=$1 account=$2 script=$3 delay=${4:-0} drive=${5:--} runms=${6:-$RUN_MS_DEFAULT}
    local dir=$OUT/$tag copy=$ROOT/work/verify-drive/$PORT-$tag
    [ "$drive" = - ] && drive=$DEFAULT_DRIVE || drive=$(abs "$drive")
    rm -rf "${dir:?}"
    mkdir -p "$dir" "$copy"
    sub "$(abs "$script")" "$dir" > "$dir/play.txt"
    rsync -a --delete --delete-excluded --exclude 'VMF/*' "$drive/" "$copy/"
    sed -i -E "s/^( *id = ).*(\r?)\$/\1$account\2/" "$copy/INN/LSCI.CFG"
    [ "$delay" -gt 0 ] && sleep "$(awk "BEGIN{print $delay/1000}")"
    local verify=0
    case $VERIFY_TAGS in *,"$tag",* | *,all,*) verify=$VERIFY_CALLS ;; esac
    (
        unset INN_VERIFY_LIFTED
        [ "$verify" -gt 0 ] && export INN_VERIFY_LIFTED=$verify
        export INN_SYNC_DIR=$OUT/sync
        timeout 900 "$BUILD/inn" ${INN_ARGS:-} --headless --run-ms "$runms" --script "$dir/play.txt" \
            --com1 127.0.0.1:"$PORT" "$copy" "${PROGRAM[@]}" > "$dir/client.log" 2>&1
        echo $? > "$dir/exit"
    )
}

for spec in "${CLIENTS[@]}"; do
    read -r -a f <<< "$spec"
    [ ${#f[@]} -ge 3 ] || { echo "manifest: bad client line '$spec'" >&2; exit 2; }
    run_client "${f[@]}" &
    PIDS+=($!)
done
for spec in "${COMMANDS[@]}"; do
    tag=${spec%% *}
    cmd=${spec#"$tag"}
    rm -rf "${OUT:?}/${tag:?}"
    mkdir -p "$OUT/$tag"
    cmd=$(echo "$cmd" | sed -e "s#@OUT@#$OUT/$tag#g" -e "s#@SYNC@#$OUT/sync#g" -e "s#@PORT@#$PORT#g")
    ( export INN_SYNC_DIR=$OUT/sync; bash -c "$cmd" > "$OUT/$tag/command.log" 2>&1; echo $? > "$OUT/$tag/exit" ) &
    PIDS+=($!)
done
wait "${PIDS[@]}" 2>/dev/null
kill "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true

FAIL=0
echo "== summary (port $PORT, output $OUT)"
for spec in "${CLIENTS[@]}" "${COMMANDS[@]/#/command }"; do
    read -r -a f <<< "$spec"
    [ "${f[0]}" = command ] && tag=${f[1]} || tag=${f[0]}
    for ppm in "$OUT/$tag"/*.ppm; do
        [ -e "$ppm" ] && python3 -I "$ROOT/tools/ppm2png.py" "$ppm" "${ppm%.ppm}.png" 2>/dev/null
    done
    st=$(cat "$OUT/$tag/exit" 2>/dev/null || echo missing)
    echo "$tag: exit $st"
    [ "$st" = 0 ] || FAIL=1
    if [ -f "$OUT/$tag/client.log" ]; then
        v=$(grep -cE '^verify:' "$OUT/$tag/client.log")
        [ "$v" -gt 0 ] && { echo "$tag: verify lines:"; grep -E '^verify:' "$OUT/$tag/client.log" | sort | uniq -c | sort -rn | head -20; }
    fi
done
echo "server.log 'not decoded' lines: $(strip_ansi "$OUT/server.log" | grep -c 'not decoded')"
for chk in "${CHECKS[@]}"; do
    read -r kind tag re <<< "$chk"
    log=$OUT/$tag/client.log
    [ "$tag" = server ] && log=$OUT/server.log
    n=$(strip_ansi "$log" 2>/dev/null | grep -cE -- "$re")
    if [ "$kind" = expect ]; then
        [ "$n" -gt 0 ] && echo "ok: expect $tag /$re/ ($n)" || { echo "FAIL: expect $tag /$re/ (0 matches)"; FAIL=1; }
    else
        [ "$n" -eq 0 ] && echo "ok: reject $tag /$re/" || { echo "FAIL: reject $tag /$re/ ($n matches)"; FAIL=1; }
    fi
done
echo "logs and screenshots: $OUT"
exit $FAIL
