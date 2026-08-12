#!/bin/sh
set -eu

test_program=${1:?missing metadata test program}
temporary_root=${TMPDIR:-/tmp}
temporary_directory=$(mktemp -d "${temporary_root%/}/cmrustc-metadata.XXXXXX")

cleanup()
{
    rm -rf "$temporary_directory"
}

trap cleanup EXIT HUP INT TERM

first_artifact=$temporary_directory/first.cmhir
second_artifact=$temporary_directory/second.cmhir

"$test_program" produce-forward "$first_artifact"
"$test_program" produce-reverse "$second_artifact"
test -s "$first_artifact"
cmp -s "$first_artifact" "$second_artifact"
"$test_program" consume "$first_artifact"

echo "HIR metadata separate-process acceptance: ok"
