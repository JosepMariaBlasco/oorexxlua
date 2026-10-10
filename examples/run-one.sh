#!/usr/bin/env bash
# Run one example against the build: examples/run-one.sh tour.rex [args...]
set -euo pipefail
WORK=${ORXLUA_WORK:-$(cd "$(dirname "$0")/../.." && pwd)}
BUILD=${BUILD:-$WORK/build}
HERE=$(cd "$(dirname "$0")" && pwd)
name=$1; shift
export LD_LIBRARY_PATH="$BUILD${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export REXX_PATH="$BUILD${REXX_PATH:+:$REXX_PATH}"
exec rexx "$HERE/$name" "$@"
