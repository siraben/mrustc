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
semantic_producer_source=$fixture_root/driver-metadata-semantic-producer.rs
semantic_consumer_source=$fixture_root/driver-metadata-semantic-consumer.rs
semantic_first=$temporary_directory/semantic-first.cmhir
semantic_second=$temporary_directory/semantic-second.cmhir
semantic_consumer=$temporary_directory/semantic-consumer.cmhir
v2_producer_source=$fixture_root/driver-metadata-v2-producer.rs
v2_first=$temporary_directory/v2-first.cmhir
v2_second=$temporary_directory/v2-second.cmhir
v2_consumer_first=$temporary_directory/v2-consumer-first.cmhir
v2_consumer_second=$temporary_directory/v2-consumer-second.cmhir
v2_transitive=$temporary_directory/v2-transitive.cmhir

"$cmrustc" --edition 2024 --emit-cmhir "$producer_source" \
    --crate-name producer -o "$producer_first"
"$cmrustc" --edition 2024 --emit-cmhir "$producer_source" \
    --crate-name producer -o "$producer_second"
test -s "$producer_first"
cmp -s "$producer_first" "$producer_second"

# Semantic metadata is an explicit exact-v1.1 file kind. It transports only
# the supported self-contained local-trait universe and remains deterministic.
"$cmrustc" --edition 2024 --emit-semantic-cmhir \
    "$semantic_producer_source" --crate-name semantic_dep \
    -o "$semantic_first"
"$cmrustc" --edition 2024 --emit-semantic-cmhir \
    "$semantic_producer_source" --crate-name semantic_dep \
    -o "$semantic_second"
test -s "$semantic_first"
cmp -s "$semantic_first" "$semantic_second"
"$cmrustc" --edition 2024 --emit-semantic-cmhir \
    "$semantic_consumer_source" --crate-name semantic_consumer \
    --extern-semantic-cmhir dep "$semantic_first" \
    -o "$semantic_consumer"
test -s "$semantic_consumer"

# Exact v2 declaration metadata adds public free-function, const, and static
# signatures while remaining deterministic and declaration-only.
"$cmrustc" --edition 2024 --emit-cmhir-v2 "$v2_producer_source" \
    --crate-name v2_producer -o "$v2_first"
"$cmrustc" --edition 2024 --emit-cmhir-v2 "$v2_producer_source" \
    --crate-name v2_producer -o "$v2_second"
test -s "$v2_first"
cmp -s "$v2_first" "$v2_second"

# Neither exact decoder may silently auto-detect the other file kind.
if "$cmrustc" --edition 2024 --emit-semantic-cmhir \
    "$semantic_consumer_source" --crate-name wrong_v10 \
    --extern-semantic-cmhir dep "$producer_first" \
    -o "$temporary_directory/wrong-v10.cmhir" \
    >"$temporary_directory/wrong-v10.stdout" \
    2>"$temporary_directory/wrong-v10.stderr"; then
    echo "v1.1 dependency mode unexpectedly accepted v1.0 bytes" >&2
    exit 1
fi
test ! -e "$temporary_directory/wrong-v10.cmhir"
if "$cmrustc" --edition 2024 --emit-cmhir \
    "$semantic_consumer_source" --crate-name wrong_v11 \
    --extern-cmhir dep "$semantic_first" \
    -o "$temporary_directory/wrong-v11.cmhir" \
    >"$temporary_directory/wrong-v11.stdout" \
    2>"$temporary_directory/wrong-v11.stderr"; then
    echo "v1.0 dependency mode unexpectedly accepted v1.1 bytes" >&2
    exit 1
fi
test ! -e "$temporary_directory/wrong-v11.cmhir"

