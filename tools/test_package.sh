#!/usr/bin/env bash
# Tests the guards of package_web.sh: lifted, native, preloaded and stray-file builds are refused.
#   tools/test_package.sh [<finished public build dir>]
# With a build directory it also packages that build and checks the output; without one only the refusals run
# (they use small fake build directories and never reach esbuild).
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
fail=0
ok()  { echo "ok   $1"; }
bad() { echo "FAIL $1"; fail=1; }

fake() {   # fake <name> [cache lines...]: a build directory that looks finished
  local d=$tmp/$1; shift
  mkdir -p "$d"; : > "$d/inn.html"; : > "$d/inn.wasm"
  printf 'CMAKE_HOME_DIRECTORY:INTERNAL=/src/inn\nINN_LIFTED_DIR:PATH=/src/inn/private/lifted\nINN_NATIVE:BOOL=OFF\nINN_NATIVE_HW:BOOL=OFF\nINN_WEB_PRELOAD_DIR:PATH=\n' > "$d/CMakeCache.txt"
  for l in "$@"; do sed -i "s|^${l%%:*}:.*|$l|" "$d/CMakeCache.txt"; done
  echo "$d"
}
refuses() {   # refuses <label> <build> <expected text>
  local msg; msg=$("$here/package_web.sh" "$2" "$tmp/out" 2>&1); local rc=$?
  if [ $rc -eq 2 ] && grep -q "$3" <<<"$msg"; then ok "$1"; else bad "$1 (rc=$rc: $msg)"; fi
  [ ! -e "$tmp/out" ] || bad "$1 left an output directory"
}

refuses "refuses INN_NATIVE=ON" "$(fake native INN_NATIVE:BOOL=ON)" INN_NATIVE
refuses "refuses INN_NATIVE_HW=ON" "$(fake hw INN_NATIVE_HW:BOOL=ON)" INN_NATIVE_HW
refuses "refuses a preloaded C: drive" "$(fake preload INN_WEB_PRELOAD_DIR:PATH=/x/inn-c)" INN_WEB_PRELOAD_DIR
d=$(fake lifted-moved); sed -i 's|^INN_LIFTED_DIR:PATH=.*|INN_LIFTED_DIR:PATH=/elsewhere/lifted|' "$d/CMakeCache.txt"
mkdir -p "$tmp/lifted-elsewhere" && sed -i "s|^INN_LIFTED_DIR:PATH=.*|INN_LIFTED_DIR:PATH=$tmp/lifted-elsewhere|" "$d/CMakeCache.txt"
refuses "refuses INN_LIFTED_DIR set to another path that exists" "$d" "INN_LIFTED_DIR exists"
d2=$(fake lifted-default); mkdir -p "$d2/home/private/lifted"; sed -i "s|^INN_LIFTED_DIR:PATH=.*|INN_LIFTED_DIR:PATH=$d2/home/private/lifted|; s|^CMAKE_HOME_DIRECTORY:INTERNAL=.*|CMAKE_HOME_DIRECTORY:INTERNAL=$d2/home|" "$d2/CMakeCache.txt"
refuses "refuses a default INN_LIFTED_DIR that exists" "$d2" "INN_LIFTED_DIR exists"
d=$(fake table); head -c 400 /dev/zero | tr '\0' 'x' > "$d/lifted_table.c"
refuses "refuses a lifted table that is not the stub" "$d" lifted_table
d=$(fake data); : > "$d/inn.data"
refuses "refuses inn.data in the build directory" "$d" "game data"
d=$(fake exe); mkdir -p "$d/c"; : > "$d/c/TSNEXEC.EXE"
refuses "refuses a game executable in the build directory" "$d" "game data"

if [ -n "${1:-}" ]; then
  b=$1
  if "$here/package_web.sh" "$b" "$tmp/pkg" --game wss://play.example.test:2316 --auth https://auth.example.test >"$tmp/log" 2>&1; then ok "packages the public build"; else bad "packages the public build: $(tail -3 "$tmp/log")"; fi
  # nothing outside the expected list, no game data, hashed names, the config is valid and under SWA's 20 KB limit
  stray=$(cd "$tmp/pkg" && find . -type f | sed 's|^\./||' | grep -Ev '^(inn\.html|site\.json|staticwebapp\.config\.json|legal/(terms|privacy|copyright)\.html|assets/(app|inn|legal)\.[0-9a-f]{10}\.(js|css|wasm)|assets/(scenes|music)/[a-z]+\.[0-9a-f]{10}\.js)$')
  [ -z "$stray" ] && ok "output holds only expected files" || bad "stray files: $stray"
  [ "$(find "$tmp/pkg" -type f | wc -l)" -ge 20 ] && ok "output file count" || bad "output file count"
  python3 -I - "$tmp/pkg/staticwebapp.config.json" <<'P' && ok "staticwebapp.config.json" || bad "staticwebapp.config.json"
import json, sys
p = sys.argv[1]; c = json.load(open(p))
assert len(open(p, 'rb').read()) < 20480
h = c['globalHeaders']
assert "frame-ancestors 'none'" in h['Content-Security-Policy'] and "'unsafe-eval'" not in h['Content-Security-Policy']
assert 'wss://play.example.test:2316' in h['Content-Security-Policy'] and 'https://auth.example.test' in h['Content-Security-Policy']
assert h['Cross-Origin-Opener-Policy'] == 'same-origin-allow-popups' and h['Referrer-Policy']
assert c['mimeTypes']['.wasm'] == 'application/wasm'
r = {x['route']: x for x in c['routes']}
assert 'immutable' in r['/assets/*']['headers']['Cache-Control'] and r['/inn.html']['headers']['Cache-Control'] == 'no-cache' and r['/site.json']['headers']['Cache-Control'] == 'no-cache'
P
  # a stray file among the lazy scenes is refused
  cp -r "$b" "$tmp/stray" && : > "$tmp/stray/scenes/notes.txt"
  msg=$("$here/package_web.sh" "$tmp/stray" "$tmp/out2" 2>&1); [ $? -eq 2 ] && grep -q "unexpected file" <<<"$msg" && ok "refuses an unexpected file among the scenes" || bad "stray scene file: $msg"
  # the sealed copy of the cache must not be edited by the test above
  cp -r "$b" "$tmp/native" && sed -i 's|^INN_NATIVE:BOOL=.*|INN_NATIVE:BOOL=ON|' "$tmp/native/CMakeCache.txt"
  msg=$("$here/package_web.sh" "$tmp/native" "$tmp/out3" 2>&1); [ $? -eq 2 ] && ok "refuses the real build flipped to native" || bad "native flip: $msg"
fi
exit $fail
