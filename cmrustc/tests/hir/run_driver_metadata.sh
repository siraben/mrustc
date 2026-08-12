#!/bin/sh
set -eu

cmrustc=${CMRUSTC:?missing cmrustc driver}
fixture_root=${FIXTURE_ROOT:?missing HIR fixture root}
temporary_root=${TMPDIR:-/tmp}
temporary_directory=$(mktemp -d \
    "${temporary_root%/}/cmrustc-driver-metadata.XXXXXX")

cleanup()
{
    rm -rf "$temporary_directory"
}

trap cleanup EXIT HUP INT TERM

producer_source=$fixture_root/driver-metadata-producer.rs
consumer_source=$fixture_root/driver-metadata-consumer.rs
transitive_source=$fixture_root/driver-metadata-transitive.rs
body_source=$fixture_root/driver-metadata-body.rs
trait_source=$fixture_root/driver-metadata-trait.rs
trait_alias_source=$fixture_root/driver-metadata-trait-alias.rs
auto_trait_source=$fixture_root/driver-metadata-auto-trait.rs
negative_impl_source=$fixture_root/driver-metadata-negative-impl.rs
producer_first=$temporary_directory/producer-first.cmhir
producer_second=$temporary_directory/producer-second.cmhir
consumer_first=$temporary_directory/consumer-first.cmhir
consumer_second=$temporary_directory/consumer-second.cmhir
transitive=$temporary_directory/transitive.cmhir
preserved=$temporary_directory/preserved.cmhir
expected=$temporary_directory/expected
corrupted=$temporary_directory/corrupted.cmhir

"$cmrustc" --edition 2024 --emit-cmhir "$producer_source" \
    --crate-name producer -o "$producer_first"
"$cmrustc" --edition 2024 --emit-cmhir "$producer_source" \
    --crate-name producer -o "$producer_second"
test -s "$producer_first"
cmp -s "$producer_first" "$producer_second"

# This is a fresh driver process. Loading the same bytes first as `filler`
# prepopulates its HIR context; loading them again as `dep` must remap all
# runtime crate/module/item/type IDs. The consumer's exact external ADT import
# only authenticates if the second decoded artifact remains coherent.
"$cmrustc" --edition 2024 --emit-cmhir "$consumer_source" \
    --crate-name consumer \
    --extern-cmhir filler "$producer_first" \
    --extern-cmhir dep "$producer_first" \
    -o "$consumer_first"
"$cmrustc" --edition 2024 --emit-cmhir "$consumer_source" \
    --crate-name consumer \
    --extern-cmhir filler "$producer_first" \
    --extern-cmhir dep "$producer_first" \
    -o "$consumer_second"
test -s "$consumer_first"
cmp -s "$consumer_first" "$consumer_second"

# The artifact produced by the dependent invocation is itself consumable in
# another fresh process and its public local declaration is remapped there.
"$cmrustc" --edition 2024 --emit-cmhir "$transitive_source" \
    --crate-name final --extern-cmhir consumer "$consumer_first" \
    -o "$transitive"
test -s "$transitive"

# The source is genuinely dependent: without loaded bytes, the external ADT
# import is unresolved and no requested artifact may be created.
if "$cmrustc" --edition 2024 --emit-cmhir "$consumer_source" \
    --crate-name consumer -o "$preserved" \
    >"$temporary_directory/missing.stdout" \
    2>"$temporary_directory/missing.stderr"; then
    echo "dependency-backed consumer unexpectedly lowered without metadata" \
        >&2
    exit 1
fi
test ! -e "$preserved"

printf '%s\n' 'preserve-existing-output' >"$preserved"
cp "$preserved" "$expected"
printf '%s\n' 'not a cmhir artifact' >"$corrupted"
if "$cmrustc" --edition 2024 --emit-cmhir "$consumer_source" \
    --crate-name consumer --extern-cmhir dep "$corrupted" \
    -o "$preserved" >"$temporary_directory/corrupt.stdout" \
    2>"$temporary_directory/corrupt.stderr"; then
    echo "corrupt dependency unexpectedly loaded" >&2
    exit 1
fi
cmp -s "$preserved" "$expected"

# Traits and bodies are not silently erased from declaration metadata.
for unsupported_source in "$body_source" "$trait_source" \
        "$trait_alias_source" "$auto_trait_source" \
        "$negative_impl_source"; do
    if "$cmrustc" --edition 2024 --emit-cmhir "$unsupported_source" \
        --crate-name unsupported -o "$preserved" \
        >"$temporary_directory/unsupported.stdout" \
        2>"$temporary_directory/unsupported.stderr"; then
        echo "unsupported HIR unexpectedly emitted as cmhir" >&2
        exit 1
    fi
    cmp -s "$preserved" "$expected"
done

# An output alias cannot overwrite a loaded dependency, including through a
# hard link rather than textual path equality.
dependency_alias=$temporary_directory/dependency-alias.cmhir
ln "$producer_first" "$dependency_alias"
if "$cmrustc" --edition 2024 --emit-cmhir "$consumer_source" \
    --crate-name consumer --extern-cmhir dep "$producer_first" \
    -o "$dependency_alias" >"$temporary_directory/alias.stdout" \
    2>"$temporary_directory/alias.stderr"; then
    echo "dependency/output hard-link alias unexpectedly accepted" >&2
    exit 1
fi
cmp -s "$producer_first" "$producer_second"

if find "$temporary_directory" -name '*.tmp.*' -print | grep . \
    >"$temporary_directory/leaked-temporaries"; then
    echo "failed cmhir publication leaked a temporary file" >&2
    exit 1
fi

echo "driver cmhir producer/dependency acceptance: ok"
