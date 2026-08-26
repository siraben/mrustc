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

typedef struct StructuralFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[2];
    CmHirDeclarationTrait traits[1];
    CmHirDeclarationGeneric generics[3];
    CmHirDeclarationType types[6];
    uint32_t application_arguments[1];
    CmHirDeclarationItem items[1];
    CmHirDeclarationValue values[1];
    uint32_t parameters[2];
    CmHirDeclarationPredicate predicates[1];
    uint32_t predicate_arguments[1];
    CmHirDeclarationNamespaceEntry namespace_entries[5];
} StructuralFixture;

typedef struct EnumFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationItem items[2];
    CmHirDeclarationVariant variants[128];
    unsigned char variant_names[128][4];
    CmHirDeclarationType types[1];
    CmHirDeclarationNamespaceEntry namespace_entries[4];
} EnumFixture;

typedef struct ConstFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationTrait traits[1];
    CmHirDeclarationGeneric generics[1];
    CmHirDeclarationType types[2];
    uint32_t application_arguments[1];
    CmHirDeclarationItem items[1];
    CmHirDeclarationValue values[1];
    uint32_t parameters[1];
    CmHirDeclarationNamespaceEntry namespace_entries[4];
} ConstFixture;

typedef struct AggregateFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[4];
    CmHirDeclarationGeneric generics[2];
    CmHirDeclarationType types[6];
    uint32_t application_arguments[1];
    uint32_t union_application_arguments[1];
    CmHirDeclarationItem items[3];
    CmHirDeclarationField manually_drop_fields[1];
    CmHirDeclarationField maybe_uninit_fields[2];
    CmHirDeclarationField assume_fields[4];
    CmHirDeclarationNamespaceEntry namespace_entries[6];
} AggregateFixture;

typedef struct LayoutFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationType types[3];
    CmHirDeclarationItem items[3];
    CmHirDeclarationField alignment_fields[1];
    CmHirDeclarationField layout_fields[2];
    CmHirDeclarationVariant alignment_variants[2];
    CmHirDeclarationNamespaceEntry namespace_entries[1];
} LayoutFixture;

typedef struct StaticFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationType types[6];
    uint32_t tuple_elements[3];
    CmHirDeclarationGeneric generics[1];
    CmHirDeclarationItem items[1];
    CmHirDeclarationValue values[1];
    CmHirDeclarationNamespaceEntry namespace_entries[3];
} StaticFixture;

typedef struct GenericEnumFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[3];
    CmHirDeclarationGeneric generics[3];
    CmHirDeclarationType types[6];
    uint32_t application_arguments[2];
    CmHirDeclarationItem items[3];
    CmHirDeclarationField aggregate_fields[4];
    CmHirDeclarationVariant variants[4];
    CmHirDeclarationVariantField variant_fields[3];
    CmHirDeclarationValue values[1];
    CmHirDeclarationNamespaceEntry namespace_entries[16];
} GenericEnumFixture;

typedef struct PrimitiveNamespaceFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationNamespaceEntry namespace_entries[2];
} PrimitiveNamespaceFixture;

typedef struct AssociatedMethodFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationTrait traits[2];
    CmHirDeclarationAssociatedItem associated_items[2];
    uint32_t allocate_parameters[2];
    uint32_t fallback_parameters[1];
    CmHirDeclarationGeneric generics[1];
    CmHirDeclarationType types[6];
    CmHirDeclarationPredicate predicates[1];
    CmHirDeclarationNamespaceEntry namespace_entries[2];
} AssociatedMethodFixture;

typedef struct AnyFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationTrait traits[1];
    CmHirDeclarationAssociatedItem associated_items[1];
    uint32_t parameters[1];
    CmHirDeclarationType types[3];
    CmHirDeclarationItem items[1];
    CmHirDeclarationOutlivesPredicate outlives[1];
    CmHirDeclarationNamespaceEntry namespace_entries[3];
} AnyFixture;

typedef struct TypeIdFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationString cfgs[2];
    CmHirDeclarationModule modules[1];
    CmHirDeclarationType types[4];
    CmHirDeclarationItem items[1];
    CmHirDeclarationField fields[1];
    CmHirDeclarationNamespaceEntry namespace_entries[1];
} TypeIdFixture;

typedef struct TypeNameFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationGeneric generics[1];
    CmHirDeclarationType types[4];
    CmHirDeclarationValue values[2];
    uint32_t parameters[2];
    CmHirDeclarationNamespaceEntry namespace_entries[2];
} TypeNameFixture;

typedef struct FromMutFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationGeneric generics[1];
    CmHirDeclarationType types[6];
    CmHirDeclarationValue values[2];
    uint32_t parameters[2];
    CmHirDeclarationNamespaceEntry namespace_entries[2];
} FromMutFixture;

typedef struct UnitFunctionFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationType types[2];
    CmHirDeclarationValue values[1];
    uint32_t parameters[1];
    CmHirDeclarationNamespaceEntry namespace_entries[2];
} UnitFunctionFixture;

typedef struct IntoIterFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[3];
    CmHirDeclarationTrait traits[1];
    CmHirDeclarationAssociatedItem associated_items[1];
    uint32_t method_parameters[2];
    CmHirDeclarationGeneric generics[4];
    CmHirDeclarationType types[11];
    uint32_t maybe_arguments[1];
    uint32_t polymorphic_arguments[1];
    CmHirDeclarationItem items[4];
    CmHirDeclarationField index_fields[2];
    CmHirDeclarationField maybe_fields[1];
    CmHirDeclarationField into_iter_fields[1];
    CmHirDeclarationField polymorphic_fields[2];
    CmHirDeclarationPredicate predicates[1];
    CmHirDeclarationNamespaceEntry namespace_entries[3];
} IntoIterFixture;

typedef struct FromFnFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationTrait traits[3];
    CmHirDeclarationSupertrait supertraits[1];
    uint32_t supertrait_arguments[1];
    CmHirDeclarationAssociatedItem associated_items[3];
    uint32_t call_mut_parameters[2];
    uint32_t call_once_parameters[2];
    CmHirDeclarationGeneric generics[5];
    CmHirDeclarationType types[13];
    uint32_t tuple_elements[1];
    uint32_t projection_mut_arguments[1];
    uint32_t projection_once_arguments[1];
    CmHirDeclarationValue values[1];
    uint32_t value_parameters[1];
    CmHirDeclarationPredicate predicates[3];
    uint32_t value_predicate_arguments[1];
    CmHirDeclarationPredicateEquality value_equalities[1];
    CmHirDeclarationNamespaceEntry namespace_entries[5];
} FromFnFixture;

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
    uint32_t index;
    cursor = section_offset(bytes, "TYPE") + 12u;
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
        if (kind == CM_HIR_DECL_TYPE_PRIMITIVE
            || kind == CM_HIR_DECL_TYPE_GENERIC
            || kind == CM_HIR_DECL_TYPE_NAMED_ADT
            || kind == CM_HIR_DECL_TYPE_SELF
            || kind == CM_HIR_DECL_TYPE_SLICE) {
            cursor += 8u;
        } else if (kind == CM_HIR_DECL_TYPE_RAW_POINTER) {
            cursor += 12u;
        } else if (kind == CM_HIR_DECL_TYPE_REFERENCE) {
            cursor += 24u;
        } else if (kind == CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION) {
            uint32_t argument_count;
            assert(cursor <= bytes->len && bytes->len - cursor >= 12u);
            argument_count = get_u32(bytes->data + cursor + 8u);
            assert((size_t)argument_count <= (bytes->len - cursor - 12u) / 4u);
            cursor += 12u + (size_t)argument_count * 4u;
        } else if (kind == CM_HIR_DECL_TYPE_TUPLE) {
            uint32_t element_count;
            assert(cursor <= bytes->len && bytes->len - cursor >= 8u);
            element_count = get_u32(bytes->data + cursor + 4u);
            assert((size_t)element_count
                <= (bytes->len - cursor - 8u) / 4u);
            cursor += 8u + (size_t)element_count * 4u;
        } else if (kind == CM_HIR_DECL_TYPE_ARRAY) {
            cursor += 28u;
        } else if (kind == CM_HIR_DECL_TYPE_PROJECTION) {
            uint32_t argument_count;
            assert(cursor <= bytes->len && bytes->len - cursor >= 20u);
            argument_count = get_u32(bytes->data + cursor + 16u);
            assert((size_t)argument_count
                <= (bytes->len - cursor - 20u) / 4u);
            cursor += 20u + (size_t)argument_count * 4u;
        } else {
            assert(0);
        }
        assert(cursor <= bytes->len);
        if (index == wanted_index) return record;
    }
    assert(0);
    return 0u;
}

static size_t generic_record_offset(const CmByteBuf *bytes,
    uint32_t wanted_index)
{
    size_t cursor;
    uint32_t count;
    uint32_t index;
    cursor = section_offset(bytes, "GPAR") + 12u;
    assert(cursor <= bytes->len && bytes->len - cursor >= 4u);
    count = get_u32(bytes->data + cursor);
    cursor += 4u;
    assert(wanted_index < count);
    for (index = 0u; index < count; ++index) {
        size_t record = cursor;
        assert(cursor <= bytes->len && bytes->len - cursor >= 16u);
        cursor = skip_string(bytes, cursor + 16u);
        assert(cursor <= bytes->len && bytes->len - cursor >= 12u);
        cursor += 12u;
        if (index == wanted_index) return record;
    }
    assert(0);
    return 0u;
}

static size_t value_record_offset(const CmByteBuf *bytes,
    uint32_t wanted_index)
{
    size_t cursor;
    uint32_t count;
    uint32_t index;
    cursor = section_offset(bytes, "VALU") + 12u;
    assert(cursor <= bytes->len && bytes->len - cursor >= 4u);
    count = get_u32(bytes->data + cursor);
    cursor += 4u;
    assert(wanted_index < count);
    for (index = 0u; index < count; ++index) {
        size_t record = cursor;
        uint8_t kind;
        uint32_t parameter_count;
        assert(cursor <= bytes->len && bytes->len - cursor >= 8u);
        kind = bytes->data[cursor];
        cursor = skip_string(bytes, cursor + 8u);
        assert(cursor <= bytes->len && bytes->len - cursor >= 44u);
        cursor += 44u;
        if (kind == CM_HIR_DECL_VALUE_FUNCTION) {
            assert(cursor <= bytes->len && bytes->len - cursor >= 16u);
            parameter_count = get_u32(bytes->data + cursor + 4u);
            assert((size_t)parameter_count
                <= (bytes->len - cursor - 16u) / 4u);
            cursor += 16u + (size_t)parameter_count * 4u;
        } else if (kind == CM_HIR_DECL_VALUE_CONST
                || kind == CM_HIR_DECL_VALUE_STATIC) {
            cursor += 8u;
        } else {
            assert(0);
        }
        assert(cursor <= bytes->len);
        if (index == wanted_index) return record;
    }
    assert(0);
    return 0u;
}

static size_t associated_record_offset(const CmByteBuf *bytes,
    uint32_t wanted_index)
{
    size_t cursor;
    uint32_t count;
    uint32_t index;
    cursor = section_offset(bytes, "AITM") + 12u;
    assert(cursor <= bytes->len && bytes->len - cursor >= 4u);
    count = get_u32(bytes->data + cursor);
    cursor += 4u;
    assert(wanted_index < count);
    for (index = 0u; index < count; ++index) {
        size_t record = cursor;
        uint8_t flags;
        uint8_t kind;
        uint32_t parameter_count;
        assert(cursor <= bytes->len && bytes->len - cursor >= 12u);
        kind = bytes->data[cursor];
        cursor = skip_string(bytes, cursor + 12u);
        assert(cursor <= bytes->len && bytes->len - cursor >= 56u);
        flags = bytes->data[cursor + 13u];
        cursor += 48u;
        if (kind == CM_HIR_DECL_ASSOCIATED_METHOD) {
            parameter_count = get_u32(bytes->data + cursor + 4u);
            assert((size_t)parameter_count
                <= (bytes->len - cursor - 12u) / 4u);
            cursor += 12u + (size_t)parameter_count * 4u;
            cursor = skip_string(bytes, cursor);
            assert(cursor <= bytes->len && bytes->len - cursor >= 8u);
            cursor += 8u;
        } else {
            assert(kind == CM_HIR_DECL_ASSOCIATED_TYPE);
        }
        if ((flags & CM_HIR_DECL_ASSOCIATED_HAS_LANG_ITEM) != 0u)
            cursor = skip_string(bytes, cursor);
        if (index == wanted_index) return record;
    }
    assert(0);
    return 0u;
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
        if (kind == CM_HIR_DECL_ITEM_STRUCT
                || kind == CM_HIR_DECL_ITEM_UNION) {
            uint32_t child;
            uint32_t field_count;
            uint16_t flags;
            assert(cursor <= bytes->len && bytes->len - cursor >= 8u);
            flags = (uint16_t)bytes->data[cursor + 2u]
                | (uint16_t)((uint16_t)bytes->data[cursor + 3u] << 8u);
            field_count = get_u32(bytes->data + cursor + 4u);
            cursor += 8u;
            for (child = 0u; child < field_count; ++child) {
                cursor = skip_string(bytes, cursor);
                assert(cursor <= bytes->len && bytes->len - cursor >= 16u);
                cursor += 16u;
            }
            if ((flags & CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM) != 0u)
                cursor = skip_string(bytes, cursor);
            if ((flags & CM_HIR_DECL_AGGREGATE_HAS_DIAGNOSTIC_ITEM) != 0u)
                cursor = skip_string(bytes, cursor);
        }
        else if (kind == CM_HIR_DECL_ITEM_ENUM) {
            uint32_t child;
            uint32_t variant_count;
            uint8_t representation;
            uint8_t enum_flags;
            assert(cursor <= bytes->len && bytes->len - cursor >= 8u);
            representation = bytes->data[cursor];
            enum_flags = bytes->data[cursor + 1u];
            variant_count = get_u32(bytes->data + cursor + 4u);
            cursor += 8u;
            for (child = 0u; child < variant_count; ++child) {
                uint8_t variant_kind;
                uint16_t variant_flags;
                assert(cursor <= bytes->len && bytes->len - cursor >= 4u);
                variant_kind = bytes->data[cursor];
                variant_flags = (uint16_t)bytes->data[cursor + 2u]
                    | (uint16_t)((uint16_t)bytes->data[cursor + 3u] << 8u);
                cursor = skip_string(bytes, cursor + 4u);
                assert(cursor <= bytes->len && bytes->len - cursor >= 20u);
                cursor += 20u;
                if (variant_kind == CM_HIR_DECL_VARIANT_TUPLE) {
                    uint32_t field_count;
                    assert(cursor <= bytes->len
                        && bytes->len - cursor >= 4u);
                    field_count = get_u32(bytes->data + cursor);
                    assert((size_t)field_count
                        <= (bytes->len - cursor - 4u) / 8u);
                    cursor += 4u + (size_t)field_count * 8u;
                }
                if ((variant_flags
                        & CM_HIR_DECL_VARIANT_HAS_LANG_ITEM) != 0u)
                    cursor = skip_string(bytes, cursor);
            }
            if (representation == CM_HIR_DECL_ENUM_REPR_RUST)
                cursor = skip_string(bytes, cursor);
            if ((enum_flags & CM_HIR_DECL_ENUM_HAS_LANG_ITEM) != 0u)
                cursor = skip_string(bytes, cursor);
        } else if (kind == CM_HIR_DECL_ITEM_TYPE_ALIAS) cursor += 4u;
        else assert(0);
        assert(cursor <= bytes->len);
        if (index == wanted_index) return record;
    }
    assert(0);
    return 0u;
}

