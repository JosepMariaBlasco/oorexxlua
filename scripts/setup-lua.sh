#!/usr/bin/env bash
# Download and build a Lua from lua.org into $ORXLUA_WORK/lua-X.Y.Z (next to
# the clone), for build.sh and tests/run.sh, which find it there by its X.Y:
#   Linux, macOS  a static liblua.a compiled with -fPIC (build.sh links it
#                 whole into the package, so C modules find every lua_* in it)
#   Windows       luaXY.dll and its import library luaXY.lib, built with MSVC
#                 (Git Bash with cl on the PATH): Windows has no global
#                 symbols, so the package and the C modules must share one DLL
# and the standalone interpreter, lua (lua.exe), which tests/run.sh uses to
# test the Lua module oorexx (it exports the lua_* symbols, as Lua's own
# Makefile builds it; no readline).
# Needed where no distribution packages the Lua (5.5 anywhere; every Lua on
# macOS and Windows).
#   scripts/setup-lua.sh [5.3|5.4|5.5|X.Y.Z]      (default 5.5)
set -euo pipefail
WORK=${ORXLUA_WORK:-$(cd "$(dirname "$0")/../.." && pwd)}   # the directory that contains the clone
. "$(dirname "$0")/platform.sh"
V=${1:-5.5}
case $V in                      # the latest release of each version
  5.3) V=5.3.6 ;; 5.4) V=5.4.9 ;; 5.5) V=5.5.1 ;;
  5.[345].*) ;;
  *) echo "setup-lua.sh: a Lua 5.3, 5.4 or 5.5, not $V" >&2; exit 2 ;;
esac
XY=${V%.*}; XY=${XY/./}                                     # 5.4.8 -> 54
DIR=$WORK/lua-$V
if [ "$OS" = Windows ]; then OUT=$DIR/src/lua$XY.dll EXE=$DIR/src/lua.exe; else OUT=$DIR/src/liblua.a EXE=$DIR/src/lua; fi
if [ ! -f "$OUT" ]; then
  mkdir -p "$WORK"
  curl -sSfL --retry 3 --retry-all-errors "https://www.lua.org/ftp/lua-$V.tar.gz" | tar xz -C "$WORK"
  cd "$DIR/src"
  case $OS in
    Windows)
      export MSYS2_ARG_CONV_EXCL='*'
      # cl -LD runs MSVC's link (Git Bash has a link of its own); it also
      # makes the import library, lua$XY.lib
      # shellcheck disable=SC2046
      quiet cl -nologo -O2 -MD -W3 -LD -DLUA_BUILD_AS_DLL $(ls ./*.c | grep -v -e '/lua\.c$' -e '/luac\.c$') \
         -Fe"lua$XY.dll"
      rm -f ./*.obj ./*.exp ;;
    Darwin) make -s liblua.a SYSCFLAGS=-DLUA_USE_MACOSX MYCFLAGS=-fPIC 2>&1 >/dev/null | grep -v "modifier ignored" >&2 || true ;;
    *)      make -s liblua.a SYSCFLAGS=-DLUA_USE_LINUX  MYCFLAGS=-fPIC 2>&1 >/dev/null | grep -v "modifier ignored" >&2 || true ;;
  esac
  [ -f "$OUT" ] || { echo "setup-lua.sh: the build of Lua $V made no $OUT" >&2; exit 1; }
fi
if [ ! -f "$EXE" ]; then
  cd "$DIR/src"
  case $OS in
    Windows)
      export MSYS2_ARG_CONV_EXCL='*'
      quiet cl -nologo -O2 -MD -W3 -DLUA_BUILD_AS_DLL lua.c -Felua.exe -link "lua$XY.lib"
      rm -f ./lua.obj ;;
    Darwin) "${CC:-cc}" -O2 -DLUA_USE_POSIX -DLUA_USE_DLOPEN lua.c liblua.a -o lua ;;
    *)      "${CC:-cc}" -O2 -DLUA_USE_POSIX -DLUA_USE_DLOPEN lua.c liblua.a -o lua -Wl,-E -ldl -lm ;;
  esac
  [ -f "$EXE" ] || { echo "setup-lua.sh: the build of Lua $V made no $EXE" >&2; exit 1; }
fi
echo "setup-lua.sh: $DIR/src ($(basename "$OUT"), $(basename "$EXE"), headers)"
