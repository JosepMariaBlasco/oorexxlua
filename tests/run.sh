#!/usr/bin/env bash
# Run every example and compare its output with the expected one
# (examples/NAME.rex -> examples/NAME.out), each under a timeout.
set -uo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
TIMEOUT=${TIMEOUT:-60}
pass=0 fail=0
for rex in "$HERE"/examples/*.rex; do
  name=$(basename "$rex")
  expected=${rex%.rex}.out
  [ -f "$expected" ] || continue
  if diff -u "$expected" <(timeout "$TIMEOUT" "$HERE/examples/run-one.sh" "$name" 2>&1) >/tmp/orxlua-diff.$$; then
    echo "ok    $name"; pass=$((pass + 1))
  else
    echo "FAIL  $name"; cat /tmp/orxlua-diff.$$; fail=$((fail + 1))
  fi
done
rm -f /tmp/orxlua-diff.$$
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
