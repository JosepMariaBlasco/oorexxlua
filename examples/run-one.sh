#!/usr/bin/env bash
# Run one example against the build: examples/run-one.sh 01-tour.rex [args...]
# A .lua example runs with a standalone lua of the build's Lua (LUA_EXE, else
# the one scripts/setup-lua.sh built next to the clone; LUA_VERSION, default
# 5.4).
set -euo pipefail
WORK=${ORXLUA_WORK:-$(cd "$(dirname "$0")/../.." && pwd)}
. "$(dirname "$0")/../scripts/platform.sh"
BUILD=${BUILD:-$WORK/build}
BUILD=$(cd "$BUILD" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
name=$1; shift
export $LIBVAR="$BUILD${!LIBVAR:+:${!LIBVAR}}"
export REXX_PATH=$(native_path_list "$BUILD")
case $name in
  *.lua)
    LUA_EXE=${LUA_EXE:-$(lua_exe "${LUA_VERSION:-5.4}")}
    [ -n "$LUA_EXE" ] || { echo "run-one.sh: no standalone lua (scripts/setup-lua.sh, or LUA_EXE)" >&2; exit 2; }
    export LUA_CPATH="$(native_path "$BUILD")/?.$MODEXT;;"
    cd "$HERE" && exec "$LUA_EXE" "$(native_path "$HERE/$name")" "$@" ;;
  *) exec rexx "$(native_path "$HERE/$name")" "$@" ;;
esac