static size_t enum_variant_record_offset(const CmByteBuf *bytes,
    uint32_t item_index, uint32_t wanted_variant)
{
    size_t cursor;
    size_t item_record;
    uint32_t count;
    uint32_t index;
    item_record = item_record_offset(bytes, item_index);
    assert(bytes->data[item_record] == CM_HIR_DECL_ITEM_ENUM);
    cursor = skip_string(bytes, item_record + 8u) + 44u;
    assert(cursor <= bytes->len && bytes->len - cursor >= 8u);
    count = get_u32(bytes->data + cursor + 4u);
    cursor += 8u;
    assert(wanted_variant < count);
    for (index = 0u; index < count; ++index) {
        size_t record = cursor;
        uint8_t variant_kind;
        uint16_t variant_flags;
        assert(cursor <= bytes->len && bytes->len - cursor >= 4u);
        variant_kind = bytes->data[cursor];
        variant_flags = (uint16_t)bytes->data[cursor + 2u]
            | (uint16_t)((uint16_t)bytes->data[cursor + 3u] << 8u);
        cursor = skip_string(bytes, cursor + 4u);
        assert(cursor <= bytes->len && bytes->len - cursor >= 20u);
        cursor += 20u;
        if (variant_kind == CM_HIR_DECL_VARIANT_TUPLE) {
            uint32_t field_count;
            assert(cursor <= bytes->len && bytes->len - cursor >= 4u);
            field_count = get_u32(bytes->data + cursor);
            assert((size_t)field_count
                <= (bytes->len - cursor - 4u) / 8u);
            cursor += 4u + (size_t)field_count * 8u;
        }
        if ((variant_flags & CM_HIR_DECL_VARIANT_HAS_LANG_ITEM) != 0u)
            cursor = skip_string(bytes, cursor);
        if (index == wanted_variant) return record;
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
    fixture->traits[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
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
    fixture->values[0].kind = CM_HIR_DECL_VALUE_FUNCTION;
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
    fixture->namespace_entries[3].export_ordinal = 2u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 4u;
}

static void primitive_namespace_fixture_init(
    PrimitiveNamespaceFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;

    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("primitive_scope");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("decl-primitive-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;

    fixture->modules[0].name = metadata->crate_name;
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 1u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[0].name =
        (CmHirDeclarationString)S("bool");
    fixture->namespace_entries[0].target_kind =
        CM_HIR_DECL_TARGET_PRIMITIVE;
    fixture->namespace_entries[0].target_local =
        CM_HIR_DECL_PRIMITIVE_BOOL;
    fixture->namespace_entries[0].export_ordinal = 1u;
    fixture->namespace_entries[1] = fixture->namespace_entries[0];
    fixture->namespace_entries[1].name =
        (CmHirDeclarationString)S("boolean");
    fixture->namespace_entries[1].export_ordinal = 2u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 2u;
}

static void associated_method_fixture_init(AssociatedMethodFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;
    CmHirDeclarationAssociatedItem *allocate;
    CmHirDeclarationAssociatedItem *fallback;

    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("allocator_like");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("decl-method-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;

    fixture->modules[0].name = metadata->crate_name;
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 1u;

    fixture->traits[0].owner_module = 1u;
    fixture->traits[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->traits[0].name =
        (CmHirDeclarationString)S("AllocatorLike");
    fixture->traits[0].source_ordinal = 1u;
    fixture->traits[0].associated_start = 1u;
    fixture->traits[0].associated_count = 2u;
    fixture->traits[0].safety = CM_HIR_DECL_SAFETY_UNSAFE;
    fixture->traits[1].owner_module = 1u;
    fixture->traits[1].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->traits[1].name = (CmHirDeclarationString)S("SizedLike");
    fixture->traits[1].source_ordinal = 4u;
    fixture->traits[1].safety = CM_HIR_DECL_SAFETY_SAFE;
    metadata->traits = fixture->traits;
    metadata->trait_count = 2u;

    fixture->types[0].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[0].primitive = CM_HIR_DECL_PRIMITIVE_UNIT;
    fixture->types[1].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[1].primitive = CM_HIR_DECL_PRIMITIVE_USIZE;
    fixture->types[2].kind = CM_HIR_DECL_TYPE_SELF;
    fixture->types[2].self_trait_local = 1u;
    fixture->types[3].kind = CM_HIR_DECL_TYPE_REFERENCE;
    fixture->types[3].child_type = 3u;
    fixture->types[3].mutability = CM_HIR_DECL_IMMUTABLE;
    fixture->types[3].region.kind = CM_HIR_DECL_REGION_ERASED;
    metadata->types = fixture->types;
    metadata->type_count = 4u;

    fixture->allocate_parameters[0] = 4u;
    fixture->allocate_parameters[1] = 2u;
    allocate = &fixture->associated_items[0];
    allocate->kind = CM_HIR_DECL_ASSOCIATED_METHOD;
    allocate->parent_kind = CM_HIR_DECL_ASSOCIATED_PARENT_NOMINAL;
    allocate->parent_local = 1u;
    allocate->name = (CmHirDeclarationString)S("allocate");
    allocate->visibility.kind = CM_HIR_DECL_VISIBILITY_PRIVATE;
    allocate->source_ordinal = 2u;
    allocate->receiver = CM_HIR_DECL_RECEIVER_REF_SHARED;
    allocate->parameter_count = 2u;
    allocate->parameter_types = fixture->allocate_parameters;
    allocate->return_type = 1u;
    allocate->abi = (CmHirDeclarationString)S("Rust");
    allocate->safety = CM_HIR_DECL_SAFETY_UNSAFE;

    fixture->fallback_parameters[0] = 4u;
    fallback = &fixture->associated_items[1];
    fallback->kind = CM_HIR_DECL_ASSOCIATED_METHOD;
    fallback->parent_kind = CM_HIR_DECL_ASSOCIATED_PARENT_NOMINAL;
    fallback->parent_local = 1u;
    fallback->name = (CmHirDeclarationString)S("fallback");
    fallback->visibility.kind = CM_HIR_DECL_VISIBILITY_PRIVATE;
    fallback->source_ordinal = 3u;
    fallback->predicate_start = 1u;
    fallback->predicate_count = 1u;
    fallback->receiver = CM_HIR_DECL_RECEIVER_REF_SHARED;
    fallback->parameter_count = 1u;
    fallback->parameter_types = fixture->fallback_parameters;
    fallback->return_type = 1u;
    fallback->abi = (CmHirDeclarationString)S("Rust");
    fallback->safety = CM_HIR_DECL_SAFETY_SAFE;
    fallback->has_default_body = 1u;
    metadata->associated_items = fixture->associated_items;
    metadata->associated_count = 2u;

    fixture->predicates[0].owner_kind =
        CM_HIR_DECL_PREDICATE_OWNER_ASSOCIATED;
    fixture->predicates[0].owner_associated = 2u;
    fixture->predicates[0].subject_type = 3u;
    fixture->predicates[0].trait_local = 2u;
    metadata->predicates = fixture->predicates;
    metadata->predicate_count = 1u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[0].name = fixture->traits[0].name;
    fixture->namespace_entries[0].target_kind =
        CM_HIR_DECL_TARGET_NOMINAL;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 1u;
    fixture->namespace_entries[1].owner_module = 1u;
    fixture->namespace_entries[1].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[1].name = fixture->traits[1].name;
    fixture->namespace_entries[1].target_kind =
        CM_HIR_DECL_TARGET_NOMINAL;
    fixture->namespace_entries[1].target_local = 2u;
    fixture->namespace_entries[1].export_ordinal = 4u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 2u;
}

static void any_fixture_init(AnyFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;
    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("any_like");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("decl-any-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;
    fixture->modules[0].name = metadata->crate_name;
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 1u;

    fixture->traits[0].owner_module = 1u;
    fixture->traits[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->traits[0].name = (CmHirDeclarationString)S("AnyLike");
    fixture->traits[0].source_ordinal = 1u;
    fixture->traits[0].outlives_start = 1u;
    fixture->traits[0].outlives_count = 1u;
    fixture->traits[0].associated_start = 1u;
    fixture->traits[0].associated_count = 1u;
    fixture->traits[0].safety = CM_HIR_DECL_SAFETY_SAFE;
    fixture->traits[0].flags = CM_HIR_DECL_TRAIT_HAS_DIAGNOSTIC_ITEM;
    fixture->traits[0].diagnostic_item =
        (CmHirDeclarationString)S("Any");
    metadata->traits = fixture->traits;
    metadata->trait_count = 1u;

    fixture->items[0].kind = CM_HIR_DECL_ITEM_STRUCT;
    fixture->items[0].aggregate_form = CM_HIR_DECL_AGGREGATE_UNIT;
    fixture->items[0].owner_module = 1u;
    fixture->items[0].name = (CmHirDeclarationString)S("TypeIdLike");
    fixture->items[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[0].source_ordinal = 3u;
    metadata->items = fixture->items;
    metadata->item_count = 1u;

    fixture->types[0].kind = CM_HIR_DECL_TYPE_NAMED_ADT;
    fixture->types[0].item_local = 1u;
    fixture->types[1].kind = CM_HIR_DECL_TYPE_SELF;
    fixture->types[1].self_trait_local = 1u;
    fixture->types[2].kind = CM_HIR_DECL_TYPE_REFERENCE;
    fixture->types[2].child_type = 2u;
    fixture->types[2].mutability = CM_HIR_DECL_IMMUTABLE;
    fixture->types[2].region.kind = CM_HIR_DECL_REGION_ERASED;
    metadata->types = fixture->types;
    metadata->type_count = 3u;

    fixture->parameters[0] = 3u;
    fixture->associated_items[0].kind = CM_HIR_DECL_ASSOCIATED_METHOD;
    fixture->associated_items[0].parent_kind =
        CM_HIR_DECL_ASSOCIATED_PARENT_NOMINAL;
    fixture->associated_items[0].parent_local = 1u;
    fixture->associated_items[0].name =
        (CmHirDeclarationString)S("type_id");
    fixture->associated_items[0].visibility.kind =
        CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->associated_items[0].source_ordinal = 2u;
    fixture->associated_items[0].receiver =
        CM_HIR_DECL_RECEIVER_REF_SHARED;
    fixture->associated_items[0].parameter_count = 1u;
    fixture->associated_items[0].parameter_types = fixture->parameters;
    fixture->associated_items[0].return_type = 1u;
    fixture->associated_items[0].abi = (CmHirDeclarationString)S("Rust");
    fixture->associated_items[0].safety = CM_HIR_DECL_SAFETY_SAFE;
    metadata->associated_items = fixture->associated_items;
    metadata->associated_count = 1u;

    fixture->outlives[0].owner_kind =
        CM_HIR_DECL_PREDICATE_OWNER_NOMINAL;
    fixture->outlives[0].owner_local = 1u;
    fixture->outlives[0].subject_type = 2u;
    fixture->outlives[0].bound.kind = CM_HIR_DECL_REGION_STATIC;
    metadata->outlives_predicates = fixture->outlives;
    metadata->outlives_predicate_count = 1u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[0].name = fixture->traits[0].name;
    fixture->namespace_entries[0].target_kind =
        CM_HIR_DECL_TARGET_NOMINAL;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 1u;
    fixture->namespace_entries[1].owner_module = 1u;
    fixture->namespace_entries[1].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[1].name = fixture->items[0].name;
    fixture->namespace_entries[1].target_kind = CM_HIR_DECL_TARGET_ITEM;
    fixture->namespace_entries[1].target_local = 1u;
    fixture->namespace_entries[1].export_ordinal = 3u;
    fixture->namespace_entries[2] = fixture->namespace_entries[1];
    fixture->namespace_entries[2].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 3u;
}

static void item_fixture_init(TestFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;
    fixture_init(fixture);
    metadata = &fixture->metadata;

    fixture->items[0].kind = CM_HIR_DECL_ITEM_STRUCT;
    fixture->items[0].aggregate_form = CM_HIR_DECL_AGGREGATE_UNIT;
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
    fixture->namespace_entries[7].export_ordinal = 2u;
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
    fixture->items[1].aggregate_form = CM_HIR_DECL_AGGREGATE_UNIT;
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

static void structural_fixture_init(StructuralFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;
    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("structural");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("decl-structural-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;

    fixture->modules[0].name = metadata->crate_name;
    fixture->modules[1].parent_module = 1u;
    fixture->modules[1].name = (CmHirDeclarationString)S("api");
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 2u;

    fixture->traits[0].owner_module = 2u;
    fixture->traits[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->traits[0].name = (CmHirDeclarationString)S("Gate");
    fixture->traits[0].source_ordinal = 1u;
    fixture->traits[0].generic_start = 1u;
    fixture->traits[0].generic_count = 1u;
    metadata->traits = fixture->traits;
    metadata->trait_count = 1u;

    fixture->items[0].kind = CM_HIR_DECL_ITEM_STRUCT;
    fixture->items[0].aggregate_form = CM_HIR_DECL_AGGREGATE_UNIT;
    fixture->items[0].owner_module = 2u;
    fixture->items[0].name = (CmHirDeclarationString)S("Wrap");
    fixture->items[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[0].source_ordinal = 2u;
    fixture->items[0].generic_start = 2u;
    fixture->items[0].generic_count = 1u;
    metadata->items = fixture->items;
    metadata->item_count = 1u;

    fixture->generics[0].owner_kind = CM_HIR_DECL_GENERIC_NOMINAL;
    fixture->generics[0].owner_local = 1u;
    fixture->generics[0].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[0].is_relaxed_sized = 1u;
    fixture->generics[0].name = (CmHirDeclarationString)S("A");
    fixture->generics[1].owner_kind = CM_HIR_DECL_GENERIC_ITEM;
    fixture->generics[1].owner_local = 1u;
    fixture->generics[1].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[1].is_relaxed_sized = 1u;
    fixture->generics[1].name = (CmHirDeclarationString)S("W");
    fixture->generics[2].owner_kind = CM_HIR_DECL_GENERIC_VALUE;
    fixture->generics[2].owner_local = 1u;
    fixture->generics[2].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[2].is_relaxed_sized = 1u;
    fixture->generics[2].name = (CmHirDeclarationString)S("T");
    metadata->generics = fixture->generics;
    metadata->generic_count = 3u;

    fixture->types[0].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[0].primitive = CM_HIR_DECL_PRIMITIVE_U8;
    fixture->types[1].kind = CM_HIR_DECL_TYPE_GENERIC;
    fixture->types[1].generic_local = 3u;
    fixture->types[2].kind = CM_HIR_DECL_TYPE_SLICE;
    fixture->types[2].child_type = 1u;
    fixture->types[3].kind = CM_HIR_DECL_TYPE_RAW_POINTER;
    fixture->types[3].mutability = CM_HIR_DECL_IMMUTABLE;
    fixture->types[3].child_type = 2u;
    fixture->application_arguments[0] = 2u;
    fixture->types[4].kind = CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION;
    fixture->types[4].item_local = 1u;
    fixture->types[4].argument_count = 1u;
    fixture->types[4].argument_types = fixture->application_arguments;
    fixture->types[5].kind = CM_HIR_DECL_TYPE_REFERENCE;
    fixture->types[5].region.kind = CM_HIR_DECL_REGION_STATIC;
    fixture->types[5].mutability = CM_HIR_DECL_IMMUTABLE;
    fixture->types[5].child_type = 3u;
    metadata->types = fixture->types;
    metadata->type_count = 6u;

    fixture->parameters[0] = 4u;
    fixture->parameters[1] = 6u;
    fixture->values[0].kind = CM_HIR_DECL_VALUE_FUNCTION;
    fixture->values[0].owner_module = 2u;
    fixture->values[0].name = (CmHirDeclarationString)S("shape");
    fixture->values[0].source_ordinal = 3u;
    fixture->values[0].generic_start = 3u;
    fixture->values[0].generic_count = 1u;
    fixture->values[0].predicate_start = 1u;
    fixture->values[0].predicate_count = 1u;
    fixture->values[0].parameter_count = 2u;
    fixture->values[0].parameter_types = fixture->parameters;
    fixture->values[0].return_type = 5u;
    fixture->values[0].has_body = 1u;
    metadata->values = fixture->values;
    metadata->value_count = 1u;

    fixture->predicate_arguments[0] = 1u;
    fixture->predicates[0].owner_value = 1u;
    fixture->predicates[0].subject_type = 2u;
    fixture->predicates[0].trait_local = 1u;
    fixture->predicates[0].argument_count = 1u;
    fixture->predicates[0].argument_types = fixture->predicate_arguments;
    metadata->predicates = fixture->predicates;
    metadata->predicate_count = 1u;

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
    fixture->namespace_entries[1].name = fixture->traits[0].name;
    fixture->namespace_entries[1].target_kind = CM_HIR_DECL_TARGET_NOMINAL;
    fixture->namespace_entries[1].target_local = 1u;
    fixture->namespace_entries[1].export_ordinal = 1u;
    fixture->namespace_entries[2].owner_module = 2u;
    fixture->namespace_entries[2].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[2].name = fixture->items[0].name;
    fixture->namespace_entries[2].target_kind = CM_HIR_DECL_TARGET_ITEM;
    fixture->namespace_entries[2].target_local = 1u;
    fixture->namespace_entries[2].export_ordinal = 2u;
    fixture->namespace_entries[3] = fixture->namespace_entries[2];
    fixture->namespace_entries[3].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[4].owner_module = 2u;
    fixture->namespace_entries[4].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[4].name = fixture->values[0].name;
    fixture->namespace_entries[4].target_kind = CM_HIR_DECL_TARGET_VALUE;
    fixture->namespace_entries[4].target_local = 1u;
    fixture->namespace_entries[4].export_ordinal = 3u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 5u;
}

static void enum_fixture_init(EnumFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;
    uint32_t index;
    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("ascii");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("decl-ascii-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;
    fixture->modules[0].name = metadata->crate_name;
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 1u;

    fixture->items[0].kind = CM_HIR_DECL_ITEM_ENUM;
    fixture->items[0].owner_module = 1u;
    fixture->items[0].name = (CmHirDeclarationString)S("AsciiChar");
    fixture->items[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[0].source_ordinal = 1u;
    fixture->items[0].enum_repr_primitive = CM_HIR_DECL_PRIMITIVE_U8;
    fixture->items[0].variant_count = 128u;
    fixture->items[0].variants = fixture->variants;
    for (index = 0u; index < 128u; ++index) {
        fixture->variant_names[index][0] = (unsigned char)'V';
        fixture->variant_names[index][1] =
            (unsigned char)('0' + (index / 100u));
        fixture->variant_names[index][2] =
            (unsigned char)('0' + ((index / 10u) % 10u));
        fixture->variant_names[index][3] =
            (unsigned char)('0' + (index % 10u));
        fixture->variants[index].kind = CM_HIR_DECL_VARIANT_UNIT;
        fixture->variants[index].name.data = fixture->variant_names[index];
        fixture->variants[index].name.length = 4u;
        fixture->variants[index].source_ordinal = index + 1u;
        fixture->variants[index].discriminant_primitive =
            CM_HIR_DECL_PRIMITIVE_ISIZE;
        fixture->variants[index].discriminant_low = index;
    }
    metadata->items = fixture->items;
    metadata->item_count = 1u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[0].name = fixture->items[0].name;
    fixture->namespace_entries[0].target_kind = CM_HIR_DECL_TARGET_ITEM;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 1u;
    fixture->namespace_entries[1].owner_module = 1u;
    fixture->namespace_entries[1].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[1].name =
        (CmHirDeclarationString)S("AsciiCharReexport");
    fixture->namespace_entries[1].target_kind = CM_HIR_DECL_TARGET_ITEM;
    fixture->namespace_entries[1].target_local = 1u;
    fixture->namespace_entries[1].export_ordinal = 2u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 2u;
}

static void const_fixture_init(ConstFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;
    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("core");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("decl-const-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;

    fixture->modules[0].name = metadata->crate_name;
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 1u;

    /* Keep NOMD nonempty so a CONST round trip covers neighboring families. */
    fixture->traits[0].owner_module = 1u;
    fixture->traits[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->traits[0].name = (CmHirDeclarationString)S("Marker");
    fixture->traits[0].source_ordinal = 1u;
    metadata->traits = fixture->traits;
    metadata->trait_count = 1u;

    fixture->types[0].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[0].primitive = CM_HIR_DECL_PRIMITIVE_CHAR;
    metadata->types = fixture->types;
    metadata->type_count = 1u;

    fixture->values[0].kind = CM_HIR_DECL_VALUE_CONST;
    fixture->values[0].owner_module = 1u;
    fixture->values[0].name = (CmHirDeclarationString)S("MAX");
    fixture->values[0].source_ordinal = 2u;
    fixture->values[0].declared_type = 1u;
    fixture->values[0].mutability = CM_HIR_DECL_IMMUTABLE;
    fixture->values[0].has_body = 1u;
    metadata->values = fixture->values;
    metadata->value_count = 1u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[0].name = fixture->traits[0].name;
    fixture->namespace_entries[0].target_kind = CM_HIR_DECL_TARGET_NOMINAL;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 1u;
    fixture->namespace_entries[1].owner_module = 1u;
    fixture->namespace_entries[1].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[1].name = fixture->values[0].name;
    fixture->namespace_entries[1].target_kind = CM_HIR_DECL_TARGET_VALUE;
    fixture->namespace_entries[1].target_local = 1u;
    fixture->namespace_entries[1].export_ordinal = 2u;
    fixture->namespace_entries[2] = fixture->namespace_entries[1];
    fixture->namespace_entries[2].name =
        (CmHirDeclarationString)S("MAX_REEXPORT");
    fixture->namespace_entries[2].export_ordinal = 3u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 3u;
}

static void type_name_fixture_init(TypeNameFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;

    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("type_name_like");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("decl-type-name-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;

    fixture->modules[0].name = metadata->crate_name;
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 1u;

    fixture->generics[0].owner_kind = CM_HIR_DECL_GENERIC_VALUE;
    fixture->generics[0].owner_local = 1u;
    fixture->generics[0].index = 0u;
    fixture->generics[0].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[0].is_relaxed_sized = 1u;
    fixture->generics[0].name = (CmHirDeclarationString)S("T");
    metadata->generics = fixture->generics;
    metadata->generic_count = 1u;

    fixture->types[0].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[0].primitive = CM_HIR_DECL_PRIMITIVE_STR;
    fixture->types[1].kind = CM_HIR_DECL_TYPE_REFERENCE;
    fixture->types[1].child_type = 1u;
    fixture->types[1].mutability = CM_HIR_DECL_IMMUTABLE;
    fixture->types[1].region.kind = CM_HIR_DECL_REGION_STATIC;
    metadata->types = fixture->types;
    metadata->type_count = 2u;

    fixture->values[0].kind = CM_HIR_DECL_VALUE_FUNCTION;
    fixture->values[0].owner_module = 1u;
    fixture->values[0].name = (CmHirDeclarationString)S("type_name");
    fixture->values[0].source_ordinal = 1u;
    fixture->values[0].generic_start = 1u;
    fixture->values[0].generic_count = 1u;
    fixture->values[0].return_type = 2u;
    fixture->values[0].has_body = 1u;
    fixture->values[0].is_const = 1u;
    metadata->values = fixture->values;
    metadata->value_count = 1u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[0].name = fixture->values[0].name;
    fixture->namespace_entries[0].target_kind = CM_HIR_DECL_TARGET_VALUE;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 1u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 1u;
}

static void type_name_of_val_fixture_init(TypeNameFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;

    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("type_name_of_val_like");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("decl-type-name-of-val-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;

    fixture->modules[0].name = metadata->crate_name;
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 1u;

    fixture->generics[0].owner_kind = CM_HIR_DECL_GENERIC_VALUE;
    fixture->generics[0].owner_local = 1u;
    fixture->generics[0].index = 0u;
    fixture->generics[0].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[0].is_relaxed_sized = 1u;
    fixture->generics[0].name = (CmHirDeclarationString)S("T");
    metadata->generics = fixture->generics;
    metadata->generic_count = 1u;

    fixture->types[0].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[0].primitive = CM_HIR_DECL_PRIMITIVE_STR;
    fixture->types[1].kind = CM_HIR_DECL_TYPE_GENERIC;
    fixture->types[1].generic_local = 1u;
    fixture->types[2].kind = CM_HIR_DECL_TYPE_REFERENCE;
    fixture->types[2].child_type = 1u;
    fixture->types[2].mutability = CM_HIR_DECL_IMMUTABLE;
    fixture->types[2].region.kind = CM_HIR_DECL_REGION_STATIC;
    fixture->types[3].kind = CM_HIR_DECL_TYPE_REFERENCE;
    fixture->types[3].child_type = 2u;
    fixture->types[3].mutability = CM_HIR_DECL_IMMUTABLE;
    fixture->types[3].region.kind = CM_HIR_DECL_REGION_ERASED;
    metadata->types = fixture->types;
    metadata->type_count = 4u;

    fixture->parameters[0] = 4u;
    fixture->values[0].kind = CM_HIR_DECL_VALUE_FUNCTION;
    fixture->values[0].owner_module = 1u;
    fixture->values[0].name =
        (CmHirDeclarationString)S("type_name_of_val");
    fixture->values[0].source_ordinal = 1u;
    fixture->values[0].generic_start = 1u;
    fixture->values[0].generic_count = 1u;
    fixture->values[0].parameter_count = 1u;
    fixture->values[0].parameter_types = fixture->parameters;
    fixture->values[0].return_type = 3u;
    fixture->values[0].has_body = 1u;
    fixture->values[0].is_const = 1u;
    metadata->values = fixture->values;
    metadata->value_count = 1u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[0].name = fixture->values[0].name;
    fixture->namespace_entries[0].target_kind = CM_HIR_DECL_TARGET_VALUE;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 1u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 1u;
}

static void from_mut_fixture_init(FromMutFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;

    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("from_mut_like");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("decl-from-mut-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;

    fixture->modules[0].name = metadata->crate_name;
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 1u;

    fixture->generics[0].owner_kind = CM_HIR_DECL_GENERIC_VALUE;
    fixture->generics[0].owner_local = 1u;
    fixture->generics[0].index = 0u;
    fixture->generics[0].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[0].name = (CmHirDeclarationString)S("T");
    metadata->generics = fixture->generics;
    metadata->generic_count = 1u;

    fixture->types[0].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[0].primitive = CM_HIR_DECL_PRIMITIVE_USIZE;
    fixture->types[1].kind = CM_HIR_DECL_TYPE_GENERIC;
    fixture->types[1].generic_local = 1u;
    fixture->types[2].kind = CM_HIR_DECL_TYPE_REFERENCE;
    fixture->types[2].child_type = 2u;
    fixture->types[2].mutability = CM_HIR_DECL_MUTABLE;
    fixture->types[2].region.kind = CM_HIR_DECL_REGION_ERASED;
    fixture->types[3].kind = CM_HIR_DECL_TYPE_ARRAY;
    fixture->types[3].child_type = 2u;
    fixture->types[3].array_length_kind =
        CM_HIR_DECL_ARRAY_LENGTH_SCALAR;
    fixture->types[3].array_length_type = 1u;
    fixture->types[3].array_length_low_bits = UINT64_C(1);
    fixture->types[4].kind = CM_HIR_DECL_TYPE_REFERENCE;
    fixture->types[4].child_type = 4u;
    fixture->types[4].mutability = CM_HIR_DECL_MUTABLE;
    fixture->types[4].region.kind = CM_HIR_DECL_REGION_ERASED;
    metadata->types = fixture->types;
    metadata->type_count = 5u;

    fixture->parameters[0] = 3u;
    fixture->values[0].kind = CM_HIR_DECL_VALUE_FUNCTION;
    fixture->values[0].owner_module = 1u;
    fixture->values[0].name = (CmHirDeclarationString)S("from_mut");
    fixture->values[0].source_ordinal = 1u;
    fixture->values[0].generic_start = 1u;
    fixture->values[0].generic_count = 1u;
    fixture->values[0].parameter_count = 1u;
    fixture->values[0].parameter_types = fixture->parameters;
    fixture->values[0].return_type = 5u;
    fixture->values[0].has_body = 1u;
    fixture->values[0].is_const = 1u;
    metadata->values = fixture->values;
    metadata->value_count = 1u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[0].name = fixture->values[0].name;
    fixture->namespace_entries[0].target_kind = CM_HIR_DECL_TARGET_VALUE;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 1u;
    fixture->namespace_entries[1] = fixture->namespace_entries[0];
    fixture->namespace_entries[1].name =
        (CmHirDeclarationString)S("from_mut_alias");
    fixture->namespace_entries[1].export_ordinal = 2u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 2u;
}

static void unit_function_fixture_init(UnitFunctionFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;

    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("breakpoint_like");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("decl-unit-function-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;

    fixture->modules[0].name = metadata->crate_name;
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 1u;

    fixture->types[0].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[0].primitive = CM_HIR_DECL_PRIMITIVE_UNIT;
    metadata->types = fixture->types;
    metadata->type_count = 1u;

    fixture->values[0].kind = CM_HIR_DECL_VALUE_FUNCTION;
    fixture->values[0].owner_module = 1u;
    fixture->values[0].name = (CmHirDeclarationString)S("breakpoint");
    fixture->values[0].source_ordinal = 1u;
    fixture->values[0].return_type = 1u;
    fixture->values[0].has_body = 1u;
    metadata->values = fixture->values;
    metadata->value_count = 1u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[0].name = fixture->values[0].name;
    fixture->namespace_entries[0].target_kind = CM_HIR_DECL_TARGET_VALUE;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 1u;
    fixture->namespace_entries[1] = fixture->namespace_entries[0];
    fixture->namespace_entries[1].name =
        (CmHirDeclarationString)S("breakpoint_alias");
    fixture->namespace_entries[1].export_ordinal = 2u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 2u;
}

static void static_fixture_init(StaticFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;
    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("core");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("decl-static-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;

    fixture->modules[0].name = metadata->crate_name;
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 1u;

    fixture->types[0].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[0].primitive = CM_HIR_DECL_PRIMITIVE_I16;
    fixture->types[1].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[1].primitive = CM_HIR_DECL_PRIMITIVE_U64;
    fixture->types[2].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[2].primitive = CM_HIR_DECL_PRIMITIVE_USIZE;
    fixture->tuple_elements[0] = 2u;
    fixture->tuple_elements[1] = 1u;
    fixture->tuple_elements[2] = 1u;
    fixture->types[3].kind = CM_HIR_DECL_TYPE_TUPLE;
    fixture->types[3].element_count = 3u;
    fixture->types[3].element_types = fixture->tuple_elements;
    fixture->types[4].kind = CM_HIR_DECL_TYPE_ARRAY;
    fixture->types[4].child_type = 4u;
    fixture->types[4].array_length_type = 3u;
    fixture->types[4].array_length_low_bits = UINT64_C(81);
    metadata->types = fixture->types;
    metadata->type_count = 5u;

    fixture->values[0].kind = CM_HIR_DECL_VALUE_STATIC;
    fixture->values[0].owner_module = 1u;
    fixture->values[0].name = (CmHirDeclarationString)S("CACHED_POW10");
    fixture->values[0].source_ordinal = 1u;
    fixture->values[0].declared_type = 5u;
    fixture->values[0].mutability = CM_HIR_DECL_IMMUTABLE;
    fixture->values[0].has_body = 1u;
    metadata->values = fixture->values;
    metadata->value_count = 1u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[0].name = fixture->values[0].name;
    fixture->namespace_entries[0].target_kind =
        CM_HIR_DECL_TARGET_VALUE;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 1u;
    fixture->namespace_entries[1] = fixture->namespace_entries[0];
    fixture->namespace_entries[1].name =
        (CmHirDeclarationString)S("POW10_REEXPORT");
    fixture->namespace_entries[1].export_ordinal = 2u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 2u;
}

static void type_id_fixture_init(TypeIdFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;

    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("type_id_like");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("decl-type-id-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;
    fixture->cfgs[0] =
        (CmHirDeclarationString)S("target_arch=x86_64");
    fixture->cfgs[1] =
        (CmHirDeclarationString)S("target_pointer_width=64");
    metadata->cfgs = fixture->cfgs;
    metadata->cfg_count = 2u;

    fixture->modules[0].name = metadata->crate_name;
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 1u;

    fixture->types[0].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[0].primitive = CM_HIR_DECL_PRIMITIVE_UNIT;
    fixture->types[1].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[1].primitive = CM_HIR_DECL_PRIMITIVE_USIZE;
    fixture->types[2].kind = CM_HIR_DECL_TYPE_RAW_POINTER;
    fixture->types[2].mutability = CM_HIR_DECL_IMMUTABLE;
    fixture->types[2].child_type = 1u;
    fixture->types[3].kind = CM_HIR_DECL_TYPE_ARRAY;
    fixture->types[3].child_type = 3u;
    fixture->types[3].array_length_type = 2u;
    fixture->types[3].array_length_low_bits = UINT64_C(2);
    metadata->types = fixture->types;
    metadata->type_count = 4u;

    fixture->fields[0].name = (CmHirDeclarationString)S("data");
    fixture->fields[0].visibility.kind =
        CM_HIR_DECL_VISIBILITY_CRATE;
    fixture->fields[0].source_ordinal = 0u;
    fixture->fields[0].type_local = 4u;

    fixture->items[0].kind = CM_HIR_DECL_ITEM_STRUCT;
    fixture->items[0].owner_module = 1u;
    fixture->items[0].name = (CmHirDeclarationString)S("TypeIdLike");
    fixture->items[0].visibility.kind =
        CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[0].source_ordinal = 1u;
    fixture->items[0].aggregate_form =
        CM_HIR_DECL_AGGREGATE_NAMED;
    fixture->items[0].aggregate_repr = CM_HIR_DECL_AGGREGATE_REPR_RUST;
    fixture->items[0].aggregate_flags =
        CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM;
    fixture->items[0].field_count = 1u;
    fixture->items[0].fields = fixture->fields;
    fixture->items[0].lang_item =
        (CmHirDeclarationString)S("type_id");
    metadata->items = fixture->items;
    metadata->item_count = 1u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[0].name = fixture->items[0].name;
    fixture->namespace_entries[0].target_kind =
        CM_HIR_DECL_TARGET_ITEM;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 1u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 1u;
}

static void aggregate_fixture_init(AggregateFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;
    uint32_t index;
    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("core");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("decl-aggregate-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;

    fixture->modules[0].name = metadata->crate_name;
    fixture->modules[1].parent_module = 1u;
    fixture->modules[1].name = (CmHirDeclarationString)S("manually_drop");
    fixture->modules[2].parent_module = 1u;
    fixture->modules[2].name = (CmHirDeclarationString)S("maybe_uninit");
    fixture->modules[3].parent_module = 1u;
    fixture->modules[3].name = (CmHirDeclarationString)S("transmutability");
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 4u;

    fixture->items[0].kind = CM_HIR_DECL_ITEM_STRUCT;
    fixture->items[0].owner_module = 2u;
    fixture->items[0].name = (CmHirDeclarationString)S("ManuallyDrop");
    fixture->items[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[0].source_ordinal = 10u;
    fixture->items[0].generic_start = 1u;
    fixture->items[0].generic_count = 1u;
    fixture->items[0].aggregate_form = CM_HIR_DECL_AGGREGATE_NAMED;
    fixture->items[0].aggregate_repr =
        CM_HIR_DECL_AGGREGATE_REPR_TRANSPARENT;
    fixture->items[0].aggregate_flags =
        CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM
        | CM_HIR_DECL_AGGREGATE_RUSTC_PUB_TRANSPARENT;
    fixture->items[0].field_count = 1u;
    fixture->items[0].fields = fixture->manually_drop_fields;
    fixture->items[0].lang_item =
        (CmHirDeclarationString)S("manually_drop");

    fixture->items[1].kind = CM_HIR_DECL_ITEM_UNION;
    fixture->items[1].owner_module = 3u;
    fixture->items[1].name = (CmHirDeclarationString)S("MaybeUninit");
    fixture->items[1].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[1].source_ordinal = 20u;
    fixture->items[1].generic_start = 2u;
    fixture->items[1].generic_count = 1u;
    fixture->items[1].aggregate_form = CM_HIR_DECL_AGGREGATE_NAMED;
    fixture->items[1].aggregate_repr =
        CM_HIR_DECL_AGGREGATE_REPR_TRANSPARENT;
    fixture->items[1].aggregate_flags =
        CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM
        | CM_HIR_DECL_AGGREGATE_RUSTC_PUB_TRANSPARENT;
    fixture->items[1].field_count = 2u;
    fixture->items[1].fields = fixture->maybe_uninit_fields;
    fixture->items[1].lang_item =
        (CmHirDeclarationString)S("maybe_uninit");

    fixture->items[2].kind = CM_HIR_DECL_ITEM_STRUCT;
    fixture->items[2].owner_module = 4u;
    fixture->items[2].name = (CmHirDeclarationString)S("Assume");
    fixture->items[2].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[2].source_ordinal = 30u;
    fixture->items[2].aggregate_form = CM_HIR_DECL_AGGREGATE_NAMED;
    fixture->items[2].aggregate_repr = CM_HIR_DECL_AGGREGATE_REPR_RUST;
    fixture->items[2].aggregate_flags =
        CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM;
    fixture->items[2].field_count = 4u;
    fixture->items[2].fields = fixture->assume_fields;
    fixture->items[2].lang_item =
        (CmHirDeclarationString)S("transmute_opts");
    metadata->items = fixture->items;
    metadata->item_count = 3u;

    fixture->generics[0].owner_kind = CM_HIR_DECL_GENERIC_ITEM;
    fixture->generics[0].owner_local = 1u;
    fixture->generics[0].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[0].is_relaxed_sized = 1u;
    fixture->generics[0].name = (CmHirDeclarationString)S("T");
    fixture->generics[1].owner_kind = CM_HIR_DECL_GENERIC_ITEM;
    fixture->generics[1].owner_local = 2u;
    fixture->generics[1].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[1].name = (CmHirDeclarationString)S("T");
    metadata->generics = fixture->generics;
    metadata->generic_count = 2u;

    fixture->types[0].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[0].primitive = CM_HIR_DECL_PRIMITIVE_UNIT;
    fixture->types[1].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[1].primitive = CM_HIR_DECL_PRIMITIVE_BOOL;
    fixture->types[2].kind = CM_HIR_DECL_TYPE_GENERIC;
    fixture->types[2].generic_local = 1u;
    fixture->types[3].kind = CM_HIR_DECL_TYPE_GENERIC;
    fixture->types[3].generic_local = 2u;
    fixture->application_arguments[0] = 4u;
    fixture->types[4].kind = CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION;
    fixture->types[4].item_local = 1u;
    fixture->types[4].argument_count = 1u;
    fixture->types[4].argument_types = fixture->application_arguments;
    metadata->types = fixture->types;
    metadata->type_count = 5u;

    fixture->manually_drop_fields[0].name =
        (CmHirDeclarationString)S("value");
    fixture->manually_drop_fields[0].visibility.kind =
        CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->manually_drop_fields[0].source_ordinal = 0u;
    fixture->manually_drop_fields[0].type_local = 3u;
    fixture->maybe_uninit_fields[0].name =
        (CmHirDeclarationString)S("uninit");
    fixture->maybe_uninit_fields[0].visibility.kind =
        CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->maybe_uninit_fields[0].source_ordinal = 0u;
    fixture->maybe_uninit_fields[0].type_local = 1u;
    fixture->maybe_uninit_fields[1].name =
        (CmHirDeclarationString)S("value");
    fixture->maybe_uninit_fields[1].visibility.kind =
        CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->maybe_uninit_fields[1].source_ordinal = 1u;
    fixture->maybe_uninit_fields[1].type_local = 5u;
    for (index = 0u; index < 4u; ++index) {
        fixture->assume_fields[index].visibility.kind =
            CM_HIR_DECL_VISIBILITY_PUBLIC;
        fixture->assume_fields[index].source_ordinal = index;
        fixture->assume_fields[index].type_local = 2u;
    }
    fixture->assume_fields[0].name =
        (CmHirDeclarationString)S("alignment");
    fixture->assume_fields[1].name =
        (CmHirDeclarationString)S("lifetimes");
    fixture->assume_fields[2].name =
        (CmHirDeclarationString)S("safety");
    fixture->assume_fields[3].name =
        (CmHirDeclarationString)S("validity");

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[0].name = fixture->items[2].name;
    fixture->namespace_entries[0].target_kind = CM_HIR_DECL_TARGET_ITEM;
    fixture->namespace_entries[0].target_local = 3u;
    fixture->namespace_entries[0].export_ordinal = 1u;
    fixture->namespace_entries[1] = fixture->namespace_entries[0];
    fixture->namespace_entries[1].name = fixture->items[0].name;
    fixture->namespace_entries[1].target_local = 1u;
    fixture->namespace_entries[1].export_ordinal = 2u;
    fixture->namespace_entries[2] = fixture->namespace_entries[0];
    fixture->namespace_entries[2].name = fixture->items[1].name;
    fixture->namespace_entries[2].target_local = 2u;
    fixture->namespace_entries[2].export_ordinal = 3u;
    fixture->namespace_entries[3] = fixture->namespace_entries[1];
    fixture->namespace_entries[3].owner_module = 2u;
    fixture->namespace_entries[3].export_ordinal = 10u;
    fixture->namespace_entries[4] = fixture->namespace_entries[2];
    fixture->namespace_entries[4].owner_module = 3u;
    fixture->namespace_entries[4].export_ordinal = 20u;
    fixture->namespace_entries[5] = fixture->namespace_entries[0];
    fixture->namespace_entries[5].owner_module = 4u;
    fixture->namespace_entries[5].export_ordinal = 30u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 6u;
}

static void layout_fixture_init(LayoutFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;
    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("core");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("decl-layout-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;
    fixture->modules[0].name = metadata->crate_name;
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 1u;

    fixture->items[0].kind = CM_HIR_DECL_ITEM_STRUCT;
    fixture->items[0].owner_module = 1u;
    fixture->items[0].name = (CmHirDeclarationString)S("Alignment");
    fixture->items[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->items[0].source_ordinal = 10u;
    fixture->items[0].aggregate_form = CM_HIR_DECL_AGGREGATE_TUPLE;
    fixture->items[0].aggregate_repr =
        CM_HIR_DECL_AGGREGATE_REPR_TRANSPARENT;
    fixture->items[0].field_count = 1u;
    fixture->items[0].fields = fixture->alignment_fields;

    fixture->items[1].kind = CM_HIR_DECL_ITEM_ENUM;
    fixture->items[1].owner_module = 1u;
    fixture->items[1].name = (CmHirDeclarationString)S("AlignmentEnum");
    fixture->items[1].visibility.kind = CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->items[1].source_ordinal = 11u;
    fixture->items[1].enum_repr_primitive = CM_HIR_DECL_ENUM_REPR_U64;
    fixture->items[1].variant_count = 2u;
    fixture->items[1].variants = fixture->alignment_variants;

    fixture->items[2].kind = CM_HIR_DECL_ITEM_STRUCT;
    fixture->items[2].owner_module = 1u;
    fixture->items[2].name = (CmHirDeclarationString)S("Layout");
    fixture->items[2].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[2].source_ordinal = 20u;
    fixture->items[2].aggregate_form = CM_HIR_DECL_AGGREGATE_NAMED;
    fixture->items[2].aggregate_repr = CM_HIR_DECL_AGGREGATE_REPR_RUST;
    fixture->items[2].aggregate_flags =
        CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM;
    fixture->items[2].field_count = 2u;
    fixture->items[2].fields = fixture->layout_fields;
    fixture->items[2].lang_item = (CmHirDeclarationString)S("alloc_layout");
    metadata->items = fixture->items;
    metadata->item_count = 3u;

    fixture->types[0].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[0].primitive = CM_HIR_DECL_PRIMITIVE_USIZE;
    fixture->types[1].kind = CM_HIR_DECL_TYPE_NAMED_ADT;
    fixture->types[1].item_local = 1u;
    fixture->types[2].kind = CM_HIR_DECL_TYPE_NAMED_ADT;
    fixture->types[2].item_local = 2u;
    metadata->types = fixture->types;
    metadata->type_count = 3u;

    fixture->alignment_fields[0].visibility.kind =
        CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->alignment_fields[0].source_ordinal = 0u;
    fixture->alignment_fields[0].type_local = 3u;
    fixture->layout_fields[0].name = (CmHirDeclarationString)S("size");
    fixture->layout_fields[0].visibility.kind =
        CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->layout_fields[0].source_ordinal = 0u;
    fixture->layout_fields[0].type_local = 1u;
    fixture->layout_fields[1].name = (CmHirDeclarationString)S("align");
    fixture->layout_fields[1].visibility.kind =
        CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->layout_fields[1].source_ordinal = 1u;
    fixture->layout_fields[1].type_local = 2u;

    fixture->alignment_variants[0].kind = CM_HIR_DECL_VARIANT_UNIT;
    fixture->alignment_variants[0].name =
        (CmHirDeclarationString)S("Align1");
    fixture->alignment_variants[0].source_ordinal = 0u;
    fixture->alignment_variants[0].discriminant_primitive =
        CM_HIR_DECL_PRIMITIVE_ISIZE;
    fixture->alignment_variants[0].discriminant_low = UINT64_C(1);
    fixture->alignment_variants[1].kind = CM_HIR_DECL_VARIANT_UNIT;
    fixture->alignment_variants[1].name =
        (CmHirDeclarationString)S("AlignMax");
    fixture->alignment_variants[1].source_ordinal = 1u;
    fixture->alignment_variants[1].discriminant_primitive =
        CM_HIR_DECL_PRIMITIVE_ISIZE;
    fixture->alignment_variants[1].discriminant_low = UINT64_MAX;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[0].name = fixture->items[2].name;
    fixture->namespace_entries[0].target_kind = CM_HIR_DECL_TARGET_ITEM;
    fixture->namespace_entries[0].target_local = 3u;
    fixture->namespace_entries[0].export_ordinal = 20u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 1u;
}

static void into_iter_fixture_init(IntoIterFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;
    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("intoiter_dep");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("decl-intoiter-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;

    fixture->modules[0].name = metadata->crate_name;
    fixture->modules[1].parent_module = 1u;
    fixture->modules[1].name = (CmHirDeclarationString)S("iter");
    fixture->modules[2].parent_module = 2u;
    fixture->modules[2].name = (CmHirDeclarationString)S("iter_inner");
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 3u;

    fixture->traits[0].owner_module = 3u;
    fixture->traits[0].name = (CmHirDeclarationString)S("PartialDrop");
    fixture->traits[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->traits[0].source_ordinal = 1u;
    fixture->traits[0].associated_start = 1u;
    fixture->traits[0].associated_count = 1u;
    fixture->traits[0].safety = CM_HIR_DECL_SAFETY_SAFE;
    metadata->traits = fixture->traits;
    metadata->trait_count = 1u;

    fixture->generics[0].owner_kind = CM_HIR_DECL_GENERIC_ITEM;
    fixture->generics[0].owner_local = 2u;
    fixture->generics[0].index = 0u;
    fixture->generics[0].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[0].name = (CmHirDeclarationString)S("T");
    fixture->generics[1].owner_kind = CM_HIR_DECL_GENERIC_ITEM;
    fixture->generics[1].owner_local = 3u;
    fixture->generics[1].index = 0u;
    fixture->generics[1].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[1].name = (CmHirDeclarationString)S("T");
    fixture->generics[2].owner_kind = CM_HIR_DECL_GENERIC_ITEM;
    fixture->generics[2].owner_local = 3u;
    fixture->generics[2].index = 1u;
    fixture->generics[2].kind = CM_HIR_DECL_GENERIC_CONST;
    fixture->generics[2].name = (CmHirDeclarationString)S("N");
    fixture->generics[2].declared_type = 2u;
    fixture->generics[3].owner_kind = CM_HIR_DECL_GENERIC_ITEM;
    fixture->generics[3].owner_local = 4u;
    fixture->generics[3].index = 0u;
    fixture->generics[3].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[3].is_relaxed_sized = 1u;
    fixture->generics[3].name = (CmHirDeclarationString)S("DATA");
    metadata->generics = fixture->generics;
    metadata->generic_count = 4u;

    fixture->types[0].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[0].primitive = CM_HIR_DECL_PRIMITIVE_UNIT;
    fixture->types[1].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[1].primitive = CM_HIR_DECL_PRIMITIVE_USIZE;
    fixture->types[2].kind = CM_HIR_DECL_TYPE_GENERIC;
    fixture->types[2].generic_local = 1u;
    fixture->types[3].kind = CM_HIR_DECL_TYPE_GENERIC;
    fixture->types[3].generic_local = 2u;
    fixture->types[4].kind = CM_HIR_DECL_TYPE_GENERIC;
    fixture->types[4].generic_local = 4u;
    fixture->types[5].kind = CM_HIR_DECL_TYPE_NAMED_ADT;
    fixture->types[5].item_local = 1u;
    fixture->types[6].kind = CM_HIR_DECL_TYPE_SELF;
    fixture->types[6].self_trait_local = 1u;
    fixture->types[7].kind = CM_HIR_DECL_TYPE_REFERENCE;
    fixture->types[7].child_type = 7u;
    fixture->types[7].mutability = CM_HIR_DECL_MUTABLE;
    fixture->types[7].region.kind = CM_HIR_DECL_REGION_ERASED;
    fixture->maybe_arguments[0] = 4u;
    fixture->types[8].kind = CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION;
    fixture->types[8].item_local = 2u;
    fixture->types[8].argument_count = 1u;
    fixture->types[8].argument_types = fixture->maybe_arguments;
    fixture->types[9].kind = CM_HIR_DECL_TYPE_ARRAY;
    fixture->types[9].child_type = 9u;
    fixture->types[9].array_length_kind =
        CM_HIR_DECL_ARRAY_LENGTH_CONST_PARAMETER;
    fixture->types[9].array_length_generic_local = 3u;
    fixture->polymorphic_arguments[0] = 10u;
    fixture->types[10].kind = CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION;
    fixture->types[10].item_local = 4u;
    fixture->types[10].argument_count = 1u;
    fixture->types[10].argument_types = fixture->polymorphic_arguments;
    metadata->types = fixture->types;
    metadata->type_count = 11u;

    fixture->index_fields[0].name = (CmHirDeclarationString)S("start");
    fixture->index_fields[0].visibility.kind =
        CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->index_fields[0].source_ordinal = 0u;
    fixture->index_fields[0].type_local = 2u;
    fixture->index_fields[1].name = (CmHirDeclarationString)S("end");
    fixture->index_fields[1].visibility.kind =
        CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->index_fields[1].source_ordinal = 1u;
    fixture->index_fields[1].type_local = 2u;
    fixture->items[0].kind = CM_HIR_DECL_ITEM_STRUCT;
    fixture->items[0].owner_module = 1u;
    fixture->items[0].name = (CmHirDeclarationString)S("IndexRange");
    fixture->items[0].visibility.kind = CM_HIR_DECL_VISIBILITY_CRATE;
    fixture->items[0].source_ordinal = 1u;
    fixture->items[0].aggregate_form = CM_HIR_DECL_AGGREGATE_NAMED;
    fixture->items[0].aggregate_repr = CM_HIR_DECL_AGGREGATE_REPR_RUST;
    fixture->items[0].field_count = 2u;
    fixture->items[0].fields = fixture->index_fields;

    fixture->maybe_fields[0].name = (CmHirDeclarationString)S("value");
    fixture->maybe_fields[0].visibility.kind =
        CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->maybe_fields[0].source_ordinal = 0u;
    fixture->maybe_fields[0].type_local = 3u;
    fixture->items[1].kind = CM_HIR_DECL_ITEM_STRUCT;
    fixture->items[1].owner_module = 1u;
    fixture->items[1].name = (CmHirDeclarationString)S("MaybeUninit");
    fixture->items[1].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[1].source_ordinal = 2u;
    fixture->items[1].generic_start = 1u;
    fixture->items[1].generic_count = 1u;
    fixture->items[1].aggregate_form = CM_HIR_DECL_AGGREGATE_NAMED;
    fixture->items[1].aggregate_repr = CM_HIR_DECL_AGGREGATE_REPR_RUST;
    fixture->items[1].field_count = 1u;
    fixture->items[1].fields = fixture->maybe_fields;

    fixture->into_iter_fields[0].name =
        (CmHirDeclarationString)S("inner");
    fixture->into_iter_fields[0].visibility.kind =
        CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->into_iter_fields[0].source_ordinal = 0u;
    fixture->into_iter_fields[0].type_local = 11u;
    fixture->items[2].kind = CM_HIR_DECL_ITEM_STRUCT;
    fixture->items[2].owner_module = 2u;
    fixture->items[2].name = (CmHirDeclarationString)S("IntoIter");
    fixture->items[2].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[2].source_ordinal = 1u;
    fixture->items[2].generic_start = 2u;
    fixture->items[2].generic_count = 2u;
    fixture->items[2].aggregate_form = CM_HIR_DECL_AGGREGATE_NAMED;
    fixture->items[2].aggregate_repr = CM_HIR_DECL_AGGREGATE_REPR_RUST;
    fixture->items[2].aggregate_flags =
        CM_HIR_DECL_AGGREGATE_HAS_DIAGNOSTIC_ITEM
        | CM_HIR_DECL_AGGREGATE_RUSTC_INSIGNIFICANT_DTOR;
    fixture->items[2].field_count = 1u;
    fixture->items[2].fields = fixture->into_iter_fields;
    fixture->items[2].diagnostic_item =
        (CmHirDeclarationString)S("ArrayIntoIter");

    fixture->polymorphic_fields[0].name =
        (CmHirDeclarationString)S("alive");
    fixture->polymorphic_fields[0].visibility.kind =
        CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->polymorphic_fields[0].source_ordinal = 0u;
    fixture->polymorphic_fields[0].type_local = 6u;
    fixture->polymorphic_fields[1].name =
        (CmHirDeclarationString)S("data");
    fixture->polymorphic_fields[1].visibility.kind =
        CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->polymorphic_fields[1].source_ordinal = 1u;
    fixture->polymorphic_fields[1].type_local = 5u;
    fixture->items[3].kind = CM_HIR_DECL_ITEM_STRUCT;
    fixture->items[3].owner_module = 3u;
    fixture->items[3].name =
        (CmHirDeclarationString)S("PolymorphicIter");
    fixture->items[3].visibility.kind =
        CM_HIR_DECL_VISIBILITY_RESTRICTED;
    fixture->items[3].visibility.restriction_module = 2u;
    fixture->items[3].source_ordinal = 2u;
    fixture->items[3].generic_start = 4u;
    fixture->items[3].generic_count = 1u;
    fixture->items[3].predicate_start = 1u;
    fixture->items[3].predicate_count = 1u;
    fixture->items[3].aggregate_form = CM_HIR_DECL_AGGREGATE_NAMED;
    fixture->items[3].aggregate_repr = CM_HIR_DECL_AGGREGATE_REPR_RUST;
    fixture->items[3].field_count = 2u;
    fixture->items[3].fields = fixture->polymorphic_fields;
    metadata->items = fixture->items;
    metadata->item_count = 4u;

    fixture->predicates[0].owner_kind =
        CM_HIR_DECL_PREDICATE_OWNER_ITEM;
    fixture->predicates[0].owner_item = 4u;
    fixture->predicates[0].ordinal = 0u;
    fixture->predicates[0].subject_type = 5u;
    fixture->predicates[0].trait_local = 1u;
    metadata->predicates = fixture->predicates;
    metadata->predicate_count = 1u;

    fixture->method_parameters[0] = 8u;
    fixture->method_parameters[1] = 6u;
    fixture->associated_items[0].kind =
        CM_HIR_DECL_ASSOCIATED_METHOD;
    fixture->associated_items[0].parent_kind =
        CM_HIR_DECL_ASSOCIATED_PARENT_NOMINAL;
    fixture->associated_items[0].parent_local = 1u;
    fixture->associated_items[0].name =
        (CmHirDeclarationString)S("partial_drop");
    fixture->associated_items[0].visibility.kind =
        CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->associated_items[0].source_ordinal = 2u;
    fixture->associated_items[0].receiver =
        CM_HIR_DECL_RECEIVER_REF_MUTABLE;
    fixture->associated_items[0].parameter_count = 2u;
    fixture->associated_items[0].parameter_types =
        fixture->method_parameters;
    fixture->associated_items[0].return_type = 1u;
    fixture->associated_items[0].abi = (CmHirDeclarationString)S("Rust");
    fixture->associated_items[0].safety = CM_HIR_DECL_SAFETY_UNSAFE;
    metadata->associated_items = fixture->associated_items;
    metadata->associated_count = 1u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[0].name = fixture->items[1].name;
    fixture->namespace_entries[0].target_kind = CM_HIR_DECL_TARGET_ITEM;
    fixture->namespace_entries[0].target_local = 2u;
    fixture->namespace_entries[0].export_ordinal = 2u;
    fixture->namespace_entries[1].owner_module = 2u;
    fixture->namespace_entries[1].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[1].name =
        (CmHirDeclarationString)S("ArrayIntoIter");
    fixture->namespace_entries[1].target_kind = CM_HIR_DECL_TARGET_ITEM;
    fixture->namespace_entries[1].target_local = 3u;
    fixture->namespace_entries[1].export_ordinal = 2u;
    fixture->namespace_entries[2].owner_module = 2u;
    fixture->namespace_entries[2].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[2].name = fixture->items[2].name;
    fixture->namespace_entries[2].target_kind = CM_HIR_DECL_TARGET_ITEM;
    fixture->namespace_entries[2].target_local = 3u;
    fixture->namespace_entries[2].export_ordinal = 1u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 3u;
}

static void from_fn_fixture_init(FromFnFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;
    CmHirDeclarationAssociatedItem *associated;
    const uint8_t callable_flags = CM_HIR_DECL_TRAIT_HAS_LANG_ITEM
        | CM_HIR_DECL_TRAIT_IS_CONST
        | CM_HIR_DECL_TRAIT_RUSTC_PAREN_SUGAR
        | CM_HIR_DECL_TRAIT_FUNDAMENTAL;
    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("from_fn_like");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("decl-from-fn-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;
    fixture->modules[0].name = metadata->crate_name;
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 1u;

    fixture->traits[0].owner_module = 1u;
    fixture->traits[0].name = (CmHirDeclarationString)S("FnMut");
    fixture->traits[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->traits[0].source_ordinal = 1u;
    fixture->traits[0].generic_start = 1u;
    fixture->traits[0].generic_count = 1u;
    fixture->traits[0].predicate_start = 2u;
    fixture->traits[0].predicate_count = 1u;
    fixture->traits[0].associated_start = 1u;
    fixture->traits[0].associated_count = 1u;
    fixture->traits[0].safety = CM_HIR_DECL_SAFETY_SAFE;
    fixture->traits[0].flags = callable_flags;
    fixture->traits[0].lang_item = (CmHirDeclarationString)S("fn_mut");
    fixture->traits[0].supertrait_count = 1u;
    fixture->traits[0].supertraits = fixture->supertraits;
    fixture->supertrait_arguments[0] = 2u;
    fixture->supertraits[0].modifier = CM_HIR_DECL_SUPERTRAIT_REQUIRED;
    fixture->supertraits[0].trait_local = 2u;
    fixture->supertraits[0].argument_count = 1u;
    fixture->supertraits[0].argument_types = fixture->supertrait_arguments;

    fixture->traits[1].owner_module = 1u;
    fixture->traits[1].name = (CmHirDeclarationString)S("FnOnce");
    fixture->traits[1].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->traits[1].source_ordinal = 2u;
    fixture->traits[1].generic_start = 2u;
    fixture->traits[1].generic_count = 1u;
    fixture->traits[1].predicate_start = 3u;
    fixture->traits[1].predicate_count = 1u;
    fixture->traits[1].associated_start = 2u;
    fixture->traits[1].associated_count = 2u;
    fixture->traits[1].safety = CM_HIR_DECL_SAFETY_SAFE;
    fixture->traits[1].flags = callable_flags;
    fixture->traits[1].lang_item = (CmHirDeclarationString)S("fn_once");

    fixture->traits[2].owner_module = 1u;
    fixture->traits[2].name = (CmHirDeclarationString)S("Tuple");
    fixture->traits[2].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->traits[2].source_ordinal = 3u;
    fixture->traits[2].safety = CM_HIR_DECL_SAFETY_SAFE;
    fixture->traits[2].flags = CM_HIR_DECL_TRAIT_HAS_LANG_ITEM
        | CM_HIR_DECL_TRAIT_DENY_EXPLICIT_IMPL
        | CM_HIR_DECL_TRAIT_DO_NOT_IMPLEMENT_VIA_OBJECT;
    fixture->traits[2].lang_item =
        (CmHirDeclarationString)S("tuple_trait");
    metadata->traits = fixture->traits;
    metadata->trait_count = 3u;

    fixture->generics[0].owner_kind = CM_HIR_DECL_GENERIC_NOMINAL;
    fixture->generics[0].owner_local = 1u;
    fixture->generics[0].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[0].name = (CmHirDeclarationString)S("Args");
    fixture->generics[1] = fixture->generics[0];
    fixture->generics[1].owner_local = 2u;
    fixture->generics[2].owner_kind = CM_HIR_DECL_GENERIC_VALUE;
    fixture->generics[2].owner_local = 1u;
    fixture->generics[2].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[2].name = (CmHirDeclarationString)S("T");
    fixture->generics[3].owner_kind = CM_HIR_DECL_GENERIC_VALUE;
    fixture->generics[3].owner_local = 1u;
    fixture->generics[3].index = 1u;
    fixture->generics[3].kind = CM_HIR_DECL_GENERIC_CONST;
    fixture->generics[3].name = (CmHirDeclarationString)S("N");
    fixture->generics[3].declared_type = 1u;
    fixture->generics[4].owner_kind = CM_HIR_DECL_GENERIC_VALUE;
    fixture->generics[4].owner_local = 1u;
    fixture->generics[4].index = 2u;
    fixture->generics[4].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[4].name = (CmHirDeclarationString)S("F");
    metadata->generics = fixture->generics;
    metadata->generic_count = 5u;

    fixture->types[0].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[0].primitive = CM_HIR_DECL_PRIMITIVE_USIZE;
    fixture->types[1].kind = CM_HIR_DECL_TYPE_GENERIC;
    fixture->types[1].generic_local = 1u;
    fixture->types[2].kind = CM_HIR_DECL_TYPE_GENERIC;
    fixture->types[2].generic_local = 2u;
    fixture->types[3].kind = CM_HIR_DECL_TYPE_GENERIC;
    fixture->types[3].generic_local = 3u;
    fixture->types[4].kind = CM_HIR_DECL_TYPE_GENERIC;
    fixture->types[4].generic_local = 5u;
    fixture->types[5].kind = CM_HIR_DECL_TYPE_SELF;
    fixture->types[5].self_trait_local = 1u;
    fixture->types[6].kind = CM_HIR_DECL_TYPE_SELF;
    fixture->types[6].self_trait_local = 2u;
    fixture->types[7].kind = CM_HIR_DECL_TYPE_REFERENCE;
    fixture->types[7].child_type = 6u;
    fixture->types[7].mutability = CM_HIR_DECL_MUTABLE;
    fixture->types[7].region.kind = CM_HIR_DECL_REGION_ERASED;
    fixture->tuple_elements[0] = 1u;
    fixture->types[8].kind = CM_HIR_DECL_TYPE_TUPLE;
    fixture->types[8].element_count = 1u;
    fixture->types[8].element_types = fixture->tuple_elements;
    fixture->types[9].kind = CM_HIR_DECL_TYPE_ARRAY;
    fixture->types[9].child_type = 4u;
    fixture->types[9].array_length_kind =
        CM_HIR_DECL_ARRAY_LENGTH_CONST_PARAMETER;
    fixture->types[9].array_length_generic_local = 4u;
    fixture->projection_mut_arguments[0] = 2u;
    fixture->types[10].kind = CM_HIR_DECL_TYPE_PROJECTION;
    fixture->types[10].projection_self_type = 6u;
    fixture->types[10].projection_trait_local = 2u;
    fixture->types[10].projection_associated_local = 2u;
    fixture->types[10].projection_argument_count = 1u;
    fixture->types[10].projection_argument_types =
        fixture->projection_mut_arguments;
    fixture->projection_once_arguments[0] = 3u;
    fixture->types[11].kind = CM_HIR_DECL_TYPE_PROJECTION;
    fixture->types[11].projection_self_type = 7u;
    fixture->types[11].projection_trait_local = 2u;
    fixture->types[11].projection_associated_local = 2u;
    fixture->types[11].projection_argument_count = 1u;
    fixture->types[11].projection_argument_types =
        fixture->projection_once_arguments;
    metadata->types = fixture->types;
    metadata->type_count = 12u;

    fixture->call_mut_parameters[0] = 8u;
    fixture->call_mut_parameters[1] = 2u;
    associated = &fixture->associated_items[0];
    associated->kind = CM_HIR_DECL_ASSOCIATED_METHOD;
    associated->parent_kind = CM_HIR_DECL_ASSOCIATED_PARENT_NOMINAL;
    associated->parent_local = 1u;
    associated->name = (CmHirDeclarationString)S("call_mut");
    associated->visibility.kind = CM_HIR_DECL_VISIBILITY_PRIVATE;
    associated->source_ordinal = 4u;
    associated->receiver = CM_HIR_DECL_RECEIVER_REF_MUTABLE;
    associated->parameter_count = 2u;
    associated->parameter_types = fixture->call_mut_parameters;
    associated->return_type = 11u;
    associated->abi = (CmHirDeclarationString)S("rust-call");

    associated = &fixture->associated_items[1];
    associated->kind = CM_HIR_DECL_ASSOCIATED_TYPE;
    associated->parent_kind = CM_HIR_DECL_ASSOCIATED_PARENT_NOMINAL;
    associated->parent_local = 2u;
    associated->name = (CmHirDeclarationString)S("Output");
    associated->visibility.kind = CM_HIR_DECL_VISIBILITY_PRIVATE;
    associated->source_ordinal = 5u;
    associated->flags = CM_HIR_DECL_ASSOCIATED_HAS_LANG_ITEM;
    associated->lang_item =
        (CmHirDeclarationString)S("fn_once_output");

    fixture->call_once_parameters[0] = 7u;
    fixture->call_once_parameters[1] = 3u;
    associated = &fixture->associated_items[2];
    associated->kind = CM_HIR_DECL_ASSOCIATED_METHOD;
    associated->parent_kind = CM_HIR_DECL_ASSOCIATED_PARENT_NOMINAL;
    associated->parent_local = 2u;
    associated->name = (CmHirDeclarationString)S("call_once");
    associated->visibility.kind = CM_HIR_DECL_VISIBILITY_PRIVATE;
    associated->source_ordinal = 6u;
    associated->receiver = CM_HIR_DECL_RECEIVER_VALUE;
    associated->parameter_count = 2u;
    associated->parameter_types = fixture->call_once_parameters;
    associated->return_type = 12u;
    associated->abi = (CmHirDeclarationString)S("rust-call");
    metadata->associated_items = fixture->associated_items;
    metadata->associated_count = 3u;

    fixture->value_predicate_arguments[0] = 9u;
    fixture->value_equalities[0].associated_local = 2u;
    fixture->value_equalities[0].value_type = 4u;
    fixture->predicates[0].owner_kind = CM_HIR_DECL_PREDICATE_OWNER_VALUE;
    fixture->predicates[0].owner_value = 1u;
    fixture->predicates[0].subject_type = 5u;
    fixture->predicates[0].trait_local = 1u;
    fixture->predicates[0].argument_count = 1u;
    fixture->predicates[0].argument_types = fixture->value_predicate_arguments;
    fixture->predicates[0].equality_count = 1u;
    fixture->predicates[0].equalities = fixture->value_equalities;
    fixture->predicates[1].owner_kind = CM_HIR_DECL_PREDICATE_OWNER_NOMINAL;
    fixture->predicates[1].owner_nominal = 1u;
    fixture->predicates[1].subject_type = 2u;
    fixture->predicates[1].trait_local = 3u;
    fixture->predicates[2].owner_kind = CM_HIR_DECL_PREDICATE_OWNER_NOMINAL;
    fixture->predicates[2].owner_nominal = 2u;
    fixture->predicates[2].subject_type = 3u;
    fixture->predicates[2].trait_local = 3u;
    metadata->predicates = fixture->predicates;
    metadata->predicate_count = 3u;

    fixture->value_parameters[0] = 5u;
    fixture->values[0].kind = CM_HIR_DECL_VALUE_FUNCTION;
    fixture->values[0].owner_module = 1u;
    fixture->values[0].name = (CmHirDeclarationString)S("from_fn");
    fixture->values[0].source_ordinal = 7u;
    fixture->values[0].generic_start = 3u;
    fixture->values[0].generic_count = 3u;
    fixture->values[0].predicate_start = 1u;
    fixture->values[0].predicate_count = 1u;
    fixture->values[0].parameter_count = 1u;
    fixture->values[0].parameter_types = fixture->value_parameters;
    fixture->values[0].return_type = 10u;
    fixture->values[0].has_body = 1u;
    metadata->values = fixture->values;
    metadata->value_count = 1u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind = CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[0].name = fixture->traits[0].name;
    fixture->namespace_entries[0].target_kind = CM_HIR_DECL_TARGET_NOMINAL;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 1u;
    fixture->namespace_entries[1] = fixture->namespace_entries[0];
    fixture->namespace_entries[1].name = fixture->traits[1].name;
    fixture->namespace_entries[1].target_local = 2u;
    fixture->namespace_entries[1].export_ordinal = 2u;
    fixture->namespace_entries[2] = fixture->namespace_entries[0];
    fixture->namespace_entries[2].name = fixture->traits[2].name;
    fixture->namespace_entries[2].target_local = 3u;
    fixture->namespace_entries[2].export_ordinal = 3u;
    fixture->namespace_entries[3].owner_module = 1u;
    fixture->namespace_entries[3].namespace_kind = CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[3].name = fixture->values[0].name;
    fixture->namespace_entries[3].target_kind = CM_HIR_DECL_TARGET_VALUE;
    fixture->namespace_entries[3].target_local = 1u;
    fixture->namespace_entries[3].export_ordinal = 7u;
    fixture->namespace_entries[4] = fixture->namespace_entries[3];
    fixture->namespace_entries[4].name =
        (CmHirDeclarationString)S("from_fn_alias");
    fixture->namespace_entries[4].export_ordinal = 8u;
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

static void test_structural_type_family(void)
{
    StructuralFixture first;
    StructuralFixture second;
    CmHirDeclarationMetadata decoded;
    CmByteBuf encoded;
    CmByteBuf repeated;
    CmByteBuf rebuilt;
    CmByteBuf bad;
    size_t record;
    size_t payload;

    structural_fixture_init(&first);
    structural_fixture_init(&second);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&repeated);
    cm_byte_buf_init(&rebuilt);
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&first.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&second.metadata, &repeated)
        == CM_HIR_DECL_METADATA_OK);
    assert(encoded.len == repeated.len
        && memcmp(encoded.data, repeated.data, encoded.len) == 0);

    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(decoded.generic_count == 3u
        && decoded.generics[1].owner_kind == CM_HIR_DECL_GENERIC_ITEM
        && decoded.items[0].generic_start == 2u
        && decoded.items[0].generic_count == 1u
        && decoded.type_count == 6u
        && decoded.types[2].kind == CM_HIR_DECL_TYPE_SLICE
        && decoded.types[2].child_type == 1u
        && decoded.types[3].kind == CM_HIR_DECL_TYPE_RAW_POINTER
        && decoded.types[3].mutability == CM_HIR_DECL_IMMUTABLE
        && decoded.types[3].child_type == 2u
        && decoded.types[4].kind
            == CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION
        && decoded.types[4].item_local == 1u
        && decoded.types[4].argument_count == 1u
        && decoded.types[4].argument_types[0] == 2u
        && decoded.types[5].kind == CM_HIR_DECL_TYPE_REFERENCE
        && decoded.types[5].region.kind == CM_HIR_DECL_REGION_STATIC
        && decoded.types[5].mutability == CM_HIR_DECL_IMMUTABLE
        && decoded.types[5].child_type == 3u);
    assert(cm_hir_declaration_metadata_encode(&decoded, &rebuilt)
        == CM_HIR_DECL_METADATA_OK);
    assert(encoded.len == rebuilt.len
        && memcmp(encoded.data, rebuilt.data, encoded.len) == 0);

    structural_fixture_init(&first);
    first.generics[1].owner_kind = CM_HIR_DECL_GENERIC_VALUE;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    structural_fixture_init(&first);
    first.items[0].generic_start = 0u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    structural_fixture_init(&first);
    first.types[2].child_type = 0u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    structural_fixture_init(&first);
    first.types[2].child_type = 4u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    structural_fixture_init(&first);
    first.types[3].mutability = 0u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    structural_fixture_init(&first);
    first.types[3].mutability = CM_HIR_DECL_MUTABLE;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_OK);
    structural_fixture_init(&first);
    first.types[5].region.kind = CM_HIR_DECL_REGION_EARLY_BOUND;
    first.types[5].region.generic_local = 3u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    structural_fixture_init(&first);
    first.types[5].region.generic_local = 3u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    structural_fixture_init(&first);
    first.types[4].argument_count = 2u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    structural_fixture_init(&first);
    first.types[4].argument_types = NULL;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    structural_fixture_init(&first);
    first.types[4].argument_count =
        (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES + 1u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    structural_fixture_init(&first);
    first.application_arguments[0] = 6u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    structural_fixture_init(&first);
    first.application_arguments[0] = 3u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    structural_fixture_init(&first);
    first.types[4].kind = CM_HIR_DECL_TYPE_NAMED_ADT;
    first.types[4].argument_count = 0u;
    first.types[4].argument_types = NULL;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    structural_fixture_init(&first);
    first.values[0].return_type = 4u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    structural_fixture_init(&first);
    first.types[2].kind = CM_HIR_DECL_TYPE_SELF;
    first.types[2].child_type = 0u;
    first.types[2].self_trait_local = 1u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    structural_fixture_init(&first);
    memset(first.types, 0, sizeof(first.types));
    first.types[0].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    first.types[0].primitive = CM_HIR_DECL_PRIMITIVE_U8;
    first.types[1].kind = CM_HIR_DECL_TYPE_GENERIC;
    first.types[1].generic_local = 2u; /* Wrap's W, not shape's X. */
    first.types[2].kind = CM_HIR_DECL_TYPE_GENERIC;
    first.types[2].generic_local = 3u;
    first.application_arguments[0] = 2u;
    first.types[3].kind = CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION;
    first.types[3].item_local = 1u;
    first.types[3].argument_count = 1u;
    first.types[3].argument_types = first.application_arguments;
    first.types[4].kind = CM_HIR_DECL_TYPE_RAW_POINTER;
    first.types[4].mutability = CM_HIR_DECL_IMMUTABLE;
    first.types[4].child_type = 4u;
    first.types[5].kind = CM_HIR_DECL_TYPE_REFERENCE;
    first.types[5].region.kind = CM_HIR_DECL_REGION_STATIC;
    first.types[5].mutability = CM_HIR_DECL_IMMUTABLE;
    first.types[5].child_type = 5u;
    first.parameters[0] = 6u;
    first.values[0].parameter_count = 1u;
    first.values[0].return_type = 1u;
    first.predicates[0].subject_type = 3u;
    /* Canonical &'static *const Wrap<W> must not escape ITEM scope into f<X>. */
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);

    /* All checksummed malformed structural records fail transactionally. */
    bad = copy_bytes(&encoded);
    record = type_record_offset(&bad, UINT32_C(2));
    put_u32(bad.data + record + 4u, UINT32_C(0));
    recompute_crc(&bad);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    record = type_record_offset(&bad, UINT32_C(3));
    bad.data[record + 4u] = UINT8_C(0);
    recompute_crc(&bad);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    record = type_record_offset(&bad, UINT32_C(4));
    put_u32(bad.data + record + 8u, UINT32_C(0));
    recompute_crc(&bad);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    record = type_record_offset(&bad, UINT32_C(4));
    put_u32(bad.data + record + 8u,
        (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES + 1u);
    recompute_crc(&bad);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    record = type_record_offset(&bad, UINT32_C(4));
    put_u32(bad.data + record + 12u, UINT32_C(6));
    recompute_crc(&bad);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    record = type_record_offset(&bad, UINT32_C(5));
    bad.data[record + 4u] = CM_HIR_DECL_REGION_EARLY_BOUND;
    recompute_crc(&bad);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    record = type_record_offset(&bad, UINT32_C(2));
    bad.data[record] = CM_HIR_DECL_TYPE_SELF;
    recompute_crc(&bad);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    record = item_record_offset(&bad, UINT32_C(0));
    payload = skip_string(&bad, record + 8u);
    put_u32(bad.data + payload + 12u, UINT32_C(0));
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    /* The same hostile graph is rejected after bounded wire parsing. Its
     * records occupy exactly the valid fixture's TYPE section length. */
    bad = copy_bytes(&encoded);
    payload = section_offset(&bad, "TYPE") + 12u;
    assert(get_u32(bad.data + payload) == UINT32_C(6));
    record = payload + 4u;
    assert(get_u64(bad.data + payload - 8u) == UINT64_C(80));
    memset(bad.data + record, 0, 76u);
    bad.data[record] = CM_HIR_DECL_TYPE_PRIMITIVE;
    bad.data[record + 4u] = CM_HIR_DECL_PRIMITIVE_U8;
    record += 8u;
    bad.data[record] = CM_HIR_DECL_TYPE_GENERIC;
    put_u32(bad.data + record + 4u, UINT32_C(2));
    record += 8u;
    bad.data[record] = CM_HIR_DECL_TYPE_GENERIC;
    put_u32(bad.data + record + 4u, UINT32_C(3));
    record += 8u;
    bad.data[record] = CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION;
    put_u32(bad.data + record + 4u, UINT32_C(1));
    put_u32(bad.data + record + 8u, UINT32_C(1));
    put_u32(bad.data + record + 12u, UINT32_C(2));
    record += 16u;
    bad.data[record] = CM_HIR_DECL_TYPE_RAW_POINTER;
    bad.data[record + 4u] = CM_HIR_DECL_IMMUTABLE;
    put_u32(bad.data + record + 8u, UINT32_C(4));
    record += 12u;
    bad.data[record] = CM_HIR_DECL_TYPE_REFERENCE;
    bad.data[record + 4u] = CM_HIR_DECL_REGION_STATIC;
    bad.data[record + 16u] = CM_HIR_DECL_IMMUTABLE;
    put_u32(bad.data + record + 20u, UINT32_C(5));
    payload = section_offset(&bad, "PRED") + 12u;
    put_u32(bad.data + payload + 24u, UINT32_C(3));
    recompute_crc(&bad);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&rebuilt);
    cm_byte_buf_destroy(&repeated);
    cm_byte_buf_destroy(&encoded);
}

static void test_enum_family(void)
{
    EnumFixture first;
    EnumFixture second;
    CmHirDeclarationMetadata decoded;
    CmByteBuf encoded;
    CmByteBuf repeated;
    CmByteBuf rebuilt;
    CmByteBuf bad;
    size_t item_record;
    size_t payload;
    size_t variant_record;
    size_t variant_payload;

    enum_fixture_init(&first);
    enum_fixture_init(&second);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&repeated);
    cm_byte_buf_init(&rebuilt);
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&first.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&second.metadata, &repeated)
        == CM_HIR_DECL_METADATA_OK);
    assert(encoded.len == repeated.len
        && memcmp(encoded.data, repeated.data, encoded.len) == 0);

    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(decoded.item_count == 1u
        && decoded.items[0].kind == CM_HIR_DECL_ITEM_ENUM
        && decoded.items[0].enum_repr_primitive
            == CM_HIR_DECL_PRIMITIVE_U8
        && decoded.items[0].variant_count == 128u
        && decoded.items[0].variants != NULL
        && decoded.items[0].variants[0].kind
            == CM_HIR_DECL_VARIANT_UNIT
        && decoded.items[0].variants[0].source_ordinal == 1u
        && decoded.items[0].variants[0].discriminant_primitive
            == CM_HIR_DECL_PRIMITIVE_ISIZE
        && decoded.items[0].variants[127].source_ordinal == 128u
        && decoded.items[0].variants[127].discriminant_low
            == UINT64_C(127)
        && decoded.items[0].variants[127].discriminant_high
            == UINT64_C(0)
        && decoded.type_count == 0u
        && decoded.namespace_count == 2u
        && decoded.namespace_entries[0].namespace_kind
            == CM_HIR_DECL_NAMESPACE_TYPE
        && decoded.namespace_entries[1].target_local == 1u);
    assert(cm_hir_declaration_metadata_encode(&decoded, &rebuilt)
        == CM_HIR_DECL_METADATA_OK);
    assert(encoded.len == rebuilt.len
        && memcmp(encoded.data, rebuilt.data, encoded.len) == 0);

    enum_fixture_init(&first);
    first.items[0].enum_repr_primitive = CM_HIR_DECL_PRIMITIVE_U16;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_OK);
    first.items[0].enum_repr_primitive = CM_HIR_DECL_PRIMITIVE_U128;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    enum_fixture_init(&first);
    first.items[0].alias_target_type = 1u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    enum_fixture_init(&first);
    first.items[0].variant_count = 0u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    enum_fixture_init(&first);
    first.items[0].variants = NULL;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    enum_fixture_init(&first);
    first.items[0].variant_count =
        (uint32_t)CM_HIR_DECL_METADATA_MAX_VARIANTS + 1u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    enum_fixture_init(&first);
    first.variants[1].kind = UINT8_C(2);
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    enum_fixture_init(&first);
    first.variants[1].source_ordinal = 1u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    enum_fixture_init(&first);
    first.variants[1].name = first.variants[0].name;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    enum_fixture_init(&first);
    first.variants[1].discriminant_primitive = CM_HIR_DECL_PRIMITIVE_U8;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    enum_fixture_init(&first);
    first.variants[1].discriminant_low = UINT64_C(0);
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    enum_fixture_init(&first);
    first.variants[1].discriminant_low = UINT64_C(256);
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    enum_fixture_init(&first);
    first.variants[1].discriminant_high = UINT64_C(1);
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    enum_fixture_init(&first);
    first.items[0].generic_start = 1u;
    first.items[0].generic_count = 1u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    enum_fixture_init(&first);
    first.items[1].kind = CM_HIR_DECL_ITEM_TYPE_ALIAS;
    first.items[1].owner_module = 1u;
    first.items[1].name = (CmHirDeclarationString)S("AsciiCharAlias");
    first.items[1].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    first.items[1].source_ordinal = 2u;
    first.items[1].alias_target_type = 1u;
    first.types[0].kind = CM_HIR_DECL_TYPE_NAMED_ADT;
    first.types[0].item_local = 1u;
    first.namespace_entries[1].name = first.items[1].name;
    first.namespace_entries[1].target_local = 2u;
    first.metadata.item_count = 2u;
    first.metadata.type_count = 1u;
    first.metadata.types = first.types;
    /* Enum nominal types are valid signature nodes, but TYPE_ALIAS scope
     * normalization is not yet represented by the bounded artifact. */
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    enum_fixture_init(&first);
    first.namespace_entries[2] = first.namespace_entries[0];
    first.namespace_entries[2].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    first.metadata.namespace_count = 3u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);

    item_record = item_record_offset(&encoded, UINT32_C(0));
    payload = skip_string(&encoded, item_record + 8u) + 44u;
    assert(encoded.data[payload] == CM_HIR_DECL_PRIMITIVE_U8
        && get_u32(encoded.data + payload + 4u) == UINT32_C(128));

    bad = copy_bytes(&encoded);
    bad.data[payload] = CM_HIR_DECL_PRIMITIVE_U128;
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    put_u32(bad.data + payload + 4u,
        (uint32_t)CM_HIR_DECL_METADATA_MAX_VARIANTS + 1u);
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    variant_record = enum_variant_record_offset(&bad, UINT32_C(0),
        UINT32_C(1));
    bad.data[variant_record] = UINT8_C(2);
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    variant_record = enum_variant_record_offset(&bad, UINT32_C(0),
        UINT32_C(1));
    bad.data[variant_record + 1u] = CM_HIR_DECL_PRIMITIVE_U8;
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    variant_record = enum_variant_record_offset(&bad, UINT32_C(0),
        UINT32_C(1));
    variant_payload = skip_string(&bad, variant_record + 4u);
    put_u32(bad.data + variant_payload, UINT32_C(1));
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    variant_record = enum_variant_record_offset(&bad, UINT32_C(0),
        UINT32_C(1));
    variant_payload = skip_string(&bad, variant_record + 4u);
    put_u32(bad.data + variant_payload + 4u, UINT32_C(0));
    put_u32(bad.data + variant_payload + 8u, UINT32_C(0));
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    variant_record = enum_variant_record_offset(&bad, UINT32_C(0),
        UINT32_C(1));
    memset(bad.data + variant_record + 8u, (int)'0', 4u);
    bad.data[variant_record + 8u] = (unsigned char)'V';
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&rebuilt);
    cm_byte_buf_destroy(&repeated);
    cm_byte_buf_destroy(&encoded);
}

static void default_enum_fixture_init(EnumFixture *fixture)
{
    enum_fixture_init(fixture);
    fixture->items[0].name = (CmHirDeclarationString)S("BasicBlock");
    fixture->items[0].enum_repr_primitive = CM_HIR_DECL_ENUM_REPR_RUST;
    fixture->items[0].variant_count = 2u;
    fixture->items[0].diagnostic_item =
        (CmHirDeclarationString)S("mir_basic_block");
    fixture->variants[0].name = (CmHirDeclarationString)S("Normal");
    fixture->variants[0].discriminant_primitive =
        CM_HIR_DECL_VARIANT_DISCRIMINANT_IMPLICIT;
    fixture->variants[0].discriminant_low = UINT64_C(0);
    fixture->variants[0].discriminant_high = UINT64_C(0);
    fixture->variants[1].name = (CmHirDeclarationString)S("Cleanup");
    fixture->variants[1].discriminant_primitive =
        CM_HIR_DECL_VARIANT_DISCRIMINANT_IMPLICIT;
    fixture->variants[1].discriminant_low = UINT64_C(0);
    fixture->variants[1].discriminant_high = UINT64_C(0);

    fixture->items[1].kind = CM_HIR_DECL_ITEM_ENUM;
    fixture->items[1].owner_module = 1u;
    fixture->items[1].name =
        (CmHirDeclarationString)S("UnwindTerminateReason");
    fixture->items[1].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[1].source_ordinal = 2u;
    fixture->items[1].enum_repr_primitive = CM_HIR_DECL_ENUM_REPR_RUST;
    fixture->items[1].variant_count = 2u;
    fixture->items[1].variants = &fixture->variants[2];
    fixture->items[1].diagnostic_item =
        (CmHirDeclarationString)S("mir_unwind_terminate_reason");
    fixture->variants[2].kind = CM_HIR_DECL_VARIANT_UNIT;
    fixture->variants[2].name = (CmHirDeclarationString)S("Abi");
    fixture->variants[2].source_ordinal = 1u;
    fixture->variants[2].discriminant_primitive =
        CM_HIR_DECL_VARIANT_DISCRIMINANT_IMPLICIT;
    fixture->variants[2].discriminant_low = UINT64_C(0);
    fixture->variants[2].discriminant_high = UINT64_C(0);
    fixture->variants[3].kind = CM_HIR_DECL_VARIANT_UNIT;
    fixture->variants[3].name = (CmHirDeclarationString)S("InCleanup");
    fixture->variants[3].source_ordinal = 2u;
    fixture->variants[3].discriminant_primitive =
        CM_HIR_DECL_VARIANT_DISCRIMINANT_IMPLICIT;
    fixture->variants[3].discriminant_low = UINT64_C(0);
    fixture->variants[3].discriminant_high = UINT64_C(0);
    fixture->metadata.item_count = 2u;

    fixture->namespace_entries[0].name = fixture->items[0].name;
    fixture->namespace_entries[1].name =
        (CmHirDeclarationString)S("ReasonAbi");
    fixture->namespace_entries[1].target_kind =
        CM_HIR_DECL_TARGET_ENUM_VARIANT;
    fixture->namespace_entries[1].target_local = 3u;
    fixture->namespace_entries[1].export_ordinal = 3u;
    fixture->namespace_entries[2].owner_module = 1u;
    fixture->namespace_entries[2].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[2].name = fixture->items[1].name;
    fixture->namespace_entries[2].target_kind = CM_HIR_DECL_TARGET_ITEM;
    fixture->namespace_entries[2].target_local = 2u;
    fixture->namespace_entries[2].export_ordinal = 2u;
    fixture->namespace_entries[3] = fixture->namespace_entries[1];
    fixture->namespace_entries[3].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->metadata.namespace_count = 4u;
}

static void test_default_enum_variant_namespace(void)
{
    EnumFixture fixture;
    CmHirDeclarationMetadata decoded;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmByteBuf bad;
    size_t record;
    size_t target;
    size_t variant;
    size_t diagnostic;

    default_enum_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    record = item_record_offset(&encoded, UINT32_C(0));
    target = skip_string(&encoded, record + 8u) + 44u;
    assert(encoded.data[target + 1u] == UINT8_C(0));
    variant = enum_variant_record_offset(&encoded, UINT32_C(0),
        UINT32_C(0));
    assert(encoded.data[variant + 2u] == UINT8_C(0)
        && encoded.data[variant + 3u] == UINT8_C(0));
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(decoded.item_count == 2u
        && decoded.items[0].enum_repr_primitive
            == CM_HIR_DECL_ENUM_REPR_RUST
        && decoded.items[0].variant_count == 2u
        && decoded.items[0].variants[0].discriminant_primitive
            == CM_HIR_DECL_VARIANT_DISCRIMINANT_IMPLICIT
        && decoded.items[0].diagnostic_item.length
            == sizeof("mir_basic_block") - 1u
        && memcmp(decoded.items[0].diagnostic_item.data,
            "mir_basic_block", sizeof("mir_basic_block") - 1u) == 0
        && decoded.items[1].variant_count == 2u
        && decoded.items[1].diagnostic_item.length
            == sizeof("mir_unwind_terminate_reason") - 1u
        && decoded.namespace_count == 4u
        && decoded.namespace_entries[1].target_kind
            == CM_HIR_DECL_TARGET_ENUM_VARIANT
        && decoded.namespace_entries[1].target_local == 3u
        && decoded.namespace_entries[3].target_kind
            == CM_HIR_DECL_TARGET_ENUM_VARIANT
        && decoded.namespace_entries[3].target_local == 3u);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
        == CM_HIR_DECL_METADATA_OK);
    assert(encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

    default_enum_fixture_init(&fixture);
    fixture.items[0].diagnostic_item.data = NULL;
    fixture.items[0].diagnostic_item.length = 0u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    default_enum_fixture_init(&fixture);
    fixture.items[0].diagnostic_item = (CmHirDeclarationString)S("bad-item");
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    default_enum_fixture_init(&fixture);
    fixture.variants[0].discriminant_primitive =
        CM_HIR_DECL_PRIMITIVE_ISIZE;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    default_enum_fixture_init(&fixture);
    fixture.variants[0].discriminant_low = UINT64_C(1);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    default_enum_fixture_init(&fixture);
    fixture.items[0].enum_repr_primitive = CM_HIR_DECL_PRIMITIVE_U8;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    default_enum_fixture_init(&fixture);
    fixture.namespace_entries[1].target_local = 5u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    default_enum_fixture_init(&fixture);
    fixture.metadata.namespace_count = 3u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    default_enum_fixture_init(&fixture);
    fixture.namespace_entries[3].target_local = 4u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    default_enum_fixture_init(&fixture);
    fixture.namespace_entries[3].export_ordinal = 4u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);

    record = namespace_record_offset(&encoded, UINT32_C(1));
    target = skip_string(&encoded, record + 8u);
    assert(encoded.data[record + 5u] == CM_HIR_DECL_TARGET_ENUM_VARIANT
        && get_u32(encoded.data + target) == UINT32_C(3));
    bad = copy_bytes(&encoded);
    record = namespace_record_offset(&bad, UINT32_C(1));
    target = skip_string(&bad, record + 8u);
    put_u32(bad.data + target, UINT32_C(5));
    recompute_module_family_crc(&bad);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    record = namespace_record_offset(&bad, UINT32_C(1));
    bad.data[record + 5u] = UINT8_C(6);
    recompute_module_family_crc(&bad);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    variant = enum_variant_record_offset(&encoded, UINT32_C(0),
        UINT32_C(1));
    diagnostic = skip_string(&encoded, variant + 4u) + 20u;
    assert(get_u32(encoded.data + diagnostic)
        == (uint32_t)(sizeof("mir_basic_block") - 1u));
    bad = copy_bytes(&encoded);
    bad.data[diagnostic + 4u] = (unsigned char)'-';
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

static void generic_enum_fixture_init(GenericEnumFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;
    uint32_t index;
    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("generic_enums");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("decl-generic-enums-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;

    fixture->modules[0].name = metadata->crate_name;
    fixture->modules[1].parent_module = 1u;
    fixture->modules[1].name = (CmHirDeclarationString)S("option");
    fixture->modules[2].parent_module = 1u;
    fixture->modules[2].name = (CmHirDeclarationString)S("prelude");
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 3u;

    fixture->items[0].kind = CM_HIR_DECL_ITEM_ENUM;
    fixture->items[0].owner_module = 2u;
    fixture->items[0].name = (CmHirDeclarationString)S("Option");
    fixture->items[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[0].source_ordinal = 1u;
    fixture->items[0].generic_start = 1u;
    fixture->items[0].generic_count = 1u;
    fixture->items[0].enum_repr_primitive = CM_HIR_DECL_ENUM_REPR_RUST;
    fixture->items[0].variant_count = 2u;
    fixture->items[0].variants = &fixture->variants[0];
    fixture->items[0].diagnostic_item =
        (CmHirDeclarationString)S("Option");
    fixture->items[0].enum_flags = CM_HIR_DECL_ENUM_HAS_LANG_ITEM;
    fixture->items[0].enum_lang_item =
        (CmHirDeclarationString)S("option_type");

    fixture->variants[0].kind = CM_HIR_DECL_VARIANT_UNIT;
    fixture->variants[0].name = (CmHirDeclarationString)S("None");
    fixture->variants[0].source_ordinal = 0u;
    fixture->variants[0].discriminant_primitive =
        CM_HIR_DECL_VARIANT_DISCRIMINANT_IMPLICIT;
    fixture->variants[0].flags = CM_HIR_DECL_VARIANT_HAS_LANG_ITEM;
    fixture->variants[0].lang_item = (CmHirDeclarationString)S("none_ctor");
    fixture->variants[1].kind = CM_HIR_DECL_VARIANT_TUPLE;
    fixture->variants[1].name = (CmHirDeclarationString)S("Some");
    fixture->variants[1].source_ordinal = 1u;
    fixture->variants[1].discriminant_primitive =
        CM_HIR_DECL_VARIANT_DISCRIMINANT_IMPLICIT;
    fixture->variants[1].flags = CM_HIR_DECL_VARIANT_HAS_LANG_ITEM;
    fixture->variants[1].field_count = 1u;
    fixture->variants[1].fields = &fixture->variant_fields[0];
    fixture->variants[1].lang_item = (CmHirDeclarationString)S("some_ctor");
    fixture->variant_fields[0].source_ordinal = 0u;
    fixture->variant_fields[0].type_local = 3u;

    fixture->items[1].kind = CM_HIR_DECL_ITEM_ENUM;
    fixture->items[1].owner_module = 2u;
    fixture->items[1].name = (CmHirDeclarationString)S("Result");
    fixture->items[1].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[1].source_ordinal = 2u;
    fixture->items[1].generic_start = 2u;
    fixture->items[1].generic_count = 2u;
    fixture->items[1].enum_repr_primitive = CM_HIR_DECL_ENUM_REPR_RUST;
    fixture->items[1].variant_count = 2u;
    fixture->items[1].variants = &fixture->variants[2];
    fixture->items[1].diagnostic_item =
        (CmHirDeclarationString)S("Result");
    fixture->variants[2].kind = CM_HIR_DECL_VARIANT_TUPLE;
    fixture->variants[2].name = (CmHirDeclarationString)S("Ok");
    fixture->variants[2].source_ordinal = 0u;
    fixture->variants[2].discriminant_primitive =
        CM_HIR_DECL_VARIANT_DISCRIMINANT_IMPLICIT;
    fixture->variants[2].flags = CM_HIR_DECL_VARIANT_HAS_LANG_ITEM;
    fixture->variants[2].field_count = 1u;
    fixture->variants[2].fields = &fixture->variant_fields[1];
    fixture->variants[2].lang_item = (CmHirDeclarationString)S("ok_ctor");
    fixture->variant_fields[1].source_ordinal = 0u;
    fixture->variant_fields[1].type_local = 4u;
    fixture->variants[3].kind = CM_HIR_DECL_VARIANT_TUPLE;
    fixture->variants[3].name = (CmHirDeclarationString)S("Err");
    fixture->variants[3].source_ordinal = 1u;
    fixture->variants[3].discriminant_primitive =
        CM_HIR_DECL_VARIANT_DISCRIMINANT_IMPLICIT;
    fixture->variants[3].flags = CM_HIR_DECL_VARIANT_HAS_LANG_ITEM;
    fixture->variants[3].field_count = 1u;
    fixture->variants[3].fields = &fixture->variant_fields[2];
    fixture->variants[3].lang_item = (CmHirDeclarationString)S("err_ctor");
    fixture->variant_fields[2].source_ordinal = 0u;
    fixture->variant_fields[2].type_local = 5u;

    fixture->items[2].kind = CM_HIR_DECL_ITEM_STRUCT;
    fixture->items[2].owner_module = 2u;
    fixture->items[2].name = (CmHirDeclarationString)S("Tag");
    fixture->items[2].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[2].source_ordinal = 3u;
    fixture->items[2].aggregate_form = CM_HIR_DECL_AGGREGATE_NAMED;
    fixture->items[2].aggregate_repr = CM_HIR_DECL_AGGREGATE_REPR_RUST;
    fixture->items[2].aggregate_flags =
        CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM;
    fixture->items[2].field_count = 4u;
    fixture->items[2].fields = fixture->aggregate_fields;
    fixture->items[2].lang_item =
        (CmHirDeclarationString)S("aggregate_tag");
    for (index = 0u; index < 4u; ++index) {
        static unsigned char names[4][1] = { { 'a' }, { 'b' }, { 'c' },
            { 'd' } };
        fixture->aggregate_fields[index].name.data = names[index];
        fixture->aggregate_fields[index].name.length = 1u;
        fixture->aggregate_fields[index].visibility.kind =
            CM_HIR_DECL_VISIBILITY_PUBLIC;
        fixture->aggregate_fields[index].source_ordinal = index;
        fixture->aggregate_fields[index].type_local = 1u;
    }
    metadata->items = fixture->items;
    metadata->item_count = 3u;

    fixture->generics[0].owner_kind = CM_HIR_DECL_GENERIC_ITEM;
    fixture->generics[0].owner_local = 1u;
    fixture->generics[0].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[0].name = (CmHirDeclarationString)S("T");
    fixture->generics[1].owner_kind = CM_HIR_DECL_GENERIC_ITEM;
    fixture->generics[1].owner_local = 2u;
    fixture->generics[1].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[1].name = (CmHirDeclarationString)S("T");
    fixture->generics[2].owner_kind = CM_HIR_DECL_GENERIC_ITEM;
    fixture->generics[2].owner_local = 2u;
    fixture->generics[2].index = 1u;
    fixture->generics[2].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[2].name = (CmHirDeclarationString)S("E");
    metadata->generics = fixture->generics;
    metadata->generic_count = 3u;

    fixture->types[0].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[0].primitive = CM_HIR_DECL_PRIMITIVE_BOOL;
    fixture->types[1].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[1].primitive = CM_HIR_DECL_PRIMITIVE_U8;
    fixture->types[2].kind = CM_HIR_DECL_TYPE_GENERIC;
    fixture->types[2].generic_local = 1u;
    fixture->types[3].kind = CM_HIR_DECL_TYPE_GENERIC;
    fixture->types[3].generic_local = 2u;
    fixture->types[4].kind = CM_HIR_DECL_TYPE_GENERIC;
    fixture->types[4].generic_local = 3u;
    fixture->application_arguments[0] = 2u;
    fixture->types[5].kind = CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION;
    fixture->types[5].item_local = 1u;
    fixture->types[5].argument_count = 1u;
    fixture->types[5].argument_types = fixture->application_arguments;
    metadata->types = fixture->types;
    metadata->type_count = 6u;

    fixture->values[0].kind = CM_HIR_DECL_VALUE_STATIC;
    fixture->values[0].owner_module = 2u;
    fixture->values[0].name = (CmHirDeclarationString)S("OPTION_U8");
    fixture->values[0].source_ordinal = 4u;
    fixture->values[0].declared_type = 6u;
    fixture->values[0].mutability = CM_HIR_DECL_IMMUTABLE;
    fixture->values[0].has_body = 1u;
    metadata->values = fixture->values;
    metadata->value_count = 1u;

    fixture->namespace_entries[0] = (CmHirDeclarationNamespaceEntry){
        1u, CM_HIR_DECL_NAMESPACE_TYPE, fixture->modules[1].name,
        CM_HIR_DECL_TARGET_MODULE, 2u, 1u };
    fixture->namespace_entries[1] = (CmHirDeclarationNamespaceEntry){
        1u, CM_HIR_DECL_NAMESPACE_TYPE, fixture->modules[2].name,
        CM_HIR_DECL_TARGET_MODULE, 3u, 2u };
    fixture->namespace_entries[2] = (CmHirDeclarationNamespaceEntry){
        2u, CM_HIR_DECL_NAMESPACE_TYPE, fixture->items[0].name,
        CM_HIR_DECL_TARGET_ITEM, 1u, 1u };
    fixture->namespace_entries[3] = (CmHirDeclarationNamespaceEntry){
        2u, CM_HIR_DECL_NAMESPACE_TYPE, fixture->items[1].name,
        CM_HIR_DECL_TARGET_ITEM, 2u, 2u };
    fixture->namespace_entries[4] = (CmHirDeclarationNamespaceEntry){
        2u, CM_HIR_DECL_NAMESPACE_TYPE, fixture->items[2].name,
        CM_HIR_DECL_TARGET_ITEM, 3u, 3u };
    fixture->namespace_entries[5] = (CmHirDeclarationNamespaceEntry){
        2u, CM_HIR_DECL_NAMESPACE_VALUE, fixture->values[0].name,
        CM_HIR_DECL_TARGET_VALUE, 1u, 4u };
    fixture->namespace_entries[6] = (CmHirDeclarationNamespaceEntry){
        3u, CM_HIR_DECL_NAMESPACE_TYPE, fixture->variants[3].name,
        CM_HIR_DECL_TARGET_ENUM_VARIANT, 4u, 2u };
    fixture->namespace_entries[7] = (CmHirDeclarationNamespaceEntry){
        3u, CM_HIR_DECL_NAMESPACE_TYPE, fixture->variants[0].name,
        CM_HIR_DECL_TARGET_ENUM_VARIANT, 1u, 1u };
    fixture->namespace_entries[8] = (CmHirDeclarationNamespaceEntry){
        3u, CM_HIR_DECL_NAMESPACE_TYPE, fixture->variants[2].name,
        CM_HIR_DECL_TARGET_ENUM_VARIANT, 3u, 2u };
    fixture->namespace_entries[9] = (CmHirDeclarationNamespaceEntry){
        3u, CM_HIR_DECL_NAMESPACE_TYPE, fixture->items[0].name,
        CM_HIR_DECL_TARGET_ITEM, 1u, 1u };
    fixture->namespace_entries[10] = (CmHirDeclarationNamespaceEntry){
        3u, CM_HIR_DECL_NAMESPACE_TYPE, fixture->items[1].name,
        CM_HIR_DECL_TARGET_ITEM, 2u, 2u };
    fixture->namespace_entries[11] = (CmHirDeclarationNamespaceEntry){
        3u, CM_HIR_DECL_NAMESPACE_TYPE, fixture->variants[1].name,
        CM_HIR_DECL_TARGET_ENUM_VARIANT, 2u, 1u };
    fixture->namespace_entries[12] = fixture->namespace_entries[6];
    fixture->namespace_entries[12].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[13] = fixture->namespace_entries[7];
    fixture->namespace_entries[13].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[14] = fixture->namespace_entries[8];
    fixture->namespace_entries[14].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[15] = fixture->namespace_entries[11];
    fixture->namespace_entries[15].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 16u;
}

static void test_generic_tuple_enum_family(void)
{
    GenericEnumFixture fixture;
    GenericEnumFixture twin;
    CmHirDeclarationMetadata decoded;
    CmByteBuf encoded;
    CmByteBuf repeated;
    CmByteBuf replay;
    CmByteBuf bad;
    size_t record;
    size_t payload;

    generic_enum_fixture_init(&fixture);
    generic_enum_fixture_init(&twin);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&repeated);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&twin.metadata, &repeated)
        == CM_HIR_DECL_METADATA_OK);
    assert(encoded.len == repeated.len
        && memcmp(encoded.data, repeated.data, encoded.len) == 0);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(decoded.item_count == 3u && decoded.generic_count == 3u
        && decoded.type_count == 6u
        && decoded.items[0].generic_count == 1u
        && decoded.items[0].enum_flags
            == CM_HIR_DECL_ENUM_HAS_LANG_ITEM
        && decoded.items[0].diagnostic_item.length == sizeof("Option") - 1u
        && memcmp(decoded.items[0].diagnostic_item.data, "Option",
            sizeof("Option") - 1u) == 0
        && decoded.items[0].enum_lang_item.length
            == sizeof("option_type") - 1u
        && memcmp(decoded.items[0].enum_lang_item.data, "option_type",
            sizeof("option_type") - 1u) == 0
        && decoded.items[0].variants[0].kind
            == CM_HIR_DECL_VARIANT_UNIT
        && decoded.items[0].variants[0].lang_item.length
            == sizeof("none_ctor") - 1u
        && decoded.items[0].variants[1].kind
            == CM_HIR_DECL_VARIANT_TUPLE
        && decoded.items[0].variants[1].field_count == 1u
        && decoded.items[0].variants[1].fields[0].source_ordinal == 0u
        && decoded.items[0].variants[1].fields[0].type_local == 3u
        && decoded.items[1].generic_count == 2u
        && decoded.items[1].diagnostic_item.length == sizeof("Result") - 1u
        && memcmp(decoded.items[1].diagnostic_item.data, "Result",
            sizeof("Result") - 1u) == 0
        && decoded.items[1].variants[0].lang_item.length
            == sizeof("ok_ctor") - 1u
        && decoded.items[1].variants[1].lang_item.length
            == sizeof("err_ctor") - 1u
        && decoded.items[1].variants[0].fields[0].type_local == 4u
        && decoded.items[1].variants[1].fields[0].type_local == 5u
        && decoded.types[5].kind
            == CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION
        && decoded.types[5].item_local == 1u
        && decoded.namespace_entries[11].target_local == 2u
        && decoded.namespace_entries[15].target_local == 2u);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
        == CM_HIR_DECL_METADATA_OK);
    assert(encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

    generic_enum_fixture_init(&fixture);
    fixture.variants[1].field_count = 0u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    generic_enum_fixture_init(&fixture);
    fixture.variant_fields[0].type_local = 4u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    generic_enum_fixture_init(&fixture);
    fixture.variants[2].field_count = 2u;
    fixture.variants[2].fields = &fixture.variant_fields[1];
    fixture.variant_fields[2].source_ordinal = 1u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    fixture.variant_fields[2].source_ordinal = 0u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    generic_enum_fixture_init(&fixture);
    fixture.variants[1].fields = NULL;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    generic_enum_fixture_init(&fixture);
    fixture.variants[1].flags = 0u;
    fixture.variants[1].lang_item = (CmHirDeclarationString){ NULL, 0u };
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    generic_enum_fixture_init(&fixture);
    fixture.items[0].enum_lang_item = fixture.variants[0].lang_item;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    generic_enum_fixture_init(&fixture);
    fixture.items[2].lang_item = fixture.items[0].enum_lang_item;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    generic_enum_fixture_init(&fixture);
    fixture.variants[2].lang_item = fixture.variants[3].lang_item;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    generic_enum_fixture_init(&fixture);
    fixture.items[0].enum_flags = UINT8_C(2);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    generic_enum_fixture_init(&fixture);
    fixture.variants[1].flags = UINT16_C(2);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    generic_enum_fixture_init(&fixture);
    fixture.items[0].enum_repr_primitive = CM_HIR_DECL_PRIMITIVE_U8;
    fixture.items[0].diagnostic_item =
        (CmHirDeclarationString){ NULL, 0u };
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    generic_enum_fixture_init(&fixture);
    fixture.generics[0].is_relaxed_sized = 1u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    generic_enum_fixture_init(&fixture);
    fixture.types[5].item_local = 2u;
    fixture.types[5].argument_count = 2u;
    fixture.application_arguments[1] = 1u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    generic_enum_fixture_init(&fixture);
    fixture.variant_fields[2].type_local = 4u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    generic_enum_fixture_init(&fixture);
    fixture.application_arguments[0] = 3u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    generic_enum_fixture_init(&fixture);
    fixture.types[5].item_local = 2u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    generic_enum_fixture_init(&fixture);
    fixture.namespace_entries[15].target_local = 1u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    generic_enum_fixture_init(&fixture);
    fixture.metadata.namespace_count = 15u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);

    record = enum_variant_record_offset(&encoded, UINT32_C(0), UINT32_C(1));
    payload = skip_string(&encoded, record + 4u) + 20u;
    assert(get_u32(encoded.data + payload) == UINT32_C(1));
    bad = copy_bytes(&encoded);
    record = enum_variant_record_offset(&bad, UINT32_C(0), UINT32_C(1));
    payload = skip_string(&bad, record + 4u) + 20u;
    put_u32(bad.data + payload, UINT32_C(0));
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    record = enum_variant_record_offset(&bad, UINT32_C(0), UINT32_C(1));
    payload = skip_string(&bad, record + 4u) + 24u;
    put_u32(bad.data + payload + 4u, UINT32_C(0));
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    record = enum_variant_record_offset(&bad, UINT32_C(0), UINT32_C(1));
    put_u16(bad.data + record + 2u, UINT16_C(2));
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    bad.len -= 1u;
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&repeated);
    cm_byte_buf_destroy(&encoded);
}

static void test_enum_named_adt_signature(void)
{
    TestFixture fixture;
    CmHirDeclarationVariant variant;

    fixture_init(&fixture);
    memset(&variant, 0, sizeof(variant));
    variant.kind = CM_HIR_DECL_VARIANT_UNIT;
    variant.name = (CmHirDeclarationString)S("Null");
    variant.discriminant_primitive = CM_HIR_DECL_PRIMITIVE_ISIZE;

    fixture.items[0].kind = CM_HIR_DECL_ITEM_ENUM;
    fixture.items[0].owner_module = 2u;
    fixture.items[0].name = (CmHirDeclarationString)S("AsciiChar");
    fixture.items[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture.items[0].source_ordinal = 3u;
    fixture.items[0].enum_repr_primitive = CM_HIR_DECL_PRIMITIVE_U8;
    fixture.items[0].variant_count = 1u;
    fixture.items[0].variants = &variant;
    fixture.metadata.items = fixture.items;
    fixture.metadata.item_count = 1u;

    memset(fixture.types, 0, sizeof(fixture.types));
    fixture.types[0].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture.types[0].primitive = CM_HIR_DECL_PRIMITIVE_U8;
    fixture.types[1].kind = CM_HIR_DECL_TYPE_GENERIC;
    fixture.types[1].generic_local = 2u;
    fixture.types[2].kind = CM_HIR_DECL_TYPE_NAMED_ADT;
    fixture.types[2].item_local = 1u;
    fixture.metadata.type_count = 3u;
    fixture.parameters[0] = 2u;
    fixture.values[0].return_type = 3u;
    fixture.predicates[0].subject_type = 2u;
    fixture.predicate_arguments[0] = 1u;

    fixture.namespace_entries[4] = fixture.namespace_entries[3];
    fixture.namespace_entries[3] = fixture.namespace_entries[2];
    fixture.namespace_entries[2].owner_module = 2u;
    fixture.namespace_entries[2].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture.namespace_entries[2].name = fixture.items[0].name;
    fixture.namespace_entries[2].target_kind = CM_HIR_DECL_TARGET_ITEM;
    fixture.namespace_entries[2].target_local = 1u;
    fixture.namespace_entries[2].export_ordinal = 3u;
    fixture.metadata.namespace_count = 5u;

    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
}

static void test_const_value_family(void)
{
    ConstFixture fixture;
    TestFixture function_fixture;
    CmHirDeclarationMetadata decoded;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmByteBuf function_bytes;
    CmByteBuf bad;
    size_t record;
    size_t common;
    size_t payload;
    size_t section;

    const_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    record = value_record_offset(&encoded, UINT32_C(0));
    common = skip_string(&encoded, record + 8u);
    payload = common + 44u;
    assert(encoded.data[record] == CM_HIR_DECL_VALUE_CONST
        && get_u32(encoded.data + payload) == UINT32_C(1)
        && encoded.data[payload + 4u] == CM_HIR_DECL_IMMUTABLE
        && encoded.data[payload + 5u] == UINT8_C(1)
        && encoded.data[payload + 6u] == UINT8_C(0)
        && encoded.data[payload + 7u] == UINT8_C(0));
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK
        && decoded.trait_count == 1u
        && decoded.value_count == 1u
        && decoded.values[0].kind == CM_HIR_DECL_VALUE_CONST
        && decoded.values[0].declared_type == 1u
        && decoded.values[0].mutability == CM_HIR_DECL_IMMUTABLE
        && decoded.values[0].has_body == 1u
        && decoded.values[0].parameter_count == 0u
        && decoded.values[0].parameter_types == NULL
        && decoded.values[0].return_type == 0u);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
        == CM_HIR_DECL_METADATA_OK
        && encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

    /* The pre-CONST function tag and payload layout remain byte-for-byte. */
    fixture_init(&function_fixture);
    cm_byte_buf_init(&function_bytes);
    assert(cm_hir_declaration_metadata_encode(&function_fixture.metadata,
        &function_bytes) == CM_HIR_DECL_METADATA_OK);
    section = section_offset(&function_bytes, "VALU");
    assert(get_u64(function_bytes.data + section + 4u) == UINT64_C(85));
    record = value_record_offset(&function_bytes, UINT32_C(0));
    common = skip_string(&function_bytes, record + 8u);
    payload = common + 44u;
    assert(function_bytes.data[record] == CM_HIR_DECL_VALUE_FUNCTION
        && get_u32(function_bytes.data + record + 4u) == UINT32_C(2)
        && get_u32(function_bytes.data + common + 8u) == UINT32_C(2)
        && get_u32(function_bytes.data + common + 12u) == UINT32_C(2)
        && get_u32(function_bytes.data + common + 16u) == UINT32_C(1)
        && get_u32(function_bytes.data + common + 28u) == UINT32_C(1)
        && get_u32(function_bytes.data + common + 32u) == UINT32_C(1)
        && get_u32(function_bytes.data + payload) == UINT32_C(0)
        && get_u32(function_bytes.data + payload + 4u) == UINT32_C(1)
        && get_u32(function_bytes.data + payload + 8u) == UINT32_C(3)
        && get_u32(function_bytes.data + payload + 12u) == UINT32_C(1)
        && function_bytes.data[payload + 16u] == UINT8_C(1)
        && function_bytes.data[payload + 17u] == UINT8_C(0)
        && function_bytes.data[payload + 18u] == UINT8_C(0)
        && function_bytes.data[payload + 19u] == UINT8_C(0));
    function_fixture.values[0].declared_type = 1u;
    assert(cm_hir_declaration_metadata_validate(&function_fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    function_fixture.values[0].declared_type = 0u;
    function_fixture.values[0].mutability = CM_HIR_DECL_IMMUTABLE;
    assert(cm_hir_declaration_metadata_validate(&function_fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    function_fixture.values[0].mutability = 0u;
    cm_byte_buf_destroy(&function_bytes);

    fixture.values[0].kind = 0u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.values[0].kind = CM_HIR_DECL_VALUE_CONST;
    fixture.values[0].declared_type = 0u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.values[0].declared_type = 1u;
    fixture.values[0].mutability = CM_HIR_DECL_MUTABLE;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.values[0].mutability = CM_HIR_DECL_IMMUTABLE;
    fixture.values[0].has_body = 0u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.values[0].has_body = 2u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.values[0].has_body = 1u;
    fixture.values[0].generic_start = 1u;
    fixture.values[0].generic_count = 1u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.values[0].generic_start = 0u;
    fixture.values[0].generic_count = 0u;
    fixture.values[0].predicate_start = 1u;
    fixture.values[0].predicate_count = 1u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.values[0].predicate_start = 0u;
    fixture.values[0].predicate_count = 0u;
    fixture.values[0].parameter_count = 1u;
    fixture.values[0].parameter_types = fixture.parameters;
    fixture.parameters[0] = 1u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.values[0].parameter_count = 0u;
    fixture.values[0].parameter_types = NULL;
    fixture.values[0].return_type = 1u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.values[0].return_type = 0u;

    fixture.namespace_entries[1].export_ordinal = 9u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.namespace_entries[1].export_ordinal = 2u;
    fixture.namespace_entries[1].name =
        (CmHirDeclarationString)S("MAX_OTHER");
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.namespace_entries[1].name = fixture.values[0].name;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);

    /* An ITEM-owned generic remains forbidden even beneath an application. */
    fixture.generics[0].owner_kind = CM_HIR_DECL_GENERIC_ITEM;
    fixture.generics[0].owner_local = 1u;
    fixture.generics[0].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture.generics[0].name = (CmHirDeclarationString)S("W");
    fixture.metadata.generics = fixture.generics;
    fixture.metadata.generic_count = 1u;
    fixture.items[0].kind = CM_HIR_DECL_ITEM_STRUCT;
    fixture.items[0].aggregate_form = CM_HIR_DECL_AGGREGATE_UNIT;
    fixture.items[0].owner_module = 1u;
    fixture.items[0].name = (CmHirDeclarationString)S("Wrap");
    fixture.items[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture.items[0].source_ordinal = 3u;
    fixture.items[0].generic_start = 1u;
    fixture.items[0].generic_count = 1u;
    fixture.metadata.items = fixture.items;
    fixture.metadata.item_count = 1u;
    memset(fixture.types, 0, sizeof(fixture.types));
    fixture.types[0].kind = CM_HIR_DECL_TYPE_GENERIC;
    fixture.types[0].generic_local = 1u;
    fixture.application_arguments[0] = 1u;
    fixture.types[1].kind = CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION;
    fixture.types[1].item_local = 1u;
    fixture.types[1].argument_count = 1u;
    fixture.types[1].argument_types = fixture.application_arguments;
    fixture.metadata.type_count = 2u;
    fixture.values[0].declared_type = 2u;
    fixture.namespace_entries[3] = fixture.namespace_entries[2];
    fixture.namespace_entries[2] = fixture.namespace_entries[1];
    fixture.namespace_entries[1].owner_module = 1u;
    fixture.namespace_entries[1].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture.namespace_entries[1].name = fixture.items[0].name;
    fixture.namespace_entries[1].target_kind = CM_HIR_DECL_TARGET_ITEM;
    fixture.namespace_entries[1].target_local = 1u;
    fixture.namespace_entries[1].export_ordinal = 3u;
    fixture.metadata.namespace_count = 4u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);

    record = value_record_offset(&encoded, UINT32_C(0));
    common = skip_string(&encoded, record + 8u);
    payload = common + 44u;
#define ASSERT_BAD_VALUE_BYTE(offset_, value_) do { \
        bad = copy_bytes(&encoded); \
        bad.data[(offset_)] = (unsigned char)(value_); \
        recompute_family_crc(&bad, UINT8_C(3), "VALU"); \
        assert_failed_transaction(&bad); \
        cm_byte_buf_destroy(&bad); \
    } while (0)
    ASSERT_BAD_VALUE_BYTE(record, UINT8_C(4));
    ASSERT_BAD_VALUE_BYTE(record + 1u, UINT8_C(1));
    ASSERT_BAD_VALUE_BYTE(payload + 4u, CM_HIR_DECL_MUTABLE);
    ASSERT_BAD_VALUE_BYTE(payload + 5u, UINT8_C(0));
    ASSERT_BAD_VALUE_BYTE(payload + 6u, UINT8_C(1));
#undef ASSERT_BAD_VALUE_BYTE
    bad = copy_bytes(&encoded);
    put_u32(bad.data + payload, UINT32_C(0));
    recompute_family_crc(&bad, UINT8_C(3), "VALU");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);
    bad = copy_bytes(&encoded);
    put_u32(bad.data + common + 16u, UINT32_C(1));
    recompute_family_crc(&bad, UINT8_C(3), "VALU");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);
    bad = copy_bytes(&encoded);
    bad.data[payload] ^= UINT8_C(1);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);
    bad = copy_bytes(&encoded);
    assert(bad.len != 0u);
    bad.len -= 1u;
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

static void test_const_function_value_family(void)
{
    TypeNameFixture fixture;
    TypeNameFixture repeated_fixture;
    ConstFixture const_fixture;
    StaticFixture static_fixture;
    CmHirDeclarationMetadata decoded;
    CmByteBuf encoded;
    CmByteBuf repeated;
    CmByteBuf replay;
    CmByteBuf bad;
    size_t record;
    size_t common;
    size_t payload;

    type_name_fixture_init(&fixture);
    type_name_fixture_init(&repeated_fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&repeated);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&repeated_fixture.metadata,
            &repeated) == CM_HIR_DECL_METADATA_OK
        && encoded.len == repeated.len
        && memcmp(encoded.data, repeated.data, encoded.len) == 0);

    record = value_record_offset(&encoded, UINT32_C(0));
    common = skip_string(&encoded, record + 8u);
    payload = common + 44u;
    assert(encoded.data[record] == CM_HIR_DECL_VALUE_FUNCTION
        && get_u32(encoded.data + common + 12u) == UINT32_C(1)
        && get_u32(encoded.data + common + 16u) == UINT32_C(1)
        && get_u32(encoded.data + common + 28u) == UINT32_C(0)
        && get_u32(encoded.data + common + 32u) == UINT32_C(0)
        && get_u32(encoded.data + payload + 4u) == UINT32_C(0)
        && get_u32(encoded.data + payload + 8u) == UINT32_C(2)
        && encoded.data[payload + 12u] == UINT8_C(1)
        && encoded.data[payload + 13u] == UINT8_C(1)
        && encoded.data[payload + 14u] == UINT8_C(0)
        && encoded.data[payload + 15u] == UINT8_C(0));

    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK
        && decoded.value_count == 1u
        && decoded.values[0].kind == CM_HIR_DECL_VALUE_FUNCTION
        && decoded.values[0].generic_count == 1u
        && decoded.values[0].predicate_start == 0u
        && decoded.values[0].predicate_count == 0u
        && decoded.values[0].parameter_count == 0u
        && decoded.values[0].return_type == 2u
        && decoded.values[0].has_body == 1u
        && decoded.values[0].is_const == 1u
        && decoded.generic_count == 1u
        && decoded.generics[0].is_relaxed_sized == 1u
        && decoded.type_count == 2u
        && decoded.types[0].primitive == CM_HIR_DECL_PRIMITIVE_STR
        && decoded.types[1].kind == CM_HIR_DECL_TYPE_REFERENCE
        && decoded.types[1].region.kind == CM_HIR_DECL_REGION_STATIC);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
        == CM_HIR_DECL_METADATA_OK
        && encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

    fixture.values[0].is_const = 2u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    type_name_fixture_init(&fixture);
    fixture.values[0].predicate_start = 1u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    type_name_fixture_init(&fixture);
    fixture.values[0].generic_start = 0u;
    fixture.values[0].generic_count = 0u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    type_name_fixture_init(&fixture);
    fixture.generics[0].owner_kind = CM_HIR_DECL_GENERIC_ITEM;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);

    /* Declared-but-unused T has no TYPE_GENERIC node; an orphan one fails. */
    type_name_fixture_init(&fixture);
    fixture.types[2] = fixture.types[1];
    memset(&fixture.types[1], 0, sizeof(fixture.types[1]));
    fixture.types[1].kind = CM_HIR_DECL_TYPE_GENERIC;
    fixture.types[1].generic_local = 1u;
    fixture.metadata.type_count = 3u;
    fixture.values[0].return_type = 3u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);

    type_name_fixture_init(&fixture);
    fixture.namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);

    const_fixture_init(&const_fixture);
    const_fixture.values[0].is_const = 1u;
    assert(cm_hir_declaration_metadata_validate(&const_fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    static_fixture_init(&static_fixture);
    static_fixture.values[0].is_const = 1u;
    assert(cm_hir_declaration_metadata_validate(&static_fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);

#define ASSERT_BAD_CONST_FUNCTION_BYTE(offset_, value_) do { \
        bad = copy_bytes(&encoded); \
        bad.data[(offset_)] = (unsigned char)(value_); \
        recompute_family_crc(&bad, UINT8_C(3), "VALU"); \
        assert_failed_transaction(&bad); \
        cm_byte_buf_destroy(&bad); \
    } while (0)
    ASSERT_BAD_CONST_FUNCTION_BYTE(payload + 13u, UINT8_C(2));
    ASSERT_BAD_CONST_FUNCTION_BYTE(payload + 14u, UINT8_C(1));
#undef ASSERT_BAD_CONST_FUNCTION_BYTE
    bad = copy_bytes(&encoded);
    put_u32(bad.data + common + 28u, UINT32_C(1));
    recompute_family_crc(&bad, UINT8_C(3), "VALU");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);
    bad = copy_bytes(&encoded);
    assert(bad.len != 0u);
    bad.len -= 1u;
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&repeated);
    cm_byte_buf_destroy(&encoded);
}

static void test_zero_generic_unit_function_family(void)
{
    UnitFunctionFixture fixture;
    UnitFunctionFixture repeated_fixture;
    CmHirDeclarationMetadata decoded;
    CmByteBuf encoded;
    CmByteBuf repeated;
    CmByteBuf replay;
    CmByteBuf bad;
    size_t record;
    size_t common;
    size_t payload;
    size_t type_record;
    size_t namespace_record;

    unit_function_fixture_init(&fixture);
    unit_function_fixture_init(&repeated_fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&repeated);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&repeated_fixture.metadata,
            &repeated) == CM_HIR_DECL_METADATA_OK
        && encoded.len == repeated.len
        && memcmp(encoded.data, repeated.data, encoded.len) == 0);

    record = value_record_offset(&encoded, UINT32_C(0));
    common = skip_string(&encoded, record + 8u);
    payload = common + 44u;
    assert(encoded.data[record] == CM_HIR_DECL_VALUE_FUNCTION
        && get_u32(encoded.data + common + 12u) == UINT32_C(0)
        && get_u32(encoded.data + common + 16u) == UINT32_C(0)
        && get_u32(encoded.data + common + 28u) == UINT32_C(0)
        && get_u32(encoded.data + common + 32u) == UINT32_C(0)
        && get_u32(encoded.data + payload + 4u) == UINT32_C(0)
        && get_u32(encoded.data + payload + 8u) == UINT32_C(1)
        && encoded.data[payload + 12u] == UINT8_C(1)
        && encoded.data[payload + 13u] == UINT8_C(0)
        && encoded.data[payload + 14u] == UINT8_C(0)
        && encoded.data[payload + 15u] == UINT8_C(0));

    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK
        && decoded.generic_count == 0u && decoded.generics == NULL
        && decoded.predicate_count == 0u && decoded.predicates == NULL
        && decoded.value_count == 1u
        && decoded.values[0].kind == CM_HIR_DECL_VALUE_FUNCTION
        && decoded.values[0].generic_start == 0u
        && decoded.values[0].generic_count == 0u
        && decoded.values[0].predicate_start == 0u
        && decoded.values[0].predicate_count == 0u
        && decoded.values[0].parameter_count == 0u
        && decoded.values[0].parameter_types == NULL
        && decoded.values[0].return_type == 1u
        && decoded.values[0].has_body == 1u
        && decoded.values[0].is_const == 0u
        && decoded.type_count == 1u
        && decoded.types[0].kind == CM_HIR_DECL_TYPE_PRIMITIVE
        && decoded.types[0].primitive == CM_HIR_DECL_PRIMITIVE_UNIT
        && decoded.namespace_count == 2u);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
        == CM_HIR_DECL_METADATA_OK
        && encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

#define ASSERT_INVALID_UNIT_FUNCTION(change_) do { \
        unit_function_fixture_init(&fixture); \
        change_; \
        assert(cm_hir_declaration_metadata_validate(&fixture.metadata) \
            == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR); \
    } while (0)
    ASSERT_INVALID_UNIT_FUNCTION(
        fixture.types[0].primitive = CM_HIR_DECL_PRIMITIVE_BOOL);
    ASSERT_INVALID_UNIT_FUNCTION(
        fixture.values[0].parameter_count = 1u;
        fixture.values[0].parameter_types = fixture.parameters;
        fixture.parameters[0] = 1u);
    ASSERT_INVALID_UNIT_FUNCTION(fixture.values[0].predicate_start = 1u);
    ASSERT_INVALID_UNIT_FUNCTION(
        fixture.values[0].predicate_start = 1u;
        fixture.values[0].predicate_count = 1u);
    ASSERT_INVALID_UNIT_FUNCTION(fixture.values[0].generic_start = 1u);
    ASSERT_INVALID_UNIT_FUNCTION(
        fixture.values[0].generic_start = 1u;
        fixture.values[0].generic_count = 1u);
    ASSERT_INVALID_UNIT_FUNCTION(fixture.values[0].has_body = 0u);
    ASSERT_INVALID_UNIT_FUNCTION(fixture.values[0].is_const = 1u);
    ASSERT_INVALID_UNIT_FUNCTION(fixture.values[0].return_type = 0u);
    ASSERT_INVALID_UNIT_FUNCTION(fixture.values[0].declared_type = 1u);
    ASSERT_INVALID_UNIT_FUNCTION(
        fixture.values[0].mutability = CM_HIR_DECL_IMMUTABLE);
    ASSERT_INVALID_UNIT_FUNCTION(
        fixture.types[1].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
        fixture.types[1].primitive = CM_HIR_DECL_PRIMITIVE_BOOL;
        fixture.metadata.type_count = 2u);
    ASSERT_INVALID_UNIT_FUNCTION(
        fixture.metadata.namespace_entries = fixture.namespace_entries + 1u;
        fixture.metadata.namespace_count = 1u);
    ASSERT_INVALID_UNIT_FUNCTION(
        fixture.namespace_entries[0].namespace_kind =
            CM_HIR_DECL_NAMESPACE_TYPE);
    ASSERT_INVALID_UNIT_FUNCTION(
        fixture.namespace_entries[0].target_local = 0u);
#undef ASSERT_INVALID_UNIT_FUNCTION

#define ASSERT_BAD_UNIT_VALUE_U32(offset_, value_) do { \
        bad = copy_bytes(&encoded); \
        put_u32(bad.data + (offset_), (value_)); \
        recompute_family_crc(&bad, UINT8_C(3), "VALU"); \
        assert_failed_transaction(&bad); \
        cm_byte_buf_destroy(&bad); \
    } while (0)
    ASSERT_BAD_UNIT_VALUE_U32(common + 12u, UINT32_C(1));
    ASSERT_BAD_UNIT_VALUE_U32(common + 16u, UINT32_C(1));
    ASSERT_BAD_UNIT_VALUE_U32(common + 28u, UINT32_C(1));
    ASSERT_BAD_UNIT_VALUE_U32(common + 32u, UINT32_C(1));
    ASSERT_BAD_UNIT_VALUE_U32(payload + 4u, UINT32_C(1));
    ASSERT_BAD_UNIT_VALUE_U32(payload + 8u, UINT32_C(0));
#undef ASSERT_BAD_UNIT_VALUE_U32

#define ASSERT_BAD_UNIT_VALUE_U8(offset_, value_) do { \
        bad = copy_bytes(&encoded); \
        bad.data[(offset_)] = (unsigned char)(value_); \
        recompute_family_crc(&bad, UINT8_C(3), "VALU"); \
        assert_failed_transaction(&bad); \
        cm_byte_buf_destroy(&bad); \
    } while (0)
    ASSERT_BAD_UNIT_VALUE_U8(payload + 12u, UINT8_C(0));
    ASSERT_BAD_UNIT_VALUE_U8(payload + 13u, UINT8_C(1));
    ASSERT_BAD_UNIT_VALUE_U8(payload + 14u, UINT8_C(1));
#undef ASSERT_BAD_UNIT_VALUE_U8

    bad = copy_bytes(&encoded);
    type_record = type_record_offset(&bad, UINT32_C(0));
    bad.data[type_record + 4u] = CM_HIR_DECL_PRIMITIVE_BOOL;
    recompute_crc(&bad);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    namespace_record = namespace_record_offset(&bad, UINT32_C(0));
    bad.data[namespace_record + 4u] = CM_HIR_DECL_NAMESPACE_TYPE;
    recompute_module_family_crc(&bad);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    namespace_record = namespace_record_offset(&bad, UINT32_C(0));
    put_u32(bad.data + skip_string(&bad, namespace_record + 8u),
        UINT32_C(0));
    recompute_module_family_crc(&bad);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    assert(bad.len != 0u);
    bad.len -= 1u;
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&repeated);
    cm_byte_buf_destroy(&encoded);
}

static void test_contextual_erased_reference_roots(void)
{
    TypeNameFixture first;
    TypeNameFixture repeated;
    AssociatedMethodFixture associated;
    AggregateFixture aggregate;
    CmHirDeclarationMetadata decoded;
    CmByteBuf encoded;
    CmByteBuf repeated_bytes;
    CmByteBuf replay;
    CmByteBuf bad;
    size_t record;

    type_name_of_val_fixture_init(&first);
    type_name_of_val_fixture_init(&repeated);
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&repeated_bytes);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&first.metadata, &encoded)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&repeated.metadata,
            &repeated_bytes) == CM_HIR_DECL_METADATA_OK
        && encoded.len == repeated_bytes.len
        && memcmp(encoded.data, repeated_bytes.data, encoded.len) == 0);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
                &decoded) == CM_HIR_DECL_METADATA_OK
        && decoded.value_count == 1u && decoded.type_count == 4u
        && decoded.values[0].is_const == 1u
        && decoded.values[0].parameter_count == 1u
        && decoded.types[decoded.values[0].parameter_types[0] - 1u]
            .region.kind == CM_HIR_DECL_REGION_ERASED
        && decoded.types[decoded.values[0].return_type - 1u].region.kind
            == CM_HIR_DECL_REGION_STATIC
        && cm_hir_declaration_metadata_encode(&decoded, &replay)
            == CM_HIR_DECL_METADATA_OK
        && encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

    type_name_of_val_fixture_init(&first);
    first.values[0].is_const = 0u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    type_name_of_val_fixture_init(&first);
    first.values[0].has_body = 0u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    type_name_of_val_fixture_init(&first);
    first.generics[0].is_relaxed_sized = 0u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    type_name_of_val_fixture_init(&first);
    first.parameters[1] = 4u;
    first.values[0].parameter_count = 2u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    type_name_of_val_fixture_init(&first);
    first.types[3].mutability = CM_HIR_DECL_MUTABLE;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    type_name_of_val_fixture_init(&first);
    first.types[2].region.kind = CM_HIR_DECL_REGION_ERASED;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);

    /* A STATIC reference is an ordinary closed CONST/STATIC type.  Sharing
     * the exact ERASED input node into either declaration is forbidden. */
    type_name_of_val_fixture_init(&first);
    first.values[1].kind = CM_HIR_DECL_VALUE_CONST;
    first.values[1].owner_module = 1u;
    first.values[1].name = (CmHirDeclarationString)S("z_value");
    first.values[1].source_ordinal = 2u;
    first.values[1].declared_type = 3u;
    first.values[1].mutability = CM_HIR_DECL_IMMUTABLE;
    first.values[1].has_body = 1u;
    first.metadata.value_count = 2u;
    first.namespace_entries[1].owner_module = 1u;
    first.namespace_entries[1].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    first.namespace_entries[1].name = first.values[1].name;
    first.namespace_entries[1].target_kind = CM_HIR_DECL_TARGET_VALUE;
    first.namespace_entries[1].target_local = 2u;
    first.namespace_entries[1].export_ordinal = 2u;
    first.metadata.namespace_count = 2u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_OK);
    first.values[1].declared_type = 4u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    first.values[1].kind = CM_HIR_DECL_VALUE_STATIC;
    first.values[1].declared_type = 3u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_OK);
    first.values[1].declared_type = 4u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);

    /* The receiver root and a receiver-related output are admitted, but an
     * additional input may not reuse the ERASED receiver node. */
    associated_method_fixture_init(&associated);
    associated.associated_items[1].return_type = 4u;
    assert(cm_hir_declaration_metadata_validate(&associated.metadata)
        == CM_HIR_DECL_METADATA_OK);
    associated.allocate_parameters[1] = 4u;
    assert(cm_hir_declaration_metadata_validate(&associated.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);

    /* Aggregate roots are never authenticated erasure boundaries. */
    aggregate_fixture_init(&aggregate);
    aggregate.types[5] = aggregate.types[4];
    memset(&aggregate.types[4], 0, sizeof(aggregate.types[4]));
    aggregate.types[4].kind = CM_HIR_DECL_TYPE_REFERENCE;
    aggregate.types[4].child_type = 3u;
    aggregate.types[4].mutability = CM_HIR_DECL_IMMUTABLE;
    aggregate.types[4].region.kind = CM_HIR_DECL_REGION_ERASED;
    aggregate.application_arguments[0] = 4u;
    aggregate.types[5].argument_types = aggregate.application_arguments;
    aggregate.maybe_uninit_fields[1].type_local = 6u;
    aggregate.manually_drop_fields[0].type_local = 5u;
    aggregate.metadata.type_count = 6u;
    assert(cm_hir_declaration_metadata_validate(&aggregate.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);

    /* A CRC-correct hostile TYPE mutation still fails transactionally. */
    bad = copy_bytes(&encoded);
    record = type_record_offset(&bad, UINT32_C(2));
    bad.data[record + 1u] = CM_HIR_DECL_REGION_ERASED;
    recompute_family_crc(&bad, UINT8_C(2), "TYPE");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&repeated_bytes);
    cm_byte_buf_destroy(&encoded);
}

