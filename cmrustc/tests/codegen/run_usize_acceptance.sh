#!/bin/sh
set -eu

: "${CMRUSTC:?CMRUSTC must name the compiler binary}"
: "${CC:?CC must name the C compiler}"
: "${CFLAGS:=}"

test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
fixture_dir="$test_dir/fixtures"
artifact_dir=$(mktemp -d "${TMPDIR:-/tmp}/cmrustc-usize.XXXXXX")
trap 'rm -rf "$artifact_dir"' EXIT HUP INT TERM

generated_c="$artifact_dir/usize.c"
second_c="$artifact_dir/usize-second.c"
generated_exe="$artifact_dir/usize"
program_stdout="$artifact_dir/program.stdout"
program_stderr="$artifact_dir/program.stderr"

"$CMRUSTC" --target x86_64-unknown-linux-gnu --edition 2021 --emit-c \
    "$fixture_dir/usize-probe.rs" -o "$generated_c"
"$CMRUSTC" --target x86_64-unknown-linux-gnu --edition 2021 --emit-c \
    "$fixture_dir/usize-probe.rs" -o "$second_c"
test -s "$generated_c"
cmp "$generated_c" "$second_c"

grep -q 'UINTPTR_MAX != UINT64_MAX' "$generated_c"
grep -q 'sizeof(uintptr_t) == 8u' "$generated_c"
grep -q '(uintptr_t)UINT64_C(4294967301)' "$generated_c"
grep -Eq '^uintptr_t probe_usize\(uintptr_t _1, uintptr_t _2\);$' \
    "$generated_c"
grep -Eq '^static uintptr_t cmrustc_const_min_' "$generated_c"
grep -Eq '^static uintptr_t cmrustc_wrap_and_limit_' "$generated_c"
grep -Eq '[[:space:]]< ' "$generated_c"
grep -Eq '=[[:space:]]*\(uintptr_t\)\(' "$generated_c"
if grep -q 'UINTPTR_C' "$generated_c"; then
    echo "non-C99 UINTPTR_C escaped into usize output" >&2
    exit 1
fi

# CFLAGS is intentionally split into compiler arguments.
# shellcheck disable=SC2086
"$CC" $CFLAGS -o "$generated_exe" "$generated_c" \
    "$fixture_dir/usize-probe-harness.c"
"$generated_exe" >"$program_stdout" 2>"$program_stderr"
test ! -s "$program_stdout"
test ! -s "$program_stderr"

expect_rejected()
{
    name=$1
    target=$2
    source=$3
    output="$artifact_dir/$name.c"
    stdout="$artifact_dir/$name.stdout"
    stderr="$artifact_dir/$name.stderr"

    printf '%s\n' sentinel >"$output"
    if "$CMRUSTC" --target "$target" --edition 2021 --emit-c \
        "$source" -o "$output" >"$stdout" 2>"$stderr"; then
        echo "$name unexpectedly compiled" >&2
        exit 1
    fi
    test "$(cat "$output")" = sentinel
    test -s "$stderr"

    rm "$output"
    if "$CMRUSTC" --target "$target" --edition 2021 --emit-c \
        "$source" -o "$output" >"$stdout" 2>"$stderr"; then
        echo "$name unexpectedly published fresh C" >&2
        exit 1
    fi
    test ! -e "$output"
    test -s "$stderr"
}

# The 64-bit literal cannot be represented by a 32-bit target usize. The
# compiler must reject before publishing even a partial translation unit.
expect_rejected overflow-i686 i686-unknown-linux-musl \
    "$fixture_dir/usize-probe.rs"
expect_rejected mixed-scalars x86_64-unknown-linux-gnu \
    "$fixture_dir/usize-mixed-scalars.rs"
expect_rejected aggregate-source-boundary x86_64-unknown-linux-gnu \
    "$fixture_dir/usize-aggregate-unsupported.rs"

# A 32-bit target produces exact uintptr_t/UINT32_C guards. Compiling that C
# on this 64-bit execution host must fail closed rather than silently taking
# the host's usize width.
target32_c="$artifact_dir/usize-target32.c"
target32_exe="$artifact_dir/usize-target32"
target32_stderr="$artifact_dir/usize-target32.stderr"
"$CMRUSTC" --target i686-unknown-linux-musl --edition 2021 --emit-c \
    "$fixture_dir/usize-target-width.rs" -o "$target32_c"
grep -q 'UINTPTR_MAX != UINT32_MAX' "$target32_c"
grep -q 'sizeof(uintptr_t) == 4u' "$target32_c"
grep -q '(uintptr_t)UINT32_C(1)' "$target32_c"
if "$CC" $CFLAGS -o "$target32_exe" "$target32_c" \
    >"$artifact_dir/usize-target32.stdout" 2>"$target32_stderr"; then
    echo "32-bit target C unexpectedly compiled for the 64-bit host" >&2
    exit 1
fi
test ! -e "$target32_exe"
test -s "$target32_stderr"

echo "target-usize executable acceptance: ok"
