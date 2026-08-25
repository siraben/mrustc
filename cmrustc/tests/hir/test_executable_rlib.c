#include "cm/hir/executable_rlib.h"
#include "cm/sha256.h"

#include <assert.h>
#include <string.h>

#define S(text) { (unsigned char *)(text), sizeof(text) - 1u }

typedef struct TestFixture {
    CmHirExecutableMetadata metadata;
    CmHirArtifactSourceEntry source;
    CmHirExecutableString cfg;
    CmHirExecutableModule module;
    CmHirExecutableTrait trait;
    CmHirExecutableType types[2];
    CmHirExecutableImpl impl;
    CmHirExecutableValue values[2];
    uint32_t recipe_parameters[2];
    uint32_t native_parameters[1];
    CmHirExecutablePredicate predicate;
    CmHirExecutableNamespaceEntry namespace_entries[3];
    CmHirExecutableBody body;
    CmHirExecutableLinkObject object;
    CmHirExecutableLinkSymbol symbol;
} TestFixture;

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
    static const unsigned char source[]
        = "pub trait Present {} impl Present for u32 {}\n"
          "pub fn bounded<T:Present>(x:u32,y:T)->T{y}\n";
    CmHirExecutableMetadata *metadata;

    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirExecutableString)S("g3_dep");
    metadata->crate_disambiguator
        = (CmHirExecutableString)S("package-test");
    metadata->edition = UINT32_C(2021);
    metadata->target_descriptor = (CmHirExecutableString)S(
        "x86_64-unknown-linux-gnu;e-m:e-p:64:64");
    metadata->panic_strategy = (CmHirExecutableString)S("abort");
    fixture->cfg = (CmHirExecutableString)S("target_pointer_width=64");
    metadata->cfgs = &fixture->cfg;
    metadata->cfg_count = 1u;
    fixture->source.logical_path.data = "src/lib.rs";
    fixture->source.logical_path.length = sizeof("src/lib.rs") - 1u;
    fixture->source.contents.data = source;
    fixture->source.contents.length = sizeof(source) - 1u;
    metadata->source_entries = &fixture->source;
    metadata->source_entry_count = 1u;
    assert(cm_hir_artifact_source_closure_digest(&fixture->source, 1u,
        &metadata->source_digest) == CM_HIR_ARTIFACT_IDENTITY_OK);

    fixture->module.name = metadata->crate_name;
    metadata->modules = &fixture->module;
    metadata->module_count = 1u;
    fixture->trait.owner_module = 1u;
    fixture->trait.name = (CmHirExecutableString)S("Present");
    fixture->trait.source_ordinal = 1u;
    metadata->traits = &fixture->trait;
    metadata->trait_count = 1u;
    fixture->types[0].kind = CM_HIR_EXEC_TYPE_PRIMITIVE;
    fixture->types[0].primitive = CM_HIR_EXEC_PRIMITIVE_U32;
    fixture->types[1].kind = CM_HIR_EXEC_TYPE_VALUE_GENERIC;
    fixture->types[1].owner_value = 1u;
    metadata->types = fixture->types;
    metadata->type_count = 2u;
    fixture->impl.owner_module = 1u;
    fixture->impl.source_ordinal = 2u;
    fixture->impl.trait_local = 1u;
    fixture->impl.self_type = 1u;
    metadata->impls = &fixture->impl;
    metadata->impl_count = 1u;

    fixture->recipe_parameters[0] = 1u;
    fixture->recipe_parameters[1] = 2u;
    fixture->values[0].owner_module = 1u;
    fixture->values[0].name = (CmHirExecutableString)S("bounded");
    fixture->values[0].source_ordinal = 3u;
    fixture->values[0].kind = CM_HIR_EXEC_VALUE_RECIPE;
    fixture->values[0].generic_name = (CmHirExecutableString)S("T");
    fixture->values[0].parameter_count = 2u;
    fixture->values[0].parameter_types = fixture->recipe_parameters;
    fixture->values[0].return_type = 2u;
    fixture->values[0].predicate_start = 1u;
    fixture->values[0].predicate_count = 1u;
    fixture->values[0].execution_local = 1u;
    fixture->native_parameters[0] = 1u;
    fixture->values[1].owner_module = 1u;
    fixture->values[1].name = (CmHirExecutableString)S("bump");
    fixture->values[1].source_ordinal = 4u;
    fixture->values[1].kind = CM_HIR_EXEC_VALUE_NATIVE_OBJECT;
    fixture->values[1].parameter_count = 1u;
    fixture->values[1].parameter_types = fixture->native_parameters;
    fixture->values[1].return_type = 1u;
    fixture->values[1].execution_local = 1u;
    metadata->values = fixture->values;
    metadata->value_count = 2u;
    fixture->predicate.owner_value = 1u;
    fixture->predicate.subject_type = 2u;
    fixture->predicate.trait_local = 1u;
    metadata->predicates = &fixture->predicate;
    metadata->predicate_count = 1u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind
        = CM_HIR_EXEC_NAMESPACE_TYPE;
    fixture->namespace_entries[0].name = fixture->trait.name;
    fixture->namespace_entries[0].target_kind
        = CM_HIR_EXEC_NAMESPACE_TRAIT;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 1u;
    fixture->namespace_entries[1].owner_module = 1u;
    fixture->namespace_entries[1].namespace_kind
        = CM_HIR_EXEC_NAMESPACE_VALUE;
    fixture->namespace_entries[1].name = fixture->values[0].name;
    fixture->namespace_entries[1].target_kind
        = CM_HIR_EXEC_NAMESPACE_VALUE_TARGET;
    fixture->namespace_entries[1].target_local = 1u;
    fixture->namespace_entries[1].export_ordinal = 3u;
    fixture->namespace_entries[2].owner_module = 1u;
    fixture->namespace_entries[2].namespace_kind
        = CM_HIR_EXEC_NAMESPACE_VALUE;
    fixture->namespace_entries[2].name = fixture->values[1].name;
    fixture->namespace_entries[2].target_kind
        = CM_HIR_EXEC_NAMESPACE_VALUE_TARGET;
    fixture->namespace_entries[2].target_local = 2u;
    fixture->namespace_entries[2].export_ordinal = 4u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 3u;
    fixture->body.owner_value = 1u;
    fixture->body.parameter_index = 1u;
    fixture->body.parameter_type = 2u;
    fixture->body.return_type = 2u;
    metadata->bodies = &fixture->body;
    metadata->body_count = 1u;

    fixture->object.archive_member_name
        = (CmHirExecutableString)S("g3_dep.o");
    fixture->object.byte_length = UINT64_C(4);
    fixture->object.object_bytes = "OBJ!";
    fixture->object.object_bytes_length = 4u;
    test_hash("OBJ!", 4u, &fixture->object.object_digest);
    fixture->object.symbol_start = 1u;
    fixture->object.symbol_count = 1u;
    metadata->objects = &fixture->object;
    metadata->object_count = 1u;
    fixture->symbol.owner_value = 2u;
    fixture->symbol.object_local = 1u;
    fixture->symbol.external_symbol
        = (CmHirExecutableString)S("cmrustc_g3_bump");
    metadata->symbols = &fixture->symbol;
    metadata->symbol_count = 1u;
}

