#!/bin/sh
# M9-06 fixture census: every G3 fixture with a C harness is compiled through
# the u-MIR C emitter, linked, and run.  Per-fixture results are reported;
# the exit status is 0 (this is a measurement, not a gate) unless REQUIRE
# names fixtures that must pass.
set -u
: "${PROBE:=build/probe-ref6/probe_core_hir}"
: "${CC:=gcc}"
command -v "$CC" >/dev/null 2>&1 || { echo "umir-fixtures: SKIP ($CC unavailable)"; exit 0; }
test -x "$PROBE" || { echo "umir-fixtures: SKIP (probe not built)"; exit 0; }
test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
out_dir=$(mktemp -d "${TMPDIR:-/tmp}/cmrustc-umir-fixtures.XXXXXX")
trap 'rm -rf "$out_dir"' EXIT HUP INT TERM
pass=0; fail=0; total=0
for harness in "$test_dir"/fixtures/*-harness.c; do
    base=$(basename "$harness" -harness.c)
    fixture="$test_dir/fixtures/$base.rs"
    test -f "$fixture" || continue
    total=$((total + 1))
    if CMRUSTC_CC="$CC" "$PROBE" "$fixture" --emit-umir-c "$out_dir/$base.c" \
            >"$out_dir/$base.probe" 2>&1 \
        && grep -q '^emit-umir-c' "$out_dir/$base.probe" \
        && "$CC" -std=c99 -w -o "$out_dir/$base" "$out_dir/$base.c" "$harness" \
            >"$out_dir/$base.cc" 2>&1 \
        && "$out_dir/$base" >/dev/null 2>&1; then
        pass=$((pass + 1)); echo "umir-fixture $base=pass"
    else
        fail=$((fail + 1)); echo "umir-fixture $base=fail"
    fi
done
echo "umir-fixtures total=$total pass=$pass fail=$fail"
if test "${CMRUSTC_REQUIRE_UMIR_FIXTURES:-0}" = 1 && test "$fail" -ne 0; then
    echo "umir-fixtures: required but $fail fixture(s) failed"
    exit 1
fi
