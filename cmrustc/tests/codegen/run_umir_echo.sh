#!/bin/sh
# M9-06 first executable gate: the u-MIR C emitter compiles the G3
# blanket-echo no_core fixture, links it with the C harness, and runs it.
set -eu
: "${PROBE:=build/probe-ref6/probe_core_hir}"
: "${CC:=gcc}"
command -v "$CC" >/dev/null 2>&1 || { echo "umir echo executable acceptance: SKIP ($CC unavailable)"; exit 0; }
test -x "$PROBE" || { echo "umir echo executable acceptance: SKIP (probe not built)"; exit 0; }
test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
fixture="$test_dir/fixtures/blanket-echo.rs"
harness="$test_dir/fixtures/blanket-echo-harness.c"
out_dir=$(mktemp -d "${TMPDIR:-/tmp}/cmrustc-umir-echo.XXXXXX")
trap 'rm -rf "$out_dir"' EXIT HUP INT TERM
CMRUSTC_CC="$CC" "$PROBE" "$fixture" --emit-umir-c "$out_dir/echo.c" \
    >"$out_dir/probe.out" 2>&1 || { cat "$out_dir/probe.out"; exit 1; }
grep -q '^emit-umir-c' "$out_dir/probe.out" || { cat "$out_dir/probe.out"; exit 1; }
"$CC" -std=c99 -w -o "$out_dir/echo" "$out_dir/echo.c" "$harness" \
    || { echo "umir echo: C compile/link failed"; head -40 "$out_dir/echo.c"; exit 1; }
if timeout 10 "$out_dir/echo"; then
    echo "umir echo executable acceptance: ok"
else
    echo "umir echo executable acceptance: FAILED (exit $?)"
    exit 1
fi