static void test_expectation_init(const TestFixture *fixture,
    CmHirExecutableMetadataExpectation *expectation)
{
    CmHirArtifactDigest link_digest;

    memset(expectation, 0, sizeof(*expectation));
    expectation->crate_name = fixture->metadata.crate_name;
    expectation->crate_disambiguator
        = fixture->metadata.crate_disambiguator;
    expectation->edition = fixture->metadata.edition;
    expectation->target_descriptor = fixture->metadata.target_descriptor;
    expectation->panic_strategy = fixture->metadata.panic_strategy;
    expectation->cfgs = fixture->metadata.cfgs;
    expectation->cfg_count = fixture->metadata.cfg_count;
    expectation->source_digest = fixture->metadata.source_digest;
    assert(cm_hir_executable_metadata_compute_identity(&fixture->metadata,
        &link_digest, &expectation->artifact_identity)
        == CM_HIR_EXEC_METADATA_OK);
}

static void test_copy(CmByteBuf *destination, const CmByteBuf *source)
{
    cm_byte_buf_clear(destination);
    cm_byte_buf_append(destination, source->data, source->len);
}

static void test_reencode(const CmRlibMember *members, size_t member_count,
    CmByteBuf *output)
{
    cm_byte_buf_clear(output);
    assert(cm_rlib_encode_members(output, members, member_count)
        == CM_RLIB_OK);
}

