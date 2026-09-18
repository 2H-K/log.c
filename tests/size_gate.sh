#!/bin/sh
# A3 size gate: fail if the LOG_MINIMAL build grows beyond the recorded
# baseline by more than the allowed slack. Guards against accidental bloat in
# the minimal/embedded configuration.
#
# Usage: tests/size_gate.sh [compiler]
# Baseline: .text + .data + .bss of `-std=c17 -O2 -DLOG_MINIMAL src/log.c`,
# measured with GNU `size` on ubuntu-latest x86_64 (GCC). Update the constant
# only with a deliberate, reviewed size change.

set -eu

CC="${1:-${CC:-cc}}"
BASELINE=18073   # bytes, LOG_MINIMAL (see header comment)
SLACK=2048       # allow +2 KiB before failing

tmp="${TMPDIR:-/tmp}/logc_size_gate.$$.o"
trap 'rm -f "$tmp"' EXIT

"$CC" -std=c17 -O2 -Isrc -c -DLOG_MINIMAL src/log.c -o "$tmp"

# shellcheck disable=SC2046
set -- $(size "$tmp" | awk 'NR==2 { print $1, $2, $3 }')
text="$1"
data="$2"
bss="$3"
total=$((text + data + bss))
limit=$((BASELINE + SLACK))

echo "LOG_MINIMAL size: text=$text data=$data bss=$bss total=$total (baseline=$BASELINE, limit=$limit)"

if [ "$total" -gt "$limit" ]; then
  echo "FAIL: LOG_MINIMAL grew by $((total - BASELINE)) bytes over baseline (limit +${SLACK})" >&2
  exit 1
fi

echo "OK: within size budget"
