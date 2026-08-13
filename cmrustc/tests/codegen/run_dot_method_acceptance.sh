#!/bin/sh
set -eu

: "${CMRUSTC:?CMRUSTC must name the compiler binary}"
: "${CC:?CC must name the C compiler}"
: "${CFLAGS:=}"

test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
fixture_dir="$test_dir/fixtures"
artifact_dir=$(mktemp -d "${TMPDIR:-/tmp}/cmrustc-dot-method.XXXXXX")
trap 'rm -rf "$artifact_dir"' EXIT HUP INT TERM

generated_c="$artifact_dir/dot-method.c"
generated_exe="$artifact_dir/dot-method"
program_stdout="$artifact_dir/program.stdout"
program_stderr="$artifact_dir/program.stderr"

printf '%s\n' sentinel >"$generated_c"
"$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/dot-method.rs" -o "$generated_c"
test -s "$generated_c"
test "$(cat "$generated_c")" != sentinel

# The wrapper must call the separately emitted selected impl method. This
# checks that reachability consumed the sealed selection instead of folding it.
awk '
    /dot_value[[:space:]]*\(uint32_t/ {
        in_wrapper = 1
        found_wrapper = 1
    }
    in_wrapper && /=.*\([^;]*\);/ {
        found_call = 1
    }
    in_wrapper && /}/ {
        finished_wrapper = 1
        in_wrapper = 0
    }
    END {
        if (!found_wrapper || !finished_wrapper || !found_call) exit 1
    }
' "$generated_c"

# CFLAGS is intentionally split into compiler arguments.
# shellcheck disable=SC2086
"$CC" $CFLAGS -Wno-unused-parameter -o "$generated_exe" "$generated_c" \
    "$fixture_dir/dot-method-harness.c"
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

expect_rejected dot-method-missing \
    "$fixture_dir/dot-method-missing.rs"
expect_rejected dot-method-ambiguous \
    "$fixture_dir/dot-method-ambiguous.rs"
expect_rejected dot-method-out-of-scope \
    "$fixture_dir/dot-method-out-of-scope.rs"

echo "dot-method executable acceptance: ok"
