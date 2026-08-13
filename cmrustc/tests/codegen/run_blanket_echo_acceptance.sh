#!/bin/sh
set -eu

: "${CMRUSTC:?CMRUSTC must name the compiler binary}"
: "${CC:?CC must name the C compiler}"
: "${CFLAGS:=}"

test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
fixture_dir="$test_dir/fixtures"
artifact_dir=$(mktemp -d "${TMPDIR:-/tmp}/cmrustc-blanket-echo.XXXXXX")
trap 'rm -rf "$artifact_dir"' EXIT HUP INT TERM

generated_c="$artifact_dir/blanket-echo.c"
generated_exe="$artifact_dir/blanket-echo"
program_stdout="$artifact_dir/program.stdout"
program_stderr="$artifact_dir/program.stderr"

"$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/blanket-echo.rs" -o "$generated_c"

# Canonical u32 and u8 impl instances must be distinct private definitions.
test "$(grep -Ec '^static (uint32_t|uint8_t) cmrustc_h[0-9a-f]{64}_echo\([^;]*\)$' \
    "$generated_c")" -eq 2
grep -Eq '^static uint32_t cmrustc_h[0-9a-f]{64}_echo\(uint32_t' \
    "$generated_c"
grep -Eq '^static uint8_t cmrustc_h[0-9a-f]{64}_echo\(uint8_t' \
    "$generated_c"

# Each exported wrapper must call one of those bodies; constant folding or
# reusing a same-width identity would not prove canonical reachability.
awk '
    /echo_qualified_u32[[:space:]]*\(uint32_t/ { in_u32 = 1 }
    in_u32 && /cmrustc_h[0-9a-f]+_echo[[:space:]]*\(/ { called_u32 = 1 }
    in_u32 && /^}/ { in_u32 = 0 }
    /echo_dot_u8[[:space:]]*\(uint8_t/ { in_u8 = 1 }
    in_u8 && /cmrustc_h[0-9a-f]+_echo[[:space:]]*\(/ { called_u8 = 1 }
    in_u8 && /^}/ { in_u8 = 0 }
    END { if (!called_u32 || !called_u8) exit 1 }
' "$generated_c"

# CFLAGS is intentionally split into compiler arguments.
# shellcheck disable=SC2086
"$CC" $CFLAGS -std=c99 -Wall -Wextra -Werror -Wno-unused-parameter \
    "$generated_c" "$fixture_dir/blanket-echo-harness.c" \
    -o "$generated_exe"
"$generated_exe" >"$program_stdout" 2>"$program_stderr"
test ! -s "$program_stdout"
test ! -s "$program_stderr"

echo "blanket impl canonical u32/u8 executable acceptance: ok"
