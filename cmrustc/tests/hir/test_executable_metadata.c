#include "cm/hir/executable_metadata.h"
#include "cm/sha256.h"
#include "metadata_codec.h"

#include <assert.h>
#include <string.h>

#define S(text) { (unsigned char *)(text), sizeof(text) - 1u }
#define HEADER_SIZE 40u
#define CRC_OFFSET 32u

typedef struct TestFixture {
    CmHirExecutableMetadata metadata;
    CmHirArtifactSourceEntry sources[2];
    CmHirExecutableString cfgs[1];
    CmHirExecutableModule modules[1];
    CmHirExecutableTrait traits[1];
    CmHirExecutableType types[2];
    CmHirExecutableImpl impls[1];
    CmHirExecutableValue values[3];
    uint32_t bounded_parameters[2];
    uint32_t bump_parameters[1];
    uint32_t bump_two_parameters[1];
    CmHirExecutablePredicate predicates[1];
    CmHirExecutableNamespaceEntry namespace_entries[4];
    CmHirExecutableBody bodies[1];
    CmHirExecutableLinkObject objects[2];
    CmHirExecutableLinkSymbol symbols[2];
} TestFixture;

static void test_put_u32(unsigned char *bytes, uint32_t value)
{
    unsigned int index;
    for (index = 0u; index < 4u; index += 1u) {
        bytes[index] = (unsigned char)(value & UINT32_C(0xff));
        value >>= 8u;
    }
}

static uint64_t test_get_u64(const unsigned char *bytes)
{
    uint64_t value = UINT64_C(0);
    unsigned int index;
    for (index = 0u; index < 8u; index += 1u)
        value |= (uint64_t)bytes[index] << (index * 8u);
    return value;
}

static void test_recompute_crc(CmByteBuf *bytes)
{
    uint32_t crc = cm_hir_metadata_crc32(bytes->data + HEADER_SIZE,
        bytes->len - HEADER_SIZE);
    test_put_u32(bytes->data + CRC_OFFSET, crc);
}

static CmByteBuf test_copy(const CmByteBuf *source)
{
    CmByteBuf copy;
    cm_byte_buf_init(&copy);
    cm_byte_buf_append(&copy, source->data, source->len);
    return copy;
}

static size_t test_section_offset(const CmByteBuf *bytes,
    const char tag[4])
{
    size_t cursor = HEADER_SIZE;
    while (cursor + 12u <= bytes->len) {
        uint64_t length;
        if (memcmp(bytes->data + cursor, tag, 4u) == 0) return cursor;
        length = test_get_u64(bytes->data + cursor + 4u);
        assert(length <= (uint64_t)(bytes->len - cursor - 12u));
        cursor += 12u + (size_t)length;
    }
    assert(0);
    return 0u;
}

static void test_hash(const void *data, size_t length,
    CmHirArtifactDigest *digest)
{
    CmSha256 sha;
    cm_sha256_init(&sha);
    cm_sha256_update(&sha, data, length);
    cm_sha256_final(&sha, digest->bytes);
}

