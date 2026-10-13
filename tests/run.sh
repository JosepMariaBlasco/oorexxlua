#!/usr/bin/env bash
# Run the test groups (tests/rexx/*.testGroup, ooRexxUnit:
# scripts/setup-oorexxunit.sh) against the build, each in its own process
# under a timeout (a deadlock is a failure).  Examples.testGroup runs every
# example and compares its output with the NAME.out next to it.  Then the
# tests of the Lua module (tests/lua/*.test.lua), each run by a standalone
# lua of the build's version: LUA_EXE, else the one scripts/setup-lua.sh
# built next to the clone.
#
# Builds the test C module (tests/cmodule) first, as LuaRocks builds them,
# with the same Lua as the build's (scripts/build-cmodule.sh: LUA_VERSION,
# default 5.4, LUA_STATIC, LUA_CFLAGS).  TESTS="Threads.testGroup" to choose;
# TIMEOUT= seconds per group (default 300); logs in OUT (default
# /tmp/orxlua-tests).
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${ORXLUA_WORK:-$(cd "$HERE/../.." && pwd)}
. "$HERE/../scripts/platform.sh"
BUILD=${BUILD:-$WORK/build}
mkdir -p "$BUILD" && BUILD=$(cd "$BUILD" && pwd)          # absolute: the tests run in their own directories
UNIT=${OOREXXUNIT_DIR:-$WORK/oorexxunit}
TESTS=${TESTS:-$(cd "$HERE/rexx" && ls ./*.testGroup | sed 's|^\./||'; cd "$HERE/lua" && ls ./*.test.lua | sed 's|^\./||')}
[ -f "$UNIT/testOORexx.rex" ] || { echo "run.sh: ooRexxUnit missing: run scripts/setup-oorexxunit.sh" >&2; exit 2; }
[ -f "$BUILD/$PKG" ] || { echo "run.sh: no $BUILD/$PKG: run scripts/build.sh" >&2; exit 2; }

CMOD=$BUILD/test-cmodules
V=${LUA_VERSION:-5.4}
"$HERE/../scripts/build-cmodule.sh" "$CMOD" || { echo "run.sh: cannot build tests/cmodule/hello.c" >&2; exit 2; }
# the Lua module first (ORXLUA_MODDIR: another build of it, such as
# build.sh's texlive/ on Windows), then the test C module
MODDIR=${ORXLUA_MODDIR:-$BUILD}
export LUA_CPATH="$(native_path "$MODDIR")/?.$MODEXT;$(native_path "$CMOD")/?.$MODEXT;;"
LUA_EXE=${LUA_EXE:-$(lua_exe "$V")}
export ORXLUA_LUA=${LUA_EXE:+$(native_path "$LUA_EXE")}     # for the .lua examples, and tests/lua
export ORXLUA_HARNESS=$(native_path "$HERE/lua/harness.lua") ORXLUA_TESTS=$(native_path "$HERE/lua")

export ORXLUA_BUILD=$(native_path "$BUILD")
export $LIBVAR="$BUILD${!LIBVAR:+:${!LIBVAR}}"
export REXX_PATH=$(native_path_list "$BUILD" "$UNIT" "$HERE/rexx")
OUT=${OUT:-/tmp/orxlua-tests}; mkdir -p "$OUT"
status=0
for t in $TESTS; do
  log="$OUT/${t%.*}-lua$V.log"
  case $t in
    *.lua)
      if [ -z "$LUA_EXE" ]; then
        echo "FAIL lua$V $t: no standalone lua $V (scripts/setup-lua.sh $V, or LUA_EXE)"; status=1; continue
      fi
      (cd "$OUT" && timeout -s KILL "${TIMEOUT:-300}" "$LUA_EXE" "$(native_path "$HERE/lua/$t")") > "$log" 2>&1 ;;
    Interrupt.testGroup)
      if [ "$OS" = Windows ]; then
        # its Ctrl-C goes to every process of the console: a console of its own
        (cd "$OUT" && timeout -s KILL "${TIMEOUT:-300}" cmd //c start '""' //wait //min cmd //c \
          "rexx $(native_path "$UNIT/testOORexx.rex") -f $(native_path "$HERE/rexx/$t") -O -U > $(native_path "$log") 2>&1")
      else
        (cd "$OUT" && timeout -s KILL "${TIMEOUT:-300}" rexx "$(native_path "$UNIT/testOORexx.rex")" \
            -f "$(native_path "$HERE/rexx/$t")" -O -U) > "$log" 2>&1
      fi ;;
    *)
      (cd "$OUT" && timeout -s KILL "${TIMEOUT:-300}" rexx "$(native_path "$UNIT/testOORexx.rex")" \
          -f "$(native_path "$HERE/rexx/$t")" -O -U) > "$log" 2>&1 ;;
  esac
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
