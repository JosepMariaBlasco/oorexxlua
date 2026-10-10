#!/usr/bin/env bash
# Download TeX Live's own LuaTeX binaries (the official ones, from CTAN) into
# $ORXLUA_WORK/texlive-luatex (next to the clone), to test the Lua module
# (require "oorexx") in LuaTeX, which is Lua 5.3: tests/run.sh with
# LUA_EXE=<texlua> and LUA_VERSION=5.3.  Prints the path of texlua.
#   Linux, macOS  luatex.<platform>: luatex exports the lua_* symbols, so the
#                 usual module (oorexx.so) works
#   Windows       luatex.windows and kpathsea.windows (msvcr100.dll): LuaTeX's
#                 Lua is lua53w64.dll, so the module is build.sh's
#                 texlive/oorexx.dll, linked against it
# CTAN_MIRROR to choose a mirror; otherwise a few known ones are tried in turn
# (mirror.ctan.org's redirection may land on one that is down).
#   scripts/setup-luatex.sh
set -euo pipefail
WORK=${ORXLUA_WORK:-$(cd "$(dirname "$0")/../.." && pwd)}   # the directory that contains the clone
. "$(dirname "$0")/platform.sh"
MIRRORS=${CTAN_MIRROR:-https://ctan.math.illinois.edu https://mirrors.mit.edu/CTAN https://ftp.fau.de/ctan https://mirror.ctan.org}
case $OS in
  Windows) P=windows; PKGS="luatex.windows kpathsea.windows"; EXE=texlua.exe ;;
  Darwin)  P=universal-darwin; PKGS="luatex.$P"; EXE=texlua ;;
  *)       case $(uname -m) in aarch64|arm64) P=aarch64-linux ;; *) P=x86_64-linux ;; esac
           PKGS="luatex.$P"; EXE=texlua ;;
esac
DIR=$WORK/texlive-luatex
BIN=$DIR/bin/$P
if [ ! -e "$BIN/$EXE" ]; then
  mkdir -p "$DIR"
  for p in $PKGS; do
    ok=
    for m in $MIRRORS; do
      if curl -sSfL --retry 2 --retry-all-errors --connect-timeout 20 -o "$DIR/$p.tar.xz" \
           "$m/systems/texlive/tlnet/archive/$p.tar.xz" && tar xJf "$DIR/$p.tar.xz" -C "$DIR"; then
        ok=1; break
      fi
      echo "setup-luatex.sh: $m failed, trying another mirror" >&2
    done
    rm -f "$DIR/$p.tar.xz"
    [ -n "$ok" ] || { echo "setup-luatex.sh: cannot download $p" >&2; exit 1; }
  done
  # the macOS and Linux packages make texlua a link to luatex; not every tar
  # keeps it
  [ -e "$BIN/$EXE" ] || ln -s luatex "$BIN/$EXE"
fi
[ -e "$BIN/$EXE" ] || { echo "setup-luatex.sh: no $BIN/$EXE" >&2; exit 1; }
echo "$BIN/$EXE"
