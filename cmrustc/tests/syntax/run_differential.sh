#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "${SCRIPT_DIR}/../../.." && pwd)
FIXTURES="${SCRIPT_DIR}/fixtures"

CC=${CC:-cc}
CXX=${CXX:-c++}
MRUSTC=${MRUSTC:-"${REPO_ROOT}/bin/mrustc"}
ORACLE_ARCHIVE=${ORACLE_ARCHIVE:-"${REPO_ROOT}/bin/mrustc.a"}
ORACLE_COMMON=${ORACLE_COMMON:-"${REPO_ROOT}/bin/common_lib.a"}
ORACLE_LDLIBS=${ORACLE_LDLIBS:-"-lz -pthread"}

WORK_DIR=$(mktemp -d)
trap 'rm -rf -- "$WORK_DIR"' EXIT HUP INT TERM

CM_DUMP="${WORK_DIR}/cm-dump-tokens"
ORACLE_DUMP="${WORK_DIR}/mrustc-lexer-oracle"

"$CC" -std=c99 -Wall -Wextra -Werror -pedantic \
    -I"${REPO_ROOT}/cmrustc/include" \
    "${REPO_ROOT}/cmrustc/src/syntax/token.c" \
    "${REPO_ROOT}/cmrustc/src/syntax/lexer.c" \
    "${REPO_ROOT}/cmrustc/tools/dump_tokens.c" \
    -o "$CM_DUMP"

# This adapter is intentionally C++: it is a developer oracle around upstream,
# not part of the TCC-bootstrapped implementation.
# shellcheck disable=SC2086
"$CXX" -std=c++14 -Wall -Wextra \
    -I"${REPO_ROOT}/src" -I"${REPO_ROOT}/src/include" \
    -I"${REPO_ROOT}/tools/common" \
    "${REPO_ROOT}/cmrustc/tools/mrustc_lexer_oracle.cpp" \
    -o "$ORACLE_DUMP" "$ORACLE_ARCHIVE" "$ORACLE_COMMON" \
    $ORACLE_LDLIBS

compare_exact()
{
    edition=$1
    fixture=$2
    cm_output="${WORK_DIR}/cm-${edition}.tokens"
    oracle_output="${WORK_DIR}/oracle-${edition}.tokens"

    "$CM_DUMP" "$edition" "$fixture" > "$cm_output"
    "$ORACLE_DUMP" "$edition" "$fixture" "$oracle_output" >/dev/null
    diff -u "$oracle_output" "$cm_output"
    printf 'exact token stream: edition %s %s\n' "$edition" \
        "$(basename -- "$fixture")"
}

expect_known_difference()
{
    name=$1
    edition=$2
    fixture=$3
    cm_output="${WORK_DIR}/cm-diverge-${name}.tokens"
    oracle_output="${WORK_DIR}/oracle-diverge-${name}.tokens"

    "$CM_DUMP" "$edition" "$fixture" > "$cm_output"
    "$ORACLE_DUMP" "$edition" "$fixture" "$oracle_output" >/dev/null
    if cmp -s "$oracle_output" "$cm_output"; then
        printf 'expected known difference disappeared: %s\n' "$name" >&2
        return 1
    fi
    printf 'known difference observed: %s\n' "$name"
}

compare_exact 2015 "${FIXTURES}/oracle_exact_2015.rs"
compare_exact 2018 "${FIXTURES}/oracle_exact_2018.rs"
compare_exact 2024 "${FIXTURES}/oracle_exact_2024.rs"

expect_known_difference raw-c-string 2024 \
    "${FIXTURES}/oracle_diverge_raw_c.rs"
expect_known_difference gen-2024 2024 \
    "${FIXTURES}/oracle_diverge_gen_2024.rs"
expect_known_difference unknown-numeric-suffix 2021 \
    "${FIXTURES}/oracle_diverge_suffix.rs"

# Prove that both the C lexer and the actual upstream executable accept the
# representative crate. stop-after=parse avoids crate loading and codegen.
"$CM_DUMP" 2021 "${FIXTURES}/oracle_accept_2021.rs" >/dev/null
sha256sum "$MRUSTC" > "${WORK_DIR}/mrustc.before.sha256"
MRUSTC_TARGET_VER=1.90 "$MRUSTC" --edition 2021 --crate-type rlib \
    -Z stop-after=parse "${FIXTURES}/oracle_accept_2021.rs" \
    >"${WORK_DIR}/mrustc-parse.stdout" \
    2>"${WORK_DIR}/mrustc-parse.stderr"
sha256sum -c "${WORK_DIR}/mrustc.before.sha256" >/dev/null
printf 'black-box parse acceptance: oracle_accept_2021.rs\n'
printf 'mrustc oracle binary preserved: %s\n' "$MRUSTC"
