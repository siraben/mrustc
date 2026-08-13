#!/bin/sh
set -eu

: "${CMRUSTC:?CMRUSTC must name the compiler binary}"
: "${CC:?CC must name the C compiler}"
: "${CFLAGS:=}"

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
fixture_dir="$test_dir/fixtures"
artifact_dir=$(mktemp -d "${TMPDIR:-/tmp}/cmrustc-semantic-method.XXXXXX")
trap 'rm -rf "$artifact_dir"' EXIT HUP INT TERM

present_c="$artifact_dir/present.c"
present_exe="$artifact_dir/present"
"$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/semantic-trait-method-present.rs" -o "$present_c"
test -s "$present_c"
grep -Eq 'uint32_t[[:space:]]+[A-Za-z_][A-Za-z0-9_]*convert[A-Za-z0-9_]*[[:space:]]*\(uint32_t' \
    "$present_c"
awk '
    /^uint32_t[[:space:]]+probe[[:space:]]*\(uint32_t/ {
        in_probe = 1
    }
    in_probe && /convert[A-Za-z0-9_]*[[:space:]]*\(/ {
        called_convert = 1
    }
    in_probe && /}/ {
        finished_probe = 1
        in_probe = 0
    }
    END {
        if (!finished_probe || !called_convert) exit 1
    }
' "$present_c"
"$CC" $CFLAGS -o "$present_exe" "$present_c" \
    "$fixture_dir/identity-probe-harness.c"
"$present_exe"

mismatch_c="$artifact_dir/mismatch.c"
mismatch_stdout="$artifact_dir/mismatch.stdout"
mismatch_stderr="$artifact_dir/mismatch.stderr"
printf '%s\n' sentinel >"$mismatch_c"
if "$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/semantic-trait-method-mismatch.rs" -o "$mismatch_c" \
    >"$mismatch_stdout" 2>"$mismatch_stderr"; then
    echo "mismatched trait method unexpectedly compiled" >&2
    exit 1
fi
test "$(cat "$mismatch_c")" = sentinel
grep -q 'semantic' "$mismatch_stderr"
grep -q 'parameter-type' "$mismatch_stderr"

rm "$mismatch_c"
if "$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/semantic-trait-method-mismatch.rs" -o "$mismatch_c" \
    >"$mismatch_stdout" 2>"$mismatch_stderr"; then
    echo "mismatched trait method published fresh C" >&2
    exit 1
fi
test ! -e "$mismatch_c"
grep -q 'parameter-type' "$mismatch_stderr"

echo "semantic trait-method conformance acceptance: ok"
