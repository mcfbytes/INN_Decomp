#!/usr/bin/env bash
# Resumable scenario suite for the M9 port. One status file per scenario; a run
# that is killed before it finishes leaves no status and is done again next time.
# usage: m9_suite.sh <build> <first_port> <out_dir> <list> [calls=20]
set -euo pipefail
# Runs repeat only with the settled server link (docs/runtime.md, "Link settling").
export LC_ALL=C INN_LINK_SETTLE_MS=${INN_LINK_SETTLE_MS-40}

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PRIVATE=$ROOT/private/lifted
BUDGET=${BUDGET:-500}

usage() {
    cat <<'EOF'
usage: m9_suite.sh <build> <first_port> <out_dir> <list> [calls=20]

For each scenario in <list> that has no <out_dir>/<name>.status yet, runs
tools/verify_lifted.sh on ports first_port .. first_port+4, at most five at a
time. <list> lines are: <name> <drive> <run_ms|auto> [VAR=value ...]
(# comments). auto is the script's largest time plus 15000 (logon: 95000).
@PRIVATE@ in a value becomes the private/lifted directory; underscores in
INN_PROGRAM and INN_ARGS become spaces. The server link settles (INN_LINK_SETTLE_MS,
default 40; 0 turns it off). Stops starting new runs after 500s and prints
"suite complete: N ok, M failed" or "suite incomplete: K left".
EOF
}

trim() {
    local s=$1
    s="${s#"${s%%[![:space:]]*}"}"
    s="${s%"${s##*[![:space:]]}"}"
    printf '%s' "$s"
}

# Largest leading event time in an input script, plus 15000 ms.
max_script_ms() {
    local line t max=0
    while IFS= read -r line || [[ -n $line ]]; do
        line=${line%$'\r'}
        line=$(trim "${line%%#*}")
        [[ -z $line ]] && continue
        read -r t _ <<< "$line"
        if [[ $t =~ ^[0-9]+$ ]] && ((10#$t > max)); then
            max=$((10#$t))
        fi
    done < "$1"
    echo $((max + 15000))
}

# Fills names, drives, mss, scripts, varblobs from the list file.
load_list() {
    local list=$1 line name drive ms script blob kv key val i
    local -a fields=()
    names=()
    drives=()
    mss=()
    scripts=()
    varblobs=()
    while IFS= read -r line || [[ -n $line ]]; do
        line=${line%$'\r'}
        line=$(trim "${line%%#*}")
        [[ -z $line ]] && continue
        read -r -a fields <<< "$line"
        if ((${#fields[@]} < 3)); then
            echo "suite: bad list line: $line" >&2
            exit 2
        fi
        name=${fields[0]}
        drive=${fields[1]}
        ms=${fields[2]}
        if [[ $name == logon ]]; then
            script=""
        else
            script=$PRIVATE/scenarios/$name.txt
            if [[ ! -f $script ]]; then
                echo "suite: missing script $script" >&2
                exit 2
            fi
        fi
        if [[ $ms == auto ]]; then
            if [[ $name == logon ]]; then
                ms=95000
            else
                ms=$(max_script_ms "$script")
            fi
        elif ! [[ $ms =~ ^[0-9]+$ ]]; then
            echo "suite: bad run_ms for $name: $ms" >&2
            exit 2
        fi
        if [[ ! -d $ROOT/work/$drive ]]; then
            echo "suite: missing drive work/$drive" >&2
            exit 2
        fi
        blob=""
        for ((i = 3; i < ${#fields[@]}; i++)); do
            kv=${fields[$i]}
            key=${kv%%=*}
            val=${kv#*=}
            val=${val//@PRIVATE@/$PRIVATE}
            if [[ $key == INN_PROGRAM || $key == INN_ARGS ]]; then
                val=${val//_/ }
            fi
            blob+="$key=$val"$'\n'
        done
        names+=("$name")
        drives+=("$drive")
        mss+=("$ms")
        scripts+=("$script")
        varblobs+=("$blob")
    done < "$list"
}

write_status() {
    local name=$1 text=$2 tmp
    tmp=$(mktemp "$OUT/.status.XXXXXX")
    printf '%s\n' "$text" > "$tmp"
    mv "$tmp" "$OUT/$name.status"
}

discard_outputs() {
    local name=$1
    rm -f "$OUT/$name.client.log" "$OUT/$name.server.log" "$OUT/$name.verify"
    if [[ -n $(shopt -s nullglob; echo "$OUT/$name".*.png) ]]; then
        # shellcheck disable=SC2086
        rm -f "$OUT/$name".*.png
    fi
}

run_one() {
    local idx=$1 port=$2
    local name=${names[$idx]} drive=${drives[$idx]} ms=${mss[$idx]}
    local script=${scripts[$idx]} blob=${varblobs[$idx]}
    local vdir=$ROOT/work/verify/$port kv rc=0 fatal_line
    local -a env_args cmd ppms
    echo "suite: start $name port $port ms $ms" >&2
    discard_outputs "$name"
    mkdir -p "$vdir"
    rm -f "$vdir/client.log" "$vdir/server.log" "$vdir/play.txt"
    shopt -s nullglob
    ppms=("$vdir"/*.ppm "$vdir"/*.png)
    ((${#ppms[@]})) && rm -f "${ppms[@]}"
    shopt -u nullglob

    env_args=("INN_DRIVE=$ROOT/work/$drive")
    while IFS= read -r kv; do
        [[ -n $kv ]] && env_args+=("$kv")
    done <<< "$blob"
    cmd=("$ROOT/tools/verify_lifted.sh" "$BUILD" "$port" "$ms" "$CALLS")
    [[ -n $script ]] && cmd+=("$script")
    env "${env_args[@]}" "${cmd[@]}" || rc=$?
    if [[ ! -f $vdir/client.log ]]; then
        if [[ $rc -eq 2 || $rc -ge 128 ]]; then
            echo "suite: $name exited $rc, will retry" >&2
            return 1
        fi
        write_status "$name" "fatal: verify_lifted.sh exited $rc"
        echo "suite: done $name fatal" >&2
        return 0
    fi
    cp "$vdir/client.log" "$OUT/$name.client.log"
    if [[ -f $vdir/server.log ]]; then
        python3 -I -c 'import re,sys; sys.stdout.write(re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", sys.stdin.read()))' \
            < "$vdir/server.log" > "$OUT/$name.server.log"
    else
        : > "$OUT/$name.server.log"
    fi
    shopt -s nullglob
    for ppm in "$vdir"/*.ppm; do
        python3 -I "$ROOT/tools/ppm2png.py" "$ppm" "$OUT/$name.$(basename "$ppm" .ppm).png"
    done
    shopt -u nullglob
    if ! grep '^verify: ' "$vdir/client.log" | sort | uniq -c > "$OUT/$name.verify"; then
        : > "$OUT/$name.verify"
    fi
    fatal_line=$(grep -m1 '^fatal:' "$vdir/client.log" || true)
    if [[ -n $fatal_line ]]; then
        write_status "$name" "$fatal_line"
        echo "suite: done $name fatal" >&2
        return 0
    fi
    # A killed client has no shutdown summary. Do not record that as success.
    if [[ $CALLS -gt 0 ]] && ! grep -qE '^verify: .*[0-9]+ calls checked, [0-9]+ mismatches' "$vdir/client.log"; then
        echo "suite: $name has no verify summary, will retry" >&2
        discard_outputs "$name"
        return 1
    fi
    write_status "$name" ok
    echo "suite: done $name ok" >&2
}

run_queue() {
    local -a pending=() order=() free=() pid_slot=()
    local i began running=0 next=0 slot port finished
    local -A slot_of=()
    for i in "${!names[@]}"; do
        [[ -f $OUT/${names[$i]}.status ]] || pending+=("$i")
    done
    if ((${#pending[@]} == 0)); then
        return 0
    fi
    while IFS= read -r i; do
        [[ -n $i ]] && order+=("$i")
    done < <(for i in "${pending[@]}"; do printf '%012d %s\n' "${mss[$i]}" "$i"; done | sort -k1,1nr -k2,2n | awk '{print $2}')
    for i in 0 1 2 3 4; do
        free+=("$i")
    done
    began=$SECONDS
    while ((next < ${#order[@]})) || ((running > 0)); do
        while ((next < ${#order[@]} && ${#free[@]} > 0 && SECONDS - began < BUDGET)); do
            slot=${free[0]}
            if ((${#free[@]} == 1)); then
                free=()
            else
                free=("${free[@]:1}")
            fi
            port=$((FIRST + slot))
            run_one "${order[$next]}" "$port" &
            slot_of[$!]=$slot
            running=$((running + 1))
            next=$((next + 1))
        done
        if ((running == 0)); then
            break
        fi
        finished=""
        wait -n -p finished || true
        slot=${slot_of[$finished]}
        unset 'slot_of[$finished]'
        free+=("$slot")
        running=$((running - 1))
    done
    if ((next < ${#order[@]})); then
        echo "suite: ${BUDGET}s elapsed, not starting more ($((${#order[@]} - next)) still queued)" >&2
    fi
}

summarize() {
    local name left=0 ok=0 bad=0 text
    for name in "${names[@]}"; do
        if [[ ! -f $OUT/$name.status ]]; then
            left=$((left + 1))
            echo "suite: left $name" >&2
        else
            text=$(head -n 1 "$OUT/$name.status")
            if [[ $text == ok ]]; then
                ok=$((ok + 1))
            else
                bad=$((bad + 1))
            fi
        fi
    done
    if ((left == 0)); then
        echo "suite complete: $ok ok, $bad failed"
    else
        echo "suite incomplete: $left left"
    fi
}

main() {
    local p
    if [[ ${1:-} == --help || ${1:-} == -h ]]; then
        usage
        exit 0
    fi
    if [[ $# -lt 4 || $# -gt 5 ]]; then
        usage >&2
        exit 2
    fi
    if [[ ! -d $1 ]]; then
        echo "suite: no build directory $1" >&2
        exit 2
    fi
    BUILD=$(cd "$1" && pwd)
    FIRST=$2
    OUT=$(mkdir -p "$3" && cd "$3" && pwd)
    local list=$4
    CALLS=${5:-20}
    if ! [[ $FIRST =~ ^[0-9]+$ && $CALLS =~ ^[0-9]+$ ]]; then
        echo "suite: port and calls must be numbers" >&2
        exit 2
    fi
    for ((p = FIRST; p < FIRST + 5; p++)); do
        if [[ $p -eq 2314 || $p -eq 2316 || $p -eq 8080 ]]; then
            echo "suite: port $p is reserved" >&2
            exit 2
        fi
    done
    if [[ ! -x $BUILD/inn ]]; then
        echo "suite: no executable $BUILD/inn" >&2
        exit 2
    fi
    list=$(cd "$(dirname "$list")" && pwd)/$(basename "$list")
    if [[ ! -f $list ]]; then
        echo "suite: no list $list" >&2
        exit 2
    fi
    rm -f "$OUT"/.status.*
    load_list "$list"
    run_queue
    summarize
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
    main "$@"
fi
