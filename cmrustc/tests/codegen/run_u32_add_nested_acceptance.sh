#!/bin/sh
set -eu

: "${CMRUSTC:?CMRUSTC must name the compiler binary}"
: "${CC:?CC must name the C compiler}"
: "${CFLAGS:=}"

test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
fixture_dir="$test_dir/fixtures"
artifact_dir=$(mktemp -d "${TMPDIR:-/tmp}/cmrustc-u32-nested.XXXXXX")
trap 'rm -rf "$artifact_dir"' EXIT HUP INT TERM

generated_c="$artifact_dir/u32-add-nested.c"
generated_exe="$artifact_dir/u32-add-nested"
program_stdout="$artifact_dir/program.stdout"
program_stderr="$artifact_dir/program.stderr"

# Success must atomically replace a caller-owned artifact with real C.
printf '%s\n' sentinel >"$generated_c"
"$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/u32-add-nested.rs" -o "$generated_c"
test -s "$generated_c"
test "$(cat "$generated_c")" != sentinel

# Both source additions must survive as the ordered MIR temporary sequence.
awk '
    /^uint32_t[[:space:]]+nested_add[[:space:]]*\(uint32_t/ {
        in_nested_add = 1
        found_nested_add = 1
    }
    in_nested_add {
        addition_count += gsub(/\+/, "&")
        if ($0 ~ /uint32_t[[:space:]]+_3[[:space:]]*;/) {
            declared_temporary = 1
        }
        if ($0 ~ /_3[[:space:]]*=/ && $0 ~ /\+/) {
            temporary_assignment = NR
        }
        if ($0 ~ /_0[[:space:]]*=/ && $0 ~ /\+/ &&
                $0 ~ /_3/) {
            return_assignment = NR
        }
    }
    in_nested_add && /}/ {
        finished_nested_add = 1
        in_nested_add = 0
    }
    END {
        if (!found_nested_add || !finished_nested_add ||
                !declared_temporary || addition_count != 2 ||
                !temporary_assignment || !return_assignment ||
                temporary_assignment >= return_assignment) exit 1
    }
' "$generated_c"

# CFLAGS is intentionally split into compiler arguments.
# shellcheck disable=SC2086
"$CC" $CFLAGS -o "$generated_exe" "$generated_c" \
    "$fixture_dir/u32-add-nested-harness.c"
test -s "$generated_exe"
"$generated_exe" >"$program_stdout" 2>"$program_stderr"
test ! -s "$program_stdout"
test ! -s "$program_stderr"

negative_c="$artifact_dir/nested-unsupported.c"
negative_stdout="$artifact_dir/nested-unsupported.stdout"
negative_stderr="$artifact_dir/nested-unsupported.stderr"

# A reachable unsupported inner multiplication must fail closed without C.
printf '%s\n' sentinel >"$negative_c"
if "$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/u32-add-nested-unsupported.rs" -o "$negative_c" \
    >"$negative_stdout" 2>"$negative_stderr"; then
    echo "unsupported nested subtree unexpectedly compiled" >&2
    exit 1
fi
test "$(cat "$negative_c")" = sentinel
test -s "$negative_stderr"

rm "$negative_c"
if "$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/u32-add-nested-unsupported.rs" -o "$negative_c" \
    >"$negative_stdout" 2>"$negative_stderr"; then
    echo "unsupported nested subtree unexpectedly published fresh C" >&2
    exit 1
fi
test ! -e "$negative_c"
test -s "$negative_stderr"

echo "nested u32 wrapping-add executable acceptance: ok"
