#!/bin/sh
set -eu

: "${CMRUSTC:?CMRUSTC must name the compiler binary}"
: "${CC:?CC must name the C compiler}"
: "${CFLAGS:=}"

test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
fixture_dir="$test_dir/fixtures"
artifact_dir=$(mktemp -d "${TMPDIR:-/tmp}/cmrustc-generic-chain.XXXXXX")
trap 'rm -rf "$artifact_dir"' EXIT HUP INT TERM

generated_c="$artifact_dir/generic-chain-probe.c"
generated_exe="$artifact_dir/generic-chain-probe"

"$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/generic-chain-probe.rs" -o "$generated_c"

# Both exact u32 specializations must be emitted, and the generic caller's
# specialization must call the identity specialization rather than bypass it.
grep -Eq 'uint32_t[[:space:]]+[A-Za-z_][A-Za-z0-9_]*identity[A-Za-z0-9_]*[[:space:]]*\(uint32_t' \
    "$generated_c"
awk '
    /^static[[:space:]]+uint32_t[[:space:]]+[A-Za-z_][A-Za-z0-9_]*caller[A-Za-z0-9_]*[[:space:]]*\(uint32_t/ &&
            $0 !~ /;[[:space:]]*$/ {
        in_caller = 1
        found_caller = 1
    }
    in_caller && /identity[A-Za-z0-9_]*[[:space:]]*\(/ {
        called_identity = 1
    }
    in_caller && /^}/ {
        in_caller = 0
    }
    END {
        if (!found_caller || !called_identity) exit 1
    }
' "$generated_c"

"$CC" $CFLAGS -std=c99 -Wall -Wextra -Werror \
    "$generated_c" "$fixture_dir/generic-chain-probe-harness.c" \
    -o "$generated_exe"
"$generated_exe"

echo "generic caller-chain executable acceptance: ok"
