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
    CmHirDeclarationItem items[1];
    CmHirDeclarationValue values[1];
    uint32_t parameters[1];
    CmHirDeclarationPredicate predicates[1];
    uint32_t predicate_arguments[1];
    CmHirDeclarationNamespaceEntry namespace_entries[8];
} TestFixture;

typedef struct AliasFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[3];
    CmHirDeclarationType types[2];
    CmHirDeclarationItem items[2];
    CmHirDeclarationNamespaceEntry namespace_entries[8];
} AliasFixture;

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

static uint32_t get_u32(const unsigned char *bytes)
{
    uint32_t value;
    unsigned int index;
    value = UINT32_C(0);
    for (index = 0u; index < 4u; ++index)
        value |= (uint32_t)bytes[index] << (index * 8u);
    return value;
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

static size_t skip_string(const CmByteBuf *bytes, size_t cursor)
{
    uint32_t length;
    assert(cursor <= bytes->len && bytes->len - cursor >= 4u);
    length = get_u32(bytes->data + cursor);
    cursor += 4u;
    assert((size_t)length <= bytes->len - cursor);
    return cursor + (size_t)length;
}

static size_t namespace_record_offset(const CmByteBuf *bytes,
    uint32_t wanted_index)
{
    size_t cursor;
    uint32_t count;
    uint32_t index;
    cursor = section_offset(bytes, "NSPC") + 12u;
    assert(cursor <= bytes->len && bytes->len - cursor >= 4u);
    count = get_u32(bytes->data + cursor);
    cursor += 4u;
    assert(wanted_index < count);
    for (index = 0u; index < count; ++index) {
        size_t record;
        record = cursor;
        assert(cursor <= bytes->len && bytes->len - cursor >= 8u);
        cursor = skip_string(bytes, cursor + 8u);
        assert(cursor <= bytes->len && bytes->len - cursor >= 16u);
        cursor += 16u;
        if (index == wanted_index) return record;
    }
    assert(0);
    return 0u;
}

static size_t type_record_offset(const CmByteBuf *bytes,
    uint32_t wanted_index)
{
    size_t cursor;
    uint32_t count;
    cursor = section_offset(bytes, "TYPE") + 12u;
    assert(cursor <= bytes->len && bytes->len - cursor >= 4u);
    count = get_u32(bytes->data + cursor);
    assert(wanted_index < count);
    /* Every currently supported canonical TYPE record is eight bytes. */
    return cursor + 4u + (size_t)wanted_index * 8u;
}

static size_t item_record_offset(const CmByteBuf *bytes,
    uint32_t wanted_index)
{
    size_t cursor;
    uint32_t count;
    uint32_t index;
    cursor = section_offset(bytes, "ITEM") + 12u;
    assert(cursor <= bytes->len && bytes->len - cursor >= 4u);
    count = get_u32(bytes->data + cursor);
    cursor += 4u;
    assert(wanted_index < count);
    for (index = 0u; index < count; ++index) {
        size_t record;
        uint8_t kind;
        record = cursor;
        assert(cursor <= bytes->len && bytes->len - cursor >= 8u);
        kind = bytes->data[cursor];
        cursor = skip_string(bytes, cursor + 8u);
        assert(cursor <= bytes->len && bytes->len - cursor >= 44u);
        cursor += 44u;
        if (kind == CM_HIR_DECL_ITEM_STRUCT) cursor += 8u;
        else if (kind == CM_HIR_DECL_ITEM_TYPE_ALIAS) cursor += 4u;
        else assert(0);
        assert(cursor <= bytes->len);
        if (index == wanted_index) return record;
    }
    assert(0);
    return 0u;
}

static size_t manifest_family_offset(const CmByteBuf *bytes, uint8_t family)
{
    size_t cursor;
    uint32_t cfg_count;
    uint32_t index;
    cursor = section_offset(bytes, "MANF") + 12u;
    cursor += 4u;
    cursor = skip_string(bytes, cursor);
    cursor = skip_string(bytes, cursor);
    cursor += 1u;
    cursor = skip_string(bytes, cursor);
    cursor = skip_string(bytes, cursor);
    cursor += 1u;
    assert(cursor <= bytes->len && bytes->len - cursor >= 4u);
    cfg_count = get_u32(bytes->data + cursor);
    cursor += 4u;
    for (index = 0u; index < cfg_count; ++index)
        cursor = skip_string(bytes, cursor);
    assert(cursor <= bytes->len && bytes->len - cursor >= 4u
        && get_u32(bytes->data + cursor) == UINT32_C(14));
    cursor += 4u;
    for (index = 0u; index < UINT32_C(14); ++index) {
        assert(cursor <= bytes->len && bytes->len - cursor >= 12u);
        if (bytes->data[cursor] == family) return cursor;
        cursor += 12u;
    }
    assert(0);
    return 0u;
}

static void recompute_family_crc(CmByteBuf *bytes, uint8_t family,
    const char section_tag[4])
{
    size_t family_offset;
    size_t payload_offset;
    uint64_t payload_length;
    family_offset = manifest_family_offset(bytes, family);
    payload_offset = section_offset(bytes, section_tag);
    payload_length = get_u64(bytes->data + payload_offset + 4u);
    assert(payload_length <= (uint64_t)SIZE_MAX);
    put_u32(bytes->data + family_offset + 8u,
        cm_hir_metadata_crc32(bytes->data + payload_offset + 12u,
            (size_t)payload_length));
    recompute_crc(bytes);
}

static void recompute_module_family_crc(CmByteBuf *bytes)
{
    CmByteBuf stream;
    size_t family_offset;
    size_t module_offset;
    size_t namespace_offset;
    uint64_t module_length;
    uint64_t namespace_length;
    family_offset = manifest_family_offset(bytes, UINT8_C(1));
    module_offset = section_offset(bytes, "MODS");
    namespace_offset = section_offset(bytes, "NSPC");
    module_length = get_u64(bytes->data + module_offset + 4u);
    namespace_length = get_u64(bytes->data + namespace_offset + 4u);
    assert(module_length <= (uint64_t)SIZE_MAX
        && namespace_length <= (uint64_t)SIZE_MAX);
    cm_byte_buf_init(&stream);
    cm_byte_buf_append(&stream, bytes->data + module_offset + 12u,
        (size_t)module_length);
    cm_byte_buf_append(&stream, bytes->data + namespace_offset + 12u,
        (size_t)namespace_length);
    put_u32(bytes->data + family_offset + 8u,
        cm_hir_metadata_crc32(stream.data, stream.len));
    cm_byte_buf_destroy(&stream);
    recompute_crc(bytes);
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

static void item_fixture_init(TestFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;
    fixture_init(fixture);
    metadata = &fixture->metadata;

    fixture->items[0].kind = CM_HIR_DECL_ITEM_STRUCT;
    fixture->items[0].owner_module = 2u;
    fixture->items[0].name = (CmHirDeclarationString)S("Packet");
    fixture->items[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[0].source_ordinal = 3u;
    metadata->items = fixture->items;
    metadata->item_count = 1u;

    fixture->namespace_entries[3].owner_module = 2u;
    fixture->namespace_entries[3].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[3].name = fixture->items[0].name;
    fixture->namespace_entries[3].target_kind = CM_HIR_DECL_TARGET_ITEM;
    fixture->namespace_entries[3].target_local = 1u;
    fixture->namespace_entries[3].export_ordinal = 3u;
    fixture->namespace_entries[4].owner_module = 2u;
    fixture->namespace_entries[4].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[4].name =
        (CmHirDeclarationString)S("PacketAlias");
    fixture->namespace_entries[4].target_kind = CM_HIR_DECL_TARGET_ITEM;
    fixture->namespace_entries[4].target_local = 1u;
    fixture->namespace_entries[4].export_ordinal = 4u;
    fixture->namespace_entries[5] = fixture->namespace_entries[3];
    fixture->namespace_entries[5].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[6] = fixture->namespace_entries[4];
    fixture->namespace_entries[6].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[7] = fixture->namespace_entries[5];
    fixture->namespace_entries[7].name = fixture->values[0].name;
    fixture->namespace_entries[7].target_kind = CM_HIR_DECL_TARGET_VALUE;
    fixture->namespace_entries[7].target_local = 1u;
    fixture->namespace_entries[7].export_ordinal = 3u;
    metadata->namespace_count = 8u;
}

static void alias_fixture_init(AliasFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;
    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("dep");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("decl-alias-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;

    fixture->modules[0].name = metadata->crate_name;
    fixture->modules[1].parent_module = 1u;
    fixture->modules[1].name = (CmHirDeclarationString)S("alloc");
    fixture->modules[2].parent_module = 2u;
    fixture->modules[2].name = (CmHirDeclarationString)S("layout");
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 3u;

    fixture->items[0].kind = CM_HIR_DECL_ITEM_TYPE_ALIAS;
    fixture->items[0].owner_module = 3u;
    fixture->items[0].name = (CmHirDeclarationString)S("LayoutErr");
    fixture->items[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[0].source_ordinal = 1u;
    fixture->items[0].alias_target_type = 1u;
    fixture->items[1].kind = CM_HIR_DECL_ITEM_STRUCT;
    fixture->items[1].owner_module = 3u;
    fixture->items[1].name = (CmHirDeclarationString)S("LayoutError");
    fixture->items[1].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[1].source_ordinal = 2u;
    metadata->items = fixture->items;
    metadata->item_count = 2u;

    fixture->types[0].kind = CM_HIR_DECL_TYPE_NAMED_ADT;
    fixture->types[0].item_local = 2u;
    metadata->types = fixture->types;
    metadata->type_count = 1u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[0].name = fixture->modules[1].name;
    fixture->namespace_entries[0].target_kind = CM_HIR_DECL_TARGET_MODULE;
    fixture->namespace_entries[0].target_local = 2u;
    fixture->namespace_entries[0].export_ordinal = 1u;
    fixture->namespace_entries[1].owner_module = 2u;
    fixture->namespace_entries[1].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[1].name = fixture->items[0].name;
    fixture->namespace_entries[1].target_kind = CM_HIR_DECL_TARGET_ITEM;
    fixture->namespace_entries[1].target_local = 1u;
    fixture->namespace_entries[1].export_ordinal = 1u;
    fixture->namespace_entries[2] = fixture->namespace_entries[1];
    fixture->namespace_entries[2].name = fixture->items[1].name;
    fixture->namespace_entries[2].target_local = 2u;
    fixture->namespace_entries[2].export_ordinal = 2u;
    /* `layout` is a complete private module owner and therefore has no
     * fabricated public module binding in NSPC. */
    fixture->namespace_entries[3] = fixture->namespace_entries[1];
    fixture->namespace_entries[3].owner_module = 3u;
    fixture->namespace_entries[3].export_ordinal = 1u;
    fixture->namespace_entries[4] = fixture->namespace_entries[2];
    fixture->namespace_entries[4].owner_module = 3u;
    fixture->namespace_entries[4].export_ordinal = 2u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 5u;
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

static void test_family_count_limits(void)
{
    TestFixture fixture;
    CmHirDeclarationMetadata *metadata;

    assert(CM_HIR_DECL_METADATA_MAX_MODULES == (size_t)4096u);
    assert(CM_HIR_DECL_METADATA_MAX_ITEMS == (size_t)65536u);
    assert(CM_HIR_DECL_METADATA_MAX_NOMINALS == (size_t)65536u);
    assert(CM_HIR_DECL_METADATA_MAX_ASSOCIATED_ITEMS == (size_t)131072u);
    assert(CM_HIR_DECL_METADATA_MAX_GENERICS == (size_t)131072u);
    assert(CM_HIR_DECL_METADATA_MAX_TYPES == (size_t)262144u);
    assert(CM_HIR_DECL_METADATA_MAX_VALUES == (size_t)131072u);
    assert(CM_HIR_DECL_METADATA_MAX_PREDICATES == (size_t)131072u);
    assert(CM_HIR_DECL_METADATA_MAX_IMPLS == (size_t)131072u);
    assert(CM_HIR_DECL_METADATA_MAX_NAMESPACE_ENTRIES == (size_t)131072u);
    assert(CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES == (size_t)131072u);

    fixture_init(&fixture);
    metadata = &fixture.metadata;
#define ASSERT_COUNT_BOUNDARY(field_, maximum_, original_) do { \
        CmHirDeclarationString saved_name_ = metadata->crate_name; \
        metadata->field_ = (maximum_); \
        metadata->crate_name.data = NULL; \
        metadata->crate_name.length = 0u; \
        assert(cm_hir_declaration_metadata_validate(metadata) \
            == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR); \
        metadata->crate_name = saved_name_; \
        metadata->field_ = (original_); \
    } while (0)
#define ASSERT_COUNT_LIMIT(field_, maximum_, original_) do { \
        ASSERT_COUNT_BOUNDARY(field_, maximum_, original_); \
        metadata->field_ = (maximum_) + 1u; \
        assert(cm_hir_declaration_metadata_validate(metadata) \
            == CM_HIR_DECL_METADATA_LIMIT_EXCEEDED); \
        metadata->field_ = (original_); \
    } while (0)
    ASSERT_COUNT_LIMIT(cfg_count, CM_HIR_DECL_METADATA_MAX_CFGS, 2u);
    ASSERT_COUNT_LIMIT(module_count, CM_HIR_DECL_METADATA_MAX_MODULES, 2u);
    ASSERT_COUNT_LIMIT(trait_count, CM_HIR_DECL_METADATA_MAX_NOMINALS, 1u);
    ASSERT_COUNT_LIMIT(generic_count, CM_HIR_DECL_METADATA_MAX_GENERICS, 2u);
    ASSERT_COUNT_LIMIT(type_count, CM_HIR_DECL_METADATA_MAX_TYPES, 3u);
    ASSERT_COUNT_LIMIT(item_count, CM_HIR_DECL_METADATA_MAX_ITEMS, 0u);
    ASSERT_COUNT_LIMIT(value_count, CM_HIR_DECL_METADATA_MAX_VALUES, 1u);
    ASSERT_COUNT_LIMIT(predicate_count, CM_HIR_DECL_METADATA_MAX_PREDICATES,
        1u);
    ASSERT_COUNT_LIMIT(namespace_count,
        CM_HIR_DECL_METADATA_MAX_NAMESPACE_ENTRIES, 4u);
#undef ASSERT_COUNT_LIMIT
#undef ASSERT_COUNT_BOUNDARY

    fixture.values[0].parameter_count =
        (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES + 1u;
    assert(cm_hir_declaration_metadata_validate(metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.values[0].parameter_count = 1u;
    fixture.predicates[0].argument_count =
        (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES + 1u;
    assert(cm_hir_declaration_metadata_validate(metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
}

static void test_item_family(void)
{
    TestFixture first;
    TestFixture second;
    CmHirDeclarationMetadata decoded;
    CmByteBuf encoded;
    CmByteBuf repeated;
    CmByteBuf rebuilt;
    CmByteBuf bad;
    size_t item_section;
    size_t item_record;
    size_t namespace_record;
    size_t family;
    uint64_t item_length;

    item_fixture_init(&first);
    item_fixture_init(&second);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&repeated);
    cm_byte_buf_init(&rebuilt);
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_OK);
    assert(first.namespace_entries[3].target_local
        == first.namespace_entries[4].target_local);
    assert(first.namespace_entries[3].target_local
        == first.namespace_entries[5].target_local
        && first.namespace_entries[5].namespace_kind
            == CM_HIR_DECL_NAMESPACE_VALUE
        && first.namespace_entries[5].target_kind
            == CM_HIR_DECL_TARGET_ITEM
        && first.namespace_entries[4].target_local
            == first.namespace_entries[6].target_local
        && first.namespace_entries[6].namespace_kind
            == CM_HIR_DECL_NAMESPACE_VALUE);
    assert(cm_hir_declaration_metadata_encode(&first.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&second.metadata, &repeated)
        == CM_HIR_DECL_METADATA_OK);
    assert(encoded.len == repeated.len
        && memcmp(encoded.data, repeated.data, encoded.len) == 0);

    item_section = section_offset(&encoded, "ITEM");
    item_length = get_u64(encoded.data + item_section + 4u);
    family = manifest_family_offset(&encoded, UINT8_C(2));
    assert(get_u32(encoded.data + family + 4u) == UINT32_C(1));
    assert(get_u32(encoded.data + family + 8u)
        == cm_hir_metadata_crc32(encoded.data + item_section + 12u,
            (size_t)item_length));

    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(decoded.item_count == 1u
        && decoded.items[0].kind == CM_HIR_DECL_ITEM_STRUCT
        && decoded.items[0].visibility.kind
            == CM_HIR_DECL_VISIBILITY_PUBLIC
        && decoded.namespace_entries[3].target_kind
            == CM_HIR_DECL_TARGET_ITEM
        && decoded.namespace_entries[3].target_local
            == decoded.namespace_entries[4].target_local
        && decoded.namespace_entries[3].target_local
            == decoded.namespace_entries[5].target_local
        && decoded.namespace_entries[5].namespace_kind
            == CM_HIR_DECL_NAMESPACE_VALUE
        && decoded.namespace_entries[5].target_kind
            == CM_HIR_DECL_TARGET_ITEM
        && decoded.namespace_entries[4].target_local
            == decoded.namespace_entries[6].target_local
        && decoded.namespace_entries[6].target_kind
            == CM_HIR_DECL_TARGET_ITEM);
    assert(cm_hir_declaration_metadata_encode(&decoded, &rebuilt)
        == CM_HIR_DECL_METADATA_OK);
    assert(encoded.len == rebuilt.len
        && memcmp(encoded.data, rebuilt.data, encoded.len) == 0);

    item_record = item_section + 12u + 4u;
    bad = copy_bytes(&encoded);
    bad.data[item_record] = UINT8_C(1);
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    bad.data[item_record + 1u] = UINT8_C(1);
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    bad.data[item_record + 18u] = CM_HIR_DECL_VISIBILITY_PRIVATE;
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    bad.data[item_record + 62u] = UINT8_C(2);
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    put_u32(bad.data + item_record + 66u, UINT32_C(1));
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    family = manifest_family_offset(&bad, UINT8_C(2));
    bad.data[family + 8u] ^= UINT8_C(1);
    recompute_crc(&bad);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    namespace_record = namespace_record_offset(&bad, UINT32_C(3));
    bad.data[namespace_record + 5u] = CM_HIR_DECL_TARGET_NOMINAL;
    recompute_module_family_crc(&bad);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    namespace_record = namespace_record_offset(&bad, UINT32_C(5));
    bad.data[namespace_record + 5u] = CM_HIR_DECL_TARGET_VALUE;
    recompute_module_family_crc(&bad);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    first.namespace_entries[3].target_kind = CM_HIR_DECL_TARGET_NOMINAL;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    first.namespace_entries[3].target_kind = CM_HIR_DECL_TARGET_ITEM;
    first.namespace_entries[3].target_local = 2u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    first.namespace_entries[3].target_local = 1u;
    first.namespace_entries[3].export_ordinal = 4u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    first.namespace_entries[3].export_ordinal = 3u;
    first.namespace_entries[5].target_kind = CM_HIR_DECL_TARGET_VALUE;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    first.namespace_entries[5].target_kind = CM_HIR_DECL_TARGET_ITEM;
    first.namespace_entries[5].target_local = 2u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    first.namespace_entries[5].target_local = 1u;
    first.items[0].visibility.kind = CM_HIR_DECL_VISIBILITY_RESTRICTED;
    first.items[0].visibility.restriction_module = 2u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);

    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&rebuilt);
    cm_byte_buf_destroy(&repeated);
    cm_byte_buf_destroy(&encoded);
}

static void test_type_alias_family(void)
{
    AliasFixture first;
    AliasFixture second;
    CmHirDeclarationMetadata decoded;
    CmByteBuf encoded;
    CmByteBuf repeated;
    CmByteBuf rebuilt;
    CmByteBuf bad;
    size_t record;
    size_t payload;

    alias_fixture_init(&first);
    alias_fixture_init(&second);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&repeated);
    cm_byte_buf_init(&rebuilt);
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_OK);
    /* LayoutError is public as a type but exposes no public constructor. */
    assert(first.metadata.namespace_count == 5u);
    assert(cm_hir_declaration_metadata_encode(&first.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&second.metadata, &repeated)
        == CM_HIR_DECL_METADATA_OK);
    assert(encoded.len == repeated.len
        && memcmp(encoded.data, repeated.data, encoded.len) == 0);

    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(decoded.item_count == 2u && decoded.type_count == 1u
        && decoded.items[0].kind == CM_HIR_DECL_ITEM_TYPE_ALIAS
        && decoded.items[0].alias_target_type == 1u
        && decoded.items[1].kind == CM_HIR_DECL_ITEM_STRUCT
        && decoded.items[1].alias_target_type == 0u
        && decoded.types[0].kind == CM_HIR_DECL_TYPE_NAMED_ADT
        && decoded.types[0].item_local == 2u
        && decoded.namespace_entries[1].target_local
            == decoded.namespace_entries[3].target_local
        && decoded.namespace_entries[1].target_local == 1u
        && decoded.namespace_entries[1].namespace_kind
            == CM_HIR_DECL_NAMESPACE_TYPE
        && decoded.namespace_entries[3].namespace_kind
            == CM_HIR_DECL_NAMESPACE_TYPE);
    assert(cm_hir_declaration_metadata_encode(&decoded, &rebuilt)
        == CM_HIR_DECL_METADATA_OK);
    assert(encoded.len == rebuilt.len
        && memcmp(encoded.data, rebuilt.data, encoded.len) == 0);

    /* A STRUCT may instead have an exact defining TYPE/VALUE constructor mate. */
    first.namespace_entries[6] = first.namespace_entries[4];
    first.namespace_entries[6].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    first.namespace_entries[5] = first.namespace_entries[4];
    first.namespace_entries[4] = first.namespace_entries[3];
    first.namespace_entries[3] = first.namespace_entries[2];
    first.namespace_entries[3].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    first.metadata.namespace_count = 7u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_OK);
    alias_fixture_init(&first);

    /* Publishing the child module is optional, but any such binding must be
     * its exact parent/name/local identity. */
    first.namespace_entries[5] = first.namespace_entries[4];
    first.namespace_entries[4] = first.namespace_entries[3];
    first.namespace_entries[3] = first.namespace_entries[1];
    first.namespace_entries[3].name = first.modules[2].name;
    first.namespace_entries[3].target_kind = CM_HIR_DECL_TARGET_MODULE;
    first.namespace_entries[3].target_local = 3u;
    first.namespace_entries[3].export_ordinal = 3u;
    first.metadata.namespace_count = 6u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_OK);
    first.namespace_entries[3].name = (CmHirDeclarationString)S("forged");
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    alias_fixture_init(&first);

    first.items[0].alias_target_type = 0u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    alias_fixture_init(&first);
    first.items[0].alias_target_type = 2u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    alias_fixture_init(&first);
    first.items[1].alias_target_type = 1u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    alias_fixture_init(&first);
    first.types[0].item_local = 0u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    alias_fixture_init(&first);
    first.types[0].item_local = 3u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    alias_fixture_init(&first);
    /* Alias-directed named types (and thus alias cycles) are not representable. */
    first.types[0].item_local = 1u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    alias_fixture_init(&first);
    first.types[0].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    first.types[0].primitive = CM_HIR_DECL_PRIMITIVE_UNIT;
    first.types[0].item_local = 0u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);

    alias_fixture_init(&first);
    first.types[1] = first.types[0];
    first.types[0].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    first.types[0].primitive = CM_HIR_DECL_PRIMITIVE_UNIT;
    first.types[0].item_local = 0u;
    first.items[0].alias_target_type = 2u;
    first.metadata.type_count = 2u;
    /* The otherwise-canonical UNIT record is unused and must be rejected. */
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    first.types[0] = first.types[1];
    first.items[0].alias_target_type = 1u;
    /* Duplicate NAMED_ADT keys violate canonical TYPE identity. */
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);

    alias_fixture_init(&first);
    first.namespace_entries[3].target_local = 2u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    alias_fixture_init(&first);
    first.namespace_entries[5] = first.namespace_entries[3];
    first.namespace_entries[5].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    first.metadata.namespace_count = 6u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);

    /* Corrupt-but-checksummed alias target is rejected transactionally. */
    bad = copy_bytes(&encoded);
    record = item_record_offset(&bad, UINT32_C(0));
    payload = skip_string(&bad, record + 8u) + 44u;
    put_u32(bad.data + payload, UINT32_C(0));
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    /* A checksummed public module binding cannot be retargeted to a private
     * non-child module. */
    bad = copy_bytes(&encoded);
    record = namespace_record_offset(&bad, UINT32_C(0));
    payload = skip_string(&bad, record + 8u);
    put_u32(bad.data + payload, UINT32_C(3));
    recompute_module_family_crc(&bad);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    /* A NAMED_ADT may not target the alias ITEM, even on valid wire shape. */
    bad = copy_bytes(&encoded);
    record = type_record_offset(&bad, UINT32_C(0));
    put_u32(bad.data + record + 4u, UINT32_C(1));
    recompute_crc(&bad);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    record = item_record_offset(&bad, UINT32_C(0));
    bad.data[record] = UINT8_C(4);
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&rebuilt);
    cm_byte_buf_destroy(&repeated);
    cm_byte_buf_destroy(&encoded);
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
    test_family_count_limits();
    test_item_family();
    test_type_alias_family();
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
    offset = section_offset(&bad, "MODS");
    put_u32(bad.data + offset + 12u,
        (uint32_t)CM_HIR_DECL_METADATA_MAX_MODULES + 1u);
    recompute_crc(&bad);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&bytes1);
    offset = section_offset(&bad, "NOMD");
    put_u32(bad.data + offset + 12u,
        (uint32_t)CM_HIR_DECL_METADATA_MAX_NOMINALS + 1u);
    recompute_crc(&bad);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&bytes1);
    offset = section_offset(&bad, "TYPE");
    put_u32(bad.data + offset + 12u,
        (uint32_t)CM_HIR_DECL_METADATA_MAX_TYPES + 1u);
    recompute_crc(&bad);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&bytes1);
    offset = section_offset(&bad, "PRED");
    put_u32(bad.data + offset + 12u + 32u,
        (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES + 1u);
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
