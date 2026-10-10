#!/usr/bin/env bash
# Download and build a Lua from lua.org into $ORXLUA_WORK/lua-X.Y.Z (next to
# the clone), as a static liblua.a compiled with -fPIC, for the versions no
# distribution packages yet (5.5).  build.sh and tests/run.sh find it there
# when LUA_VERSION is 5.5 and pkg-config has no Lua 5.5.
#   scripts/setup-lua.sh [5.5.0]
set -euo pipefail
WORK=${ORXLUA_WORK:-$(cd "$(dirname "$0")/../.." && pwd)}
V=${1:-5.5.0}
DIR=$WORK/lua-$V
if [ ! -f "$DIR/src/liblua.a" ]; then
  mkdir -p "$WORK"
  curl -sSfL "https://www.lua.org/ftp/lua-$V.tar.gz" | tar xz -C "$WORK"
  make -s -C "$DIR" linux MYCFLAGS=-fPIC 2>&1 >/dev/null | grep -v "modifier ignored" >&2 || true
fi
echo "setup-lua.sh: $DIR/src (liblua.a, headers)"
