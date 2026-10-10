#!/usr/bin/env bash
# Build ooRexx from SVN with its API headers and libraries, for building and
# testing the bridge (Linux; an alternative to an installed ooRexx, such as
# the .deb, or the portable builds that the CI uses).  Idempotent: skips what
# is already there.  build.sh uses it with OOREXX_HOME=$ORXLUA_WORK/oorexx-inst.
#   setup-oorexx.sh            (OOREXX_REV, OOREXX_SRC, OOREXX_BUILD, OOREXX_INST)
set -euo pipefail
WORK=${ORXLUA_WORK:-$(cd "$(dirname "$0")/../.." && pwd)}   # the directory that contains the clone
REV=${OOREXX_REV:-13268}
SRC=${OOREXX_SRC:-$WORK/oorexx}
BUILD=${OOREXX_BUILD:-$WORK/oorexx-build}
INST=${OOREXX_INST:-$WORK/oorexx-inst}
need() { command -v "$1" >/dev/null || { apt-get update -qq && apt-get install -y -qq "$2" >/dev/null; }; }
need svn subversion; need cmake cmake; need g++ g++; need make make
if [[ ! -d "$SRC" ]]; then
  echo "== svn checkout r$REV -> $SRC"
  svn checkout -q -r "$REV" https://svn.code.sf.net/p/oorexx/code-0/main/trunk "$SRC"
fi
if [[ ! -x "$INST/bin/rexx" ]]; then
  echo "== build -> $BUILD, install -> $INST"
  mkdir -p "$BUILD"
  cmake -S "$SRC" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$INST" >/dev/null
  cmake --build "$BUILD" -j"$(nproc)" >/dev/null
  cmake --install "$BUILD" >/dev/null
fi
echo "== $(LD_LIBRARY_PATH="$INST/lib" "$INST/bin/rexx" -v | head -1)"
ls "$INST/include" | tr '\n' ' '; echo
echo "use: LD_LIBRARY_PATH=$INST/lib $INST/bin/rexx ...; headers in $INST/include"
