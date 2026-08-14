#!/bin/sh
set -eu

: "${CMRUSTC:?CMRUSTC must name the compiler binary}"
: "${TEST_SOURCE_DIAG:?TEST_SOURCE_DIAG must name the source test binary}"

version=$("$CMRUSTC" --version)
case "$version" in
    "cmrustc "*"; built with "*) ;;
    *)
        echo "unexpected version identity: $version" >&2
        exit 1
        ;;
esac

if test "${EXPECT_TCC:-0}" = 1; then
    case "$version" in
        *"tinycc (__TINYC__="*) ;;
        *)
            echo "TCC build lacks __TINYC__ identity: $version" >&2
            exit 1
            ;;
    esac
fi

target=$("$CMRUSTC" --target i386-unknown-linux-musl --print target)
case "$target" in
    *"triple=i386-unknown-linux-musl"*"pointer-bits=32"*) ;;
    *)
        echo "unexpected target descriptor" >&2
        echo "$target" >&2
        exit 1
        ;;
esac

source_output=$("$CMRUSTC" --probe-source tests/fixtures/one.rs tests/fixtures/two.rs)
case "$source_output" in
    *"tests/fixtures/one.rs:"*"note: source loaded"*\
*"tests/fixtures/two.rs:"*"note: source loaded"*) ;;
    *)
        echo "unexpected source diagnostics" >&2
        echo "$source_output" >&2
        exit 1
        ;;
esac

unit_output=$("$TEST_SOURCE_DIAG")
test "$unit_output" = "second.rs:2:3: note: snapshot"

token_output=$("$CMRUSTC" --edition 2024 --dump-tokens \
    tests/syntax/fixtures/lexer_cases.rs)
case "$token_output" in
    *"IDENT"*"async"*"RAW_IDENT"*"RAW_C_STRING"*"EOF"*) ;;
    *)
        echo "unexpected canonical token dump" >&2
        echo "$token_output" >&2
        exit 1
        ;;
esac

tree_output=$("$CMRUSTC" --edition 2024 --dump-token-tree \
    tests/fixtures/one.rs)
case "$tree_output" in
    "token-tree-v1"*"group=bracket"*"group=paren"*"group=brace"*"errors 0"*) ;;
    *)
        echo "unexpected canonical token-tree dump" >&2
        echo "$tree_output" >&2
        exit 1
        ;;
esac

ast_output=$("$CMRUSTC" --edition 2024 --dump-ast \
    tests/syntax/fixtures/parser_items.rs)
case "$ast_output" in
    "(crate"*"(struct inherited \"Buffer\""*"(trait inherited \"Show\""*\
"(impl inherited _"*")") ;;
    *)
        echo "unexpected canonical AST dump" >&2
        echo "$ast_output" >&2
        exit 1
        ;;
esac

"$CMRUSTC" --run "$TEST_SOURCE_DIAG" >/dev/null

artifact_dir=$(mktemp -d "${TMPDIR:-/tmp}/cmrustc-no-core.XXXXXX")
trap 'rm -rf "$artifact_dir"' EXIT HUP INT TERM
generated_c="$artifact_dir/no-core-exit.c"
generated_exe="$artifact_dir/no-core-exit"
program_stdout="$artifact_dir/stdout"
program_stderr="$artifact_dir/stderr"

printf '%s\n' sentinel >"$generated_c"
"$CMRUSTC" --edition 2021 --emit-c \
    tests/codegen/fixtures/no-core-exit.rs -o "$generated_c"
test -s "$generated_c"
"$CC" $CFLAGS -o "$generated_exe" "$generated_c"
test -s "$generated_exe"
set +e
"$generated_exe" >"$program_stdout" 2>"$program_stderr"
program_status=$?
set -e
test "$program_status" -eq 7
test ! -s "$program_stdout"
test ! -s "$program_stderr"

rejected_c="$artifact_dir/rejected.c"
printf '%s\n' sentinel >"$rejected_c"
if "$CMRUSTC" --edition 2021 --emit-c \
    tests/codegen/fixtures/no-core-unsupported-body.rs -o "$rejected_c" \
    >"$artifact_dir/rejected.stdout" 2>"$artifact_dir/rejected.stderr"; then
    echo "unsupported body unexpectedly compiled" >&2
    exit 1
fi
test "$(cat "$rejected_c")" = sentinel

extra_export_c="$artifact_dir/extra-export.c"
printf '%s\n' sentinel >"$extra_export_c"
if "$CMRUSTC" --edition 2021 --emit-c \
    tests/codegen/fixtures/no-core-extra-export.rs -o "$extra_export_c" \
    >"$artifact_dir/extra-export.stdout" \
    2>"$artifact_dir/extra-export.stderr"; then
    echo "unemitted exported function unexpectedly compiled" >&2
    exit 1
fi
test "$(cat "$extra_export_c")" = sentinel

