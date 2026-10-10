#!/usr/bin/env bash
# Run one example against the build: examples/run-one.sh 01-tour.rex [args...]
set -euo pipefail
WORK=${ORXLUA_WORK:-$(cd "$(dirname "$0")/../.." && pwd)}
. "$(dirname "$0")/../scripts/platform.sh"
BUILD=${BUILD:-$WORK/build}
BUILD=$(cd "$BUILD" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
name=$1; shift
export $LIBVAR="$BUILD${!LIBVAR:+:${!LIBVAR}}"
export REXX_PATH=$(native_path_list "$BUILD")
exec rexx "$(native_path "$HERE/$name")" "$@"