static void test_from_mut_erased_pair(void)
{
    FromMutFixture fixture;
    FromMutFixture repeated;
    CmHirDeclarationMetadata decoded;
    CmByteBuf encoded;
    CmByteBuf repeated_bytes;
    CmByteBuf replay;
    CmByteBuf bad;
    size_t record;

    from_mut_fixture_init(&fixture);
    from_mut_fixture_init(&repeated);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&repeated_bytes);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&repeated.metadata,
            &repeated_bytes) == CM_HIR_DECL_METADATA_OK
        && encoded.len == repeated_bytes.len
        && memcmp(encoded.data, repeated_bytes.data, encoded.len) == 0);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
                &decoded) == CM_HIR_DECL_METADATA_OK
        && decoded.generic_count == 1u && decoded.type_count == 5u
        && decoded.value_count == 1u && decoded.namespace_count == 2u
        && decoded.values[0].kind == CM_HIR_DECL_VALUE_FUNCTION
        && decoded.values[0].is_const == 1u
        && decoded.values[0].has_body == 1u
        && decoded.values[0].generic_start == 1u
        && decoded.values[0].generic_count == 1u
        && decoded.values[0].predicate_start == 0u
        && decoded.values[0].predicate_count == 0u
        && decoded.values[0].parameter_count == 1u
        && decoded.values[0].parameter_types[0] == 3u
        && decoded.values[0].return_type == 5u
        && decoded.types[2].kind == CM_HIR_DECL_TYPE_REFERENCE
        && decoded.types[2].mutability == CM_HIR_DECL_MUTABLE
        && decoded.types[2].region.kind == CM_HIR_DECL_REGION_ERASED
        && decoded.types[3].kind == CM_HIR_DECL_TYPE_ARRAY
        && decoded.types[3].array_length_kind
            == CM_HIR_DECL_ARRAY_LENGTH_SCALAR
        && decoded.types[3].array_length_type == 1u
        && decoded.types[3].array_length_low_bits == UINT64_C(1)
        && decoded.types[3].array_length_high_bits == UINT64_C(0)
        && decoded.types[4].kind == CM_HIR_DECL_TYPE_REFERENCE
        && decoded.types[4].mutability == CM_HIR_DECL_MUTABLE
        && decoded.types[4].region.kind == CM_HIR_DECL_REGION_ERASED
        && cm_hir_declaration_metadata_encode(&decoded, &replay)
            == CM_HIR_DECL_METADATA_OK
        && encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