static void test_fixture_init(TestFixture *fixture)
{
    static const unsigned char rust_source[] =
        "pub trait Present {} impl Present for u32 {}\n"
        "pub fn bounded<T: Present>(prefix:u32,x:T)->T{x}\n";
    static const unsigned char native_source[] =
        "unsigned bump(unsigned x){return x+1;}\n";
    CmHirExecutableMetadata *metadata;
    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirExecutableString)S("g3_dep");
    metadata->crate_disambiguator = (CmHirExecutableString)S("fixture-v1");
    metadata->edition = UINT32_C(2021);
    metadata->target_descriptor = (CmHirExecutableString)S(
        "x86_64-unknown-linux-gnu;e-m:e-p270:32:32-p271:32:32-p272:64:64");
    metadata->panic_strategy = (CmHirExecutableString)S("abort");
    fixture->cfgs[0] = (CmHirExecutableString)S("target_pointer_width=64");
    metadata->cfgs = fixture->cfgs;
    metadata->cfg_count = 1u;
    fixture->sources[0].logical_path.data = "native/bump.c";
    fixture->sources[0].logical_path.length = sizeof("native/bump.c") - 1u;
    fixture->sources[0].contents.data = native_source;
    fixture->sources[0].contents.length = sizeof(native_source) - 1u;
    fixture->sources[1].logical_path.data = "src/lib.rs";
    fixture->sources[1].logical_path.length = sizeof("src/lib.rs") - 1u;
    fixture->sources[1].contents.data = rust_source;
    fixture->sources[1].contents.length = sizeof(rust_source) - 1u;
    metadata->source_entries = fixture->sources;
    metadata->source_entry_count = 2u;
    assert(cm_hir_artifact_source_closure_digest(fixture->sources, 2u,
        &metadata->source_digest) == CM_HIR_ARTIFACT_IDENTITY_OK);

    fixture->modules[0].parent_module = 0u;
    fixture->modules[0].name = metadata->crate_name;
    metadata->modules = fixture->modules;
    metadata->module_count = 1u;
    fixture->traits[0].owner_module = 1u;
    fixture->traits[0].name = (CmHirExecutableString)S("Present");
    fixture->traits[0].source_ordinal = 1u;
    metadata->traits = fixture->traits;
    metadata->trait_count = 1u;
    fixture->types[0].kind = CM_HIR_EXEC_TYPE_PRIMITIVE;
    fixture->types[0].primitive = CM_HIR_EXEC_PRIMITIVE_U32;
    fixture->types[1].kind = CM_HIR_EXEC_TYPE_VALUE_GENERIC;
    fixture->types[1].owner_value = 1u;
    fixture->types[1].generic_index = 0u;
    metadata->types = fixture->types;
    metadata->type_count = 2u;
    fixture->impls[0].owner_module = 1u;
    fixture->impls[0].source_ordinal = 2u;
    fixture->impls[0].trait_local = 1u;
    fixture->impls[0].self_type = 1u;
    metadata->impls = fixture->impls;
    metadata->impl_count = 1u;

    fixture->bounded_parameters[0] = 1u;
    fixture->bounded_parameters[1] = 2u;
    fixture->values[0].owner_module = 1u;
    fixture->values[0].name = (CmHirExecutableString)S("bounded");
    fixture->values[0].source_ordinal = 3u;
    fixture->values[0].kind = CM_HIR_EXEC_VALUE_RECIPE;
    fixture->values[0].generic_name = (CmHirExecutableString)S("T");
    fixture->values[0].parameter_count = 2u;
    fixture->values[0].parameter_types = fixture->bounded_parameters;
    fixture->values[0].return_type = 2u;
    fixture->values[0].predicate_start = 1u;
    fixture->values[0].predicate_count = 1u;
    fixture->values[0].execution_local = 1u;
    fixture->bump_parameters[0] = 1u;
    fixture->values[1].owner_module = 1u;
    fixture->values[1].name = (CmHirExecutableString)S("bump");
    fixture->values[1].source_ordinal = 4u;
    fixture->values[1].kind = CM_HIR_EXEC_VALUE_NATIVE_OBJECT;
    fixture->values[1].parameter_count = 1u;
    fixture->values[1].parameter_types = fixture->bump_parameters;
    fixture->values[1].return_type = 1u;
    fixture->values[1].execution_local = 1u;
    metadata->values = fixture->values;
    metadata->value_count = 2u;
    fixture->predicates[0].owner_value = 1u;
    fixture->predicates[0].ordinal = 0u;
    fixture->predicates[0].subject_type = 2u;
    fixture->predicates[0].trait_local = 1u;
    metadata->predicates = fixture->predicates;
    metadata->predicate_count = 1u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind = CM_HIR_EXEC_NAMESPACE_TYPE;
    fixture->namespace_entries[0].name = fixture->traits[0].name;
    fixture->namespace_entries[0].target_kind = CM_HIR_EXEC_NAMESPACE_TRAIT;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 1u;
    fixture->namespace_entries[1].owner_module = 1u;
    fixture->namespace_entries[1].namespace_kind = CM_HIR_EXEC_NAMESPACE_VALUE;
    fixture->namespace_entries[1].name = fixture->values[0].name;
    fixture->namespace_entries[1].target_kind
        = CM_HIR_EXEC_NAMESPACE_VALUE_TARGET;
    fixture->namespace_entries[1].target_local = 1u;
    fixture->namespace_entries[1].export_ordinal = 3u;
    fixture->namespace_entries[2].owner_module = 1u;
    fixture->namespace_entries[2].namespace_kind = CM_HIR_EXEC_NAMESPACE_VALUE;
    fixture->namespace_entries[2].name = fixture->values[1].name;
    fixture->namespace_entries[2].target_kind
        = CM_HIR_EXEC_NAMESPACE_VALUE_TARGET;
    fixture->namespace_entries[2].target_local = 2u;
    fixture->namespace_entries[2].export_ordinal = 4u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 3u;
    fixture->bodies[0].owner_value = 1u;
    fixture->bodies[0].parameter_index = 1u;
    fixture->bodies[0].parameter_type = 2u;
    fixture->bodies[0].return_type = 2u;
    metadata->bodies = fixture->bodies;
    metadata->body_count = 1u;
    fixture->objects[0].archive_member_name
        = (CmHirExecutableString)S("g3_dep.o");
    fixture->objects[0].byte_length = UINT64_C(4);
    fixture->objects[0].object_bytes = "OBJ!";
    fixture->objects[0].object_bytes_length = 4u;
    test_hash("OBJ!", 4u, &fixture->objects[0].object_digest);
    fixture->objects[0].symbol_start = 1u;
    fixture->objects[0].symbol_count = 1u;
    metadata->objects = fixture->objects;
    metadata->object_count = 1u;
    fixture->symbols[0].owner_value = 2u;
    fixture->symbols[0].object_local = 1u;
    fixture->symbols[0].external_symbol
        = (CmHirExecutableString)S("cmrustc_g3_bump");
    metadata->symbols = fixture->symbols;
    metadata->symbol_count = 1u;
}