hardlink_input="$artifact_dir/hardlink-input.rs"
hardlink_output="$artifact_dir/hardlink-output.c"
cp tests/codegen/fixtures/no-core-exit.rs "$hardlink_input"
ln "$hardlink_input" "$hardlink_output"
if "$CMRUSTC" --edition 2021 --emit-c \
    "$hardlink_input" -o "$hardlink_output" \
    >"$artifact_dir/hardlink.stdout" 2>"$artifact_dir/hardlink.stderr"; then
    echo "hard-linked input/output unexpectedly compiled" >&2
    exit 1
fi
cmp "$hardlink_input" tests/codegen/fixtures/no-core-exit.rs
cmp "$hardlink_output" "$hardlink_input"

symlink_input="$artifact_dir/symlink-input.rs"
symlink_output="$artifact_dir/symlink-output.c"
cp tests/codegen/fixtures/no-core-exit.rs "$symlink_input"
ln -s symlink-input.rs "$symlink_output"
if "$CMRUSTC" --edition 2021 --emit-c \
    "$symlink_input" -o "$symlink_output" \
    >"$artifact_dir/symlink.stdout" 2>"$artifact_dir/symlink.stderr"; then
    echo "symlinked input/output unexpectedly compiled" >&2
    exit 1
fi
test -L "$symlink_output"
cmp "$symlink_input" tests/codegen/fixtures/no-core-exit.rs

CMRUSTC="$CMRUSTC" CC="$CC" CFLAGS="$CFLAGS" \
    tests/codegen/run_identity_acceptance.sh
CMRUSTC="$CMRUSTC" CC="$CC" CFLAGS="$CFLAGS" \
    tests/codegen/run_generic_chain_acceptance.sh
CMRUSTC="$CMRUSTC" CC="$CC" CFLAGS="$CFLAGS" \
    tests/codegen/run_recursive_publication_acceptance.sh
CMRUSTC="$CMRUSTC" CC="$CC" CFLAGS="$CFLAGS" \
    tests/codegen/run_identity_add_acceptance.sh
CMRUSTC="$CMRUSTC" CC="$CC" CFLAGS="$CFLAGS" \
    tests/codegen/run_u32_add_acceptance.sh
CMRUSTC="$CMRUSTC" CC="$CC" CFLAGS="$CFLAGS" \
    tests/codegen/run_u32_sub_acceptance.sh
CMRUSTC="$CMRUSTC" CC="$CC" CFLAGS="$CFLAGS" \
    tests/codegen/run_u32_if_eq_acceptance.sh
CMRUSTC="$CMRUSTC" CC="$CC" CFLAGS="$CFLAGS" \
    tests/codegen/run_u32_add_nested_acceptance.sh
CMRUSTC="$CMRUSTC" CC="$CC" CFLAGS="$CFLAGS" \
    tests/codegen/run_pair_add_acceptance.sh
CMRUSTC="$CMRUSTC" CC="$CC" CFLAGS="$CFLAGS" \
    tests/codegen/run_nested_call_acceptance.sh
CMRUSTC="$CMRUSTC" CC="$CC" CFLAGS="$CFLAGS" \
    tests/codegen/run_qualified_ufcs_acceptance.sh
CMRUSTC="$CMRUSTC" CC="$CC" CFLAGS="$CFLAGS" \
    tests/codegen/run_qualified_default_acceptance.sh
CMRUSTC="$CMRUSTC" CC="$CC" CFLAGS="$CFLAGS" \
    tests/codegen/run_dot_method_acceptance.sh
CMRUSTC="$CMRUSTC" CC="$CC" CFLAGS="$CFLAGS" \
    tests/codegen/run_autoref_method_acceptance.sh
CMRUSTC="$CMRUSTC" CC="$CC" CFLAGS="$CFLAGS" \
    tests/codegen/run_blanket_echo_acceptance.sh
CMRUSTC="$CMRUSTC" CC="$CC" CFLAGS="$CFLAGS" \
    tests/codegen/run_semantic_bound_acceptance.sh
CMRUSTC="$CMRUSTC" CC="$CC" CFLAGS="$CFLAGS" \
    tests/codegen/run_semantic_trait_method_acceptance.sh
CMRUSTC="$CMRUSTC" CC="$CC" CFLAGS="$CFLAGS" \
    tests/codegen/run_let_acceptance.sh
CMRUSTC="$CMRUSTC" CC="$CC" CFLAGS="$CFLAGS" \
    tests/codegen/run_named_aggregate_acceptance.sh
CMRUSTC="$CMRUSTC" CC="$CC" CFLAGS="$CFLAGS" \
    tests/codegen/run_aggregate_call_acceptance.sh
CMRUSTC="$CMRUSTC" CC="$CC" CFLAGS="$CFLAGS" \
    tests/codegen/run_usize_acceptance.sh
CMRUSTC="$CMRUSTC" tests/codegen/run_target_cfg_acceptance.sh

echo "cmrustc smoke: ok ($version)"
