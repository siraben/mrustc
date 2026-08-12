#!/bin/sh
set -eu

: "${CMRUSTC:?CMRUSTC must name the compiler binary}"
: "${CC:?CC must name the C compiler}"
: "${CFLAGS:=}"

test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
fixture_dir="$test_dir/fixtures"
artifact_dir=$(mktemp -d "${TMPDIR:-/tmp}/cmrustc-u32-sub.XXXXXX")
trap 'rm -rf "$artifact_dir"' EXIT HUP INT TERM

generated_c="$artifact_dir/u32-sub.c"
second_c="$artifact_dir/u32-sub-second.c"
generated_exe="$artifact_dir/u32-sub"
program_stdout="$artifact_dir/program.stdout"
program_stderr="$artifact_dir/program.stderr"

printf '%s\n' sentinel >"$generated_c"
"$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/u32-sub.rs" -o "$generated_c"
test -s "$generated_c"
test "$(cat "$generated_c")" != sentinel
"$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/u32-sub.rs" -o "$second_c"
cmp "$generated_c" "$second_c"

# The source-backed let must remain a real ordered unsigned subtraction.
awk '
    /^uint32_t[[:space:]]+sub[[:space:]]*\(uint32_t/ {
        in_sub = 1
        found_sub = 1
    }
    in_sub && /_3[[:space:]]*=/ && /_1[[:space:]]*-[[:space:]]*_2/ {
        subtraction = NR
    }
    in_sub && /_0[[:space:]]*=/ && /_3/ { returned = NR }
    in_sub && /}/ { finished_sub = 1; in_sub = 0 }
    END {
        if (!found_sub || !finished_sub || !subtraction || !returned ||
                subtraction >= returned) exit 1
    }
' "$generated_c"

# CFLAGS is intentionally split into compiler arguments.
# shellcheck disable=SC2086
"$CC" $CFLAGS -o "$generated_exe" "$generated_c" \
    "$fixture_dir/u32-sub-harness.c"
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

expect_rejected unsupported-u32-operator \
    "$fixture_dir/u32-sub-unsupported-operator.rs"
expect_rejected unsupported-signed-subtraction \
    "$fixture_dir/u32-sub-signed.rs"

echo "u32 wrapping-subtract executable acceptance: ok"