static void test_expectation_init(const TestFixture *fixture,
    CmHirExecutableMetadataExpectation *expectation)
{
    CmHirArtifactDigest link;
    memset(expectation, 0, sizeof(*expectation));
    expectation->crate_name = fixture->metadata.crate_name;
    expectation->crate_disambiguator = fixture->metadata.crate_disambiguator;
    expectation->edition = fixture->metadata.edition;
    expectation->target_descriptor = fixture->metadata.target_descriptor;
    expectation->panic_strategy = fixture->metadata.panic_strategy;
    expectation->cfgs = fixture->metadata.cfgs;
    expectation->cfg_count = fixture->metadata.cfg_count;
    expectation->source_digest = fixture->metadata.source_digest;
    assert(cm_hir_executable_metadata_compute_identity(&fixture->metadata,
        &link, &expectation->artifact_identity) == CM_HIR_EXEC_METADATA_OK);
}

static void test_add_second_native(TestFixture *fixture)
{
    CmHirExecutableMetadata *metadata = &fixture->metadata;
    fixture->bump_two_parameters[0] = 1u;
    fixture->values[2].owner_module = 1u;
    fixture->values[2].name = (CmHirExecutableString)S("bump_two");
    fixture->values[2].source_ordinal = 5u;
    fixture->values[2].kind = CM_HIR_EXEC_VALUE_NATIVE_OBJECT;
    fixture->values[2].parameter_count = 1u;
    fixture->values[2].parameter_types = fixture->bump_two_parameters;
    fixture->values[2].return_type = 1u;
    fixture->values[2].execution_local = 2u;
    metadata->value_count = 3u;
    fixture->namespace_entries[3].owner_module = 1u;
    fixture->namespace_entries[3].namespace_kind = CM_HIR_EXEC_NAMESPACE_VALUE;
    fixture->namespace_entries[3].name = fixture->values[2].name;
    fixture->namespace_entries[3].target_kind
        = CM_HIR_EXEC_NAMESPACE_VALUE_TARGET;
    fixture->namespace_entries[3].target_local = 3u;
    fixture->namespace_entries[3].export_ordinal = 5u;
    metadata->namespace_count = 4u;
    fixture->objects[1].archive_member_name
        = (CmHirExecutableString)S("g3_more.o");
    fixture->objects[1].byte_length = UINT64_C(4);
    fixture->objects[1].object_bytes = "OBJ2";
    fixture->objects[1].object_bytes_length = 4u;
    test_hash("OBJ2", 4u, &fixture->objects[1].object_digest);
    fixture->objects[1].symbol_start = 2u;
    fixture->objects[1].symbol_count = 1u;
    metadata->object_count = 2u;
    fixture->symbols[1].owner_value = 3u;
    fixture->symbols[1].object_local = 2u;
    fixture->symbols[1].external_symbol
        = (CmHirExecutableString)S("cmrustc_g3_bump_two");
    metadata->symbol_count = 2u;
}

