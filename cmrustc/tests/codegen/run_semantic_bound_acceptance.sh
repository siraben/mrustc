#!/bin/sh
set -eu

: "${CMRUSTC:?CMRUSTC must name the compiler binary}"
: "${CC:?CC must name the C compiler}"
: "${CFLAGS:=}"

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
fixture_dir="$test_dir/fixtures"
artifact_dir=$(mktemp -d "${TMPDIR:-/tmp}/cmrustc-semantic-bound.XXXXXX")
trap 'rm -rf "$artifact_dir"' EXIT HUP INT TERM

present_c="$artifact_dir/present.c"
present_exe="$artifact_dir/present"
"$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/semantic-bound-present.rs" -o "$present_c"
test -s "$present_c"
grep -Eq 'bounded[A-Za-z0-9_]*[[:space:]]*\(' "$present_c"
"$CC" $CFLAGS -o "$present_exe" "$present_c" \
    "$fixture_dir/identity-probe-harness.c"
"$present_exe"

missing_c="$artifact_dir/missing.c"
missing_stdout="$artifact_dir/missing.stdout"
missing_stderr="$artifact_dir/missing.stderr"
printf '%s\n' sentinel >"$missing_c"
if "$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/semantic-bound-missing.rs" -o "$missing_c" \
    >"$missing_stdout" 2>"$missing_stderr"; then
    echo "unsatisfied generic call predicate unexpectedly compiled" >&2
    exit 1
fi
test "$(cat "$missing_c")" = sentinel
grep -q 'semantic' "$missing_stderr"
grep -q 'deferred-metadata' "$missing_stderr"

rm "$missing_c"
if "$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/semantic-bound-missing.rs" -o "$missing_c" \
    >"$missing_stdout" 2>"$missing_stderr"; then
    echo "unsatisfied generic call predicate unexpectedly compiled fresh" >&2
    exit 1
fi
test ! -e "$missing_c"
grep -q 'deferred-metadata' "$missing_stderr"

echo "semantic generic-bound acceptance: ok"