static void test_deterministic_round_trip(void)
{
    TestFixture fixture;
    CmHirExecutableMetadataExpectation expectation;
    CmHirExecutableRlib decoded;
    CmHirExecutableRlibObjectView copied_view;
    CmRlibArchiveView archive_view;
    CmByteBuf first;
    CmByteBuf second;

    test_fixture_init(&fixture);
    test_expectation_init(&fixture, &expectation);
    cm_byte_buf_init(&first);
    cm_byte_buf_init(&second);
    assert(cm_hir_executable_rlib_encode(&fixture.metadata, &first)
        == CM_HIR_EXECUTABLE_RLIB_OK);
    assert(cm_hir_executable_rlib_encode(&fixture.metadata, &second)
        == CM_HIR_EXECUTABLE_RLIB_OK);
    assert(first.len == second.len);
    assert(memcmp(first.data, second.data, first.len) == 0);
    assert(cm_rlib_decode_members(first.data, first.len, &archive_view)
        == CM_RLIB_OK);
    assert(archive_view.member_count == 2u);
    assert(strcmp(archive_view.members[0].name, "cmrustc.rmeta") == 0);
    assert(strcmp(archive_view.members[1].name, "g3_dep.o") == 0);

    cm_hir_executable_rlib_init(&decoded);
    assert(cm_hir_executable_rlib_decode(first.data, first.len,
        &expectation, &decoded) == CM_HIR_EXECUTABLE_RLIB_OK);
    assert(decoded.metadata.owns_storage);
    assert(decoded.metadata.objects[0].object_bytes == NULL);
    assert(decoded.metadata.objects[0].object_bytes_length == 0u);
    assert(decoded.object_count == 1u);
    assert(strcmp(decoded.objects[0].archive_member_name, "g3_dep.o") == 0);
    assert(decoded.objects[0].length == 4u);
    assert(memcmp(decoded.objects[0].data, "OBJ!", 4u) == 0);
    copied_view = decoded.objects[0];
    cm_hir_executable_rlib_destroy(&decoded);
    assert(strcmp(copied_view.archive_member_name, "g3_dep.o") == 0);
    assert(memcmp(copied_view.data, "OBJ!", 4u) == 0);

    fixture.object.archive_member_name = (CmHirExecutableString)S("a.o");
    test_expectation_init(&fixture, &expectation);
    assert(cm_hir_executable_rlib_encode(&fixture.metadata, &first)
        == CM_HIR_EXECUTABLE_RLIB_OK);
    assert(cm_rlib_decode_members(first.data, first.len, &archive_view)
        == CM_RLIB_OK);
    assert(strcmp(archive_view.members[0].name, "a.o") == 0);
    assert(strcmp(archive_view.members[1].name, "cmrustc.rmeta") == 0);
    cm_hir_executable_rlib_init(&decoded);
    assert(cm_hir_executable_rlib_decode(first.data, first.len,
        &expectation, &decoded) == CM_HIR_EXECUTABLE_RLIB_OK);
    cm_hir_executable_rlib_destroy(&decoded);

    cm_byte_buf_destroy(&second);
    cm_byte_buf_destroy(&first);
}