static void test_round_trip_and_twin(void)
{
    TestFixture first;
    TestFixture second;
    CmHirExecutableMetadataExpectation expectation;
    CmHirExecutableMetadata decoded;
    CmByteBuf bytes1;
    CmByteBuf bytes2;
    test_fixture_init(&first);
    test_fixture_init(&second);
    test_expectation_init(&first, &expectation);
    cm_byte_buf_init(&bytes1);
    cm_byte_buf_init(&bytes2);
    assert(cm_hir_executable_metadata_encode(&first.metadata, &bytes1)
        == CM_HIR_EXEC_METADATA_OK);
    assert(cm_hir_executable_metadata_encode(&second.metadata, &bytes2)
        == CM_HIR_EXEC_METADATA_OK);
    assert(bytes1.len != 0u && bytes1.len == bytes2.len);
    assert(memcmp(bytes1.data, bytes2.data, bytes1.len) == 0);
    cm_hir_executable_metadata_init(&decoded);
    assert(cm_hir_executable_metadata_decode(bytes1.data, bytes1.len,
        &expectation, &decoded) == CM_HIR_EXEC_METADATA_OK);
    assert(decoded.owns_storage && decoded.trait_count == 1u);
    assert(decoded.impl_count == 1u && decoded.body_count == 1u);
    assert(decoded.object_count == 1u && decoded.symbol_count == 1u);
    assert(decoded.values[0].kind == CM_HIR_EXEC_VALUE_RECIPE);
    assert(decoded.values[1].kind == CM_HIR_EXEC_VALUE_NATIVE_OBJECT);
    assert(cm_hir_executable_metadata_validate(&decoded)
        == CM_HIR_EXEC_METADATA_OK);
    decoded.source_digest.bytes[0] ^= UINT8_C(1);
    assert(cm_hir_executable_metadata_validate(&decoded)
        == CM_HIR_EXEC_METADATA_IDENTITY_MISMATCH);
    decoded.source_digest.bytes[0] ^= UINT8_C(1);
    decoded.bodies[0].parameter_index = decoded.values[0].parameter_count;
    assert(cm_hir_executable_metadata_validate(&decoded)
        == CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR);
    cm_hir_executable_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&bytes1);
    cm_byte_buf_destroy(&bytes2);
}

