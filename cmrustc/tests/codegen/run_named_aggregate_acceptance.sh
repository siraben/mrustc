#!/bin/sh
set -eu

: "${CMRUSTC:?CMRUSTC must name the compiler binary}"
: "${CC:?CC must name the C compiler}"
: "${CFLAGS:=}"

test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
fixture_dir="$test_dir/fixtures"
artifact_dir=$(mktemp -d "${TMPDIR:-/tmp}/cmrustc-aggregate.XXXXXX")
trap 'rm -rf "$artifact_dir"' EXIT HUP INT TERM

generated_c="$artifact_dir/named-aggregate.c"
second_c="$artifact_dir/named-aggregate-second.c"
generated_exe="$artifact_dir/named-aggregate"
program_stdout="$artifact_dir/program.stdout"
program_stderr="$artifact_dir/program.stderr"

printf '%s\n' sentinel >"$generated_c"
"$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/named-aggregate-probe.rs" -o "$generated_c"
test -s "$generated_c"
test "$(cat "$generated_c")" != sentinel
"$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/named-aggregate-probe.rs" -o "$second_c"
cmp "$generated_c" "$second_c"

grep -q '#include <stddef.h>' "$generated_c"
grep -Eq 'sizeof\(struct cmrustc_struct_c[0-9]+_d[0-9]+\) == 12u' \
    "$generated_c"
grep -Eq 'sizeof\(struct cmrustc_struct_c[0-9]+_d[0-9]+\) == 16u' \
    "$generated_c"
grep -Eq 'offsetof\(struct cmrustc_struct_c[0-9]+_d[0-9]+, _f1\) == 12u' \
    "$generated_c"
grep -Eq '_[0-9]+\._f0\._f2' "$generated_c"

# Outer is declared first in Rust, so a valid C translation must emit the
# dependency Inner before Outer. The first emitted nominal has three scalar
# fields; the second has the first nominal as its first field.
awk '
    /^struct cmrustc_struct_c[0-9]+_d[0-9]+ \{$/ {
        definition += 1
        in_definition = 1
        if (definition == 1) first_name = $2
        next
    }
    in_definition && /^};$/ { in_definition = 0; next }
    in_definition && definition == 1 && /(int32_t|uint32_t) _f[012];/ {
        first_scalar += 1
    }
    in_definition && definition == 2 && $0 ~ ("struct " first_name " _f0;") {
        second_depends = 1
    }
    /_2[[:space:]]*=/ && /_1/ && /UINT32_C\(3\)/ { assign2 = NR }
    /_3[[:space:]]*=/ && /_1/ && /UINT32_C\(2\)/ { assign3 = NR }
    /_4[[:space:]]*=/ && /_1/ && /UINT32_C\(1\)/ { assign4 = NR }
    /_5[[:space:]]*=/ && /\._f0 = \(int32_t\)\(17\)/ &&
            /\._f1 = _4/ && /\._f2 = _3/ { assign5 = NR }
    /_6[[:space:]]*=/ && /\._f0 = _5/ && /\._f1 = _2/ { assign6 = NR }
    /_0[[:space:]]*=/ && /_6\._f0\._f2/ { assign0 = NR }
    END {
        if (definition != 2 || first_scalar != 3 || !second_depends ||
                !assign2 || !assign3 || !assign4 || !assign5 || !assign6 ||
                !assign0 || assign2 >= assign3 || assign3 >= assign4 ||
                assign4 >= assign5 || assign5 >= assign6 ||
                assign6 >= assign0) {
            exit 1
        }
    }
' "$generated_c"

# CFLAGS is intentionally split into compiler arguments.
# shellcheck disable=SC2086
"$CC" $CFLAGS -o "$generated_exe" "$generated_c" \
    "$fixture_dir/named-aggregate-probe-harness.c"
test -s "$generated_exe"
"$generated_exe" >"$program_stdout" 2>"$program_stderr"
test ! -s "$program_stdout"
test ! -s "$program_stderr"

for rejected in named-aggregate-export-abi named-aggregate-repr \
        named-aggregate-call; do
    rejected_c="$artifact_dir/$rejected.c"
    rejected_stdout="$artifact_dir/$rejected.stdout"
    rejected_stderr="$artifact_dir/$rejected.stderr"
    printf '%s\n' sentinel >"$rejected_c"
    if "$CMRUSTC" --edition 2021 --emit-c \
        "$fixture_dir/$rejected.rs" -o "$rejected_c" \
        >"$rejected_stdout" 2>"$rejected_stderr"; then
        echo "$rejected unexpectedly compiled" >&2
        exit 1
    fi
    test "$(cat "$rejected_c")" = sentinel
    test -s "$rejected_stderr"
    rm "$rejected_c"
    if "$CMRUSTC" --edition 2021 --emit-c \
        "$fixture_dir/$rejected.rs" -o "$rejected_c" \
        >"$rejected_stdout" 2>"$rejected_stderr"; then
        echo "$rejected published fresh C" >&2
        exit 1
    fi
    test ! -e "$rejected_c"
    test -s "$rejected_stderr"
done

echo "named aggregate executable acceptance: ok"
