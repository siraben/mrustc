#!/bin/sh
set -eu

: "${CMRUSTC:?CMRUSTC must name the compiler binary}"
: "${CC:?CC must name the C compiler}"
: "${CFLAGS:=}"

test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
fixture_dir="$test_dir/fixtures"
artifact_dir=$(mktemp -d "${TMPDIR:-/tmp}/cmrustc-nested-call.XXXXXX")
trap 'rm -rf "$artifact_dir"' EXIT HUP INT TERM

generated_c="$artifact_dir/nested-call-probe.c"
generated_exe="$artifact_dir/nested-call-probe"
program_stdout="$artifact_dir/program.stdout"
program_stderr="$artifact_dir/program.stderr"

printf '%s\n' sentinel >"$generated_c"
"$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/nested-call-probe.rs" -o "$generated_c"
test -s "$generated_c"
test "$(cat "$generated_c")" != sentinel

# The inner call result must be materialized before the later outer argument
# is evaluated, and both calls must target the real private add_pair body.
awk '
    /^uint32_t[[:space:]]+probe_chain[[:space:]]*\(uint32_t/ &&
            $0 !~ /;[[:space:]]*$/ {
        in_probe = 1
        found_probe = 1
    }
    in_probe {
        addition_count += gsub(/\+/, "&")
        if ($0 ~ /uint32_t[[:space:]]+_3[[:space:]]*;/) temp3 = 1
        if ($0 ~ /uint32_t[[:space:]]+_4[[:space:]]*;/) temp4 = 1
        if ($0 ~ /uint32_t[[:space:]]+_5[[:space:]]*;/) temp5 = 1
        if ($0 ~ /uint32_t[[:space:]]+_6[[:space:]]*;/) temp6 = 1
        if ($0 ~ /_3[[:space:]]*=/ && $0 ~ /_1/ && $0 ~ /\+/) {
            assign3 = NR
        }
        if ($0 ~ /_4[[:space:]]*=/ && $0 ~ /_2/ && $0 ~ /\+/) {
            assign4 = NR
        }
        if ($0 ~ /_5[[:space:]]*=/ &&
                $0 ~ /add_pair[A-Za-z0-9_]*[[:space:]]*\(_3,[[:space:]]*_4\)/) {
            inner_call = NR
        }
        if ($0 ~ /_6[[:space:]]*=/ && $0 ~ /_1/ && $0 ~ /\+/) {
            assign6 = NR
        }
        if ($0 ~ /_0[[:space:]]*=/ &&
                $0 ~ /add_pair[A-Za-z0-9_]*[[:space:]]*\(_5,[[:space:]]*_6\)/) {
            outer_call = NR
        }
    }
    in_probe && /}/ {
        finished_probe = 1
        in_probe = 0
    }
    END {
        if (!found_probe || !finished_probe || !temp3 || !temp4 ||
                !temp5 || !temp6 || addition_count != 3 || !assign3 ||
                !assign4 || !inner_call || !assign6 || !outer_call ||
                assign3 >= assign4 || assign4 >= inner_call ||
                inner_call >= assign6 || assign6 >= outer_call) {
            exit 1
        }
    }
' "$generated_c"

# A call nested inside a root addition must continue into the later operand
# and final root assignment rather than being mistaken for the function tail.
awk '
    /^uint32_t[[:space:]]+probe_after_call[[:space:]]*\(uint32_t/ &&
            $0 !~ /;[[:space:]]*$/ {
        in_probe = 1
        found_probe = 1
    }
    in_probe {
        addition_count += gsub(/\+/, "&")
        if ($0 ~ /_3[[:space:]]*=/ && $0 ~ /_1/ && $0 ~ /\+/) {
            assign3 = NR
        }
        if ($0 ~ /_4[[:space:]]*=/ && $0 ~ /_2/ && $0 ~ /\+/) {
            assign4 = NR
        }
        if ($0 ~ /_5[[:space:]]*=/ &&
                $0 ~ /add_pair[A-Za-z0-9_]*[[:space:]]*\(_3,[[:space:]]*_4\)/) {
            nested_call = NR
        }
        if ($0 ~ /_6[[:space:]]*=/ && $0 ~ /_1/ && $0 ~ /\+/) {
            assign6 = NR
        }
        if ($0 ~ /_0[[:space:]]*=/ && $0 ~ /_5/ && $0 ~ /_6/ &&
                $0 ~ /\+/) {
            root_add = NR
        }
    }
    in_probe && /}/ {
        finished_probe = 1
        in_probe = 0
    }
    END {
        if (!found_probe || !finished_probe || addition_count != 4 ||
                !assign3 || !assign4 || !nested_call || !assign6 ||
                !root_add || assign3 >= assign4 ||
                assign4 >= nested_call || nested_call >= assign6 ||
                assign6 >= root_add) {
            exit 1
        }
    }
' "$generated_c"

# CFLAGS is intentionally split into compiler arguments.
# shellcheck disable=SC2086
"$CC" $CFLAGS -o "$generated_exe" "$generated_c" \
    "$fixture_dir/nested-call-probe-harness.c"
test -s "$generated_exe"
"$generated_exe" >"$program_stdout" 2>"$program_stderr"
test ! -s "$program_stdout"
test ! -s "$program_stderr"

negative_c="$artifact_dir/nested-call-unsupported.c"
negative_stdout="$artifact_dir/nested-call-unsupported.stdout"
negative_stderr="$artifact_dir/nested-call-unsupported.stderr"

printf '%s\n' sentinel >"$negative_c"
if "$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/nested-call-probe-unsupported.rs" -o "$negative_c" \
    >"$negative_stdout" 2>"$negative_stderr"; then
    echo "unsupported nested call subtree unexpectedly compiled" >&2
    exit 1
fi
test "$(cat "$negative_c")" = sentinel
test -s "$negative_stderr"

rm "$negative_c"
if "$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/nested-call-probe-unsupported.rs" -o "$negative_c" \
    >"$negative_stdout" 2>"$negative_stderr"; then
    echo "unsupported nested call subtree published fresh C" >&2
    exit 1
fi
test ! -e "$negative_c"
test -s "$negative_stderr"

echo "nested call-result executable acceptance: ok"
