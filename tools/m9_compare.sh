#!/usr/bin/env bash
# Run one scenario list on a hybrid build and a native build (verification off)
# and compare traffic and screens.
# usage: m9_compare.sh <hybrid_build> <native_build> <first_port> <out_dir> <list>
set -euo pipefail
export LC_ALL=C

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

usage() {
    cat <<'EOF'
usage: m9_compare.sh <hybrid_build> <native_build> <first_port> <out_dir> <list>

Runs tools/m9_suite.sh once for each build (calls 0) into <out_dir>/hybrid and
<out_dir>/native, then compares every scenario's server log and every
screenshot both sides have (m9_screens.py --tolerant 3). Writes
<out_dir>/report.txt with one line per scenario: PASS, or FAIL and the checks
that did not match. M9_TRAFFIC_OPTS adds options to m9_traffic.py, e.g.
M9_TRAFFIC_OPTS='--ignore-client-kinds 2F' (a diagnostic).

A scenario whose game draws a random value that follows the timing model (docs/native.md,
section 5) can narrow its comparison with a sidecar file
private/lifted/scenarios/<name>.compare, one directive per line (# comments):
  mask-client <HEXPREFIX>@<FROM>-<TO>   m9_traffic.py --mask-client: ignore those bytes of the
                                        matching client messages (the random value itself)
  stop-before-client <HEXPREFIX>        m9_traffic.py --stop-before-client: compare traffic only
                                        up to the first matching message (the rest depends on it)
  screens <name> ...                    compare only these screenshots (names as in
                                        <scenario>.<name>.png); the others depend on the value
Without a sidecar every message and every screenshot is compared.
EOF
}

trim() {
    local s=$1
    s="${s#"${s%%[![:space:]]*}"}"
    s="${s%"${s##*[![:space:]]}"}"
    printf '%s' "$s"
}

# Reads private/lifted/scenarios/<name>.compare into side_opts (m9_traffic.py
# options) and side_screens (screenshot names; empty means all).
read_sidecar() {
    local file=$ROOT/private/lifted/scenarios/$1.compare line key rest
    side_opts=()
    side_screens=()
    [[ -f $file ]] || return 0
    while IFS= read -r line || [[ -n $line ]]; do
        line=${line%$'\r'}
        line=$(trim "${line%%#*}")
        [[ -z $line ]] && continue
        key=${line%%[[:space:]]*}
        rest=$(trim "${line#"$key"}")
        case $key in
        mask-client) side_opts+=(--mask-client "$rest") ;;
        stop-before-client) side_opts+=(--stop-before-client "$rest") ;;
        screens) read -r -a side_screens <<< "$rest" ;;
        *) echo "compare: $file: unknown directive $key" >&2; exit 2 ;;
        esac
    done < "$file"
}

main() {
    local hy na first out list name line
    local -a names=() checks=() side_opts=() side_screens=()
    local hy_status na_status shot base dest shotname failed=0 tmp
    if [[ ${1:-} == --help || ${1:-} == -h ]]; then
        usage
        exit 0
    fi
    if [[ $# -ne 5 ]]; then
        usage >&2
        exit 2
    fi
    hy=$(cd "$1" && pwd)
    na=$(cd "$2" && pwd)
    first=$3
    out=$(mkdir -p "$4" && cd "$4" && pwd)
    list=$5
    if [[ ! -f $list ]]; then
        echo "compare: no list $list" >&2
        exit 2
    fi
    "$ROOT/tools/m9_suite.sh" "$hy" "$first" "$out/hybrid" "$list" 0
    "$ROOT/tools/m9_suite.sh" "$na" "$first" "$out/native" "$list" 0
    while IFS= read -r line || [[ -n $line ]]; do
        line=${line%$'\r'}
        line=$(trim "${line%%#*}")
        [[ -z $line ]] && continue
        read -r name _ <<< "$line"
        names+=("$name")
    done < "$list"
    : > "$out/report.txt"
    tmp=$(mktemp)
    for name in "${names[@]}"; do
        checks=()
        read_sidecar "$name"
        if [[ ! -f $out/hybrid/$name.status || ! -f $out/native/$name.status ]]; then
            checks+=(missing)
        else
            hy_status=$(head -n 1 "$out/hybrid/$name.status")
            na_status=$(head -n 1 "$out/native/$name.status")
            [[ $hy_status == ok && $na_status == ok ]] || checks+=(status)
            if [[ -f $out/hybrid/$name.server.log && -f $out/native/$name.server.log ]]; then
                # shellcheck disable=SC2086 # M9_TRAFFIC_OPTS is a list of options
                if ! "$ROOT/tools/m9_traffic.py" "${side_opts[@]}" ${M9_TRAFFIC_OPTS:-} "$out/hybrid/$name.server.log" \
                    "$out/native/$name.server.log" > "$tmp" 2>&1; then
                    checks+=(traffic)
                fi
            else
                checks+=(traffic)
            fi
            shopt -s nullglob
            for shot in "$out/hybrid/$name".*.png; do
                base=$(basename "$shot")
                dest=$out/native/$base
                [[ -f $dest ]] || continue
                shotname=${base#"$name."}
                shotname=${shotname%.png}
                if ((${#side_screens[@]})) && [[ " ${side_screens[*]} " != *" $shotname "* ]]; then
                    continue
                fi
                if ! "$ROOT/tools/m9_screens.py" --tolerant 3 "$shot" "$dest" > "$tmp" 2>&1; then
                    checks+=("screen:${base#"$name."}")
                fi
            done
            shopt -u nullglob
        fi
        if ((${#checks[@]} == 0)); then
            echo "$name PASS" >> "$out/report.txt"
        else
            echo "$name FAIL ${checks[*]}" >> "$out/report.txt"
            failed=1
        fi
    done
    rm -f "$tmp"
    cat "$out/report.txt"
    return "$failed"
}

main "$@"
