#include "cm/hir/declaration_metadata.h"
#include "metadata_codec.h"

#include <assert.h>
#include <string.h>

#define S(text) { (unsigned char *)(text), sizeof(text) - 1u }
#define HEADER_SIZE 40u
#define CRC_OFFSET 32u

typedef struct TestFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationString cfgs[2];
    CmHirDeclarationModule modules[2];
    CmHirDeclarationTrait traits[1];
    CmHirDeclarationGeneric generics[2];
    CmHirDeclarationType types[4];
    CmHirDeclarationValue values[1];
    uint32_t parameters[1];
    CmHirDeclarationPredicate predicates[1];
    uint32_t predicate_arguments[1];
    CmHirDeclarationNamespaceEntry namespace_entries[4];
} TestFixture;

static void put_u16(unsigned char *bytes, uint16_t value)
{
    bytes[0] = (unsigned char)(value & UINT16_C(0xff));
    bytes[1] = (unsigned char)((value >> 8u) & UINT16_C(0xff));
}

static void put_u32(unsigned char *bytes, uint32_t value)
{
    unsigned int index;
    for (index = 0u; index < 4u; ++index) {
        bytes[index] = (unsigned char)(value & UINT32_C(0xff));
        value >>= 8u;
    }
}

static uint64_t get_u64(const unsigned char *bytes)
{
    uint64_t value;
    unsigned int index;
    value = UINT64_C(0);
    for (index = 0u; index < 8u; ++index)
        value |= (uint64_t)bytes[index] << (index * 8u);
    return value;
}

static void recompute_crc(CmByteBuf *bytes)
{
    uint32_t crc;
    crc = cm_hir_metadata_crc32(bytes->data + HEADER_SIZE,
        bytes->len - HEADER_SIZE);
    put_u32(bytes->data + CRC_OFFSET, crc);
}

static size_t section_offset(const CmByteBuf *bytes, const char tag[4])
{
    size_t cursor;
    cursor = HEADER_SIZE;
    while (cursor + 12u <= bytes->len) {
        uint64_t length;
        if (memcmp(bytes->data + cursor, tag, 4u) == 0) return cursor;
        length = get_u64(bytes->data + cursor + 4u);
        assert(length <= (uint64_t)(bytes->len - cursor - 12u));
        cursor += 12u + (size_t)length;
    }
    assert(0);
    return 0u;
}

static CmByteBuf copy_bytes(const CmByteBuf *source)
{
    CmByteBuf copy;
    cm_byte_buf_init(&copy);
    cm_byte_buf_append(&copy, source->data, source->len);
    return copy;
}

