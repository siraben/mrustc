#!/bin/sh
set -eu

: "${CMRUSTC:?CMRUSTC must name the compiler binary}"

test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
fixture="$test_dir/fixtures/target-cfg.rs"
artifact_dir=$(mktemp -d "${TMPDIR:-/tmp}/cmrustc-target-cfg.XXXXXX")
trap 'rm -rf "$artifact_dir"' EXIT HUP INT TERM

x86_64_c="$artifact_dir/x86-64.c"
i386_c="$artifact_dir/i386.c"

"$CMRUSTC" --edition 2024 --target x86_64-unknown-linux-gnu --emit-c \
    "$fixture" -o "$x86_64_c"
"$CMRUSTC" --edition 2024 --target i386-unknown-linux-musl --emit-c \
    "$fixture" -o "$i386_c"

test -s "$x86_64_c"
test -s "$i386_c"
grep -q 'UINT32_C(64)' "$x86_64_c"
if grep -q 'UINT32_C(32)' "$x86_64_c"; then
    echo "i386 cfg branch leaked into x86-64 output" >&2
    exit 1
fi
grep -q 'UINT32_C(32)' "$i386_c"
if grep -q 'UINT32_C(64)' "$i386_c"; then
    echo "x86-64 cfg branch leaked into i386 output" >&2
    exit 1
fi

echo "target-derived cfg acceptance: ok"
