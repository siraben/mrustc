#!/bin/sh
set -eu

: "${CMRUSTC:?CMRUSTC must name the compiler binary}"
: "${CC:?CC must name the C compiler}"
: "${CFLAGS:=}"

test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
fixture_dir="$test_dir/fixtures"
artifact_dir=$(mktemp -d "${TMPDIR:-/tmp}/cmrustc-u32-add.XXXXXX")
trap 'rm -rf "$artifact_dir"' EXIT HUP INT TERM

generated_c="$artifact_dir/u32-add.c"
generated_exe="$artifact_dir/u32-add"
program_stdout="$artifact_dir/program.stdout"
program_stderr="$artifact_dir/program.stderr"

# Success must atomically replace a caller-owned artifact with real C.
printf '%s\n' sentinel >"$generated_c"
"$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/u32-add.rs" -o "$generated_c"
test -s "$generated_c"
test "$(cat "$generated_c")" != sentinel

# The exported function must contain real addition. A constant or projection
# implementation can satisfy isolated inputs but must not pass this checkpoint.
awk '
    /^uint32_t[[:space:]]+add[[:space:]]*\(uint32_t/ {
        in_add = 1
        found_add = 1
    }
    in_add && /\+/ {
        found_addition = 1
    }
    in_add && /}/ {
        finished_add = 1
        in_add = 0
    }
    END {
        if (!found_add || !finished_add || !found_addition) exit 1
    }
' "$generated_c"

# CFLAGS is intentionally split into compiler arguments.
# shellcheck disable=SC2086
"$CC" $CFLAGS -o "$generated_exe" "$generated_c" \
    "$fixture_dir/u32-add-harness.c"
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

    # Rejection must preserve an existing artifact byte-for-byte.
    printf '%s\n' sentinel >"$output"
    if "$CMRUSTC" --edition 2021 --emit-c "$source" -o "$output" \
        >"$stdout" 2>"$stderr"; then
        echo "$name unexpectedly compiled" >&2
        exit 1
    fi
    test "$(cat "$output")" = sentinel
    test -s "$stderr"

    # Rejection against a fresh path must publish no artifact.
    rm "$output"
    if "$CMRUSTC" --edition 2021 --emit-c "$source" -o "$output" \
        >"$stdout" 2>"$stderr"; then
        echo "$name unexpectedly compiled without a prior artifact" >&2
        exit 1
    fi
    test ! -e "$output"
    test -s "$stderr"
}

expect_rejected unsupported-u32-operator \
    "$fixture_dir/u32-add-unsupported-operator.rs"
expect_rejected u32-add-type-mismatch \
    "$fixture_dir/u32-add-type-mismatch.rs"
expect_rejected unsupported-exported-root \
    "$fixture_dir/u32-add-exported-unsupported.rs"

# A private unsupported body outside the exported-root reachability closure is
# neither lowered nor emitted and must not poison the supported program.
unreachable_c="$artifact_dir/unreachable-unsupported.c"
unreachable_exe="$artifact_dir/unreachable-unsupported"
"$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/u32-add-unreachable-unsupported.rs" -o "$unreachable_c"
test -s "$unreachable_c"
if grep -q dormant "$unreachable_c"; then
    echo "unreachable private function was emitted" >&2
    exit 1
fi
awk '
    /^uint32_t[[:space:]]+add[[:space:]]*\(uint32_t/ {
        in_add = 1
    }
    in_add && /\+/ {
        found_addition = 1
    }
    in_add && /}/ {
        in_add = 0
    }
    END {
        if (!found_addition) exit 1
    }
' "$unreachable_c"
# CFLAGS is intentionally split into compiler arguments.
# shellcheck disable=SC2086
"$CC" $CFLAGS -o "$unreachable_exe" "$unreachable_c" \
    "$fixture_dir/u32-add-harness.c"
"$unreachable_exe"

echo "u32 wrapping-add executable acceptance: ok"