# V2 is also an exact file kind: all six cross-version decode directions
# among v1.0, v1.1, and v2.0 must reject before publishing an output.
if "$cmrustc" --edition 2024 --emit-cmhir-v2 "$producer_source" \
    --crate-name v2_rejects_v10 \
    --extern-cmhir-v2 dep "$producer_first" \
    -o "$temporary_directory/v2-rejects-v10.cmhir" \
    >"$temporary_directory/v2-rejects-v10.stdout" \
    2>"$temporary_directory/v2-rejects-v10.stderr"; then
    echo "v2 dependency mode unexpectedly accepted v1.0 bytes" >&2
    exit 1
fi
test ! -e "$temporary_directory/v2-rejects-v10.cmhir"
if "$cmrustc" --edition 2024 --emit-cmhir-v2 "$producer_source" \
    --crate-name v2_rejects_v11 \
    --extern-cmhir-v2 dep "$semantic_first" \
    -o "$temporary_directory/v2-rejects-v11.cmhir" \
    >"$temporary_directory/v2-rejects-v11.stdout" \
    2>"$temporary_directory/v2-rejects-v11.stderr"; then
    echo "v2 dependency mode unexpectedly accepted v1.1 bytes" >&2
    exit 1
fi
test ! -e "$temporary_directory/v2-rejects-v11.cmhir"
if "$cmrustc" --edition 2024 --emit-cmhir "$producer_source" \
    --crate-name v10_rejects_v2 --extern-cmhir dep "$v2_first" \
    -o "$temporary_directory/v10-rejects-v2.cmhir" \
    >"$temporary_directory/v10-rejects-v2.stdout" \
    2>"$temporary_directory/v10-rejects-v2.stderr"; then
    echo "v1.0 dependency mode unexpectedly accepted v2 bytes" >&2
    exit 1
fi
test ! -e "$temporary_directory/v10-rejects-v2.cmhir"
if "$cmrustc" --edition 2024 --emit-semantic-cmhir \
    "$semantic_producer_source" --crate-name v11_rejects_v2 \
    --extern-semantic-cmhir dep "$v2_first" \
    -o "$temporary_directory/v11-rejects-v2.cmhir" \
    >"$temporary_directory/v11-rejects-v2.stdout" \
    2>"$temporary_directory/v11-rejects-v2.stderr"; then
    echo "v1.1 dependency mode unexpectedly accepted v2 bytes" >&2
    exit 1
fi
test ! -e "$temporary_directory/v11-rejects-v2.cmhir"

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

# Loading the value-bearing v2 bytes twice forces independent remapping of
# their reserved value DefIds and every signature type. The ordinary external
# type import then authenticates the second artifact, and the resulting local
# declaration remains consumable in one more fresh process.
"$cmrustc" --edition 2024 --emit-cmhir-v2 "$consumer_source" \
    --crate-name v2_consumer \
    --extern-cmhir-v2 filler "$v2_first" \
    --extern-cmhir-v2 dep "$v2_first" \
    -o "$v2_consumer_first"
"$cmrustc" --edition 2024 --emit-cmhir-v2 "$consumer_source" \
    --crate-name v2_consumer \
    --extern-cmhir-v2 filler "$v2_first" \
    --extern-cmhir-v2 dep "$v2_first" \
    -o "$v2_consumer_second"
test -s "$v2_consumer_first"
cmp -s "$v2_consumer_first" "$v2_consumer_second"
"$cmrustc" --edition 2024 --emit-cmhir-v2 "$transitive_source" \
    --crate-name v2_final \
    --extern-cmhir-v2 consumer "$v2_consumer_first" \
    -o "$v2_transitive"
test -s "$v2_transitive"

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

# V2 failures preserve an existing destination too; decoding corrupt input
# cannot truncate or replace it.
if "$cmrustc" --edition 2024 --emit-cmhir-v2 "$producer_source" \
    --crate-name v2_preserved --extern-cmhir-v2 dep "$corrupted" \
    -o "$preserved" >"$temporary_directory/v2-corrupt.stdout" \
    2>"$temporary_directory/v2-corrupt.stderr"; then
    echo "corrupt v2 dependency unexpectedly loaded" >&2
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
