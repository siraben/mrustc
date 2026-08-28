#!/bin/sh
# M9-06 core-linked census: fixtures under core-fixtures/ compile against
# the real core crate (--with-core) through the u-MIR C emitter, link with
# their C harness, and run.  A measurement unless CMRUSTC_REQUIRE_UMIR_CORE=1.
set -u
: "${PROBE:=build/probe-ref6/probe_core_hir}"
: "${CC:=gcc}"
: "${CORE_LIB:=/tmp/cmrustc-rust190-source/rustc-1.90.0-src/library/core/src/lib.rs}"
command -v "$CC" >/dev/null 2>&1 || { echo "umir-core: SKIP ($CC unavailable)"; exit 0; }
test -x "$PROBE" || { echo "umir-core: SKIP (probe not built)"; exit 0; }
test -f "$CORE_LIB" || { echo "umir-core: SKIP (core source unavailable)"; exit 0; }
test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
out_dir=$(mktemp -d "${TMPDIR:-/tmp}/cmrustc-umir-core.XXXXXX")
trap 'rm -rf "$out_dir"' EXIT HUP INT TERM
pass=0; fail=0; total=0
for harness in "$test_dir"/core-fixtures/*-harness.c; do
    base=$(basename "$harness" -harness.c)
    fixture="$test_dir/core-fixtures/$base.rs"
    test -f "$fixture" || continue
    total=$((total + 1))
    if CMRUSTC_CC="$CC" timeout 1800 "$PROBE" "$fixture" --with-core "$CORE_LIB" \
            --emit-umir-c "$out_dir/$base.c" >"$out_dir/$base.probe" 2>&1 \
        && grep -q '^emit-umir-c' "$out_dir/$base.probe" \
        && "$CC" -std=c99 -w -o "$out_dir/$base" "$out_dir/$base.c" "$harness" \
            >"$out_dir/$base.cc" 2>&1 \
        && timeout 10 "$out_dir/$base" >/dev/null 2>&1; then
        pass=$((pass + 1)); echo "umir-core $base=pass"
    else
        fail=$((fail + 1)); echo "umir-core $base=fail"
        tail -3 "$out_dir/$base.probe" 2>/dev/null | grep -aE "^hir errors|^emit|error" | head -2
        head -3 "$out_dir/$base.cc" 2>/dev/null
    fi
done
echo "umir-core total=$total pass=$pass fail=$fail"
if test "${CMRUSTC_REQUIRE_UMIR_CORE:-0}" = 1 && test "$fail" -ne 0; then exit 1; fi
