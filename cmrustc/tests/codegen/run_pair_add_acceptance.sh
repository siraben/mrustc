#!/bin/sh
set -eu

: "${CMRUSTC:?CMRUSTC must name the compiler binary}"
: "${CC:?CC must name the C compiler}"
: "${CFLAGS:=}"

test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
fixture_dir="$test_dir/fixtures"
artifact_dir=$(mktemp -d "${TMPDIR:-/tmp}/cmrustc-pair-add.XXXXXX")
trap 'rm -rf "$artifact_dir"' EXIT HUP INT TERM

generated_c="$artifact_dir/pair-add-probe.c"
generated_exe="$artifact_dir/pair-add-probe"
program_stdout="$artifact_dir/program.stdout"
program_stderr="$artifact_dir/program.stderr"

printf '%s\n' sentinel >"$generated_c"
"$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/pair-add-probe.rs" -o "$generated_c"
test -s "$generated_c"
test "$(cat "$generated_c")" != sentinel

# The exported caller must evaluate both arguments left-to-right into distinct
# temporaries, then pass both to the real emitted monomorphic callee.
awk '
    /^uint32_t[[:space:]]+probe_pair[[:space:]]*\(uint32_t/ &&
            $0 !~ /;[[:space:]]*$/ {
        in_probe = 1
        found_probe = 1
    }
    in_probe {
        addition_count += gsub(/\+/, "&")
        if ($0 ~ /uint32_t[[:space:]]+_3[[:space:]]*;/) temp3 = 1
        if ($0 ~ /uint32_t[[:space:]]+_4[[:space:]]*;/) temp4 = 1
        if ($0 ~ /_3[[:space:]]*=/ && $0 ~ /_1/ && $0 ~ /\+/) {
            assign3 = NR
        }
        if ($0 ~ /_4[[:space:]]*=/ && $0 ~ /_2/ && $0 ~ /\+/) {
            assign4 = NR
        }
        if ($0 ~ /_0[[:space:]]*=/ &&
                $0 ~ /add_pair[A-Za-z0-9_]*[[:space:]]*\(_3,[[:space:]]*_4\)/) {
            call_pair = NR
        }
    }
    in_probe && /}/ {
        finished_probe = 1
        in_probe = 0
    }
    END {
        if (!found_probe || !finished_probe || !temp3 || !temp4 ||
                addition_count != 2 || !assign3 || !assign4 ||
                !call_pair || assign3 >= assign4 || assign4 >= call_pair) {
            exit 1
        }
    }
' "$generated_c"

# CFLAGS is intentionally split into compiler arguments.
# shellcheck disable=SC2086
"$CC" $CFLAGS -o "$generated_exe" "$generated_c" \
    "$fixture_dir/pair-add-probe-harness.c"
test -s "$generated_exe"
"$generated_exe" >"$program_stdout" 2>"$program_stderr"
test ! -s "$program_stdout"
test ! -s "$program_stderr"

negative_c="$artifact_dir/pair-add-unsupported.c"
negative_stdout="$artifact_dir/pair-add-unsupported.stdout"
negative_stderr="$artifact_dir/pair-add-unsupported.stderr"

printf '%s\n' sentinel >"$negative_c"
if "$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/pair-add-probe-unsupported.rs" -o "$negative_c" \
    >"$negative_stdout" 2>"$negative_stderr"; then
    echo "unsupported second call argument unexpectedly compiled" >&2
    exit 1
fi
test "$(cat "$negative_c")" = sentinel
test -s "$negative_stderr"

rm "$negative_c"
if "$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/pair-add-probe-unsupported.rs" -o "$negative_c" \
    >"$negative_stdout" 2>"$negative_stderr"; then
    echo "unsupported second call argument published fresh C" >&2
    exit 1
fi
test ! -e "$negative_c"
test -s "$negative_stderr"

echo "computed two-argument call executable acceptance: ok"
