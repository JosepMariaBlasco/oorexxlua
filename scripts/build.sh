#!/usr/bin/env bash
# Build Lua embedded in ooRexx into $BUILD (default $ORXLUA_WORK/build;
# ORXLUA_WORK is the directory that contains the clone):
#   librexxlua.so   the ooRexx native package, with Lua inside
#                   (librexxlua.dylib on macOS; rexxlua.dll on Windows, with
#                   the luaXY.dll it uses copied next to it)
#   lua.cls         the Rexx side (.lua, .LuaState, .LuaObject)
# ooRexx: the installation of the `rexx` on the PATH (OOREXX_HOME to choose
# another), which must have include/oorexxapi.h and lib/librexx.* (rexx.lib
# on Windows); an installation outside /usr and /usr/local is also recorded
# as the library's run path.
# Lua: LUA_VERSION (5.3, 5.4 or 5.5; default 5.4), found
#   - with LUA_CFLAGS and LUA_LIBS, if given (Linux and macOS);
#   - on Linux, with pkg-config (Debian/Ubuntu: liblua5.4-dev, liblua5.3-dev);
#   - else the one that scripts/setup-lua.sh built next to the clone: a
#     static liblua.a, linked whole (so that C modules find every lua_* in
#     the package), or on Windows luaXY.dll.
# Windows: run from Git Bash with MSVC's cl on the PATH (a developer command
# prompt's environment).
set -euo pipefail
WORK=${ORXLUA_WORK:-$(cd "$(dirname "$0")/../.." && pwd)}   # the directory that contains the clone
SRC=$(cd "$(dirname "$0")/../src" && pwd)
. "$(dirname "$0")/platform.sh"
BUILD=${BUILD:-$WORK/build}

if [ -z "${OOREXX_HOME:-}" ]; then
  REXX=$(command -v rexx) || { echo "build.sh: no rexx on the PATH (or set OOREXX_HOME)" >&2; exit 2; }
  OOREXX_HOME=$(cd "$(dirname "$(resolve_path "$REXX")")/.." && pwd)
fi
[ -f "$OOREXX_HOME/include/oorexxapi.h" ] || { echo "build.sh: no $OOREXX_HOME/include/oorexxapi.h" >&2; exit 2; }

LUA_VERSION=${LUA_VERSION:-5.4}
case $LUA_VERSION in 5.3|5.4|5.5) ;; *) echo "build.sh: LUA_VERSION must be 5.3, 5.4 or 5.5" >&2; exit 2 ;; esac
V=$LUA_VERSION
nolua() { echo "build.sh: no Lua $V $1 (scripts/setup-lua.sh $V builds one next to the clone)" >&2; exit 2; }
mkdir -p "$BUILD"

if [ "$OS" = Windows ]; then
  LUADIR=$(lua_dir "$V"); LUALIB=lua${V/./}
  [ -n "$LUADIR" ] && [ -f "$LUADIR/$LUALIB.dll" ] || nolua "next to the clone"
  # MSVC; no argument conversion by MSYS: every path is given the Windows way
  export MSYS2_ARG_CONV_EXCL='*'
  W() { cygpath -w "$1"; }
  OBJ=$(mktemp -d); trap 'rm -rf "$OBJ"' EXIT
  "$CXX" -nologo -LD -EHsc -O2 -MD -std:c++17 -W3 -Fo"$(W "$OBJ")\\" -DLUA_BUILD_AS_DLL \
      -I"$(W "$LUADIR")" -I"$(W "$OOREXX_HOME/include")" "$(W "$SRC/rexxlua.cpp")" -Fe"$(W "$BUILD/$LIB")" \
      -link -LIBPATH:"$(W "$OOREXX_HOME/lib")" -LIBPATH:"$(W "$LUADIR")" rexx.lib rexxapi.lib "$LUALIB.lib" >/dev/null
  rm -f "$BUILD"/*.exp "$BUILD"/*.lib
  cp "$LUADIR/$LUALIB.dll" "$BUILD/"
else
  if [ -z "${LUA_CFLAGS:-}${LUA_LIBS:-}" ]; then
    if [ "$OS" = Linux ]; then
      for pc in "lua$V" "lua-$V" "lua${V/./}"; do
        if pkg-config --exists "$pc" 2>/dev/null; then
          LUA_CFLAGS=$(pkg-config --cflags "$pc"); LUA_LIBS=$(pkg-config --libs "$pc"); break
        fi
      done
    fi
    if [ -z "${LUA_LIBS:-}" ]; then
      LUADIR=$(lua_dir "$V")
      [ -n "$LUADIR" ] && [ -f "$LUADIR/liblua.a" ] || nolua "in pkg-config or next to the clone, and no LUA_CFLAGS and LUA_LIBS"
      LUA_CFLAGS="-I$LUADIR"
      if [ "$OS" = Darwin ]; then LUA_LIBS="-Wl,-force_load,$LUADIR/liblua.a"
      else LUA_LIBS="-Wl,--whole-archive $LUADIR/liblua.a -Wl,--no-whole-archive -lm -ldl"; fi
    fi
  fi
  ORX="-I$OOREXX_HOME/include -L$OOREXX_HOME/lib"
  case $OOREXX_HOME in /usr|/usr/local) ;; *) ORX="$ORX -Wl,-rpath,$OOREXX_HOME/lib" ;; esac
  CXXFLAGS="-O2 -g -fPIC -Wall -Wno-unused-parameter -std=c++17"
  if [ "$OS" = Darwin ]; then LDFLAGS="-dynamiclib -Wl,-install_name,$LIB"; else LDFLAGS="-shared"; fi
  # shellcheck disable=SC2086
  "$CXX" $CXXFLAGS $LDFLAGS $LUA_CFLAGS $ORX "$SRC/rexxlua.cpp" \
      -o "$BUILD/$LIB" -lrexx -lrexxapi $LUA_LIBS
fi
cp "$SRC/lua.cls" "$BUILD/"
echo "build.sh: $BUILD/$LIB, $BUILD/lua.cls (Lua $V; ooRexx: $OOREXX_HOME)"
