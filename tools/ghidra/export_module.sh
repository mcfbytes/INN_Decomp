#!/usr/bin/env bash
# usage: export_module.sh <exe> <MODULE_NAME> <out_dir> [names.tsv]
# The project goes to <repo>/work/ghidra/<MODULE_NAME>/; GHIDRA_HOME selects the install.
set -euo pipefail

if [[ $# -lt 3 || $# -gt 4 ]]; then
    echo "usage: export_module.sh <exe> <MODULE_NAME> <out_dir> [names.tsv]" >&2
    exit 2
fi

EXE=$1
MODULE=$2
OUT_DIR=$3
NAMES=${4:-}

if [[ -z "$MODULE" || "$MODULE" == */* || "$MODULE" == *..* ]]; then
    echo "invalid module name: $MODULE" >&2
    exit 2
fi

GHIDRA_HOME=${GHIDRA_HOME:-/mnt/source/tools/ghidra/ghidra_12.1.4_PUBLIC}
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJ_DIR=$SCRIPT_DIR/../../work/ghidra/"$MODULE"

EXE=$(readlink -f "$EXE")
mkdir -p "$OUT_DIR" "$PROJ_DIR"
OUT_DIR=$(cd "$OUT_DIR" && pwd)
if [[ -n "$NAMES" ]]; then
    NAMES=$(readlink -f "$NAMES")
    if [[ ! -f "$NAMES" ]]; then
        echo "names file not found: $NAMES" >&2
        exit 1
    fi
fi
if [[ ! -f "$EXE" ]]; then
    echo "exe not found: $EXE" >&2
    exit 1
fi

# Overwrite this module's project only.
rm -rf "$PROJ_DIR/$MODULE.gpr" "$PROJ_DIR/$MODULE.rep" \
    "$PROJ_DIR/$MODULE.lock" "$PROJ_DIR/$MODULE.lock~"
find "$OUT_DIR" -maxdepth 1 -type f \
    \( -name '*.c' -o -name '*.asm' -o -name 'index.tsv' \) -delete

POST=(ExportFunctions.java "$OUT_DIR")
if [[ -n "$NAMES" ]]; then
    POST+=("$NAMES")
fi

export GHIDRA_HEADLESS_MAXMEM="${GHIDRA_HEADLESS_MAXMEM:-8G}"

exec "$GHIDRA_HOME/support/analyzeHeadless" \
    "$PROJ_DIR" "$MODULE" \
    -import "$EXE" \
    -loader MzLoader \
    -overwrite \
    -scriptPath "$SCRIPT_DIR" \
    -log "$PROJ_DIR/headless.log" \
    -scriptlog "$PROJ_DIR/script.log" \
    -postScript "${POST[@]}"
