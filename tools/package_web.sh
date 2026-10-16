#!/usr/bin/env bash
# Packages the PUBLIC (hybrid) web build for Azure Static Web Apps. See docs/web.md, "Packaging and deployment".
#
#   tools/package_web.sh <build-dir> <out-dir> [--game wss://host:port]... [--auth https://host]... [--connect-any]
#
# Refuses lifted or native builds (they contain Sierra-derived code), builds with a preloaded C: drive,
# and any build directory holding game data. The real work is in package_web.mjs; this file holds the
# guards and fetches the pinned esbuild.
set -euo pipefail

ESBUILD_VERSION=0.28.2
ESBUILD_SHA256=9573bb2233aab0f9ea7647d5cca9726113cc1768de61d66b17267f4db84488f6   # @esbuild/linux-x64 tarball
ESBUILD_URL=https://registry.npmjs.org/@esbuild/linux-x64/-/linux-x64-${ESBUILD_VERSION}.tgz

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
die() { echo "package_web: refused: $*" >&2; exit 2; }

[ $# -ge 2 ] || { sed -n 2,8p "$0" >&2; exit 64; }
build=$1; out=$2; shift 2
[ -f "$build/CMakeCache.txt" ] || die "$build has no CMakeCache.txt (is it a CMake build directory?)"
[ -f "$build/inn.html" ] && [ -f "$build/inn.wasm" ] || die "$build is not a finished web build (inn.html or inn.wasm is missing)"

# ---- IP guard: only the hybrid public build may be packaged ----
cache() { sed -n "s/^$1:[A-Z]*=//p" "$build/CMakeCache.txt" | head -n1; }
home=$(cache CMAKE_HOME_DIRECTORY)
native=$(cache INN_NATIVE); hw=$(cache INN_NATIVE_HW)
lifted=$(cache INN_LIFTED_DIR); preload=$(cache INN_WEB_PRELOAD_DIR)
case "${native^^}" in ON|TRUE|1|YES|Y) die "INN_NATIVE is ON: the native build contains Sierra-derived code";; esac
case "${hw^^}" in ON|TRUE|1|YES|Y) die "INN_NATIVE_HW is ON: the native build contains Sierra-derived code";; esac
[ -z "$preload" ] || die "INN_WEB_PRELOAD_DIR is set: the build holds a user's game data"
# INN_LIFTED_DIR always has a value (its default is <source>/private/lifted, which the public checkout lacks).
# It counts as set when it was changed from the default or when the directory is there.
if [ -n "$lifted" ]; then
  if [ "$lifted" != "$home/private/lifted" ]; then die "INN_LIFTED_DIR is set ($lifted): lifted code is Sierra-derived"; fi
  if [ -e "$lifted" ]; then die "INN_LIFTED_DIR exists ($lifted): the build compiled lifted code"; fi
fi
# The generated table is a two-line stub in a public build; anything larger registers lifted code.
if [ -f "$build/lifted_table.c" ] && [ "$(wc -c < "$build/lifted_table.c")" -gt 200 ]; then die "lifted_table.c is not the empty stub"; fi
[ ! -e "$build/lifted_entries.h" ] || die "lifted_entries.h exists: the build compiled lifted code"
# Game data must not sit in the build directory either (a stray inn.data, disks, archives).
bad=$(find "$build" -path "$build/CMakeFiles" -prune -o -type f \( -iname 'inn.data' -o -iname '*.data' -o -iname '*.exe' -o -iname '*.drv' -o -iname '*.dll' -o -iname 'resource.*' -o -iname '*.zip' -o -iname '*.img' \) -print | head -n3)
[ -z "$bad" ] || die "game data or binaries in the build directory: $bad"

# ---- the pinned esbuild, downloaded once into the gitignored cache ----
dir=$root/.cache/esbuild-$ESBUILD_VERSION
esbuild=$dir/package/bin/esbuild
if [ ! -x "$esbuild" ]; then
  mkdir -p "$dir"
  curl -fsSL --max-time 120 -o "$dir/esbuild.tgz" "$ESBUILD_URL"
  echo "$ESBUILD_SHA256  $dir/esbuild.tgz" | sha256sum -c --status || { rm -f "$dir/esbuild.tgz"; die "esbuild download failed its SHA-256 check"; }
  tar xzf "$dir/esbuild.tgz" -C "$dir" package/bin/esbuild
fi
[ "$("$esbuild" --version)" = "$ESBUILD_VERSION" ] || die "unexpected esbuild version"

command -v node >/dev/null || { echo "package_web: node is required" >&2; exit 69; }
exec node "$here/package_web.mjs" "$build" "$out" "$esbuild" "$@"
