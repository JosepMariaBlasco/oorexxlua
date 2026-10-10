#!/usr/bin/env bash
# build-cmodule.sh DEST: build the test C module (tests/cmodule/hello.c) into
# DEST/hello.so (hello.dll on Windows) as LuaRocks builds Lua C modules: on
# Linux and macOS not linked against liblua (the lua_* symbols come from
# whoever loads it); on Windows linked against luaXY.dll, as every Lua module
# on Windows is.  The Lua is the build's: LUA_VERSION (default 5.4) found as
# build.sh finds it (LUA_STATIC=1 too), or LUA_CFLAGS.  Used by tests/run.sh
# and scripts/package.sh.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=${ORXLUA_WORK:-$(cd "$ROOT/.." && pwd)}
. "$ROOT/scripts/platform.sh"
DEST=${1:?usage: build-cmodule.sh DEST}
mkdir -p "$DEST" && DEST=$(cd "$DEST" && pwd)
V=${LUA_VERSION:-5.4}
SRC=$ROOT/tests/cmodule/hello.c
if [ "$OS" = Windows ]; then
  LUADIR=$(lua_dir "$V")
  [ -n "$LUADIR" ] || { echo "build-cmodule.sh: no Lua $V next to the clone (scripts/setup-lua.sh $V)" >&2; exit 2; }
  export MSYS2_ARG_CONV_EXCL='*'
  cd "$DEST"
  quiet cl -nologo -O2 -MD -LD -DLUA_BUILD_AS_DLL -I"$(native_path "$LUADIR")" "$(native_path "$SRC")" \
     -Fehello.dll -link -LIBPATH:"$(native_path "$LUADIR")" "lua${V/./}.lib" -EXPORT:luaopen_hello
  rm -f hello.obj hello.exp hello.lib
else
  if [ -z "${LUA_CFLAGS:-}" ]; then
    if [ "$OS" = Linux ] && [ "${LUA_STATIC:-}" != 1 ] && pkg-config --exists "lua$V" 2>/dev/null; then LUA_CFLAGS=$(pkg-config --cflags "lua$V")
    else LUADIR=$(lua_dir "$V"); [ -n "$LUADIR" ] && LUA_CFLAGS="-I$LUADIR"; fi
  fi
  if [ "$OS" = Darwin ]; then MODFLAGS="-bundle -undefined dynamic_lookup"; else MODFLAGS="-shared"; fi
  # shellcheck disable=SC2086
  "$CC" -O2 -fPIC $MODFLAGS ${LUA_CFLAGS:-} "$SRC" -o "$DEST/hello.$MODEXT"
fi
