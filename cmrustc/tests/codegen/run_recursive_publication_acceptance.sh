#!/bin/sh
set -eu

: "${CMRUSTC:?CMRUSTC must name the compiler binary}"
: "${CC:?CC must name the C compiler}"
: "${CFLAGS:=}"

test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
fixture_dir="$test_dir/fixtures"
artifact_dir=$(mktemp -d "${TMPDIR:-/tmp}/cmrustc-recursive.XXXXXX")
trap 'rm -rf "$artifact_dir"' EXIT HUP INT TERM

generated_c="$artifact_dir/self-recursive-probe.c"
generated_o="$artifact_dir/self-recursive-probe.o"
mutual_c="$artifact_dir/mutual-recursive-probe.c"
mutual_o="$artifact_dir/mutual-recursive-probe.o"

"$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/self-recursive-probe.rs" -o "$generated_c"

# This deliberately divergent program is compile-only. It proves that a
# self-edge was reserved, defined, validated, committed and emitted with a
# declaration preceding its recursive call.
awk '
    /^uint32_t[[:space:]]+probe_recursive[[:space:]]*\(uint32_t/ &&
            /;[[:space:]]*$/ { declaration = NR }
    /^uint32_t[[:space:]]+probe_recursive[[:space:]]*\(uint32_t/ &&
            $0 !~ /;[[:space:]]*$/ { definition = NR; in_body = 1 }
    in_body && /probe_recursive[[:space:]]*\(_1\)/ { call = NR }
    in_body && /^}/ { in_body = 0 }
    END {
        if (!declaration || !definition || !call ||
                declaration >= definition || definition >= call) exit 1
    }
' "$generated_c"

"$CC" $CFLAGS -std=c99 -Wall -Wextra -Werror \
    -Wno-infinite-recursion -c "$generated_c" \
    -o "$generated_o"

"$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/mutual-recursive-probe.rs" -o "$mutual_c"
grep -Eq '^static uint32_t[[:space:]]+cmrustc_h[0-9a-f]{64}_first\(uint32_t _1\);[[:space:]]*$' \
    "$mutual_c"
grep -Eq '^static uint32_t[[:space:]]+cmrustc_h[0-9a-f]{64}_second\(uint32_t _1\);[[:space:]]*$' \
    "$mutual_c"
"$CC" $CFLAGS -std=c99 -Wall -Wextra -Werror \
    -Wno-infinite-recursion -c "$mutual_c" -o "$mutual_o"

echo "recursive publication acceptance: ok"
