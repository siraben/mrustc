#!/bin/sh
set -eu

: "${CMRUSTC:?CMRUSTC must name the compiler binary}"
: "${CC:?CC must name the C compiler}"
: "${CFLAGS:=}"

test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
fixture_dir="$test_dir/fixtures"
artifact_dir=$(mktemp -d "${TMPDIR:-/tmp}/cmrustc-autoref-method.XXXXXX")
trap 'rm -rf "$artifact_dir"' EXIT HUP INT TERM

generated_c="$artifact_dir/autoref-method.c"
generated_exe="$artifact_dir/autoref-method"

"$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/autoref-method.rs" -o "$generated_c"
test -s "$generated_c"

# Both capability shapes must survive erasure into distinct C pointer types.
grep -Eq 'uint32_t const \* _[0-9]+' "$generated_c"
grep -Eq 'uint32_t \* _[0-9]+' "$generated_c"

# Each wrapper must borrow the direct receiver before evaluating the
# statement-producing second argument, and then consume both in the call.
awk '
    /^uint32_t (shared_autoref|mutable_autoref)\(uint32_t/ && $0 !~ /;$/ {
        in_wrapper = 1
        wrapper_count += 1
        borrow_line = 0
        first_call_line = 0
        second_call_line = 0
    }
    in_wrapper && /= &\(/ && borrow_line == 0 { borrow_line = NR }
    in_wrapper && /= [A-Za-z_][A-Za-z0-9_]*\([^;]*\);/ {
        if (first_call_line == 0) first_call_line = NR
        else if (second_call_line == 0) second_call_line = NR
    }
    in_wrapper && /^}/ {
        if (!(borrow_line < first_call_line &&
                first_call_line < second_call_line)) {
            exit 1
        }
        checked_count += 1
        in_wrapper = 0
    }
    END {
        if (wrapper_count != 2 || checked_count != 2 || in_wrapper) exit 1
    }
' "$generated_c"

# CFLAGS is intentionally split into compiler arguments.
# shellcheck disable=SC2086
"$CC" $CFLAGS -Wno-unused-parameter -o "$generated_exe" "$generated_c" \
    "$fixture_dir/autoref-method-harness.c"
"$generated_exe"

echo "shared/mutable autoref method executable acceptance: ok"