static void test_rejections_are_transactional(void)
{
    TestFixture fixture;
    CmHirExecutableMetadataExpectation expectation;
    CmHirExecutableMetadata output;
    CmHirExecutableMetadata saved;
    CmByteBuf original;
    CmByteBuf bad;
    size_t offset;
    size_t index;
    test_fixture_init(&fixture);
    test_expectation_init(&fixture, &expectation);
    cm_byte_buf_init(&original);
    assert(cm_hir_executable_metadata_encode(&fixture.metadata, &original)
        == CM_HIR_EXEC_METADATA_OK);
    memset(&output, 0, sizeof(output));
    output.edition = UINT32_C(0xdeadbeef);
    saved = output;

    assert(cm_hir_executable_metadata_decode(original.data,
        original.len - 1u, &expectation, &output)
        == CM_HIR_EXEC_METADATA_INVALID_FORMAT);
    assert(memcmp(&output, &saved, sizeof(output)) == 0);
    for (index = 0u; index < original.len; index += 1u) {
        CmHirExecutableMetadataStatus status
            = cm_hir_executable_metadata_decode(original.data, index,
                &expectation, &output);
        assert(status != CM_HIR_EXEC_METADATA_OK);
        assert(memcmp(&output, &saved, sizeof(output)) == 0);
    }
    for (index = 0u; index < original.len; index += 1u) {
        CmHirExecutableMetadataStatus status;
        bad = test_copy(&original);
        bad.data[index] ^= UINT8_C(0x80);
        status = cm_hir_executable_metadata_decode(bad.data, bad.len,
            &expectation, &output);
        assert(status != CM_HIR_EXEC_METADATA_OK);
        assert(memcmp(&output, &saved, sizeof(output)) == 0);
        cm_byte_buf_destroy(&bad);
    }
    bad = test_copy(&original);
    bad.data[bad.len - 1u] ^= UINT8_C(1);
    assert(cm_hir_executable_metadata_decode(bad.data, bad.len,
        &expectation, &output) == CM_HIR_EXEC_METADATA_INVALID_FORMAT);
    assert(memcmp(&output, &saved, sizeof(output)) == 0);
    cm_byte_buf_destroy(&bad);

    bad = test_copy(&original);
    offset = test_section_offset(&bad, "BODY");
    memcpy(bad.data + offset, "LINK", 4u);
    test_recompute_crc(&bad);
    assert(cm_hir_executable_metadata_decode(bad.data, bad.len,
        &expectation, &output) == CM_HIR_EXEC_METADATA_INVALID_FORMAT);
    assert(memcmp(&output, &saved, sizeof(output)) == 0);
    cm_byte_buf_destroy(&bad);

    bad = test_copy(&original);
    offset = test_section_offset(&bad, "BODY") + 12u + 4u + 1u;
    bad.data[offset] = UINT8_C(1);
    test_recompute_crc(&bad);
    assert(cm_hir_executable_metadata_decode(bad.data, bad.len,
        &expectation, &output) == CM_HIR_EXEC_METADATA_INVALID_FORMAT);
    cm_byte_buf_destroy(&bad);

    bad = test_copy(&original);
    offset = test_section_offset(&bad, "BODY") + 12u + 4u + 4u;
    test_put_u32(bad.data + offset, UINT32_C(2));
    test_recompute_crc(&bad);
    assert(cm_hir_executable_metadata_decode(bad.data, bad.len,
        &expectation, &output) == CM_HIR_EXEC_METADATA_INVALID_FORMAT);
    assert(memcmp(&output, &saved, sizeof(output)) == 0);
    cm_byte_buf_destroy(&bad);

    bad = test_copy(&original);
    offset = test_section_offset(&bad, "MANF") + 12u;
    /* schema/profile/universe/reserved, then crate string and identity fields.
       Flip the first family state by locating the fixed family table tail. */
    offset += (size_t)test_get_u64(bad.data
        + test_section_offset(&bad, "MANF") + 4u);
    assert(offset <= bad.len);
    /* A simpler fail-closed manifest mutation: OPEN -> an unknown universe. */
    offset = test_section_offset(&bad, "MANF") + 12u + 2u;
    bad.data[offset] = UINT8_C(1);
    test_recompute_crc(&bad);
    assert(cm_hir_executable_metadata_decode(bad.data, bad.len,
        &expectation, &output) == CM_HIR_EXEC_METADATA_INVALID_FORMAT);
    cm_byte_buf_destroy(&bad);

    bad = test_copy(&original);
    offset = test_section_offset(&bad, "MANF");
    offset += 12u + (size_t)test_get_u64(bad.data + offset + 4u)
        - 14u * 12u;
    /* First family logical count is after tag/state/reserved. */
    test_put_u32(bad.data + offset + 4u, UINT32_C(0));
    test_recompute_crc(&bad);
    assert(cm_hir_executable_metadata_decode(bad.data, bad.len,
        &expectation, &output) == CM_HIR_EXEC_METADATA_INVALID_FORMAT);
    assert(memcmp(&output, &saved, sizeof(output)) == 0);
    cm_byte_buf_destroy(&bad);

    expectation.edition = UINT32_C(2018);
    assert(cm_hir_executable_metadata_decode(original.data, original.len,
        &expectation, &output) == CM_HIR_EXEC_METADATA_IDENTITY_MISMATCH);
    expectation.edition = UINT32_C(2022);
    assert(cm_hir_executable_metadata_decode(original.data, original.len,
        &expectation, &output) == CM_HIR_EXEC_METADATA_INVALID_ARGUMENT);
    expectation.edition = UINT32_C(2021);
    expectation.panic_strategy = (CmHirExecutableString)S("unwind");
    assert(cm_hir_executable_metadata_decode(original.data, original.len,
        &expectation, &output) == CM_HIR_EXEC_METADATA_INVALID_ARGUMENT);
    expectation.panic_strategy = fixture.metadata.panic_strategy;
    expectation.target_descriptor = (CmHirExecutableString)S("wrong-target");
    assert(cm_hir_executable_metadata_decode(original.data, original.len,
        &expectation, &output) == CM_HIR_EXEC_METADATA_IDENTITY_MISMATCH);
    expectation.target_descriptor = fixture.metadata.target_descriptor;
    expectation.crate_name = (CmHirExecutableString)S("wrong_crate");
    assert(cm_hir_executable_metadata_decode(original.data, original.len,
        &expectation, &output) == CM_HIR_EXEC_METADATA_IDENTITY_MISMATCH);
    expectation.crate_name = fixture.metadata.crate_name;
    expectation.crate_disambiguator
        = (CmHirExecutableString)S("wrong-disambiguator");
    assert(cm_hir_executable_metadata_decode(original.data, original.len,
        &expectation, &output) == CM_HIR_EXEC_METADATA_IDENTITY_MISMATCH);
    expectation.crate_disambiguator = fixture.metadata.crate_disambiguator;
    {
        CmHirExecutableString wrong_cfg
            = (CmHirExecutableString)S("target_pointer_width=32");
        expectation.cfgs = &wrong_cfg;
        assert(cm_hir_executable_metadata_decode(original.data, original.len,
            &expectation, &output) == CM_HIR_EXEC_METADATA_IDENTITY_MISMATCH);
        expectation.cfgs = fixture.metadata.cfgs;
    }
    expectation.source_digest.bytes[0] ^= UINT8_C(1);
    assert(cm_hir_executable_metadata_decode(original.data, original.len,
        &expectation, &output) == CM_HIR_EXEC_METADATA_IDENTITY_MISMATCH);
    expectation.source_digest.bytes[0] ^= UINT8_C(1);
    expectation.artifact_identity.bytes[0] ^= UINT8_C(1);
    assert(cm_hir_executable_metadata_decode(original.data, original.len,
        &expectation, &output) == CM_HIR_EXEC_METADATA_IDENTITY_MISMATCH);
    cm_byte_buf_destroy(&original);
}

