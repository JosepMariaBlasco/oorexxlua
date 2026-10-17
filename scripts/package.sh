#!/usr/bin/env bash
# package.sh [DEST]: the bridge for this platform and Lua as a zip of
# binaries, for people who want to use it without building it.  Run after
# build.sh, with the same LUA_VERSION (and on Linux LUA_STATIC=1): the
# package carries its Lua, so that users need none installed.
#
#   oorexxlua-VERSION-PLATFORM-luaX.Y.zip, one folder oorexxlua/: the ooRexx
#   package with Lua linked in (librexxlua.so / .dylib, stripped; on Windows
#   rexxlua.dll and the luaXY.dll it shares with Lua's C modules), lua.cls,
#   the Lua module (oorexx.so / oorexx.dll; on Windows with Lua 5.3 also
#   texlive/, the module for TeX Live's LuaTeX), README.md (dist/README.md:
#   how to install), check.rex and check.lua, the examples with their
#   outputs, the licence, and Lua's (LICENSE-Lua.txt).
#
# VERSION: ORXLUA_VERSION, else YYYYMMDD-COMMIT.  PLATFORM: linux-x64,
# macos-arm64, macos-x64, windows-x64...  The folder is checked before it is
# zipped: check.rex runs from outside it, finding everything through the
# variables the README tells users to set, and loads a Lua C module built as
# LuaRocks builds them (scripts/build-cmodule.sh); and check.lua runs the Lua
# module from a standalone lua of the same version (scripts/setup-lua.sh's).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
WORK=${ORXLUA_WORK:-$(cd "$ROOT/.." && pwd)}
. "$HERE/platform.sh"
BUILD=${BUILD:-$WORK/build}
DEST=${1:-.}
V=${LUA_VERSION:-5.4}
XY=${V/./}

# the Lua: the one setup-lua.sh built, which build.sh linked in (or, on
# Windows, put next to the package); a liblua the users would need is refused
LUADIR=$(lua_dir "$V")
[ -n "$LUADIR" ] || { echo "package.sh: no Lua $V next to the clone (scripts/setup-lua.sh $V)" >&2; exit 2; }
[ -f "$BUILD/$PKG" ] || { echo "package.sh: no $BUILD/$PKG (run build.sh)" >&2; exit 2; }
case $OS in
  Linux)  if ldd "$BUILD/$PKG" | grep -q liblua; then
            echo "package.sh: $BUILD/$PKG uses a shared liblua: build with LUA_STATIC=1" >&2; exit 2; fi ;;
  Darwin) if otool -L "$BUILD/$PKG" | grep -q liblua; then
            echo "package.sh: $BUILD/$PKG uses a shared liblua" >&2; exit 2; fi ;;
esac
RELEASE=$(basename "$(dirname "$LUADIR")"); RELEASE=${RELEASE#lua-}     # lua-5.4.9/src -> 5.4.9

case $(uname -m) in arm64|aarch64) ARCH=arm64 ;; *) ARCH=x64 ;; esac
case $OS in Darwin) PLAT=macos-$ARCH ;; Windows) PLAT=windows-$ARCH ;; *) PLAT=linux-$ARCH ;; esac
VERSION=${ORXLUA_VERSION:-$(date +%Y%m%d)-$(git -C "$ROOT" rev-parse --short HEAD)}
NAME=oorexxlua-$VERSION-$PLAT-lua$V

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
STAGE=$TMP/oorexxlua
mkdir -p "$STAGE/examples" "$TMP/elsewhere"
[ -f "$BUILD/$MOD" ] || { echo "package.sh: no $BUILD/$MOD (run build.sh)" >&2; exit 2; }
cp "$BUILD/$PKG" "$BUILD/$MOD" "$BUILD/lua.cls" "$BUILD/boolean.cls" "$STAGE/"
[ "$OS" = Windows ] && cp "$BUILD/lua$XY.dll" "$STAGE/"
if [ -d "$BUILD/texlive" ]; then
  mkdir -p "$STAGE/texlive" && cp "$BUILD/texlive/$MOD" "$BUILD/texlive/lua.cls" "$BUILD/texlive/boolean.cls" "$STAGE/texlive/"
fi
# build.sh keeps debug information (-g); users get the binaries without it
# (and without local symbols), but with the lua_* that C modules need.  On
# Windows the debug information is in .pdb files, which are not packaged.
case $OS in
  Linux)  strip --strip-unneeded "$STAGE/$PKG" "$STAGE/$MOD" ;;
  Darwin) strip -x "$STAGE/$PKG" "$STAGE/$MOD" ;;
esac
cp "$ROOT/dist/README.md" "$ROOT/dist/check.rex" "$ROOT/dist/check.lua" "$ROOT/LICENSE" "$STAGE/"
cp "$ROOT"/examples/*.rex "$ROOT"/examples/*.lua "$ROOT"/examples/*.out "$STAGE/examples/"
mkdir -p "$STAGE/examples/latex" && cp "$ROOT"/examples/latex/* "$STAGE/examples/latex/"
{ echo "This package contains Lua $RELEASE (https://www.lua.org), under this licence:"
  echo
  sed -n '/^\* Copyright/,/^\*\*\*\*/p' "$LUADIR/lua.h" | sed -e '$d' -e 's/^\* \{0,1\}//'
} > "$STAGE/LICENSE-Lua.txt"
grep -q "Permission is hereby granted" "$STAGE/LICENSE-Lua.txt" ||
  { echo "package.sh: no licence found in $LUADIR/lua.h" >&2; exit 1; }

# the check, as a user would run it: from another directory, with a C module
"$HERE/build-cmodule.sh" "$TMP/modules"
DIR=$(native_path "$STAGE")
(cd "$TMP/elsewhere" &&
  env "$LIBVAR=$STAGE${!LIBVAR:+:${!LIBVAR}}" REXX_PATH="$DIR" \
      LUA_CPATH="$(native_path "$TMP/modules")/?.$MODEXT" ORXLUA_CHECK_MODULE=hello \
      rexx "$DIR/check.rex")
LUA_EXE=$(lua_exe "$V")
SEP=/; [ "$OS" = Windows ] && SEP='\'

[ -n "$LUA_EXE" ] || { echo "package.sh: no standalone lua $V to check the Lua module (scripts/setup-lua.sh $V)" >&2; exit 2; }
(cd "$TMP/elsewhere" &&
  env LUA_CPATH="$DIR$SEP?.$MODEXT" "$LUA_EXE" "$(native_path "$STAGE/check.lua")")

mkdir -p "$DEST"
ZIP=$(cd "$DEST" && pwd)/$NAME.zip
rm -f "$ZIP"
# (Windows: python; its python3 may be the Store's stub)
if [ "$OS" = Windows ]; then PY=$(command -v python || true); else PY=$(command -v python3 || command -v python || true); fi
[ -n "$PY" ] || { echo "package.sh: no Python to make the zip" >&2; exit 2; }
(cd "$TMP" && "$PY" -m zipfile -c "$(native_path "$ZIP")" oorexxlua)
echo "$ZIP"
