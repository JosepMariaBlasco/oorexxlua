#!/usr/bin/env bash
# Run the test groups (tests/rexx/*.testGroup, ooRexxUnit:
# scripts/setup-oorexxunit.sh) against the build, each in its own process
# under a timeout (a deadlock is a failure).  Examples.testGroup runs every
# example and compares its output with the NAME.out next to it.
#
# Builds the test C module (tests/cmodule) first, as LuaRocks builds them,
# with the same Lua as the build's: LUA_VERSION (default 5.4) found as
# build.sh finds it, or LUA_CFLAGS.  TESTS="Threads.testGroup" to choose;
# TIMEOUT= seconds per group (default 300); logs in OUT (default
# /tmp/orxlua-tests).
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${ORXLUA_WORK:-$(cd "$HERE/../.." && pwd)}
. "$HERE/../scripts/platform.sh"
BUILD=${BUILD:-$WORK/build}
mkdir -p "$BUILD" && BUILD=$(cd "$BUILD" && pwd)          # absolute: the tests run in their own directories
UNIT=${OOREXXUNIT_DIR:-$WORK/oorexxunit}
TESTS=${TESTS:-$(cd "$HERE/rexx" && ls ./*.testGroup | sed 's|^\./||')}
[ -f "$UNIT/testOORexx.rex" ] || { echo "run.sh: ooRexxUnit missing: run scripts/setup-oorexxunit.sh" >&2; exit 2; }
[ -f "$BUILD/$LIB" ] || { echo "run.sh: no $BUILD/$LIB: run scripts/build.sh" >&2; exit 2; }

# The C module: on Linux and macOS not linked against liblua (the lua_*
# symbols come from whoever loads it); on Windows linked against the luaXY.dll
# of the build, as every Lua module on Windows is
CMOD=$BUILD/test-cmodules
mkdir -p "$CMOD"
V=${LUA_VERSION:-5.4}
if [ "$OS" = Windows ]; then
  LUADIR=$(lua_dir "$V")
  [ -n "$LUADIR" ] || { echo "run.sh: no Lua $V next to the clone (scripts/setup-lua.sh $V)" >&2; exit 2; }
  (export MSYS2_ARG_CONV_EXCL='*'; cd "$CMOD" &&
   cl -nologo -O2 -MD -LD -DLUA_BUILD_AS_DLL -I"$(native_path "$LUADIR")" "$(native_path "$HERE/cmodule/hello.c")" \
      -Fehello.dll -link -LIBPATH:"$(native_path "$LUADIR")" "lua${V/./}.lib" -EXPORT:luaopen_hello >/dev/null &&
   rm -f hello.obj hello.exp hello.lib) ||
    { echo "run.sh: cannot build tests/cmodule/hello.c" >&2; exit 2; }
else
  if [ -z "${LUA_CFLAGS:-}" ]; then
    if [ "$OS" = Linux ] && pkg-config --exists "lua$V" 2>/dev/null; then LUA_CFLAGS=$(pkg-config --cflags "lua$V")
    else LUADIR=$(lua_dir "$V"); [ -n "$LUADIR" ] && LUA_CFLAGS="-I$LUADIR"; fi
  fi
  if [ "$OS" = Darwin ]; then MODFLAGS="-bundle -undefined dynamic_lookup"; else MODFLAGS="-shared"; fi
  # shellcheck disable=SC2086
  "$CC" -O2 -fPIC $MODFLAGS ${LUA_CFLAGS:-} "$HERE/cmodule/hello.c" -o "$CMOD/hello.$MODEXT" ||
    { echo "run.sh: cannot build tests/cmodule/hello.c" >&2; exit 2; }
fi
export LUA_CPATH="$(native_path "$CMOD")/?.$MODEXT;;"

export ORXLUA_BUILD=$(native_path "$BUILD")
export $LIBVAR="$BUILD${!LIBVAR:+:${!LIBVAR}}"
export REXX_PATH=$(native_path_list "$BUILD" "$UNIT" "$HERE/rexx")
OUT=${OUT:-/tmp/orxlua-tests}; mkdir -p "$OUT"
status=0
for t in $TESTS; do
  log="$OUT/${t%.*}-lua$V.log"
  (cd "$OUT" && timeout -s KILL "${TIMEOUT:-300}" rexx "$(native_path "$UNIT/testOORexx.rex")" \
      -f "$(native_path "$HERE/rexx/$t")" -O -U) > "$log" 2>&1
  rc=$?
  n() { grep -E "^$1:" "$log" | tail -1 | awk '{print $NF}'; }
  summary="$(n 'Tests ran') tests, $(n Assertions) assertions, $(n Failures) failures, $(n Errors) errors"
  # testOORexx's exit code is non-zero on failures; also require that tests ran
  { [ $rc -eq 0 ] && [ -n "$(n 'Tests ran')" ] && [ "$(n 'Tests ran')" != 0 ]; } || rc=${rc/#0/1}
  if [ $rc -eq 0 ]; then echo "PASS lua$V $t: $summary"
  else
    echo "FAIL lua$V $t (rc $rc): ${summary:-see $log}"
    grep -E -A12 '^\[(failure|error)\]' "$log" | head -60
    tail -3 "$log"; status=1
  fi
done
exit $status
