#!/bin/sh
set -eu

: "${CMRUSTC:?CMRUSTC must name the compiler binary}"
: "${CC:?CC must name the C compiler}"
: "${CFLAGS:=}"

test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
fixture_dir="$test_dir/fixtures"
artifact_dir=$(mktemp -d "${TMPDIR:-/tmp}/cmrustc-let.XXXXXX")
trap 'rm -rf "$artifact_dir"' EXIT HUP INT TERM

generated_c="$artifact_dir/let-probe.c"
generated_exe="$artifact_dir/let-probe"
program_stdout="$artifact_dir/program.stdout"
program_stderr="$artifact_dir/program.stderr"

printf '%s\n' sentinel >"$generated_c"
"$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/let-probe.rs" -o "$generated_c"
test -s "$generated_c"
test "$(cat "$generated_c")" != sentinel

if grep -q 'private_unsupported' "$generated_c"; then
    echo "private unreachable unsupported body was emitted" >&2
    exit 1
fi

# User locals _3 and _4 precede generated temporaries. The call initializer
# continues into _4, and the tail continues through _6 before assigning _0.
awk '
    /^uint32_t[[:space:]]+probe_let[[:space:]]*\(uint32_t/ &&
            $0 !~ /;[[:space:]]*$/ {
        in_probe = 1
        found_probe = 1
    }
    in_probe {
        addition_count += gsub(/\+/, "&")
        if ($0 ~ /uint32_t[[:space:]]+_3[[:space:]]*;/) local3 = 1
        if ($0 ~ /uint32_t[[:space:]]+_4[[:space:]]*;/) local4 = 1
        if ($0 ~ /uint32_t[[:space:]]+_5[[:space:]]*;/) temp5 = 1
        if ($0 ~ /uint32_t[[:space:]]+_6[[:space:]]*;/) temp6 = 1
        if ($0 ~ /_3[[:space:]]*=/ && $0 ~ /_1/ && $0 ~ /\+/) {
            assign3 = NR
        }
        if ($0 ~ /_5[[:space:]]*=/ && $0 ~ /_2/ && $0 ~ /\+/) {
            assign5 = NR
        }
        if ($0 ~ /_4[[:space:]]*=/ &&
                $0 ~ /add_pair[A-Za-z0-9_]*[[:space:]]*\(_3,[[:space:]]*_5\)/) {
            assign4 = NR
        }
        if ($0 ~ /_6[[:space:]]*=/ && $0 ~ /_1/ && $0 ~ /\+/) {
            assign6 = NR
        }
        if ($0 ~ /_0[[:space:]]*=/ && $0 ~ /_4/ && $0 ~ /_6/ &&
                $0 ~ /\+/) {
            assign0 = NR
        }
    }
    in_probe && /}/ {
        finished_probe = 1
        in_probe = 0
    }
    END {
        if (!found_probe || !finished_probe || !local3 || !local4 ||
                !temp5 || !temp6 || addition_count != 4 || !assign3 ||
                !assign5 || !assign4 || !assign6 || !assign0 ||
                assign3 >= assign5 || assign5 >= assign4 ||
                assign4 >= assign6 || assign6 >= assign0) {
            exit 1
        }
    }
' "$generated_c"

# CFLAGS is intentionally split into compiler arguments.
# shellcheck disable=SC2086
"$CC" $CFLAGS -o "$generated_exe" "$generated_c" \
    "$fixture_dir/let-probe-harness.c"
test -s "$generated_exe"
"$generated_exe" >"$program_stdout" 2>"$program_stderr"
test ! -s "$program_stdout"
test ! -s "$program_stderr"

negative_c="$artifact_dir/let-unsupported.c"
negative_stdout="$artifact_dir/let-unsupported.stdout"
negative_stderr="$artifact_dir/let-unsupported.stderr"

printf '%s\n' sentinel >"$negative_c"
if "$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/let-probe-unsupported.rs" -o "$negative_c" \
    >"$negative_stdout" 2>"$negative_stderr"; then
    echo "unsupported let initializer unexpectedly compiled" >&2
    exit 1
fi
test "$(cat "$negative_c")" = sentinel
test -s "$negative_stderr"

rm "$negative_c"
if "$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/let-probe-unsupported.rs" -o "$negative_c" \
    >"$negative_stdout" 2>"$negative_stderr"; then
    echo "unsupported let initializer published fresh C" >&2
    exit 1
fi
test ! -e "$negative_c"
test -s "$negative_stderr"

echo "immutable let executable acceptance: ok"
