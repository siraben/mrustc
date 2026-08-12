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

CM_DUMP="${WORK_DIR}/cm-dump-ast"
ORACLE_DUMP="${WORK_DIR}/mrustc-ast-oracle"
BEFORE_HASHES="${WORK_DIR}/oracle.before.sha256"
AFTER_HASHES="${WORK_DIR}/oracle.after.sha256"

sha256sum "$MRUSTC" "$ORACLE_ARCHIVE" "$ORACLE_COMMON" > "$BEFORE_HASHES"

"$CC" -std=c99 -Wall -Wextra -Werror -pedantic \
    -I"${REPO_ROOT}/cmrustc/include" \
    "${REPO_ROOT}/cmrustc/src/base/alloc.c" \
    "${REPO_ROOT}/cmrustc/src/base/arena.c" \
    "${REPO_ROOT}/cmrustc/src/base/buf.c" \
    "${REPO_ROOT}/cmrustc/src/base/interner.c" \
    "${REPO_ROOT}/cmrustc/src/base/map.c" \
    "${REPO_ROOT}/cmrustc/src/base/vec.c" \
    "${REPO_ROOT}/cmrustc/src/syntax/token.c" \
    "${REPO_ROOT}/cmrustc/src/syntax/lexer.c" \
    "${REPO_ROOT}/cmrustc/src/syntax/ast.c" \
    "${REPO_ROOT}/cmrustc/src/syntax/parser.c" \
    "${REPO_ROOT}/cmrustc/tools/dump_ast.c" \
    -o "$CM_DUMP"

# The C++ adapter is a developer oracle over upstream's parsed AST.  It is
# deliberately excluded from cmrustc's trusted C/TCC build.
# shellcheck disable=SC2086
"$CXX" -std=c++14 -Wall -Wextra \
    -I"${REPO_ROOT}/src" -I"${REPO_ROOT}/src/include" \
    -I"${REPO_ROOT}/tools/common" \
    "${REPO_ROOT}/cmrustc/tools/mrustc_ast_oracle.cpp" \
    -o "$ORACLE_DUMP" "$ORACLE_ARCHIVE" "$ORACLE_COMMON" \
    $ORACLE_LDLIBS

compare_outputs()
{
    category=$1
    mode=$2
    fixture=$3
    cm_output="${WORK_DIR}/cm-${category}.ast"
    oracle_output="${WORK_DIR}/oracle-${category}.ast"

    "$CM_DUMP" "$mode" 2021 "$fixture" > "$cm_output"
    "$ORACLE_DUMP" "$mode" 2021 "$fixture" > "$oracle_output"
    diff -u "$oracle_output" "$cm_output"
    printf '%s AST match: %s (%s mode)\n' "$category" \
        "$(basename -- "$fixture")" "$mode"
}

expect_representation_difference()
{
    fixture=$1
    cm_output="${WORK_DIR}/cm-expected-divergence.ast"
    oracle_output="${WORK_DIR}/oracle-expected-divergence.ast"

    "$CM_DUMP" exact 2021 "$fixture" > "$cm_output"
    "$ORACLE_DUMP" exact 2021 "$fixture" > "$oracle_output"
    if cmp -s "$oracle_output" "$cm_output"; then
        printf 'expected AST representation difference disappeared: %s\n' \
            "$(basename -- "$fixture")" >&2
        return 1
    fi
    printf 'expected AST representation difference: %s\n' \
        "$(basename -- "$fixture")"
}

black_box_parse()
{
    fixture=$1
    stem=$(basename -- "$fixture" .rs)

    MRUSTC_TARGET_VER=1.90 "$MRUSTC" --edition 2021 --crate-type rlib \
        -Z stop-after=parse "$fixture" \
        >"${WORK_DIR}/${stem}.parse.stdout" \
        2>"${WORK_DIR}/${stem}.parse.stderr"
    printf 'black-box parse acceptance: %s\n' "$(basename -- "$fixture")"
}

compare_outputs exact exact "${FIXTURES}/ast_exact.rs"
compare_outputs normalized semantic "${FIXTURES}/ast_semantic.rs"
expect_representation_difference "${FIXTURES}/ast_expected_divergence.rs"
# The known exact representation delta must vanish under the documented
# semantic normalization; this keeps the divergence narrow and intentional.
compare_outputs divergence-normalized semantic \
    "${FIXTURES}/ast_expected_divergence.rs"

black_box_parse "${FIXTURES}/ast_exact.rs"
black_box_parse "${FIXTURES}/ast_semantic.rs"
black_box_parse "${FIXTURES}/ast_expected_divergence.rs"

sha256sum "$MRUSTC" "$ORACLE_ARCHIVE" "$ORACLE_COMMON" > "$AFTER_HASHES"
diff -u "$BEFORE_HASHES" "$AFTER_HASHES"
printf 'oracle artifacts preserved (SHA-256):\n'
cat "$AFTER_HASHES"
