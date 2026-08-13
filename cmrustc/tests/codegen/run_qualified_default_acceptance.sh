#!/bin/sh
set -eu

: "${CMRUSTC:?CMRUSTC must name the compiler binary}"
: "${CC:?CC must name the C compiler}"
: "${CFLAGS:=}"

test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
fixture_dir="$test_dir/fixtures"
artifact_dir=$(mktemp -d "${TMPDIR:-/tmp}/cmrustc-qualified-default.XXXXXX")
trap 'rm -rf "$artifact_dir"' EXIT HUP INT TERM

generated_c="$artifact_dir/qualified-default.c"
generated_exe="$artifact_dir/qualified-default"
program_stdout="$artifact_dir/program.stdout"
program_stderr="$artifact_dir/program.stderr"

printf '%s\n' sentinel >"$generated_c"
"$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/qualified-default.rs" -o "$generated_c"
test -s "$generated_c"
test "$(cat "$generated_c")" != sentinel

# Both wrappers must call separately emitted instances of the trait-owned
# default body. Each instance symbol is keyed by its concrete impl dispatch.
awk '
    /^    _0 = cmrustc_h[0-9a-f][0-9a-f]*_value\(_1\);$/ { ++found_calls }
    END {
        if (found_calls != 2) exit 1
    }
' "$generated_c"

# The two inherited instances share executable HIR but must retain distinct
# concrete dispatch identities and therefore distinct C symbols.
test "$(sed -n \
    's/^static uint32_t \(cmrustc_h[0-9a-f][0-9a-f]*_value\)(.*/\1/p' \
    "$generated_c" | sort -u | wc -l)" -eq 2

# The wrappers must actually reference both concrete dispatch symbols.
test "$(sed -n \
    's/^    _0 = \(cmrustc_h[0-9a-f][0-9a-f]*_value\)(_1);$/\1/p' \
    "$generated_c" | sort -u | wc -l)" -eq 2

# CFLAGS is intentionally split into compiler arguments.
# shellcheck disable=SC2086
"$CC" $CFLAGS -o "$generated_exe" "$generated_c" \
    "$fixture_dir/qualified-default-harness.c"
"$generated_exe" >"$program_stdout" 2>"$program_stderr"
test ! -s "$program_stdout"
test ! -s "$program_stderr"

override_c="$artifact_dir/override.c"
override_exe="$artifact_dir/override"
"$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/qualified-default-override.rs" -o "$override_c"
test -s "$override_c"

# One wrapper must execute the explicit impl body (+2), while the other still
# executes the trait-owned inherited body (+1).
# shellcheck disable=SC2086
"$CC" $CFLAGS -o "$override_exe" "$override_c" \
    "$fixture_dir/qualified-default-override-harness.c"
"$override_exe"

required_c="$artifact_dir/required.c"
printf '%s\n' sentinel >"$required_c"
if "$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/qualified-default-required.rs" -o "$required_c" \
    >"$artifact_dir/required.stdout" 2>"$artifact_dir/required.stderr"; then
    echo "required trait method unexpectedly inherited" >&2
    exit 1
fi
test "$(cat "$required_c")" = sentinel
test -s "$artifact_dir/required.stderr"

bodyless_c="$artifact_dir/bodyless-override.c"
printf '%s\n' sentinel >"$bodyless_c"
if "$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/qualified-default-bodyless-override.rs" \
    -o "$bodyless_c" >"$artifact_dir/bodyless.stdout" \
    2>"$artifact_dir/bodyless.stderr"; then
    echo "bodyless linked override unexpectedly inherited the default" >&2
    exit 1
fi
test "$(cat "$bodyless_c")" = sentinel
test -s "$artifact_dir/bodyless.stderr"

echo "qualified inherited-default executable acceptance: ok"
