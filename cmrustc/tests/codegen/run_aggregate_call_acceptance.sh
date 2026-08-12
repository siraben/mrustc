#!/bin/sh
set -eu

: "${CMRUSTC:?CMRUSTC must name the compiler binary}"
: "${CC:?CC must name the C compiler}"
: "${CFLAGS:=}"

test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
fixture_dir="$test_dir/fixtures"
artifact_dir=$(mktemp -d "${TMPDIR:-/tmp}/cmrustc-aggregate-call.XXXXXX")
trap 'rm -rf "$artifact_dir"' EXIT HUP INT TERM

generated_c="$artifact_dir/aggregate-call.c"
second_c="$artifact_dir/aggregate-call-second.c"
generated_exe="$artifact_dir/aggregate-call"
program_stdout="$artifact_dir/program.stdout"
program_stderr="$artifact_dir/program.stderr"

printf '%s\n' sentinel >"$generated_c"
"$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/aggregate-call-probe.rs" -o "$generated_c"
test -s "$generated_c"
test "$(cat "$generated_c")" != sentinel
"$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/aggregate-call-probe.rs" -o "$second_c"
cmp "$generated_c" "$second_c"

# The internal function must have one checked nominal parameter followed by
# a scalar parameter, while the only exported declaration stays scalar-only.
grep -Eq '^static uint32_t cmrustc_select_c[0-9]+_d[0-9]+\(struct cmrustc_struct_c[0-9]+_d[0-9]+ _1, uint32_t _2\)$' \
    "$generated_c"
grep -Eq '^uint32_t probe_aggregate_call\(uint32_t _1\);$' "$generated_c"
if grep -Eq '^uint32_t probe_aggregate_call\(struct ' "$generated_c"; then
    echo "aggregate escaped through the exported C ABI" >&2
    exit 1
fi

# A fresh aggregate argument is first assigned to a local and only then moved
# by value into the call. The callee reads the nested field from its parameter.
awk '
    /^static uint32_t cmrustc_select_/ { in_select = 1; next }
    in_select && /^}/ { in_select = 0 }
    in_select && /_1\._f0\._f1/ { selected_field = 1 }
    / = \(struct cmrustc_struct_/ && /\._f0 = _[0-9]+/ &&
            /\._f1 = _[0-9]+/ { outer_assignment = NR }
    / = UINT32_C\(3\);/ { bias_assignment = NR }
    / = cmrustc_select_/ && /_[0-9]+, _[0-9]+/ {
        call_assignment = NR
    }
    END {
        if (!selected_field || !bias_assignment || !outer_assignment ||
                !call_assignment || bias_assignment >= call_assignment ||
                outer_assignment >= call_assignment) {
            exit 1
        }
    }
' "$generated_c"

# CFLAGS is intentionally split into compiler arguments.
# shellcheck disable=SC2086
"$CC" $CFLAGS -o "$generated_exe" "$generated_c" \
    "$fixture_dir/aggregate-call-probe-harness.c"
test -s "$generated_exe"
"$generated_exe" >"$program_stdout" 2>"$program_stderr"
test ! -s "$program_stdout"
test ! -s "$program_stderr"

echo "aggregate by-value call executable acceptance: ok"