#define ASSERT_INVALID_FROM_MUT(change_) do { \
        from_mut_fixture_init(&fixture); \
        change_; \
        assert(cm_hir_declaration_metadata_validate(&fixture.metadata) \
            == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR); \
    } while (0)
    ASSERT_INVALID_FROM_MUT(fixture.values[0].is_const = 0u);
    ASSERT_INVALID_FROM_MUT(fixture.values[0].has_body = 0u);
    ASSERT_INVALID_FROM_MUT(fixture.values[0].generic_start = 0u);
    ASSERT_INVALID_FROM_MUT(fixture.values[0].generic_count = 2u);
    ASSERT_INVALID_FROM_MUT(fixture.values[0].predicate_start = 1u);
    ASSERT_INVALID_FROM_MUT(
        fixture.parameters[1] = 3u;
        fixture.values[0].parameter_count = 2u);
    ASSERT_INVALID_FROM_MUT(
        fixture.generics[0].is_relaxed_sized = 1u);
    ASSERT_INVALID_FROM_MUT(
        fixture.generics[0].owner_local = 2u);
    ASSERT_INVALID_FROM_MUT(
        fixture.types[2].mutability = CM_HIR_DECL_IMMUTABLE);
    ASSERT_INVALID_FROM_MUT(
        fixture.types[4].mutability = CM_HIR_DECL_IMMUTABLE);
    ASSERT_INVALID_FROM_MUT(
        fixture.types[2].region.kind = CM_HIR_DECL_REGION_STATIC);
    ASSERT_INVALID_FROM_MUT(
        fixture.types[4].region.kind = CM_HIR_DECL_REGION_STATIC);
    ASSERT_INVALID_FROM_MUT(
        fixture.types[2].region.generic_local = 1u);
    ASSERT_INVALID_FROM_MUT(
        fixture.types[4].region.binder_index = 1u);
    ASSERT_INVALID_FROM_MUT(fixture.values[0].return_type = 4u);
    ASSERT_INVALID_FROM_MUT(fixture.types[3].child_type = 1u);
    ASSERT_INVALID_FROM_MUT(
        fixture.types[3].array_length_type = 2u);
    ASSERT_INVALID_FROM_MUT(
        fixture.types[3].array_length_low_bits = UINT64_C(2));
    ASSERT_INVALID_FROM_MUT(
        fixture.types[3].array_length_high_bits = UINT64_C(1));
    ASSERT_INVALID_FROM_MUT(
        fixture.namespace_entries[1].target_local = 0u);