static void test_rejections_and_atomic_output(void)
{
    TestFixture fixture;
    CmHirExecutableMetadataExpectation expectation;
    CmHirExecutableMetadataExpectation wrong_expectation;
    CmHirExecutableRlib output;
    CmRlibArchiveView view;
    CmRlibMember members[3];
    CmByteBuf archive;
    CmByteBuf changed;
    CmByteBuf missing;
    CmByteBuf extra;
    CmByteBuf wrong_length;
    CmByteBuf duplicate;
    unsigned char *saved_crate_name;
    const unsigned char *saved_object;
    size_t metadata_index;
    size_t object_index;
    size_t extra_index;

    test_fixture_init(&fixture);
    test_expectation_init(&fixture, &expectation);
    cm_byte_buf_init(&archive);
    cm_byte_buf_init(&changed);
    cm_byte_buf_init(&missing);
    cm_byte_buf_init(&extra);
    cm_byte_buf_init(&wrong_length);
    cm_byte_buf_init(&duplicate);
    assert(cm_hir_executable_rlib_encode(&fixture.metadata, &archive)
        == CM_HIR_EXECUTABLE_RLIB_OK);
    assert(cm_rlib_decode_members(archive.data, archive.len, &view)
        == CM_RLIB_OK);
    metadata_index = strcmp(view.members[0].name, "cmrustc.rmeta") == 0
        ? 0u : 1u;
    object_index = 1u - metadata_index;

    cm_hir_executable_rlib_init(&output);
    assert(cm_hir_executable_rlib_decode(archive.data, archive.len,
        &expectation, &output) == CM_HIR_EXECUTABLE_RLIB_OK);
    saved_crate_name = output.metadata.crate_name.data;
    saved_object = output.objects[0].data;

    test_copy(&changed, &archive);
    changed.data[0] ^= UINT8_C(1);
    assert(cm_hir_executable_rlib_decode(changed.data, changed.len,
        &expectation, &output) == CM_HIR_EXECUTABLE_RLIB_INVALID_ARCHIVE);
    assert(output.metadata.crate_name.data == saved_crate_name);
    assert(output.objects[0].data == saved_object);

    test_copy(&changed, &archive);
    changed.data[(size_t)(view.members[metadata_index].data - archive.data)]
        ^= UINT8_C(1);
    assert(cm_hir_executable_rlib_decode(changed.data, changed.len,
        &expectation, &output) == CM_HIR_EXECUTABLE_RLIB_INVALID_METADATA);
    assert(output.metadata.crate_name.data == saved_crate_name);

    test_copy(&changed, &archive);
    changed.data[(size_t)(view.members[object_index].data - archive.data)]
        ^= UINT8_C(1);
    assert(cm_hir_executable_rlib_decode(changed.data, changed.len,
        &expectation, &output) == CM_HIR_EXECUTABLE_RLIB_OBJECT_MISMATCH);
    assert(output.objects[0].data == saved_object);

    members[0].name = "cmrustc.rmeta";
    members[0].data = view.members[metadata_index].data;
    members[0].length = view.members[metadata_index].length;
    test_reencode(members, 1u, &missing);
    assert(cm_hir_executable_rlib_decode(missing.data, missing.len,
        &expectation, &output) == CM_HIR_EXECUTABLE_RLIB_MEMBER_MISMATCH);

    members[0].name = "g3_dep.o";
    members[0].data = view.members[object_index].data;
    members[0].length = view.members[object_index].length;
    test_reencode(members, 1u, &missing);
    assert(cm_hir_executable_rlib_decode(missing.data, missing.len,
        &expectation, &output) == CM_HIR_EXECUTABLE_RLIB_MEMBER_MISMATCH);

    members[0].name = "cmrustc.rmeta";
    members[0].data = view.members[metadata_index].data;
    members[0].length = view.members[metadata_index].length;
    members[1].name = "g3_dep.o";
    members[1].data = view.members[object_index].data;
    members[1].length = view.members[object_index].length;
    members[2].name = "zz.extra";
    members[2].data = "x";
    members[2].length = 1u;
    test_reencode(members, 3u, &extra);
    assert(cm_hir_executable_rlib_decode(extra.data, extra.len,
        &expectation, &output) == CM_HIR_EXECUTABLE_RLIB_MEMBER_MISMATCH);

    members[1].length -= 1u;
    test_reencode(members, 2u, &wrong_length);
    assert(cm_hir_executable_rlib_decode(wrong_length.data,
        wrong_length.len, &expectation, &output)
        == CM_HIR_EXECUTABLE_RLIB_OBJECT_MISMATCH);

    test_copy(&duplicate, &extra);
    assert(cm_rlib_decode_members(duplicate.data, duplicate.len, &view)
        == CM_RLIB_OK);
    object_index = strcmp(view.members[0].name, "g3_dep.o") == 0 ? 0u : 1u;
    extra_index = 2u;
    memcpy((unsigned char *)view.members[extra_index].data - 60u,
        (unsigned char *)view.members[object_index].data - 60u, 16u);
    assert(cm_hir_executable_rlib_decode(duplicate.data, duplicate.len,
        &expectation, &output) == CM_HIR_EXECUTABLE_RLIB_INVALID_ARCHIVE);

    wrong_expectation = expectation;
    wrong_expectation.artifact_identity.bytes[0] ^= UINT8_C(1);
    assert(cm_hir_executable_rlib_decode(archive.data, archive.len,
        &wrong_expectation, &output)
        == CM_HIR_EXECUTABLE_RLIB_IDENTITY_MISMATCH);
    assert(output.metadata.crate_name.data == saved_crate_name);
    assert(output.objects[0].data == saved_object);
    assert(output.object_count == 1u);

    cm_hir_executable_rlib_destroy(&output);
    cm_byte_buf_destroy(&duplicate);
    cm_byte_buf_destroy(&wrong_length);
    cm_byte_buf_destroy(&extra);
    cm_byte_buf_destroy(&missing);
    cm_byte_buf_destroy(&changed);
    cm_byte_buf_destroy(&archive);
}