static void fixture_init(TestFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;
    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("dep");
    metadata->crate_disambiguator = (CmHirDeclarationString)S("decl-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;
    fixture->cfgs[0] = (CmHirDeclarationString)S("target_arch=x86_64");
    fixture->cfgs[1] = (CmHirDeclarationString)S("target_pointer_width=64");
    metadata->cfgs = fixture->cfgs;
    metadata->cfg_count = 2u;

    fixture->modules[0].name = metadata->crate_name;
    fixture->modules[1].parent_module = 1u;
    fixture->modules[1].name = (CmHirDeclarationString)S("api");
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 2u;

    fixture->traits[0].owner_module = 2u;
    fixture->traits[0].name = (CmHirDeclarationString)S("Marker");
    fixture->traits[0].source_ordinal = 1u;
    fixture->traits[0].generic_start = 1u;
    fixture->traits[0].generic_count = 1u;
    metadata->traits = fixture->traits;
    metadata->trait_count = 1u;

    fixture->generics[0].owner_kind = CM_HIR_DECL_GENERIC_NOMINAL;
    fixture->generics[0].owner_local = 1u;
    fixture->generics[0].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[0].is_relaxed_sized = 1u;
    fixture->generics[0].name = (CmHirDeclarationString)S("A");
    fixture->generics[1].owner_kind = CM_HIR_DECL_GENERIC_VALUE;
    fixture->generics[1].owner_local = 1u;
    fixture->generics[1].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[1].name = (CmHirDeclarationString)S("T");
    metadata->generics = fixture->generics;
    metadata->generic_count = 2u;

    fixture->types[0].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[0].primitive = CM_HIR_DECL_PRIMITIVE_UNIT;
    fixture->types[1].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[1].primitive = CM_HIR_DECL_PRIMITIVE_U8;
    fixture->types[2].kind = CM_HIR_DECL_TYPE_GENERIC;
    fixture->types[2].generic_local = 2u;
    metadata->types = fixture->types;
    metadata->type_count = 3u;

    fixture->parameters[0] = 3u;
    fixture->values[0].owner_module = 2u;
    fixture->values[0].name = (CmHirDeclarationString)S("apply");
    fixture->values[0].source_ordinal = 2u;
    fixture->values[0].generic_start = 2u;
    fixture->values[0].generic_count = 1u;
    fixture->values[0].predicate_start = 1u;
    fixture->values[0].predicate_count = 1u;
    fixture->values[0].parameter_count = 1u;
    fixture->values[0].parameter_types = fixture->parameters;
    fixture->values[0].return_type = 1u;
    fixture->values[0].has_body = 1u;
    metadata->values = fixture->values;
    metadata->value_count = 1u;

    fixture->predicate_arguments[0] = 2u;
    fixture->predicates[0].owner_value = 1u;
    fixture->predicates[0].subject_type = 3u;
    fixture->predicates[0].trait_local = 1u;
    fixture->predicates[0].argument_count = 1u;
    fixture->predicates[0].argument_types = fixture->predicate_arguments;
    metadata->predicates = fixture->predicates;
    metadata->predicate_count = 1u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind = CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[0].name = (CmHirDeclarationString)S("api");
    fixture->namespace_entries[0].target_kind = CM_HIR_DECL_TARGET_MODULE;
    fixture->namespace_entries[0].target_local = 2u;
    fixture->namespace_entries[0].export_ordinal = 1u;
    fixture->namespace_entries[1].owner_module = 2u;
    fixture->namespace_entries[1].namespace_kind = CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[1].name = (CmHirDeclarationString)S("Alias");
    fixture->namespace_entries[1].target_kind = CM_HIR_DECL_TARGET_NOMINAL;
    fixture->namespace_entries[1].target_local = 1u;
    fixture->namespace_entries[1].export_ordinal = 2u;
    fixture->namespace_entries[2].owner_module = 2u;
    fixture->namespace_entries[2].namespace_kind = CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[2].name = fixture->traits[0].name;
    fixture->namespace_entries[2].target_kind = CM_HIR_DECL_TARGET_NOMINAL;
    fixture->namespace_entries[2].target_local = 1u;
    fixture->namespace_entries[2].export_ordinal = 1u;
    fixture->namespace_entries[3].owner_module = 2u;
    fixture->namespace_entries[3].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[3].name = fixture->values[0].name;
    fixture->namespace_entries[3].target_kind = CM_HIR_DECL_TARGET_VALUE;
    fixture->namespace_entries[3].target_local = 1u;
    fixture->namespace_entries[3].export_ordinal = 3u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 4u;
}

static void assert_failed_transaction(const CmByteBuf *bytes)
{
    CmHirDeclarationMetadata sentinel;
    memset(&sentinel, 0, sizeof(sentinel));
    sentinel.root_module = UINT32_C(77);
    assert(cm_hir_declaration_metadata_decode(bytes->data, bytes->len,
        &sentinel) != CM_HIR_DECL_METADATA_OK);
    assert(sentinel.root_module == UINT32_C(77));
}

int main(void)
{
    TestFixture first;
    TestFixture second;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMetadata zero_decoded;
    CmByteBuf bytes1;
    CmByteBuf bytes2;
    CmByteBuf bytes3;
    CmByteBuf bad;
    size_t offset;

    fixture_init(&first);
    fixture_init(&second);
    cm_byte_buf_init(&bytes1);
    cm_byte_buf_init(&bytes2);
    cm_byte_buf_init(&bytes3);
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&first.metadata, &bytes1)
        == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&second.metadata, &bytes2)
        == CM_HIR_DECL_METADATA_OK);
    assert(bytes1.len != 0u && bytes1.len == bytes2.len
        && memcmp(bytes1.data, bytes2.data, bytes1.len) == 0);

    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(bytes1.data, bytes1.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(decoded.trait_count == 1u && decoded.generic_count == 2u
        && decoded.generics[0].is_relaxed_sized == 1u
        && decoded.types[0].primitive == CM_HIR_DECL_PRIMITIVE_UNIT
        && decoded.values[0].return_type == 1u
        && decoded.namespace_entries[1].target_local
            == decoded.namespace_entries[2].target_local);
    assert(cm_hir_declaration_metadata_encode(&decoded, &bytes3)
        == CM_HIR_DECL_METADATA_OK);
    assert(bytes1.len == bytes3.len
        && memcmp(bytes1.data, bytes3.data, bytes1.len) == 0);

    bad = copy_bytes(&bytes1);
    bad.data[bad.len - 1u] ^= 1u;
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&bytes1);
    bad.len -= 1u;
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&bytes1);
    offset = section_offset(&bad, "NOMD");
    bad.data[offset] = (unsigned char)'X';
    recompute_crc(&bad);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&bytes1);
    put_u16(bad.data + 8u, UINT16_C(2));
    assert(cm_hir_declaration_metadata_decode(bad.data, bad.len, &decoded)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&bytes1);
    offset = section_offset(&bad, "GPAR");
    bad.data[offset + 12u + 17u] = 2u;
    recompute_crc(&bad);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&bytes1);
    offset = section_offset(&bad, "PRED");
    put_u32(bad.data + offset + 12u + 32u, UINT32_C(0));
    recompute_crc(&bad);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    first.predicates[0].argument_count = 0u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    first.predicates[0].argument_count = 1u;
    first.predicates[0].trait_local = 2u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    first.predicates[0].trait_local = 1u;
    first.namespace_entries[2].target_local = 2u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    first.namespace_entries[2].target_local = 1u;
    first.namespace_entries[0].name = (CmHirDeclarationString)S("missing");
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    first.namespace_entries[0].name = first.modules[1].name;
    first.namespace_entries[2].name = (CmHirDeclarationString)S("OnlyAlias");
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    first.namespace_entries[2].name = first.traits[0].name;
    first.namespace_entries[3].name = (CmHirDeclarationString)S("OnlyAlias");
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    first.namespace_entries[3].name = first.values[0].name;
    first.generics[0].is_relaxed_sized = 2u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    first.generics[0].is_relaxed_sized = 1u;
    first.values[0].return_type = 0u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    first.values[0].return_type = 1u;
    first.types[0].primitive = 0u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    first.types[0].primitive = CM_HIR_DECL_PRIMITIVE_UNIT;

    first.types[2].generic_local = 1u;
    first.types[3].kind = CM_HIR_DECL_TYPE_GENERIC;
    first.types[3].generic_local = 2u;
    first.parameters[0] = 4u;
    first.predicates[0].subject_type = 4u;
    first.metadata.type_count = 4u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    first.metadata.type_count = 3u;
    first.types[2].generic_local = 2u;
    first.parameters[0] = 3u;
    first.predicates[0].subject_type = 3u;

    first.values[0].parameter_count = 0u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    first.values[0].parameter_types = NULL;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&bad);
    assert(cm_hir_declaration_metadata_encode(&first.metadata, &bad)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&zero_decoded);
    assert(cm_hir_declaration_metadata_decode(bad.data, bad.len,
        &zero_decoded) == CM_HIR_DECL_METADATA_OK);
    assert(zero_decoded.values[0].parameter_count == 0u
        && zero_decoded.values[0].parameter_types == NULL);
    cm_hir_declaration_metadata_destroy(&zero_decoded);
    cm_byte_buf_destroy(&bad);
    first.values[0].parameter_count = 1u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    first.values[0].parameter_types = first.parameters;

    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&bytes3);
    cm_byte_buf_destroy(&bytes2);
    cm_byte_buf_destroy(&bytes1);
    return 0;
}