#undef ASSERT_INVALID_FROM_MUT

    /* A second declaration may use ordinary closed types, but cannot share
     * either side of the authenticated ERASED relation. */
    from_mut_fixture_init(&fixture);
    fixture.values[1].kind = CM_HIR_DECL_VALUE_CONST;
    fixture.values[1].owner_module = 1u;
    fixture.values[1].name = (CmHirDeclarationString)S("z_value");
    fixture.values[1].source_ordinal = 2u;
    fixture.values[1].declared_type = 1u;
    fixture.values[1].mutability = CM_HIR_DECL_IMMUTABLE;
    fixture.values[1].has_body = 1u;
    fixture.metadata.value_count = 2u;
    fixture.namespace_entries[1].name = fixture.values[1].name;
    fixture.namespace_entries[1].target_local = 2u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    fixture.values[1].declared_type = 3u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.values[1].declared_type = 5u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);

    /* CRC-correct semantic mutations and truncation remain transactional. */
    bad = copy_bytes(&encoded);
    record = type_record_offset(&bad, UINT32_C(2));
    bad.data[record + 4u] = CM_HIR_DECL_REGION_STATIC;
    recompute_family_crc(&bad, UINT8_C(2), "TYPE");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    record = type_record_offset(&bad, UINT32_C(4));
    bad.data[record + 16u] = CM_HIR_DECL_IMMUTABLE;
    recompute_family_crc(&bad, UINT8_C(2), "TYPE");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    assert(bad.len != 0u);
    bad.len -= 1u;
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&repeated_bytes);
    cm_byte_buf_destroy(&encoded);
}

