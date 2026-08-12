#!/bin/sh
set -eu

cc=${CC:-cc}
cflags=${CFLAGS:-"-O2 -std=c99"}
probe_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/cmrustc-abi.XXXXXX")
trap 'rm -rf "$work_dir"' EXIT HUP INT TERM

compile_and_run_required()
{
    name=$1
    shift
    if ! "$cc" $cflags "$@" -o "$work_dir/$name"; then
        echo "$name required compile-failed" >&2
        exit 1
    fi
    if ! "$work_dir/$name"; then
        echo "$name required run-failed" >&2
        exit 1
    fi
    echo "$name required pass"
}

compile_and_classify()
{
    name=$1
    fallback=$2
    shift 2
    if "$cc" $cflags "$@" -o "$work_dir/$name" >/dev/null 2>&1 \
        && "$work_dir/$name" >/dev/null 2>&1; then
        echo "$name extension pass"
    else
        echo "$name $fallback"
    fi
}

compile_and_run_required core "$probe_dir/probe_core.c"

compile_and_classify tls "avoided" "$probe_dir/probe_tls.c"
compile_and_classify atomics "emulated-lock" "$probe_dir/probe_atomics.c"
compile_and_classify weak "emulated-strong-symbols" \
    "$probe_dir/probe_weak_main.c" "$probe_dir/probe_weak_default.c"
compile_and_classify sections "avoided" "$probe_dir/probe_sections.c"

response_file="$work_dir/core.response"
{
    printf '%s\n' "$cflags"
    printf '%s\n' "$probe_dir/probe_core.c"
    printf '%s\n' -o "$work_dir/response"
} >"$response_file"
if ! "$cc" @"$response_file" || ! "$work_dir/response" >/dev/null; then
    echo "response-files required failed" >&2
    exit 1
fi
echo "response-files required pass"

large_source="$work_dir/large.c"
awk 'BEGIN {
    print "static int f0(void) { return 0; }"
    for (i = 1; i < 4096; i += 1) {
        printf "static int f%d(void) { return f%d() + 1; }\n", i, i - 1
    }
    print "int main(void) { return f4095() == 4095 ? 0 : 1; }"
}' >"$large_source"
if ! "$cc" $cflags "$large_source" -o "$work_dir/large" \
    || ! "$work_dir/large"; then
    echo "large-translation-unit required failed" >&2
    exit 1
fi
echo "large-translation-unit required pass"
