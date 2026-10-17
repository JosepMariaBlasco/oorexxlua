#!/usr/bin/env bash
# Build Lua embedded in ooRexx into $BUILD (default $ORXLUA_WORK/build;
# ORXLUA_WORK is the directory that contains the clone):
#   librexxlua.so   the ooRexx native package, with Lua inside
#                   (librexxlua.dylib on macOS; rexxlua.dll on Windows, with
#                   the luaXY.dll it uses copied next to it)
#   lua.cls         the Rexx side (.lua, .LuaState, .LuaObject)
#   oorexx.so       the Lua module, require "oorexx" (Lua -> ooRexx): the same
#                   source, not linked against any Lua, so that it uses the
#                   Lua that loads it (on Windows, oorexx.dll: a copy of
#                   rexxlua.dll, since there every Lua module is linked
#                   against the Lua DLL anyway)
#   texlive/        on Windows with Lua 5.3, the module for TeX Live's LuaTeX,
#                   whose Lua is lua53w64.dll: oorexx.dll linked against it,
#                   and lua.cls
# ooRexx: the installation of the `rexx` on the PATH (OOREXX_HOME to choose
# another), which must have include/oorexxapi.h and lib/librexx.* (rexx.lib
# on Windows); an installation outside /usr and /usr/local is also recorded
# as the library's run path.
# Lua: LUA_VERSION (5.3, 5.4 or 5.5; default 5.4), found
#   - with LUA_CFLAGS and LUA_LIBS, if given (Linux and macOS);
#   - on Linux, with pkg-config (Debian/Ubuntu: liblua5.4-dev, liblua5.3-dev),
#     unless LUA_STATIC=1 (what package.sh needs: a package with no liblua
#     to install);
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
  # _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR: std::mutex made by the constexpr
  # constructor of a recent MSVC crashes with an older msvcp140.dll, and a host
  # may have loaded one (TeX Live ships its own): Microsoft's workaround
  MSVCFLAGS="-nologo -LD -EHsc -O2 -MD -Zi -std:c++17 -W3 -Fo$(W "$OBJ")\\ -Fd$(W "$OBJ")\\ -DLUA_BUILD_AS_DLL -D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR"
  quiet "$CXX" $MSVCFLAGS \
      -I"$(W "$LUADIR")" -I"$(W "$OOREXX_HOME/include")" "$(W "$SRC/rexxlua.cpp")" -Fe"$(W "$BUILD/$PKG")" \
      -link -DEBUG -OPT:REF -OPT:ICF -LIBPATH:"$(W "$OOREXX_HOME/lib")" -LIBPATH:"$(W "$LUADIR")" rexx.lib rexxapi.lib "$LUALIB.lib"
  rm -f "$BUILD"/*.exp "$BUILD"/*.lib
  cp "$LUADIR/$LUALIB.dll" "$BUILD/"
  cp "$BUILD/$PKG" "$BUILD/$MOD"
  if [ "$V" = 5.3 ]; then
    # TeX Live's LuaTeX: its Lua is lua53w64.dll (Lua 5.3, the same API), and
    # Windows binds a DLL's imports by the DLL's name.  An import library for
    # that name, made from the exports of our lua53.dll
    { echo "LIBRARY lua53w64.dll"; echo "EXPORTS"
      dumpbin -nologo -exports "$(W "$LUADIR/$LUALIB.dll")" | tr -d '\r' |
        awk '$1 ~ /^[0-9]+$/ && $4 ~ /^lua/ { print "    " $4 }'
    } > "$OBJ/lua53w64.def"
    quiet lib -nologo -def:"$(W "$OBJ/lua53w64.def")" -machine:x64 -out:"$(W "$OBJ/lua53w64.lib")"
    mkdir -p "$BUILD/texlive"
    quiet "$CXX" $MSVCFLAGS \
        -I"$(W "$LUADIR")" -I"$(W "$OOREXX_HOME/include")" "$(W "$SRC/rexxlua.cpp")" -Fe"$(W "$BUILD/texlive/$MOD")" \
        -link -DEBUG -OPT:REF -OPT:ICF -LIBPATH:"$(W "$OOREXX_HOME/lib")" rexx.lib rexxapi.lib "$(W "$OBJ/lua53w64.lib")"
    rm -f "$BUILD"/texlive/*.exp "$BUILD"/texlive/*.lib
    cp "$SRC/lua.cls" "$SRC/boolean.cls" "$BUILD/texlive/"
  fi
else
  if [ -z "${LUA_CFLAGS:-}${LUA_LIBS:-}" ]; then
    if [ "$OS" = Linux ] && [ "${LUA_STATIC:-}" != 1 ]; then
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
  if [ "$OS" = Darwin ]; then LDFLAGS="-dynamiclib -Wl,-install_name,$PKG"; else LDFLAGS="-shared"; fi
  # shellcheck disable=SC2086
  "$CXX" $CXXFLAGS $LDFLAGS $LUA_CFLAGS $ORX "$SRC/rexxlua.cpp" \
      -o "$BUILD/$PKG" -lrexx -lrexxapi $LUA_LIBS
  if [ "$OS" = Darwin ]; then MODFLAGS="-bundle -undefined dynamic_lookup"; else MODFLAGS="-shared"; fi
  # shellcheck disable=SC2086
  "$CXX" $CXXFLAGS $MODFLAGS $LUA_CFLAGS $ORX "$SRC/rexxlua.cpp" -o "$BUILD/$MOD" -lrexx -lrexxapi
fi
cp "$SRC/lua.cls" "$SRC/boolean.cls" "$BUILD/"
echo "build.sh: $BUILD/$PKG, $BUILD/$MOD, $BUILD/lua.cls, boolean.cls (Lua $V; ooRexx: $OOREXX_HOME)"