static void test_static_tuple_array_family(void)
{
    StaticFixture fixture;
    CmHirDeclarationMetadata decoded;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmByteBuf bad;
    size_t tuple_record;
    size_t array_record;
    size_t value_record;
    size_t value_common;
    size_t value_payload;

    static_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    tuple_record = type_record_offset(&encoded, UINT32_C(3));
    array_record = type_record_offset(&encoded, UINT32_C(4));
    value_record = value_record_offset(&encoded, UINT32_C(0));
    value_common = skip_string(&encoded, value_record + 8u);
    value_payload = value_common + 44u;
    assert(encoded.data[tuple_record] == CM_HIR_DECL_TYPE_TUPLE
        && get_u32(encoded.data + tuple_record + 4u) == UINT32_C(3)
        && get_u32(encoded.data + tuple_record + 8u) == UINT32_C(2)
        && get_u32(encoded.data + tuple_record + 12u) == UINT32_C(1)
        && get_u32(encoded.data + tuple_record + 16u) == UINT32_C(1));
    assert(encoded.data[array_record] == CM_HIR_DECL_TYPE_ARRAY
        && get_u32(encoded.data + array_record + 4u) == UINT32_C(4)
        && get_u32(encoded.data + array_record + 8u) == UINT32_C(3)
        && get_u64(encoded.data + array_record + 12u) == UINT64_C(81)
        && get_u64(encoded.data + array_record + 20u) == UINT64_C(0));
    assert(encoded.data[value_record] == CM_HIR_DECL_VALUE_STATIC
        && get_u32(encoded.data + value_payload) == UINT32_C(5)
        && encoded.data[value_payload + 4u] == CM_HIR_DECL_IMMUTABLE
        && encoded.data[value_payload + 5u] == UINT8_C(1));

    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(decoded.type_count == 5u
        && decoded.types[3].kind == CM_HIR_DECL_TYPE_TUPLE
        && decoded.types[3].element_count == 3u
        && decoded.types[3].element_types != NULL
        && decoded.types[3].element_types[0] == 2u
        && decoded.types[3].element_types[1] == 1u
        && decoded.types[3].element_types[2] == 1u
        && decoded.types[4].kind == CM_HIR_DECL_TYPE_ARRAY
        && decoded.types[4].child_type == 4u
        && decoded.types[4].array_length_type == 3u
        && decoded.types[4].array_length_low_bits == UINT64_C(81)
        && decoded.types[4].array_length_high_bits == UINT64_C(0)
        && decoded.value_count == 1u
        && decoded.values[0].kind == CM_HIR_DECL_VALUE_STATIC
        && decoded.values[0].declared_type == 5u
        && decoded.values[0].mutability == CM_HIR_DECL_IMMUTABLE
        && decoded.values[0].has_body == 1u
        && decoded.namespace_count == 2u
        && decoded.namespace_entries[0].target_local == 1u
        && decoded.namespace_entries[1].target_local == 1u);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
        == CM_HIR_DECL_METADATA_OK
        && encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

    static_fixture_init(&fixture);
    fixture.values[0].mutability = CM_HIR_DECL_MUTABLE;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    fixture.types[4].array_length_low_bits = UINT64_MAX;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);

    static_fixture_init(&fixture);
    fixture.types[3].element_count = 0u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    static_fixture_init(&fixture);
    fixture.types[3].element_types = NULL;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    static_fixture_init(&fixture);
    fixture.types[3].element_count =
        (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES + UINT32_C(1);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    static_fixture_init(&fixture);
    fixture.tuple_elements[1] = 5u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    static_fixture_init(&fixture);
    fixture.types[4].child_type = 0u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    static_fixture_init(&fixture);
    fixture.types[4].array_length_type = 2u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    static_fixture_init(&fixture);
    fixture.types[4].array_length_high_bits = UINT64_C(1);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    static_fixture_init(&fixture);
    fixture.values[0].kind = CM_HIR_DECL_VALUE_CONST;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    fixture.values[0].mutability = CM_HIR_DECL_MUTABLE;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    static_fixture_init(&fixture);
    fixture.values[0].mutability = 0u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.values[0].mutability = CM_HIR_DECL_IMMUTABLE;
    fixture.values[0].has_body = 0u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    static_fixture_init(&fixture);
    fixture.namespace_entries[1].target_local = 2u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);

    /* A generic hidden beneath TUPLE->ARRAY cannot escape into STATIC. */
    static_fixture_init(&fixture);
    fixture.generics[0].owner_kind = CM_HIR_DECL_GENERIC_ITEM;
    fixture.generics[0].owner_local = 1u;
    fixture.generics[0].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture.generics[0].name = (CmHirDeclarationString)S("T");
    fixture.metadata.generics = fixture.generics;
    fixture.metadata.generic_count = 1u;
    fixture.items[0].kind = CM_HIR_DECL_ITEM_STRUCT;
    fixture.items[0].owner_module = 1u;
    fixture.items[0].name = (CmHirDeclarationString)S("Wrap");
    fixture.items[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture.items[0].source_ordinal = 2u;
    fixture.items[0].generic_start = 1u;
    fixture.items[0].generic_count = 1u;
    fixture.items[0].aggregate_form = CM_HIR_DECL_AGGREGATE_UNIT;
    fixture.metadata.items = fixture.items;
    fixture.metadata.item_count = 1u;
    memset(fixture.types + 3u, 0,
        3u * sizeof(fixture.types[0]));
    fixture.types[3].kind = CM_HIR_DECL_TYPE_GENERIC;
    fixture.types[3].generic_local = 1u;
    fixture.tuple_elements[0] = 2u;
    fixture.tuple_elements[1] = 4u;
    fixture.tuple_elements[2] = 1u;
    fixture.types[4].kind = CM_HIR_DECL_TYPE_TUPLE;
    fixture.types[4].element_count = 3u;
    fixture.types[4].element_types = fixture.tuple_elements;
    fixture.types[5].kind = CM_HIR_DECL_TYPE_ARRAY;
    fixture.types[5].child_type = 5u;
    fixture.types[5].array_length_type = 3u;
    fixture.types[5].array_length_low_bits = UINT64_C(81);
    fixture.metadata.type_count = 6u;
    fixture.values[0].declared_type = 6u;
    fixture.namespace_entries[2] = fixture.namespace_entries[1];
    fixture.namespace_entries[1] = fixture.namespace_entries[0];
    fixture.namespace_entries[0].owner_module = 1u;
    fixture.namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture.namespace_entries[0].name = fixture.items[0].name;
    fixture.namespace_entries[0].target_kind = CM_HIR_DECL_TARGET_ITEM;
    fixture.namespace_entries[0].target_local = 1u;
    fixture.namespace_entries[0].export_ordinal = 2u;
    fixture.metadata.namespace_count = 3u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);

    bad = copy_bytes(&encoded);
    tuple_record = type_record_offset(&bad, UINT32_C(3));
    put_u32(bad.data + tuple_record + 4u, UINT32_C(0));
    recompute_family_crc(&bad, UINT8_C(2), "TYPE");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);
    bad = copy_bytes(&encoded);
    array_record = type_record_offset(&bad, UINT32_C(4));
    put_u32(bad.data + array_record + 8u, UINT32_C(2));
    recompute_family_crc(&bad, UINT8_C(2), "TYPE");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);
    bad = copy_bytes(&encoded);
    array_record = type_record_offset(&bad, UINT32_C(4));
    bad.data[array_record + 20u] = UINT8_C(1);
    recompute_family_crc(&bad, UINT8_C(2), "TYPE");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);
    bad = copy_bytes(&encoded);
    value_record = value_record_offset(&bad, UINT32_C(0));
    bad.data[value_record] = UINT8_C(4);
    recompute_family_crc(&bad, UINT8_C(3), "VALU");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);
    bad = copy_bytes(&encoded);
    bad.data[array_record + 12u] ^= UINT8_C(1);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);
    bad = copy_bytes(&encoded);
    assert(bad.len != 0u);
    bad.len -= 1u;
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

