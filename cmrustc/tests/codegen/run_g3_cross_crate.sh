#!/bin/sh
set -eu

# This is the executable G3 boundary. Until both switches are implemented it
# is an expected-red test, not evidence that private declaration metadata is an
# rlib. Set CMRUSTC_REQUIRE_G3=1 to turn an unavailable boundary into failure.
: "${CMRUSTC_REQUIRE_G3:=0}"
: "${CMRUSTC_G3_TARGET:=x86_64-unknown-linux-gnu}"
: "${CMRUSTC_G3_WRONG_TARGET:=i386-unknown-linux-musl}"
: "${CMRUSTC_G3_CCS:=gcc tcc}"
: "${AR:=ar}"

case "$CMRUSTC_REQUIRE_G3" in
    0|1) ;;
    *)
        echo "CMRUSTC_REQUIRE_G3 must be 0 or 1" >&2
        exit 2
        ;;
esac

skip_g3()
{
    if test "$CMRUSTC_REQUIRE_G3" = 1; then
        echo "g3 cross-crate acceptance: required but unavailable: $1" >&2
        exit 1
    fi
    echo "g3 cross-crate acceptance: SKIP ($1)"
    exit 0
}

test -n "${CMRUSTC:-}" || skip_g3 "CMRUSTC is not set"
command -v "$CMRUSTC" >/dev/null 2>&1 \
    || skip_g3 "CMRUSTC is not executable"

version=$("$CMRUSTC" --version 2>/dev/null) \
    || skip_g3 "compiler has no cmrustc version identity"
case "$version" in
    cmrustc\ *) ;;
    *) skip_g3 "compiler is not the C cmrustc driver" ;;
esac

help=$("$CMRUSTC" --help 2>&1) || skip_g3 "compiler help failed"
printf '%s\n' "$help" | grep -q -- '--emit-cmrlib' \
    || skip_g3 "--emit-cmrlib is not implemented"
printf '%s\n' "$help" | grep -q -- '--extern-cmrlib' \
    || skip_g3 "--extern-cmrlib is not implemented"
command -v "$AR" >/dev/null 2>&1 || skip_g3 "archive reader is unavailable"
for candidate_cc in $CMRUSTC_G3_CCS; do
    command -v "$candidate_cc" >/dev/null 2>&1 \
        || skip_g3 "$candidate_cc is unavailable"
done

test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
fixture_dir="$test_dir/fixtures"
provider_fixture="$fixture_dir/g3-cross-crate-provider.rs"
consumer_fixture="$fixture_dir/g3-cross-crate-consumer.rs"
harness_fixture="$fixture_dir/g3-cross-crate-harness.c"
artifact_dir=$(mktemp -d "${TMPDIR:-/tmp}/cmrustc-g3-cross-crate.XXXXXX")
trap 'rm -rf "$artifact_dir"' EXIT HUP INT TERM

compile_consumer()
{
    output=$1
    target=$2
    extern_spec=$3
    stderr_path=$4
    "$CMRUSTC" --edition 2021 --target "$target" --emit-c \
        "$consumer_fixture" --crate-name g3_consumer \
        --extern-cmrlib "$extern_spec" -o "$output" \
        >"$stderr_path.stdout" 2>"$stderr_path"
}

expect_consumer_rejected()
{
    label=$1
    target=$2
    extern_spec=$3
    output="$artifact_dir/reject-$label.c"
    diagnostic="$artifact_dir/reject-$label.stderr"

    printf '%s\n' sentinel >"$output"
    if compile_consumer "$output" "$target" "$extern_spec" "$diagnostic"; then
        echo "$label artifact unexpectedly compiled" >&2
        exit 1
    fi
    test "$(cat "$output")" = sentinel
    test -s "$diagnostic"

    rm -f "$output"
    if compile_consumer "$output" "$target" "$extern_spec" "$diagnostic"; then
        echo "$label artifact unexpectedly compiled without prior output" >&2
        exit 1
    fi
    test ! -e "$output"
    test -s "$diagnostic"
}