static void test_unsupported_shapes(void)
{
    TestFixture fixture;
    CmByteBuf output;
    test_fixture_init(&fixture);
    cm_byte_buf_init(&output);
    cm_byte_buf_append(&output, "sentinel", 8u);
    fixture.metadata.impls[0].self_type = 2u;
    assert(cm_hir_executable_metadata_encode(&fixture.metadata, &output)
        == CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR);
    assert(output.len == 8u && memcmp(output.data, "sentinel", 8u) == 0);
    fixture.metadata.impls[0].self_type = 1u;
    fixture.metadata.values[0].predicate_count = 0u;
    assert(cm_hir_executable_metadata_encode(&fixture.metadata, &output)
        == CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR);
    assert(output.len == 8u && memcmp(output.data, "sentinel", 8u) == 0);
    fixture.metadata.values[0].predicate_count = 1u;
    fixture.metadata.bodies[0].parameter_index = 2u;
    assert(cm_hir_executable_metadata_encode(&fixture.metadata, &output)
        == CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR);
    assert(output.len == 8u && memcmp(output.data, "sentinel", 8u) == 0);
    fixture.metadata.bodies[0].parameter_index = 1u;
    {
        CmHirExecutableType saved_type = fixture.metadata.types[0];
        fixture.metadata.types[0] = fixture.metadata.types[1];
        fixture.metadata.types[1] = saved_type;
        assert(cm_hir_executable_metadata_encode(&fixture.metadata, &output)
            == CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR);
        fixture.metadata.types[1] = fixture.metadata.types[0];
        fixture.metadata.types[0] = saved_type;
    }
    fixture.metadata.types[1] = fixture.metadata.types[0];
    assert(cm_hir_executable_metadata_encode(&fixture.metadata, &output)
        == CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR);
    test_fixture_init(&fixture);
    fixture.metadata.edition = UINT32_C(2022);
    assert(cm_hir_executable_metadata_encode(&fixture.metadata, &output)
        == CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR);
    test_fixture_init(&fixture);
    fixture.metadata.panic_strategy = (CmHirExecutableString)S("unwind");
    assert(cm_hir_executable_metadata_encode(&fixture.metadata, &output)
        == CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR);
    test_fixture_init(&fixture);
    test_add_second_native(&fixture);
    {
        CmHirArtifactDigest link;
        CmHirArtifactDigest identity;
        assert(cm_hir_executable_metadata_compute_identity(&fixture.metadata,
            &link, &identity) == CM_HIR_EXEC_METADATA_OK);
    }
    fixture.metadata.symbols[0].object_local = 2u;
    assert(cm_hir_executable_metadata_encode(&fixture.metadata, &output)
        == CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.metadata.symbols[0].object_local = 1u;
    fixture.metadata.symbols[1].external_symbol
        = fixture.metadata.symbols[0].external_symbol;
    assert(cm_hir_executable_metadata_encode(&fixture.metadata, &output)
        == CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR);
    cm_byte_buf_destroy(&output);
}

int main(void)
{
    test_round_trip_and_twin();
    test_rejections_are_transactional();
    test_unsupported_shapes();
    assert(strcmp(cm_hir_executable_metadata_status_name(
        CM_HIR_EXEC_METADATA_IDENTITY_MISMATCH), "identity mismatch") == 0);
    return 0;
}
