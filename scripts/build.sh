#!/usr/bin/env bash
# Build Lua embedded in ooRexx into $BUILD (default $ORXLUA_WORK/build;
# ORXLUA_WORK is the directory that contains the clone):
#   librexxlua.so   the ooRexx native package, with Lua 5.4 inside
#   lua.cls         the Rexx side (.lua, .LuaObject)
# ooRexx: the installation of the `rexx` on the PATH (OOREXX_HOME to choose
# another), which must have include/oorexxapi.h and lib/librexx.*; an
# installation outside /usr and /usr/local is also recorded as the library's
# run path.  Lua: pkg-config's lua5.4 (Debian/Ubuntu: liblua5.4-dev), or
# LUA_CFLAGS and LUA_LIBS.  Linux only, for now.
set -euo pipefail
WORK=${ORXLUA_WORK:-$(cd "$(dirname "$0")/../.." && pwd)}   # the directory that contains the clone
SRC=$(cd "$(dirname "$0")/../src" && pwd)
BUILD=${BUILD:-$WORK/build}
CXX=${CXX:-g++}

if [ -z "${OOREXX_HOME:-}" ]; then
  REXX=$(command -v rexx) || { echo "build.sh: no rexx on the PATH (or set OOREXX_HOME)" >&2; exit 2; }
  OOREXX_HOME=$(cd "$(dirname "$(readlink -f "$REXX")")/.." && pwd)
fi
[ -f "$OOREXX_HOME/include/oorexxapi.h" ] || { echo "build.sh: no $OOREXX_HOME/include/oorexxapi.h" >&2; exit 2; }

if [ -z "${LUA_CFLAGS:-}${LUA_LIBS:-}" ]; then
  for pc in lua5.4 lua-5.4 lua54; do
    if pkg-config --exists "$pc" 2>/dev/null; then
      LUA_CFLAGS=$(pkg-config --cflags "$pc"); LUA_LIBS=$(pkg-config --libs "$pc"); break
    fi
  done
  [ -n "${LUA_LIBS:-}" ] || { echo "build.sh: no Lua 5.4 in pkg-config (or set LUA_CFLAGS and LUA_LIBS)" >&2; exit 2; }
fi

ORX="-I$OOREXX_HOME/include -L$OOREXX_HOME/lib"
case $OOREXX_HOME in /usr|/usr/local) ;; *) ORX="$ORX -Wl,-rpath,$OOREXX_HOME/lib" ;; esac
CXXFLAGS="-O2 -g -fPIC -Wall -Wno-unused-parameter -std=c++17"

mkdir -p "$BUILD"
# shellcheck disable=SC2086
"$CXX" $CXXFLAGS -shared $LUA_CFLAGS $ORX "$SRC/rexxlua.cpp" \
    -o "$BUILD/librexxlua.so" -lrexx -lrexxapi $LUA_LIBS
cp "$SRC/lua.cls" "$BUILD/"
echo "build.sh: $BUILD/librexxlua.so, $BUILD/lua.cls"