static void test_encode_atomic_and_statuses(void)
{
    TestFixture fixture;
    CmByteBuf output;
    unsigned char *saved_data;
    size_t saved_length;

    test_fixture_init(&fixture);
    cm_byte_buf_init(&output);
    cm_byte_buf_append(&output, "sentinel", 8u);
    saved_data = output.data;
    saved_length = output.len;
    fixture.object.archive_member_name
        = (CmHirExecutableString)S("sixteen-byte-nam");
    assert(fixture.object.archive_member_name.length == 16u);
    assert(cm_hir_executable_rlib_encode(&fixture.metadata, &output)
        == CM_HIR_EXECUTABLE_RLIB_UNSUPPORTED_DESCRIPTOR);
    assert(output.data == saved_data && output.len == saved_length);
    fixture.object.archive_member_name
        = (CmHirExecutableString)S("cmrustc.rmeta");
    assert(cm_hir_executable_rlib_encode(&fixture.metadata, &output)
        == CM_HIR_EXECUTABLE_RLIB_UNSUPPORTED_DESCRIPTOR);
    assert(output.data == saved_data && output.len == saved_length);
    fixture.object.archive_member_name
        = (CmHirExecutableString)S("g3_dep.o");
    fixture.object.object_bytes = "BAD!";
    assert(cm_hir_executable_rlib_encode(&fixture.metadata, &output)
        == CM_HIR_EXECUTABLE_RLIB_UNSUPPORTED_DESCRIPTOR);
    assert(output.data == saved_data && output.len == saved_length);
    assert(memcmp(output.data, "sentinel", 8u) == 0);
    assert(strcmp(cm_hir_executable_rlib_status_name(
        CM_HIR_EXECUTABLE_RLIB_OBJECT_MISMATCH), "object mismatch") == 0);
    assert(strcmp(cm_hir_executable_rlib_status_name(
        (CmHirExecutableRlibStatus)999),
        "unknown executable rlib status") == 0);
    cm_byte_buf_destroy(&output);
}

int main(void)
{
    test_deterministic_round_trip();
    test_rejections_and_atomic_output();
    test_encode_atomic_and_statuses();
    return 0;
}