static void test_named_aggregate_family(void)
{
    AggregateFixture fixture;
    CmHirDeclarationMetadata decoded;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmByteBuf bad;
    size_t record;
    size_t payload;
    size_t field;
    size_t field_tail;

    aggregate_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK
        && decoded.item_count == 3u
        && decoded.items[0].kind == CM_HIR_DECL_ITEM_STRUCT
        && decoded.items[0].aggregate_form
            == CM_HIR_DECL_AGGREGATE_NAMED
        && decoded.items[0].aggregate_repr
            == CM_HIR_DECL_AGGREGATE_REPR_TRANSPARENT
        && decoded.items[0].field_count == 1u
        && decoded.items[0].fields[0].source_ordinal == 0u
        && decoded.items[0].fields[0].type_local == 3u
        && decoded.items[1].kind == CM_HIR_DECL_ITEM_UNION
        && decoded.items[1].field_count == 2u
        && decoded.items[1].fields[1].source_ordinal == 1u
        && decoded.items[1].fields[1].type_local == 5u
        && decoded.items[2].aggregate_repr
            == CM_HIR_DECL_AGGREGATE_REPR_RUST
        && decoded.items[2].field_count == 4u
        && decoded.types[4].kind
            == CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION
        && decoded.types[4].item_local == 1u
        && decoded.namespace_entries[0].target_local == 3u
        && decoded.namespace_entries[2].target_local == 2u);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
        == CM_HIR_DECL_METADATA_OK
        && encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

    /* Generic applications may name either bounded aggregate family. */
    fixture.types[5].kind = CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION;
    fixture.types[5].item_local = 2u;
    fixture.types[5].argument_count = 1u;
    fixture.union_application_arguments[0] = 2u;
    fixture.types[5].argument_types = fixture.union_application_arguments;
    fixture.assume_fields[0].type_local = 6u;
    fixture.metadata.type_count = 6u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    fixture.metadata.type_count = 5u;
    fixture.assume_fields[0].type_local = 2u;

    fixture.manually_drop_fields[0].type_local = 4u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.manually_drop_fields[0].type_local = 5u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.manually_drop_fields[0].type_local = 3u;

    fixture.manually_drop_fields[0].source_ordinal = 1u;
    fixture.maybe_uninit_fields[1].source_ordinal = 0u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.manually_drop_fields[0].source_ordinal = 0u;
    fixture.maybe_uninit_fields[1].source_ordinal = 1u;
    fixture.maybe_uninit_fields[1].name =
        fixture.maybe_uninit_fields[0].name;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.maybe_uninit_fields[1].name =
        (CmHirDeclarationString)S("value");

    fixture.items[1].aggregate_repr = CM_HIR_DECL_AGGREGATE_REPR_RUST;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.items[1].aggregate_repr =
        CM_HIR_DECL_AGGREGATE_REPR_TRANSPARENT;
    fixture.items[2].aggregate_flags |=
        CM_HIR_DECL_AGGREGATE_RUSTC_PUB_TRANSPARENT;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.items[2].aggregate_flags =
        CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM;
    fixture.items[1].lang_item = fixture.items[0].lang_item;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.items[1].lang_item =
        (CmHirDeclarationString)S("maybe_uninit");
    fixture.items[0].aggregate_flags =
        CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.items[0].aggregate_flags =
        CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM
        | CM_HIR_DECL_AGGREGATE_RUSTC_PUB_TRANSPARENT;
    fixture.items[2].aggregate_flags |= UINT16_C(4);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.items[2].aggregate_flags =
        CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM;

    /* `str` is unsized/non-Copy and cannot be a direct bounded union field. */
    fixture.types[0].primitive = CM_HIR_DECL_PRIMITIVE_STR;
    fixture.types[1].primitive = CM_HIR_DECL_PRIMITIVE_U8;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.types[0].primitive = CM_HIR_DECL_PRIMITIVE_UNIT;
    fixture.types[1].primitive = CM_HIR_DECL_PRIMITIVE_BOOL;

    /* Named structs and unions are TYPE-only, including defining entries. */
    fixture.namespace_entries[4].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.namespace_entries[4].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;

    record = item_record_offset(&encoded, UINT32_C(1));
    payload = skip_string(&encoded, record + 8u) + 44u;
    field = payload + 8u;
    field_tail = skip_string(&encoded, field);
    bad = copy_bytes(&encoded);
    bad.data[payload] = CM_HIR_DECL_AGGREGATE_UNIT;
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);
    bad = copy_bytes(&encoded);
    put_u32(bad.data + payload + 4u,
        (uint32_t)CM_HIR_DECL_METADATA_MAX_FIELDS + 1u);
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);
    bad = copy_bytes(&encoded);
    bad.data[field_tail] = CM_HIR_DECL_VISIBILITY_RESTRICTED;
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);
    bad = copy_bytes(&encoded);
    put_u32(bad.data + field_tail + 12u, UINT32_C(0));
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);
    bad = copy_bytes(&encoded);
    put_u32(bad.data + field, UINT32_MAX);
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

static void test_type_id_crate_field_family(void)
{
    TypeIdFixture fixture;
    TypeIdFixture repeated_fixture;
    CmHirDeclarationMetadata decoded;
    CmByteBuf encoded;
    CmByteBuf repeated;
    CmByteBuf replay;
    CmByteBuf bad;
    size_t record;
    size_t payload;
    size_t field;
    size_t field_visibility;
    size_t array_record;

    type_id_fixture_init(&fixture);
    type_id_fixture_init(&repeated_fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_validate(&repeated_fixture.metadata)
            == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&repeated);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&repeated_fixture.metadata,
            &repeated) == CM_HIR_DECL_METADATA_OK
        && encoded.len == repeated.len
        && memcmp(encoded.data, repeated.data, encoded.len) == 0);

    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
            &decoded) == CM_HIR_DECL_METADATA_OK
        && decoded.item_count == 1u
        && decoded.items[0].kind == CM_HIR_DECL_ITEM_STRUCT
        && decoded.items[0].aggregate_form
            == CM_HIR_DECL_AGGREGATE_NAMED
        && decoded.items[0].aggregate_repr
            == CM_HIR_DECL_AGGREGATE_REPR_RUST
        && decoded.items[0].field_count == 1u
        && decoded.items[0].fields[0].visibility.kind
            == CM_HIR_DECL_VISIBILITY_CRATE
        && decoded.items[0].fields[0].visibility.restriction_module == 0u
        && decoded.items[0].fields[0].source_ordinal == 0u
        && decoded.items[0].fields[0].type_local == 4u
        && decoded.types[0].primitive == CM_HIR_DECL_PRIMITIVE_UNIT
        && decoded.types[1].primitive == CM_HIR_DECL_PRIMITIVE_USIZE
        && decoded.types[2].kind == CM_HIR_DECL_TYPE_RAW_POINTER
        && decoded.types[2].mutability == CM_HIR_DECL_IMMUTABLE
        && decoded.types[2].child_type == 1u
        && decoded.types[3].kind == CM_HIR_DECL_TYPE_ARRAY
        && decoded.types[3].child_type == 3u
        && decoded.types[3].array_length_type == 2u
        && decoded.types[3].array_length_low_bits == UINT64_C(2)
        && decoded.types[3].array_length_high_bits == UINT64_C(0)
        && decoded.namespace_count == 1u
        && decoded.namespace_entries[0].namespace_kind
            == CM_HIR_DECL_NAMESPACE_TYPE
        && decoded.namespace_entries[0].target_kind
            == CM_HIR_DECL_TARGET_ITEM);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
            == CM_HIR_DECL_METADATA_OK
        && encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

    type_id_fixture_init(&fixture);
    fixture.fields[0].visibility.kind =
        CM_HIR_DECL_VISIBILITY_RESTRICTED;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    type_id_fixture_init(&fixture);
    fixture.fields[0].visibility.restriction_module = 1u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    type_id_fixture_init(&fixture);
    fixture.items[0].aggregate_form = CM_HIR_DECL_AGGREGATE_TUPLE;
    fixture.fields[0].name.data = NULL;
    fixture.fields[0].name.length = 0u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    type_id_fixture_init(&fixture);
    fixture.types[3].array_length_type = 1u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    type_id_fixture_init(&fixture);
    fixture.types[3].array_length_high_bits = UINT64_C(1);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    type_id_fixture_init(&fixture);
    fixture.metadata.namespace_count = 0u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);

    record = item_record_offset(&encoded, UINT32_C(0));
    payload = skip_string(&encoded, record + 8u) + 44u;
    field = payload + 8u;
    field_visibility = skip_string(&encoded, field);
    bad = copy_bytes(&encoded);
    bad.data[field_visibility] = CM_HIR_DECL_VISIBILITY_RESTRICTED;
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    put_u32(bad.data + field_visibility + 4u, UINT32_C(1));
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    array_record = type_record_offset(&encoded, UINT32_C(3));
    bad = copy_bytes(&encoded);
    bad.data[array_record + 20u] = UINT8_C(1);
    recompute_family_crc(&bad, UINT8_C(2), "TYPE");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    assert(bad.len != 0u);
    bad.len -= 1u;
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&repeated);
    cm_byte_buf_destroy(&encoded);
}

static void test_layout_private_tuple_and_wide_enum_family(void)
{
    LayoutFixture fixture;
    CmHirDeclarationMetadata decoded;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmByteBuf bad;
    size_t record;
    size_t payload;
    size_t variant;
    CmHirDeclarationString saved_name;
    uint32_t saved_type;
    uint8_t saved_visibility;

    layout_fixture_init(&fixture);
    assert(CM_HIR_DECL_ENUM_REPR_U8 == CM_HIR_DECL_PRIMITIVE_U8
        && CM_HIR_DECL_ENUM_REPR_U16 == CM_HIR_DECL_PRIMITIVE_U16
        && CM_HIR_DECL_ENUM_REPR_U32 == CM_HIR_DECL_PRIMITIVE_U32
        && CM_HIR_DECL_ENUM_REPR_U64 == CM_HIR_DECL_PRIMITIVE_U64
        && cm_hir_declaration_metadata_validate(&fixture.metadata)
            == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
            &decoded) == CM_HIR_DECL_METADATA_OK
        && decoded.item_count == 3u
        && decoded.items[0].visibility.kind
            == CM_HIR_DECL_VISIBILITY_PRIVATE
        && decoded.items[0].aggregate_form
            == CM_HIR_DECL_AGGREGATE_TUPLE
        && decoded.items[0].fields[0].name.data == NULL
        && decoded.items[0].fields[0].name.length == 0u
        && decoded.items[1].enum_repr_primitive
            == CM_HIR_DECL_ENUM_REPR_U64
        && decoded.items[1].variants[1].discriminant_low == UINT64_MAX
        && decoded.items[2].visibility.kind
            == CM_HIR_DECL_VISIBILITY_PUBLIC
        && decoded.namespace_count == 1u
        && decoded.namespace_entries[0].target_local == 3u);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
            == CM_HIR_DECL_METADATA_OK
        && replay.len == encoded.len
        && memcmp(replay.data, encoded.data, encoded.len) == 0);

    fixture.items[1].enum_repr_primitive = CM_HIR_DECL_ENUM_REPR_U8;
    fixture.alignment_variants[1].discriminant_low = UINT64_C(255);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    fixture.alignment_variants[1].discriminant_low = UINT64_C(256);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.items[1].enum_repr_primitive = CM_HIR_DECL_ENUM_REPR_U16;
    fixture.alignment_variants[1].discriminant_low = UINT64_C(65535);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    fixture.alignment_variants[1].discriminant_low = UINT64_C(65536);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.items[1].enum_repr_primitive = CM_HIR_DECL_ENUM_REPR_U32;
    fixture.alignment_variants[1].discriminant_low = UINT64_C(4294967295);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    fixture.alignment_variants[1].discriminant_low = UINT64_C(4294967296);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.items[1].enum_repr_primitive = CM_HIR_DECL_ENUM_REPR_U64;
    fixture.alignment_variants[1].discriminant_low = UINT64_MAX;
    fixture.alignment_variants[1].discriminant_high = UINT64_C(1);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.alignment_variants[1].discriminant_high = UINT64_C(0);
    fixture.alignment_variants[1].discriminant_low = UINT64_C(1);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.alignment_variants[1].discriminant_low = UINT64_MAX;
    fixture.alignment_variants[1].discriminant_primitive =
        CM_HIR_DECL_PRIMITIVE_U64;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.alignment_variants[1].discriminant_primitive =
        CM_HIR_DECL_PRIMITIVE_ISIZE;

    saved_name = fixture.alignment_fields[0].name;
    fixture.alignment_fields[0].name = (CmHirDeclarationString)S("field");
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.alignment_fields[0].name = saved_name;
    fixture.alignment_fields[0].visibility.kind =
        CM_HIR_DECL_VISIBILITY_PUBLIC;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.alignment_fields[0].visibility.kind =
        CM_HIR_DECL_VISIBILITY_PRIVATE;

    saved_type = fixture.layout_fields[1].type_local;
    fixture.layout_fields[1].type_local = 1u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.layout_fields[1].type_local = saved_type;

    saved_visibility = fixture.items[0].visibility.kind;
    fixture.items[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.items[0].visibility.kind = saved_visibility;
    fixture.namespace_entries[0].name = fixture.items[0].name;
    fixture.namespace_entries[0].target_local = 1u;
    fixture.namespace_entries[0].export_ordinal = 10u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.namespace_entries[0].name = fixture.items[2].name;
    fixture.namespace_entries[0].target_local = 3u;
    fixture.namespace_entries[0].export_ordinal = 20u;
    fixture.metadata.namespace_count = 0u;
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    fixture.metadata.namespace_count = 1u;

    record = item_record_offset(&encoded, UINT32_C(0));
    payload = skip_string(&encoded, record + 8u) + 44u;
    bad = copy_bytes(&encoded);
    bad.data[payload] = UINT8_C(0);
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    record = item_record_offset(&encoded, UINT32_C(1));
    payload = skip_string(&encoded, record + 8u) + 44u;
    bad = copy_bytes(&encoded);
    bad.data[payload] = CM_HIR_DECL_PRIMITIVE_U128;
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    variant = enum_variant_record_offset(&encoded, UINT32_C(1),
        UINT32_C(1));
    bad = copy_bytes(&encoded);
    bad.data[variant + 1u] = CM_HIR_DECL_PRIMITIVE_U64;
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    payload = skip_string(&encoded, variant + 4u);
    bad = copy_bytes(&encoded);
    bad.data[payload + 12u] = UINT8_C(1);
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    put_u32(bad.data + payload + 4u, UINT32_C(1));
    put_u32(bad.data + payload + 8u, UINT32_C(0));
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    record = item_record_offset(&encoded, UINT32_C(2));
    payload = skip_string(&encoded, record + 8u);
    bad = copy_bytes(&encoded);
    bad.data[payload] = CM_HIR_DECL_VISIBILITY_PRIVATE;
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    bad.len -= 1u;
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

static void test_associated_method_family(void)
{
    AssociatedMethodFixture first;
    AssociatedMethodFixture second;
    TestFixture old_fixture;
    CmHirDeclarationMetadata decoded;
    CmByteBuf encoded;
    CmByteBuf repeated;
    CmByteBuf replay;
    CmByteBuf old_bytes;
    CmByteBuf bad;
    size_t record;
    size_t cursor;
    size_t predicate_record;
    size_t aitm;

    associated_method_fixture_init(&first);
    associated_method_fixture_init(&second);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&repeated);
    cm_byte_buf_init(&replay);
    cm_byte_buf_init(&old_bytes);
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&first.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&second.metadata, &repeated)
        == CM_HIR_DECL_METADATA_OK);
    assert(encoded.len == repeated.len
        && memcmp(encoded.data, repeated.data, encoded.len) == 0);
    assert(get_u32(encoded.data
        + manifest_family_offset(&encoded, UINT8_C(6)) + 4u) == 2u);

    record = associated_record_offset(&encoded, UINT32_C(0));
    assert(encoded.data[record] == CM_HIR_DECL_ASSOCIATED_METHOD
        && encoded.data[record + 1u]
            == CM_HIR_DECL_ASSOCIATED_PARENT_NOMINAL
        && get_u32(encoded.data + record + 4u) == 1u);
    cursor = skip_string(&encoded, record + 12u) + 48u;
    assert(encoded.data[cursor] == CM_HIR_DECL_RECEIVER_REF_SHARED
        && get_u32(encoded.data + cursor + 4u) == 2u);

    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(decoded.associated_count == 2u
        && decoded.traits[0].safety == CM_HIR_DECL_SAFETY_UNSAFE
        && decoded.traits[0].associated_start == 1u
        && decoded.traits[0].associated_count == 2u
        && decoded.associated_items[0].receiver
            == CM_HIR_DECL_RECEIVER_REF_SHARED
        && decoded.associated_items[0].parameter_count == 2u
        && decoded.associated_items[0].parameter_types[0] == 4u
        && decoded.associated_items[1].has_default_body == 1u
        && decoded.types[2].kind == CM_HIR_DECL_TYPE_SELF
        && decoded.types[3].region.kind == CM_HIR_DECL_REGION_ERASED
        && decoded.predicates[0].owner_kind
            == CM_HIR_DECL_PREDICATE_OWNER_ASSOCIATED
        && decoded.predicates[0].owner_associated == 2u
        && decoded.predicates[0].owner_value == 0u);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
        == CM_HIR_DECL_METADATA_OK);
    assert(encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

    fixture_init(&old_fixture);
    assert(cm_hir_declaration_metadata_encode(&old_fixture.metadata,
        &old_bytes) == CM_HIR_DECL_METADATA_OK);
    aitm = section_offset(&old_bytes, "AITM");
    assert(get_u64(old_bytes.data + aitm + 4u) == UINT64_C(4)
        && get_u32(old_bytes.data + aitm + 12u) == 0u
        && get_u32(old_bytes.data
            + manifest_family_offset(&old_bytes, UINT8_C(6)) + 4u) == 0u);

    associated_method_fixture_init(&first);
    first.traits[0].associated_count = 1u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    associated_method_fixture_init(&first);
    first.traits[0].safety = CM_HIR_DECL_SAFETY_SAFE;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_OK);
    associated_method_fixture_init(&first);
    first.associated_items[1].source_ordinal = 2u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    associated_method_fixture_init(&first);
    first.associated_items[1].name = first.associated_items[0].name;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    associated_method_fixture_init(&first);
    first.associated_items[0].receiver = CM_HIR_DECL_RECEIVER_REF_MUTABLE;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    associated_method_fixture_init(&first);
    first.allocate_parameters[0] = 2u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    associated_method_fixture_init(&first);
    first.types[3].region.kind = CM_HIR_DECL_REGION_STATIC;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    associated_method_fixture_init(&first);
    first.types[2].self_trait_local = 2u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    associated_method_fixture_init(&first);
    first.associated_items[0].abi = (CmHirDeclarationString)S("C");
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    associated_method_fixture_init(&first);
    first.predicates[0].owner_associated = 1u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    associated_method_fixture_init(&first);
    first.predicates[0].owner_kind = CM_HIR_DECL_PREDICATE_OWNER_VALUE;
    first.predicates[0].owner_associated = 0u;
    first.predicates[0].owner_value = 1u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    associated_method_fixture_init(&first);
    first.generics[0].owner_kind = CM_HIR_DECL_GENERIC_NOMINAL;
    first.generics[0].owner_local = 1u;
    first.generics[0].kind = CM_HIR_DECL_GENERIC_TYPE;
    first.generics[0].name = (CmHirDeclarationString)S("T");
    first.metadata.generics = first.generics;
    first.metadata.generic_count = 1u;
    first.traits[0].generic_start = 1u;
    first.traits[0].generic_count = 1u;
    memset(&first.types[2], 0, 4u * sizeof(first.types[0]));
    first.types[2].kind = CM_HIR_DECL_TYPE_GENERIC;
    first.types[2].generic_local = 1u;
    first.types[3].kind = CM_HIR_DECL_TYPE_SELF;
    first.types[3].self_trait_local = 1u;
    first.types[4].kind = CM_HIR_DECL_TYPE_RAW_POINTER;
    first.types[4].child_type = 3u;
    first.types[4].mutability = CM_HIR_DECL_IMMUTABLE;
    first.types[5].kind = CM_HIR_DECL_TYPE_REFERENCE;
    first.types[5].child_type = 4u;
    first.types[5].mutability = CM_HIR_DECL_IMMUTABLE;
    first.types[5].region.kind = CM_HIR_DECL_REGION_ERASED;
    first.metadata.type_count = 6u;
    first.allocate_parameters[0] = 6u;
    first.allocate_parameters[1] = 5u;
    first.fallback_parameters[0] = 6u;
    first.predicates[0].subject_type = 4u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);

    bad = copy_bytes(&encoded);
    record = associated_record_offset(&bad, UINT32_C(0));
    bad.data[record] = UINT8_C(2);
    recompute_family_crc(&bad, UINT8_C(6), "AITM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    record = associated_record_offset(&bad, UINT32_C(0));
    cursor = skip_string(&bad, record + 12u) + 48u;
    bad.data[cursor] = CM_HIR_DECL_RECEIVER_REF_MUTABLE;
    recompute_family_crc(&bad, UINT8_C(6), "AITM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    predicate_record = section_offset(&bad, "PRED") + 24u;
    bad.data[predicate_record + 1u] = UINT8_C(2);
    recompute_crc(&bad);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    aitm = section_offset(&bad, "AITM");
    put_u32(bad.data + aitm + 12u,
        (uint32_t)CM_HIR_DECL_METADATA_MAX_ASSOCIATED_ITEMS + 1u);
    recompute_crc(&bad);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    assert(bad.len != 0u);
    --bad.len;
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&old_bytes);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&repeated);
    cm_byte_buf_destroy(&encoded);
}

