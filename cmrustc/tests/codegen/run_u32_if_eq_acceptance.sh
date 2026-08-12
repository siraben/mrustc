#!/bin/sh
set -eu

: "${CMRUSTC:?CMRUSTC must name the compiler binary}"
: "${CC:?CC must name the C compiler}"
: "${CFLAGS:=}"

test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
fixture_dir="$test_dir/fixtures"
artifact_dir=$(mktemp -d "${TMPDIR:-/tmp}/cmrustc-u32-if-eq.XXXXXX")
trap 'rm -rf "$artifact_dir"' EXIT HUP INT TERM

generated_c="$artifact_dir/u32-if-eq.c"
second_c="$artifact_dir/u32-if-eq-second.c"
generated_exe="$artifact_dir/u32-if-eq"
program_stdout="$artifact_dir/program.stdout"
program_stderr="$artifact_dir/program.stderr"

printf '%s\n' sentinel >"$generated_c"
"$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/u32-if-eq.rs" -o "$generated_c"
test -s "$generated_c"
test "$(cat "$generated_c")" != sentinel
"$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/u32-if-eq.rs" -o "$second_c"
cmp "$generated_c" "$second_c"

# The condition must be materialized as an exact 0/1 uint8_t before the
# canonical C if/else. Both u32 arms must remain explicit ordered arithmetic.
awk '
    /^uint32_t[[:space:]]+select[[:space:]]*\(uint32_t/ {
        in_select = 1
        found_select = 1
    }
    in_select && /uint8_t[[:space:]]+_[0-9]+/ { bool_local = 1 }
    in_select && /=[[:space:]]*\(uint8_t\)/ && /==/ { equality = NR }
    in_select && /^[[:space:]]*if[[:space:]]*\(_[0-9]+ != UINT8_C\(0\)\)/ {
        branch = NR
    }
    in_select && /=[[:space:]]*\(uint32_t\)/ && /_1[[:space:]]*\+/ {
        addition = NR
    }
    in_select && /else/ { otherwise = NR }
    in_select && /=[[:space:]]*\(uint32_t\)/ && /_1[[:space:]]*-/ {
        subtraction = NR
    }
    in_select && /return[[:space:]]+_0/ { returned = NR }
    in_select && /^}/ { finished_select = 1; in_select = 0 }
    END {
        if (!found_select || !finished_select || !bool_local || !equality ||
                !branch || !addition || !otherwise || !subtraction ||
                !returned || equality >= branch || branch >= addition ||
                addition >= otherwise || otherwise >= subtraction ||
                subtraction >= returned) exit 1
    }
' "$generated_c"

# CFLAGS is intentionally split into compiler arguments.
# shellcheck disable=SC2086
"$CC" $CFLAGS -o "$generated_exe" "$generated_c" \
    "$fixture_dir/u32-if-eq-harness.c"
test -s "$generated_exe"
"$generated_exe" >"$program_stdout" 2>"$program_stderr"
test ! -s "$program_stdout"
test ! -s "$program_stderr"

expect_rejected()
{
    name=$1
    source=$2
    output="$artifact_dir/$name.c"
    stdout="$artifact_dir/$name.stdout"
    stderr="$artifact_dir/$name.stderr"

    printf '%s\n' sentinel >"$output"
    if "$CMRUSTC" --edition 2021 --emit-c "$source" -o "$output" \
        >"$stdout" 2>"$stderr"; then
        echo "$name unexpectedly compiled" >&2
        exit 1
    fi
    test "$(cat "$output")" = sentinel
    test -s "$stderr"

    rm "$output"
    if "$CMRUSTC" --edition 2021 --emit-c "$source" -o "$output" \
        >"$stdout" 2>"$stderr"; then
        echo "$name unexpectedly published fresh C" >&2
        exit 1
    fi
    test ! -e "$output"
    test -s "$stderr"
}

expect_rejected unsupported-not-equal "$fixture_dir/u32-if-ne.rs"
expect_rejected unsupported-signed-equality "$fixture_dir/i32-if-eq.rs"
expect_rejected missing-else "$fixture_dir/u32-if-missing-else.rs"

echo "u32 equality if/else executable acceptance: ok"
