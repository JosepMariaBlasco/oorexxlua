#!/usr/bin/env bash
# Run every example (examples/*.rex) and every test (tests/rexx/*.rex) and
# compare its output with the expected one (NAME.out next to it), each in its
# own process under a timeout (a deadlock is a failure).  Builds the test C
# module (tests/cmodule) first, with the headers of LUA_VERSION (default 5.4)
# or LUA_CFLAGS: the same Lua as the build's.  TESTS="01-tour.rex
# threads.rex" chooses.
set -uo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
WORK=${ORXLUA_WORK:-$(cd "$HERE/.." && pwd)}
BUILD=${BUILD:-$WORK/build}
mkdir -p "$BUILD" && BUILD=$(cd "$BUILD" && pwd)          # absolute: the tests run in their own directories
TIMEOUT=${TIMEOUT:-120}

# the C module, as LuaRocks builds them: not linked against liblua
CMOD=$BUILD/test-cmodules
mkdir -p "$CMOD"
V=${LUA_VERSION:-5.4}
if [ -z "${LUA_CFLAGS:-}" ]; then
  if pkg-config --exists "lua$V" 2>/dev/null; then LUA_CFLAGS=$(pkg-config --cflags "lua$V")
  else LUADIR=$(ls -d "$WORK/lua-$V".*/src 2>/dev/null | sort -V | tail -1 || true)
       [ -n "$LUADIR" ] && LUA_CFLAGS="-I$LUADIR"; fi
fi
# shellcheck disable=SC2086
cc -O2 -shared -fPIC $LUA_CFLAGS "$HERE/tests/cmodule/hello.c" -o "$CMOD/hello.so" ||
  { echo "run.sh: cannot build tests/cmodule/hello.c" >&2; exit 2; }
export LUA_CPATH="$CMOD/?.so;;"
export LD_LIBRARY_PATH="$BUILD${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export REXX_PATH="$BUILD${REXX_PATH:+:$REXX_PATH}"

pass=0 fail=0
out=$(mktemp); trap 'rm -f "$out"' EXIT
for rex in "$HERE"/examples/*.rex "$HERE"/tests/rexx/*.rex; do
  name=$(basename "$rex")
  expected=${rex%.rex}.out
  [ -f "$expected" ] || continue
  if [ -n "${TESTS:-}" ] && [[ " $TESTS " != *" $name "* ]]; then continue; fi
  (cd "$(dirname "$rex")" && timeout -k 5 "$TIMEOUT" rexx "$rex") >"$out" 2>&1
  if diff -u "$expected" "$out" >"$out.diff"; then
    echo "ok    $name"; pass=$((pass + 1))
  else
    echo "FAIL  $name"; cat "$out.diff"; fail=$((fail + 1))
  fi
  rm -f "$out.diff"
done
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