for cc in $CMRUSTC_G3_CCS; do
    cc_name=$(basename -- "$cc")
    cc_tag=$(printf '%s' "$cc_name" | tr -c 'A-Za-z0-9_' '_')
    first_root="$artifact_dir/$cc_tag-first"
    second_root="$artifact_dir/$cc_tag-second"
    consumer_root="$artifact_dir/$cc_tag-consumer"
    mkdir "$first_root" "$second_root" "$consumer_root"
    cp "$provider_fixture" "$first_root/provider.rs"
    cp "$provider_fixture" "$second_root/provider.rs"
    cp "$consumer_fixture" "$consumer_root/consumer.rs"

    first_rlib="$first_root/libg3_provider.rlib"
    second_rlib="$second_root/libg3_provider.rlib"

    # Each invocation is a separate producer process. Different absolute roots
    # must not leak into the authenticated bytes or the native object.
    "$CMRUSTC" --edition 2021 --target "$CMRUSTC_G3_TARGET" \
        --emit-cmrlib "$first_root/provider.rs" \
        --crate-name g3_provider --cc "$cc" -o "$first_rlib"
    "$CMRUSTC" --edition 2021 --target "$CMRUSTC_G3_TARGET" \
        --emit-cmrlib "$second_root/provider.rs" \
        --crate-name g3_provider --cc "$cc" -o "$second_rlib"
    test -s "$first_rlib"
    test -s "$second_rlib"
    cmp "$first_rlib" "$second_rlib"

    member_list="$consumer_root/members"
    "$AR" t "$first_rlib" >"$member_list"
    grep -qx 'cmrustc.rmeta' "$member_list"
    grep -qx 'cmrustc.object' "$member_list"
    provider_object="$consumer_root/provider.o"
    "$AR" p "$first_rlib" cmrustc.object >"$provider_object"
    test -s "$provider_object"

    # The fresh consumer root contains no producer source. Its only provider
    # input is the completed artifact from the first process.
    rm -f "$first_root/provider.rs" "$second_root/provider.rs"
    consumer_c="$consumer_root/consumer.c"
    consumer_stderr="$consumer_root/consumer.stderr"
    "$CMRUSTC" --edition 2021 --target "$CMRUSTC_G3_TARGET" --emit-c \
        "$consumer_root/consumer.rs" --crate-name g3_consumer \
        --extern-cmrlib "provider=$first_rlib" -o "$consumer_c" \
        >"$consumer_stderr.stdout" 2>"$consumer_stderr"
    test -s "$consumer_c"

    case "$cc_name" in
        tcc*) compile_flags=${CMRUSTC_G3_TCC_CFLAGS:--std=c99 -Wall -Werror} ;;
        *) compile_flags=${CMRUSTC_G3_GCC_CFLAGS:--std=c99 -pedantic-errors -Wall -Wextra -Werror} ;;
    esac
    executable="$consumer_root/g3-probe"
    # Intentional field splitting permits the conventional CFLAGS-style
    # variables above to carry more than one option.
    "$cc" $compile_flags -o "$executable" "$consumer_c" \
        "$provider_object" "$harness_fixture"
    "$executable"

    corrupt_rlib="$consumer_root/corrupt.rlib"
    {
        printf 'X'
        dd if="$first_rlib" bs=1 skip=1 2>/dev/null
    } >"$corrupt_rlib"
    incomplete_rlib="$consumer_root/incomplete.rlib"
    artifact_size=$(wc -c <"$first_rlib" | tr -d ' ')
    dd if="$first_rlib" of="$incomplete_rlib" bs=1 \
        count=$((artifact_size - 1)) 2>/dev/null

    expect_consumer_rejected "$cc_tag-corrupt" "$CMRUSTC_G3_TARGET" \
        "provider=$corrupt_rlib"
    expect_consumer_rejected "$cc_tag-incomplete" "$CMRUSTC_G3_TARGET" \
        "provider=$incomplete_rlib"
    expect_consumer_rejected "$cc_tag-missing" "$CMRUSTC_G3_TARGET" \
        "provider=$consumer_root/missing.rlib"
    expect_consumer_rejected "$cc_tag-wrong-identity" "$CMRUSTC_G3_TARGET" \
        "impostor=$first_rlib"
    expect_consumer_rejected "$cc_tag-wrong-target" \
        "$CMRUSTC_G3_WRONG_TARGET" "provider=$first_rlib"
done

echo "g3 cross-crate executable artifact acceptance: ok"
