#!/usr/bin/env bash
# Build Lua embedded in ooRexx into $BUILD (default $ORXLUA_WORK/build;
# ORXLUA_WORK is the directory that contains the clone):
#   librexxlua.so   the ooRexx native package, with Lua inside
#   lua.cls         the Rexx side (.lua, .LuaState, .LuaObject)
# ooRexx: the installation of the `rexx` on the PATH (OOREXX_HOME to choose
# another), which must have include/oorexxapi.h and lib/librexx.*; an
# installation outside /usr and /usr/local is also recorded as the library's
# run path.  Lua: LUA_VERSION (5.3, 5.4 or 5.5; default 5.4), found with
# pkg-config (Debian/Ubuntu: liblua5.4-dev, liblua5.3-dev), or LUA_CFLAGS and
# LUA_LIBS (for a Lua built from source: a static liblua.a must be compiled
# with -fPIC), or one that scripts/setup-lua.sh built next to the clone
# (5.5).  Linux only, for now.
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

LUA_VERSION=${LUA_VERSION:-5.4}
case $LUA_VERSION in 5.3|5.4|5.5) ;; *) echo "build.sh: LUA_VERSION must be 5.3, 5.4 or 5.5" >&2; exit 2 ;; esac
if [ -z "${LUA_CFLAGS:-}${LUA_LIBS:-}" ]; then
  V=$LUA_VERSION
  for pc in "lua$V" "lua-$V" "lua${V/./}"; do
    if pkg-config --exists "$pc" 2>/dev/null; then
      LUA_CFLAGS=$(pkg-config --cflags "$pc"); LUA_LIBS=$(pkg-config --libs "$pc"); break
    fi
  done
  if [ -z "${LUA_LIBS:-}" ]; then          # one built by setup-lua.sh, next to the clone
    LUADIR=$(ls -d "$WORK/lua-$V".*/src 2>/dev/null | sort -V | tail -1 || true)
    [ -n "$LUADIR" ] && [ -f "$LUADIR/liblua.a" ] &&
      { LUA_CFLAGS="-I$LUADIR"; LUA_LIBS="$LUADIR/liblua.a -lm -ldl"; }
  fi
  [ -n "${LUA_LIBS:-}" ] || { echo "build.sh: no Lua $LUA_VERSION in pkg-config or next to the clone (scripts/setup-lua.sh), and no LUA_CFLAGS and LUA_LIBS" >&2; exit 2; }
fi

ORX="-I$OOREXX_HOME/include -L$OOREXX_HOME/lib"
case $OOREXX_HOME in /usr|/usr/local) ;; *) ORX="$ORX -Wl,-rpath,$OOREXX_HOME/lib" ;; esac
CXXFLAGS="-O2 -g -fPIC -Wall -Wno-unused-parameter -std=c++17"

mkdir -p "$BUILD"
# shellcheck disable=SC2086
"$CXX" $CXXFLAGS -shared $LUA_CFLAGS $ORX "$SRC/rexxlua.cpp" \
    -o "$BUILD/librexxlua.so" -lrexx -lrexxapi $LUA_LIBS
cp "$SRC/lua.cls" "$BUILD/"
echo "build.sh: $BUILD/librexxlua.so, $BUILD/lua.cls (Lua $LUA_VERSION)"