static void test_any_trait_outlives_family(void)
{
    AnyFixture first;
    AnyFixture second;
    CmHirDeclarationMetadata decoded;
    CmByteBuf encoded;
    CmByteBuf repeated;
    CmByteBuf replay;
    CmByteBuf bad;
    size_t record;
    size_t cursor;

    any_fixture_init(&first);
    any_fixture_init(&second);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&repeated);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&first.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&second.metadata, &repeated)
        == CM_HIR_DECL_METADATA_OK);
    assert(encoded.len == repeated.len
        && memcmp(encoded.data, repeated.data, encoded.len) == 0);
    record = section_offset(&encoded, "PRED") + 24u;
    assert(encoded.data[record] == UINT8_C(5)
        && encoded.data[record + 1u]
            == CM_HIR_DECL_PREDICATE_OWNER_NOMINAL
        && get_u32(encoded.data + record + 4u) == 1u
        && get_u32(encoded.data + record + 8u) == 0u
        && get_u32(encoded.data + record + 16u) == 2u
        && encoded.data[record + 20u] == CM_HIR_DECL_REGION_STATIC);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(decoded.trait_count == 1u
        && decoded.traits[0].safety == CM_HIR_DECL_SAFETY_SAFE
        && decoded.traits[0].flags
            == CM_HIR_DECL_TRAIT_HAS_DIAGNOSTIC_ITEM
        && decoded.traits[0].diagnostic_item.length == 3u
        && memcmp(decoded.traits[0].diagnostic_item.data, "Any", 3u) == 0
        && decoded.traits[0].outlives_start == 1u
        && decoded.traits[0].outlives_count == 1u
        && decoded.outlives_predicate_count == 1u
        && decoded.outlives_predicates[0].owner_kind
            == CM_HIR_DECL_PREDICATE_OWNER_NOMINAL
        && decoded.outlives_predicates[0].subject_type == 2u
        && decoded.outlives_predicates[0].bound.kind
            == CM_HIR_DECL_REGION_STATIC
        && decoded.associated_items[0].safety
            == CM_HIR_DECL_SAFETY_SAFE);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
        == CM_HIR_DECL_METADATA_OK);
    assert(encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

    any_fixture_init(&first);
    first.traits[0].flags = 0u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    any_fixture_init(&first);
    first.traits[0].diagnostic_item = (CmHirDeclarationString)S("bad-name");
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    any_fixture_init(&first);
    first.traits[0].predicate_start = 1u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    any_fixture_init(&first);
    first.traits[0].outlives_start = 0u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    any_fixture_init(&first);
    first.outlives[0].owner_kind =
        CM_HIR_DECL_PREDICATE_OWNER_ASSOCIATED;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    any_fixture_init(&first);
    first.outlives[0].owner_local = 2u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    any_fixture_init(&first);
    first.outlives[0].ordinal = 1u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    any_fixture_init(&first);
    first.outlives[0].subject_type = 1u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    any_fixture_init(&first);
    first.outlives[0].bound.kind = CM_HIR_DECL_REGION_ERASED;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    any_fixture_init(&first);
    first.outlives[0].bound.generic_local = 1u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    any_fixture_init(&first);
    first.outlives[0].scope = 1u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    any_fixture_init(&first);
    first.metadata.outlives_predicate_count = 0u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    {
        AssociatedMethodFixture collision;
        associated_method_fixture_init(&collision);
        collision.traits[0].flags =
            CM_HIR_DECL_TRAIT_HAS_DIAGNOSTIC_ITEM;
        collision.traits[0].diagnostic_item =
            (CmHirDeclarationString)S("Duplicate");
        collision.traits[1].flags =
            CM_HIR_DECL_TRAIT_HAS_DIAGNOSTIC_ITEM;
        collision.traits[1].diagnostic_item =
            (CmHirDeclarationString)S("Duplicate");
        assert(cm_hir_declaration_metadata_validate(&collision.metadata)
            == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    }

    bad = copy_bytes(&encoded);
    record = section_offset(&bad, "PRED") + 24u;
    bad.data[record] = UINT8_C(4);
    recompute_crc(&bad);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    record = section_offset(&bad, "NOMD") + 16u;
    cursor = skip_string(&bad, record + 8u) + 53u;
    bad.data[cursor] = UINT8_C(0);
    recompute_family_crc(&bad, UINT8_C(4), "NOMD");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    --bad.len;
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&repeated);
    cm_byte_buf_destroy(&encoded);
}

static void test_primitive_namespace_target(void)
{
    PrimitiveNamespaceFixture first;
    PrimitiveNamespaceFixture second;
    CmHirDeclarationMetadata decoded;
    CmByteBuf encoded;
    CmByteBuf repeated;
    CmByteBuf replay;
    CmByteBuf bad;
    CmHirDeclarationNamespaceEntry swapped;
    uint32_t primitive;
    size_t record;
    size_t target_payload;

    primitive_namespace_fixture_init(&first);
    primitive_namespace_fixture_init(&second);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&repeated);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&first.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&second.metadata, &repeated)
        == CM_HIR_DECL_METADATA_OK);
    assert(encoded.len == repeated.len
        && memcmp(encoded.data, repeated.data, encoded.len) == 0);

    record = namespace_record_offset(&encoded, UINT32_C(0));
    target_payload = skip_string(&encoded, record + 8u);
    assert(encoded.data[record + 4u] == CM_HIR_DECL_NAMESPACE_TYPE
        && encoded.data[record + 5u] == CM_HIR_DECL_TARGET_PRIMITIVE
        && get_u32(encoded.data + target_payload)
            == (uint32_t)CM_HIR_DECL_PRIMITIVE_BOOL);

    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(decoded.type_count == 0u && decoded.item_count == 0u
        && decoded.value_count == 0u && decoded.namespace_count == 2u
        && decoded.namespace_entries[0].target_kind
            == CM_HIR_DECL_TARGET_PRIMITIVE
        && decoded.namespace_entries[0].target_local
            == (uint32_t)CM_HIR_DECL_PRIMITIVE_BOOL
        && decoded.namespace_entries[1].target_kind
            == CM_HIR_DECL_TARGET_PRIMITIVE
        && decoded.namespace_entries[1].target_local
            == decoded.namespace_entries[0].target_local);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
        == CM_HIR_DECL_METADATA_OK);
    assert(encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

    for (primitive = (uint32_t)CM_HIR_DECL_PRIMITIVE_BOOL;
         primitive <= (uint32_t)CM_HIR_DECL_PRIMITIVE_F64; ++primitive) {
        primitive_namespace_fixture_init(&first);
        first.namespace_entries[0].target_local = primitive;
        first.namespace_entries[1].target_local = primitive;
        assert(cm_hir_declaration_metadata_validate(&first.metadata)
            == CM_HIR_DECL_METADATA_OK);
    }

    primitive_namespace_fixture_init(&first);
    first.namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    primitive_namespace_fixture_init(&first);
    first.namespace_entries[0].target_local =
        CM_HIR_DECL_PRIMITIVE_UNIT;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    primitive_namespace_fixture_init(&first);
    first.namespace_entries[0].target_local =
        (uint32_t)CM_HIR_DECL_PRIMITIVE_F64 + UINT32_C(1);
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    primitive_namespace_fixture_init(&first);
    first.namespace_entries[1].name = first.namespace_entries[0].name;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    primitive_namespace_fixture_init(&first);
    swapped = first.namespace_entries[0];
    first.namespace_entries[0] = first.namespace_entries[1];
    first.namespace_entries[1] = swapped;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);

    bad = copy_bytes(&encoded);
    record = namespace_record_offset(&bad, UINT32_C(0));
    bad.data[record + 5u] = UINT8_C(7);
    recompute_module_family_crc(&bad);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    record = namespace_record_offset(&bad, UINT32_C(0));
    target_payload = skip_string(&bad, record + 8u);
    put_u32(bad.data + target_payload,
        (uint32_t)CM_HIR_DECL_PRIMITIVE_UNIT);
    recompute_module_family_crc(&bad);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    record = namespace_record_offset(&bad, UINT32_C(0));
    bad.data[record + 4u] = CM_HIR_DECL_NAMESPACE_VALUE;
    recompute_module_family_crc(&bad);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&repeated);
    cm_byte_buf_destroy(&encoded);
}

static void test_into_iter_const_generic_closure(void)
{
    IntoIterFixture first;
    IntoIterFixture second;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMetadata sentinel;
    CmByteBuf encoded;
    CmByteBuf repeated;
    CmByteBuf replay;
    CmByteBuf bad;
    size_t record;

    into_iter_fixture_init(&first);
    into_iter_fixture_init(&second);
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&repeated);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&first.metadata, &encoded)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&second.metadata, &repeated)
            == CM_HIR_DECL_METADATA_OK
        && encoded.len == repeated.len
        && memcmp(encoded.data, repeated.data, encoded.len) == 0);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
            &decoded) == CM_HIR_DECL_METADATA_OK
        && decoded.generic_count == 4u
        && decoded.generics[2].kind == CM_HIR_DECL_GENERIC_CONST
        && decoded.generics[2].declared_type == 2u
        && decoded.generics[2].has_default == 0u
        && decoded.types[9].array_length_kind
            == CM_HIR_DECL_ARRAY_LENGTH_CONST_PARAMETER
        && decoded.types[9].array_length_generic_local == 3u
        && decoded.items[2].aggregate_flags
            == (CM_HIR_DECL_AGGREGATE_HAS_DIAGNOSTIC_ITEM
                | CM_HIR_DECL_AGGREGATE_RUSTC_INSIGNIFICANT_DTOR)
        && decoded.items[3].visibility.kind
            == CM_HIR_DECL_VISIBILITY_RESTRICTED
        && decoded.items[3].visibility.restriction_module == 2u
        && decoded.items[3].predicate_start == 1u
        && decoded.items[3].predicate_count == 1u
        && decoded.predicates[0].owner_kind
            == CM_HIR_DECL_PREDICATE_OWNER_ITEM
        && decoded.predicates[0].owner_item == 4u
        && decoded.traits[0].visibility.kind
            == CM_HIR_DECL_VISIBILITY_PRIVATE
        && decoded.associated_items[0].receiver
            == CM_HIR_DECL_RECEIVER_REF_MUTABLE
        && cm_hir_declaration_metadata_encode(&decoded, &replay)
            == CM_HIR_DECL_METADATA_OK
        && encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

    into_iter_fixture_init(&first);
    first.generics[2].declared_type = 0u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    into_iter_fixture_init(&first);
    first.generics[2].declared_type = 1u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    into_iter_fixture_init(&first);
    first.generics[2].has_default = 1u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    into_iter_fixture_init(&first);
    first.metadata.types = NULL;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);

    into_iter_fixture_init(&first);
    first.types[9].array_length_kind = UINT8_C(2);
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    into_iter_fixture_init(&first);
    first.types[9].array_length_generic_local = 2u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    into_iter_fixture_init(&first);
    first.types[9].array_length_generic_local = 0u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    into_iter_fixture_init(&first);
    first.types[9].array_length_generic_local = 1u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    into_iter_fixture_init(&first);
    first.types[9].array_length_type = 2u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    into_iter_fixture_init(&first);
    first.types[9].array_length_low_bits = UINT64_C(1);
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);

    into_iter_fixture_init(&first);
    first.items[3].predicate_scope_start = 1u;
    first.items[3].predicate_scope_count = 1u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    into_iter_fixture_init(&first);
    first.items[3].outlives_start = 1u;
    first.items[3].outlives_count = 1u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    into_iter_fixture_init(&first);
    first.items[3].predicate_start = 0u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    into_iter_fixture_init(&first);
    first.predicates[0].owner_item = 3u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    into_iter_fixture_init(&first);
    first.predicates[0].subject_type = 4u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);

    into_iter_fixture_init(&first);
    first.items[3].predicate_start = 0u;
    first.items[3].predicate_count = 0u;
    first.metadata.predicates = NULL;
    first.metadata.predicate_count = 0u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    into_iter_fixture_init(&first);
    first.traits[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    into_iter_fixture_init(&first);
    first.items[3].visibility.restriction_module = 3u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    into_iter_fixture_init(&first);
    first.items[3].visibility.kind = CM_HIR_DECL_VISIBILITY_CRATE;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    into_iter_fixture_init(&first);
    first.namespace_entries[0].name = first.items[0].name;
    first.namespace_entries[0].target_local = 1u;
    first.namespace_entries[0].export_ordinal = 1u;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);

    into_iter_fixture_init(&first);
    first.items[2].aggregate_flags &=
        (uint16_t)~CM_HIR_DECL_AGGREGATE_HAS_DIAGNOSTIC_ITEM;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    into_iter_fixture_init(&first);
    first.items[2].diagnostic_item = (CmHirDeclarationString)S("bad-name");
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    into_iter_fixture_init(&first);
    first.items[1].aggregate_flags =
        CM_HIR_DECL_AGGREGATE_RUSTC_INSIGNIFICANT_DTOR;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    into_iter_fixture_init(&first);
    first.traits[0].flags = CM_HIR_DECL_TRAIT_HAS_DIAGNOSTIC_ITEM;
    first.traits[0].diagnostic_item = first.items[2].diagnostic_item;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);

    into_iter_fixture_init(&first);
    first.associated_items[0].receiver =
        CM_HIR_DECL_RECEIVER_REF_SHARED;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    into_iter_fixture_init(&first);
    first.types[7].mutability = CM_HIR_DECL_IMMUTABLE;
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);

    bad = copy_bytes(&encoded);
    record = generic_record_offset(&bad, UINT32_C(2));
    record = skip_string(&bad, record + 16u);
    bad.data[record] = UINT8_C(1);
    recompute_crc(&bad);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);
    bad = copy_bytes(&encoded);
    record = type_record_offset(&bad, UINT32_C(9));
    bad.data[record + 1u] = UINT8_C(2);
    recompute_family_crc(&bad, UINT8_C(2), "TYPE");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);
    bad = copy_bytes(&encoded);
    record = item_record_offset(&bad, UINT32_C(3));
    record = skip_string(&bad, record + 8u);
    put_u32(bad.data + record + 28u, UINT32_C(2));
    recompute_family_crc(&bad, UINT8_C(2), "ITEM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);
    bad = copy_bytes(&encoded);
    record = type_record_offset(&bad, UINT32_C(9));
    bad.data[record + 12u] = UINT8_C(1);
    recompute_family_crc(&bad, UINT8_C(2), "TYPE");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);
    bad = copy_bytes(&encoded);
    bad.data[bad.len - 1u] ^= UINT8_C(1);
    memset(&sentinel, 0, sizeof(sentinel));
    sentinel.root_module = UINT32_C(77);
    assert(cm_hir_declaration_metadata_decode(bad.data, bad.len, &sentinel)
            != CM_HIR_DECL_METADATA_OK
        && sentinel.root_module == UINT32_C(77));
    cm_byte_buf_destroy(&bad);
    bad = copy_bytes(&encoded);
    bad.len -= 1u;
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&repeated);
    cm_byte_buf_destroy(&encoded);
}

static void test_from_fn_callable_closure(void)
{
    FromFnFixture first;
    FromFnFixture second;
    CmHirDeclarationMetadata decoded;
    CmByteBuf encoded;
    CmByteBuf repeated;
    CmByteBuf replay;
    CmByteBuf bad;
    size_t record;

    from_fn_fixture_init(&first);
    from_fn_fixture_init(&second);
    assert(cm_hir_declaration_metadata_validate(&first.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&repeated);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&first.metadata, &encoded)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&second.metadata, &repeated)
            == CM_HIR_DECL_METADATA_OK
        && encoded.len == repeated.len
        && memcmp(encoded.data, repeated.data, encoded.len) == 0);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
            &decoded) == CM_HIR_DECL_METADATA_OK
        && decoded.trait_count == 3u
        && decoded.traits[0].supertrait_count == 1u
        && decoded.associated_count == 3u
        && decoded.associated_items[1].kind
            == CM_HIR_DECL_ASSOCIATED_TYPE
        && decoded.type_count == 12u
        && decoded.types[10].kind == CM_HIR_DECL_TYPE_PROJECTION
        && decoded.value_count == 1u
        && decoded.values[0].generic_count == 3u
        && decoded.predicate_count == 3u
        && decoded.predicates[0].equality_count == 1u
        && cm_hir_declaration_metadata_encode(&decoded, &replay)
            == CM_HIR_DECL_METADATA_OK
        && encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

#define ASSERT_INVALID_FROM_FN(change_) do { \
        from_fn_fixture_init(&first); \
        change_; \
        assert(cm_hir_declaration_metadata_validate(&first.metadata) \
            == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR); \
    } while (0)
    ASSERT_INVALID_FROM_FN(first.traits[0].flags &=
        (uint8_t)~CM_HIR_DECL_TRAIT_FUNDAMENTAL);
    ASSERT_INVALID_FROM_FN(first.traits[2].lang_item =
        first.traits[1].lang_item);
    ASSERT_INVALID_FROM_FN(first.supertraits[0].trait_local = 1u);
    ASSERT_INVALID_FROM_FN(first.supertrait_arguments[0] = 3u);
    ASSERT_INVALID_FROM_FN(first.traits[0].supertrait_count = 0u;
        first.traits[0].supertraits = NULL);
    ASSERT_INVALID_FROM_FN(first.associated_items[1].flags = 0u;
        first.associated_items[1].lang_item.data = NULL;
        first.associated_items[1].lang_item.length = 0u);
    ASSERT_INVALID_FROM_FN(first.associated_items[1].parent_local = 1u);
    ASSERT_INVALID_FROM_FN(first.associated_items[0].receiver =
        CM_HIR_DECL_RECEIVER_REF_SHARED);
    ASSERT_INVALID_FROM_FN(first.associated_items[2].abi =
        (CmHirDeclarationString)S("Rust"));
    ASSERT_INVALID_FROM_FN(first.types[10].projection_trait_local = 1u);
    ASSERT_INVALID_FROM_FN(
        first.types[10].projection_associated_local = 1u);
    ASSERT_INVALID_FROM_FN(first.types[10].projection_self_type = 7u);
    ASSERT_INVALID_FROM_FN(first.projection_mut_arguments[0] = 3u);
    ASSERT_INVALID_FROM_FN(first.generics[3].owner_local = 2u);
    ASSERT_INVALID_FROM_FN(first.generics[3].declared_type = 0u);
    ASSERT_INVALID_FROM_FN(first.types[9].array_length_generic_local = 1u);
    ASSERT_INVALID_FROM_FN(first.values[0].generic_count = 2u);
    ASSERT_INVALID_FROM_FN(first.values[0].predicate_count = 0u;
        first.values[0].predicate_start = 0u);
    ASSERT_INVALID_FROM_FN(first.predicates[0].equality_count = 0u;
        first.predicates[0].equalities = NULL);
    ASSERT_INVALID_FROM_FN(first.value_equalities[0].associated_local = 1u);
    ASSERT_INVALID_FROM_FN(first.value_equalities[0].value_type = 5u);
    ASSERT_INVALID_FROM_FN(first.value_predicate_arguments[0] = 1u);
    ASSERT_INVALID_FROM_FN(first.metadata.types = NULL);
#undef ASSERT_INVALID_FROM_FN

    bad = copy_bytes(&encoded);
    record = type_record_offset(&bad, UINT32_C(10));
    put_u32(bad.data + record + 12u, UINT32_C(0));
    recompute_family_crc(&bad, UINT8_C(2), "TYPE");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    record = associated_record_offset(&bad, UINT32_C(1));
    record = skip_string(&bad, record + 12u);
    bad.data[record + 13u] = UINT8_C(0);
    recompute_family_crc(&bad, UINT8_C(6), "AITM");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    record = section_offset(&bad, "NOMD") + 16u;
    record = skip_string(&bad, record + 8u);
    put_u32(bad.data + record + 60u, UINT32_MAX);
    recompute_family_crc(&bad, UINT8_C(4), "NOMD");
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    record = section_offset(&bad, "PRED") + 24u;
    put_u32(bad.data + record + 28u, UINT32_MAX);
    recompute_crc(&bad);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    bad = copy_bytes(&encoded);
    bad.data[bad.len - 1u] ^= UINT8_C(1);
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);
    bad = copy_bytes(&encoded);
    assert(bad.len != 0u);
    bad.len -= 1u;
    assert_failed_transaction(&bad);
    cm_byte_buf_destroy(&bad);

    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
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
    test_structural_type_family();
    test_enum_family();
    test_default_enum_variant_namespace();
    test_generic_tuple_enum_family();
    test_enum_named_adt_signature();
    test_const_value_family();
    test_const_function_value_family();
    test_zero_generic_unit_function_family();
    test_contextual_erased_reference_roots();
    test_from_mut_erased_pair();
    test_static_tuple_array_family();
    test_named_aggregate_family();
    test_type_id_crate_field_family();
    test_layout_private_tuple_and_wide_enum_family();
    test_associated_method_family();
    test_any_trait_outlives_family();
    test_primitive_namespace_target();
    test_into_iter_const_generic_closure();
    test_from_fn_callable_closure();
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
        && decoded.values[0].is_const == 0u
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
