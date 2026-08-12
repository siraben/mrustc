#!/bin/sh
set -eu

: "${STAGE0_TCC_ROOT:?STAGE0_TCC_ROOT must name the TinyCC output}"

stage0_cc="$STAGE0_TCC_ROOT/bin/tcc"
stage0_lib="$STAGE0_TCC_ROOT/lib"
build_dir=${BUILD_DIR:-build/stage0-hcc}

test -x "$stage0_cc"
test -s "$stage0_lib/libc.a"
test -s "$stage0_lib/libtcc1.a"

case "$build_dir" in
    /*) build_root=$build_dir ;;
    *) build_root=$(pwd)/$build_dir ;;
esac

compiler_flags='-O2 -std=c99 -Wall -Werror -DCM_REQUIRE_TCC=1'
linker_flags="-B$stage0_lib"

make BUILD_DIR="$build_dir" CC="$stage0_cc" \
    CFLAGS="$compiler_flags" LDFLAGS="$linker_flags" \
    all "$build_dir/test-source-diag" "$build_dir/test_lexer" \
    "$build_dir/test_codegen_c"

version=$($build_root/cmrustc --version)
case "$version" in
    *"built with tinycc (__TINYC__="*) ;;
    *)
        echo "stage0 compiler lacks TinyCC identity: $version" >&2
        exit 1
        ;;
esac

test "$($build_root/test-source-diag)" = "second.rs:2:3: note: snapshot"
$build_root/test_lexer tests/syntax/fixtures/lexer_cases.rs
$build_root/test_codegen_c

smoke_dir=$(mktemp -d "${TMPDIR:-/tmp}/cmrustc-stage0.XXXXXX")
trap 'rm -rf "$smoke_dir"' EXIT HUP INT TERM
$build_root/cmrustc --edition 2024 --dump-ast \
    tests/syntax/fixtures/parser_items.rs >"$smoke_dir/parser.ast"
test -s "$smoke_dir/parser.ast"

smoke_flags="$linker_flags -I$(pwd)/include -include cm/config.h"
CMRUSTC="$build_root/cmrustc" \
TEST_SOURCE_DIAG="$build_root/test-source-diag" \
EXPECT_TCC=1 CC="$stage0_cc" CFLAGS="$smoke_flags" tests/run.sh

echo "stage0 HCC/TinyCC cmrustc gate: ok ($version)"
