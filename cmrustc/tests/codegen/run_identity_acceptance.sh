#!/bin/sh
set -eu

: "${CMRUSTC:?CMRUSTC must name the compiler binary}"
: "${CC:?CC must name the C compiler}"
: "${CFLAGS:=}"

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
fixture_dir="$test_dir/fixtures"
artifact_dir=$(mktemp -d "${TMPDIR:-/tmp}/cmrustc-identity.XXXXXX")
trap 'rm -rf "$artifact_dir"' EXIT HUP INT TERM

generated_c="$artifact_dir/identity-probe.c"
generated_exe="$artifact_dir/identity-probe"
program_stdout="$artifact_dir/program.stdout"
program_stderr="$artifact_dir/program.stderr"

# A successful compile must atomically replace an old artifact with real C.
printf '%s\n' sentinel >"$generated_c"
"$CMRUSTC" --edition 2021 --emit-c \
    "$fixture_dir/identity-probe.rs" -o "$generated_c"
test -s "$generated_c"
test "$(cat "$generated_c")" != sentinel

# This milestone emits the reachable u32 specialization as a real function.
# The exported wrapper must call it; merely returning a constant or dropping
# the admitted generic body is not an acceptable implementation.
grep -Eq 'uint32_t[[:space:]]+[A-Za-z_][A-Za-z0-9_]*identity[A-Za-z0-9_]*[[:space:]]*\(uint32_t' \
    "$generated_c"
awk '
    /^uint32_t[[:space:]]+probe[[:space:]]*\(uint32_t/ {
        in_probe = 1
    }
    in_probe && /identity[A-Za-z0-9_]*[[:space:]]*\(/ {
        called_identity = 1
    }
    in_probe && /}/ {
        finished_probe = 1
        in_probe = 0
    }
    END {
        if (!finished_probe || !called_identity) exit 1
    }
' "$generated_c"

"$CC" $CFLAGS -o "$generated_exe" "$generated_c" \
    "$fixture_dir/identity-probe-harness.c"
test -s "$generated_exe"
"$generated_exe" >"$program_stdout" 2>"$program_stderr"
test ! -s "$program_stdout"
test ! -s "$program_stderr"

expect_rejected()
{
    name=$1
    source=$2
    output="$artifact_dir/$name.c"
    stdout="$artifact_dir/$name.stdout"
    stderr="$artifact_dir/$name.stderr"

    # Failure must preserve a caller-owned prior artifact byte-for-byte.
    printf '%s\n' sentinel >"$output"
    if "$CMRUSTC" --edition 2021 --emit-c "$source" -o "$output" \
        >"$stdout" 2>"$stderr"; then
        echo "$name unexpectedly compiled" >&2
        exit 1
    fi
    test "$(cat "$output")" = sentinel
    test -s "$stderr"

    # The same rejection against a fresh path must publish no artifact.
    rm "$output"
    if "$CMRUSTC" --edition 2021 --emit-c "$source" -o "$output" \
        >"$stdout" 2>"$stderr"; then
        echo "$name unexpectedly compiled without a prior artifact" >&2
        exit 1
    fi
    test ! -e "$output"
    test -s "$stderr"
}

expect_rejected unresolved-generic-call \
    "$fixture_dir/identity-probe-unresolved.rs"
expect_rejected missing-generic-substitution \
    "$fixture_dir/identity-probe-missing-substitution.rs"
expect_rejected unsupported-generic-substitution \
    "$fixture_dir/identity-probe-unsupported-substitution.rs"
expect_rejected reserved-export-name \
    "$fixture_dir/identity-probe-reserved-export.rs"

expect_rejected unreachable-unsupported-body \
    "$fixture_dir/identity-probe-unreachable-unsupported.rs"

expect_rejected exported-unsupported-body \
    "$fixture_dir/identity-probe-exported-unsupported.rs"

echo "identity executable acceptance: ok"
