# shellcheck shell=bash
# What differs between Linux, macOS and Windows (Git Bash, with MSVC's cl on
# the PATH), for the build and test scripts (sourced, not run:
# `. "$(dirname "$0")/../scripts/platform.sh"`). Sets:
#
#   OS        Linux | Darwin | Windows
#   PKG       the ooRexx package's file (not LIB: MSVC's linker uses that): librexxlua.so | librexxlua.dylib |
#             rexxlua.dll (ooRexx loads lib<name>.so/.dylib, <name>.dll)
#   MODEXT    the extension of Lua C modules: so | so | dll (LuaRocks's)
#   MOD       the Lua module (require "oorexx"): oorexx.so | oorexx.so | oorexx.dll
#   LIBVAR    LD_LIBRARY_PATH | DYLD_LIBRARY_PATH | PATH: where ooRexx finds
#             external libraries by name (macOS drops DYLD_* variables when
#             it starts a protected program such as /bin/sh, so a command run
#             through a shell must set it itself)
#   CXX, CC   g++, cc | c++, cc | cl, cl
#
# and the functions resolve_path PATH (symbolic links followed), native_path
# PATH and native_path_list PATH... (as the platform's programs want them:
# C:\x and ";" on Windows) and timeout (coreutils' timeout, else Homebrew's
# gtimeout, else none).

OS=$(uname -s)
case $OS in MINGW*|MSYS*|CYGWIN*) OS=Windows ;; esac

resolve_path() {
  local p=$1 t
  while [ -L "$p" ]; do
    t=$(readlink "$p")
    case $t in /*) p=$t ;; *) p=$(dirname "$p")/$t ;; esac
  done
  echo "$(cd "$(dirname "$p")" && pwd)/$(basename "$p")"
}

if [ "$OS" = Windows ]; then
  native_path() { cygpath -w "$1"; }
  native_path_list() { local IFS=:; cygpath -w -p "$*"; }
else
  native_path() { echo "$1"; }
  native_path_list() { local IFS=:; echo "$*"; }
fi

MOD=oorexx.so; [ "$OS" = Windows ] && MOD=oorexx.dll
case $OS in
  Darwin)  PKG=librexxlua.dylib MODEXT=so  LIBVAR=DYLD_LIBRARY_PATH CXX=${CXX:-c++} CC=${CC:-cc} ;;
  Windows) PKG=rexxlua.dll      MODEXT=dll LIBVAR=PATH              CXX=${CXX:-cl}  CC=${CC:-cl} ;;
  *)       PKG=librexxlua.so    MODEXT=so  LIBVAR=LD_LIBRARY_PATH   CXX=${CXX:-g++} CC=${CC:-cc} ;;
esac

if ! command -v timeout >/dev/null 2>&1; then
  if command -v gtimeout >/dev/null 2>&1; then timeout() { gtimeout "$@"; }
  else timeout() { while [ "${1#-}" != "$1" ]; do shift 2; done; shift; "$@"; }
  fi
fi

# quiet CMD...: runs CMD with its output kept, shown only if it fails (cl
# writes its errors to stdout, among the names of the files it compiles)
quiet() {
  local log rc=0
  log=$(mktemp)
  "$@" >"$log" 2>&1 || rc=$?
  [ $rc -eq 0 ] || cat "$log" >&2
  rm -f "$log"
  return $rc
}

# lua_exe V: the standalone lua that setup-lua.sh built next to the clone
# (empty if none)
lua_exe() {
  local d; d=$(lua_dir "$1")
  if [ -n "$d" ] && [ -f "$d/lua.exe" ]; then echo "$d/lua.exe"
  elif [ -n "$d" ] && [ -f "$d/lua" ]; then echo "$d/lua"; fi
}

# lua_dir V: the src/ of the Lua V (5.3, 5.4, 5.5) that setup-lua.sh built
# next to the clone, the newest if there are several; empty if none
lua_dir() {
  ls -d "$WORK/lua-$1".*/src 2>/dev/null | sort -V | tail -1 || true
}
