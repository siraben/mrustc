#include "cm/hir/metadata.h"

#include "library_internal.h"
#include "metadata_codec.h"

#include <stdlib.h>

#include "cm/alloc.h"

#include <string.h>

#define CM_META_MAX_MODULES UINT32_C(4096)
#define CM_META_MAX_ITEMS UINT32_C(65536)
#define CM_META_MAX_GENERICS UINT32_C(131072)
#define CM_META_MAX_TYPES UINT32_C(262144)
#define CM_META_MAX_ENTRIES UINT32_C(131072)
#define CM_META_MAX_TRAITS UINT32_C(65536)
#define CM_META_MAX_IMPLS UINT32_C(131072)
#define CM_META_MAX_VALUES UINT32_C(131072)
#define CM_META_MAX_STRING UINT32_C(1048576)

#define CM_META_BINDING_MODULE UINT8_C(1)
#define CM_META_BINDING_TYPE UINT8_C(2)
#define CM_META_BINDING_PRIMITIVE UINT8_C(3)
#define CM_META_BINDING_TRAIT UINT8_C(4)
#define CM_META_BINDING_VALUE UINT8_C(5)

#define CM_META_UNIVERSE_OPEN UINT8_C(0)

#define CM_META_ITEM_EXTERN_TYPE UINT8_C(1)
#define CM_META_ITEM_STRUCT UINT8_C(2)
#define CM_META_ITEM_UNION UINT8_C(3)
#define CM_META_ITEM_ENUM UINT8_C(4)
#define CM_META_ITEM_ALIAS UINT8_C(5)

#define CM_META_VALUE_FUNCTION UINT8_C(1)
#define CM_META_VALUE_CONST UINT8_C(2)
#define CM_META_VALUE_STATIC UINT8_C(3)

#define CM_META_GENERIC_LIFETIME UINT8_C(1)
#define CM_META_GENERIC_TYPE UINT8_C(2)
#define CM_META_GENERIC_CONST UINT8_C(3)
#define CM_META_GENERIC_OWNER_ITEM UINT8_C(1)
#define CM_META_GENERIC_OWNER_VALUE UINT8_C(2)

#define CM_META_TYPE_NEVER UINT8_C(1)
#define CM_META_TYPE_UNIT UINT8_C(2)
#define CM_META_TYPE_BOOL UINT8_C(3)
#define CM_META_TYPE_CHAR UINT8_C(4)
#define CM_META_TYPE_STR UINT8_C(5)
#define CM_META_TYPE_INTEGER UINT8_C(6)
#define CM_META_TYPE_FLOAT UINT8_C(7)
#define CM_META_TYPE_REFERENCE UINT8_C(8)
#define CM_META_TYPE_RAW_POINTER UINT8_C(9)
#define CM_META_TYPE_TUPLE UINT8_C(10)
#define CM_META_TYPE_ARRAY UINT8_C(11)
#define CM_META_TYPE_SLICE UINT8_C(12)
#define CM_META_TYPE_ADT UINT8_C(13)
#define CM_META_TYPE_ALIAS UINT8_C(14)
#define CM_META_TYPE_PARAMETER UINT8_C(15)
#define CM_META_TYPE_FOREIGN UINT8_C(16)

#define CM_META_ARG_LIFETIME UINT8_C(1)
#define CM_META_ARG_TYPE UINT8_C(2)
#define CM_META_ARG_CONST UINT8_C(3)
#define CM_META_CONST_VALUE UINT8_C(1)
#define CM_META_CONST_PARAMETER UINT8_C(2)
#define CM_META_REGION_STATIC UINT8_C(1)
#define CM_META_REGION_EARLY_BOUND UINT8_C(2)

#define CM_META_VIS_PRIVATE UINT8_C(1)
#define CM_META_VIS_PUBLIC UINT8_C(2)
#define CM_META_VIS_CRATE UINT8_C(3)
#define CM_META_VIS_RESTRICTED UINT8_C(4)

#define CM_META_FORM_UNIT UINT8_C(1)
#define CM_META_FORM_TUPLE UINT8_C(2)
#define CM_META_FORM_NAMED UINT8_C(3)

#define CM_META_MUT_IMMUTABLE UINT8_C(1)
#define CM_META_MUT_MUTABLE UINT8_C(2)

static const unsigned char cm_meta_tag_crate[4] = {
    (unsigned char)'C', (unsigned char)'R',
    (unsigned char)'A', (unsigned char)'T'
};
static const unsigned char cm_meta_tag_modules[4] = {
    (unsigned char)'M', (unsigned char)'O',
    (unsigned char)'D', (unsigned char)'S'
};
static const unsigned char cm_meta_tag_generics[4] = {
    (unsigned char)'G', (unsigned char)'P',
    (unsigned char)'A', (unsigned char)'R'
};
static const unsigned char cm_meta_tag_types[4] = {
    (unsigned char)'T', (unsigned char)'Y',
    (unsigned char)'P', (unsigned char)'E'
};
static const unsigned char cm_meta_tag_items[4] = {
    (unsigned char)'I', (unsigned char)'T',
    (unsigned char)'E', (unsigned char)'M'
};
static const unsigned char cm_meta_tag_namespace[4] = {
    (unsigned char)'N', (unsigned char)'S',
    (unsigned char)'P', (unsigned char)'C'
};
static const unsigned char cm_meta_tag_values[4] = {
    (unsigned char)'V', (unsigned char)'A',
    (unsigned char)'L', (unsigned char)'U'
};
static const unsigned char cm_meta_tag_trait_universe[4] = {
    (unsigned char)'T', (unsigned char)'U',
    (unsigned char)'N', (unsigned char)'I'
};

typedef struct CmMetaEncodeModule {
    CmHirDefId definition;
    const CmHirModule *module;
    CmByteBuf path;
} CmMetaEncodeModule;

typedef struct CmMetaEncodeItem {
    CmHirDefId definition;
    const CmHirItem *item;
    const CmInternedString *name;
    uint32_t owner;
    uint8_t kind;
} CmMetaEncodeItem;

typedef struct CmMetaEncodeGeneric {
    CmHirGenericParamId id;
    const CmHirGenericParam *parameter;
    uint8_t owner_kind;
    uint32_t owner;
} CmMetaEncodeGeneric;

typedef struct CmMetaEncodeType {
    CmHirTypeId id;
    const CmHirType *type;
} CmMetaEncodeType;

typedef struct CmMetaEncodeEntry {
    const CmInternedString *name;
    uint32_t module;
    uint8_t kind;
    uint32_t target;
} CmMetaEncodeEntry;

typedef struct CmMetaEncodeTrait {
    CmHirDefId definition;
    const CmHirItem *item;
    const CmInternedString *name;
    uint32_t owner;
} CmMetaEncodeTrait;

typedef struct CmMetaEncodeImpl {
    CmHirDefId definition;
    const CmHirItem *item;
    uint32_t owner;
    uint32_t trait_local;
} CmMetaEncodeImpl;

typedef struct CmMetaEncodeValue {
    CmHirDefId definition;
    const CmHirLibraryOwnedValue *value;
    const CmInternedString *canonical_name;
    uint32_t canonical_module;
    uint8_t kind;
} CmMetaEncodeValue;

typedef struct CmMetaWireName {
    const unsigned char *bytes;
    size_t length;
} CmMetaWireName;

typedef struct CmMetaWireModule {
    uint32_t parent;
    CmMetaWireName name;
} CmMetaWireModule;

typedef struct CmMetaWireRegion {
    uint8_t kind;
    uint32_t parameter;
} CmMetaWireRegion;

typedef struct CmMetaWireConst {
    uint8_t kind;
    uint32_t type;
    union {
        struct {
            uint64_t low_bits;
            uint64_t high_bits;
        } value;
        uint32_t parameter;
    } data;
} CmMetaWireConst;

typedef struct CmMetaWireArg {
    uint8_t kind;
    union {
        CmMetaWireRegion lifetime;
        uint32_t type;
        CmMetaWireConst constant;
    } data;
} CmMetaWireArg;

typedef struct CmMetaWireNamed {
    uint32_t item;
    CmMetaWireArg *arguments;
    uint32_t argument_count;
} CmMetaWireNamed;

typedef struct CmMetaWireGeneric {
    uint8_t owner_kind;
    uint32_t owner;
    uint32_t index;
    uint8_t kind;
    CmMetaWireName name;
    int is_relaxed_sized;
    int has_default;
    uint32_t default_type;
} CmMetaWireGeneric;

typedef struct CmMetaWireType {
    uint8_t kind;
    union {
        uint8_t scalar_kind;
        struct {
            CmMetaWireRegion region;
            uint32_t pointee;
            uint8_t mutability;
        } reference_type;
        struct {
            uint32_t pointee;
            uint8_t mutability;
        } raw_pointer_type;
        struct {
            uint32_t *elements;
            uint32_t element_count;
        } tuple_type;
        struct {
            uint32_t element;
            CmMetaWireConst length;
        } array_type;
        struct {
            uint32_t element;
        } slice_type;
        CmMetaWireNamed named_type;
        struct {
            uint32_t parameter;
        } parameter_type;
    } data;
} CmMetaWireType;

typedef struct CmMetaWireVisibility {
    uint8_t kind;
    uint32_t restriction;
} CmMetaWireVisibility;

typedef struct CmMetaWireField {
    CmMetaWireName name;
    uint32_t type;
    CmMetaWireVisibility visibility;
} CmMetaWireField;

typedef struct CmMetaWireVariant {
    CmMetaWireName name;
    uint8_t form;
    CmMetaWireField *fields;
    uint32_t field_count;
    int has_discriminant;
    uint32_t discriminant_type;
    uint64_t discriminant_low;
    uint64_t discriminant_high;
    CmHirDefId runtime_definition;
} CmMetaWireVariant;

typedef struct CmMetaWireItem {
    uint8_t kind;
    uint32_t owner;
    CmMetaWireName name;
    CmMetaWireVisibility visibility;
    uint32_t generic_start;
    uint32_t generic_count;
    union {
        struct {
            uint8_t form;
            CmMetaWireField *fields;
            uint32_t field_count;
        } aggregate_item;
        struct {
            CmMetaWireVariant *variants;
            uint32_t variant_count;
        } enum_item;
        struct {
            uint32_t target;
        } alias_item;
    } data;
} CmMetaWireItem;

typedef struct CmMetaWireEntry {
    uint32_t module;
    CmMetaWireName name;
    uint8_t kind;
    uint32_t target;
} CmMetaWireEntry;

typedef struct CmMetaWireTrait {
    uint32_t owner;
    CmMetaWireName name;
    CmMetaWireVisibility visibility;
    uint8_t safety;
    int is_auto;
} CmMetaWireTrait;

typedef struct CmMetaWireImpl {
    uint32_t owner;
    uint32_t trait_local;
    uint32_t self_type;
    uint8_t safety;
    int is_negative;
} CmMetaWireImpl;

typedef struct CmMetaWireValue {
    uint8_t kind;
    union {
        struct {
            uint32_t *parameter_types;
            uint32_t parameter_count;
            uint32_t return_type;
            uint32_t generic_start;
            uint32_t generic_count;
            CmMetaWireName abi;
            uint8_t safety;
            int is_const;
            int is_async;
            int is_variadic;
        } function;
        struct {
            uint32_t type;
            uint8_t mutability;
        } value;
    } data;
} CmMetaWireValue;

static CmHirMetadataArtifactResult cm_meta_result(
    CmHirMetadataArtifactStatus status)
{
    CmHirMetadataArtifactResult result;

    memset(&result, 0, sizeof(result));
    result.status = status;
    return result;
}

static int cm_meta_identifier_bytes_valid(const unsigned char *bytes,
    size_t length)
{
    size_t index;

    if (bytes == NULL || length == 0u
        || length > (size_t)CM_META_MAX_STRING) return 0;
    if (!((bytes[0] >= (unsigned char)'a'
                && bytes[0] <= (unsigned char)'z')
            || (bytes[0] >= (unsigned char)'A'
                && bytes[0] <= (unsigned char)'Z')
            || bytes[0] == (unsigned char)'_')) return 0;
    for (index = 1u; index < length; ++index) {
        if (!((bytes[index] >= (unsigned char)'a'
                    && bytes[index] <= (unsigned char)'z')
                || (bytes[index] >= (unsigned char)'A'
                    && bytes[index] <= (unsigned char)'Z')
                || (bytes[index] >= (unsigned char)'0'
                    && bytes[index] <= (unsigned char)'9')
                || bytes[index] == (unsigned char)'_')) return 0;
    }
    return 1;
}

static int cm_meta_identifier_c_str_valid(const char *text)
{
    return text != NULL && cm_meta_identifier_bytes_valid(
        (const unsigned char *)text, strlen(text));
}

static int cm_meta_generic_name_bytes_valid(const unsigned char *bytes,
    size_t length, uint8_t kind)
{
    if (kind == CM_META_GENERIC_LIFETIME) {
        return bytes != NULL && length > 1u
            && bytes[0] == (unsigned char)'\''
            && cm_meta_identifier_bytes_valid(bytes + 1u, length - 1u);
    }
    if (kind == CM_META_GENERIC_CONST) {
        return cm_meta_identifier_bytes_valid(bytes, length);
    }
    return kind == CM_META_GENERIC_TYPE
        && cm_meta_identifier_bytes_valid(bytes, length);
}

static int cm_meta_name_equal(CmMetaWireName left, CmMetaWireName right)
{
    return left.length == right.length
        && memcmp(left.bytes, right.bytes, left.length) == 0;
}

static int cm_meta_section_tag_is(const CmHirMetadataSection *section,
    const unsigned char tag[4])
{
    return section != NULL && memcmp(section->tag, tag, 4u) == 0;
}

static CmHirMetadataArtifactStatus cm_meta_codec_status(
    CmHirMetadataStatus status)
{
    switch (status) {
    case CM_HIR_METADATA_OK:
        return CM_HIR_METADATA_ARTIFACT_OK;
    case CM_HIR_METADATA_INVALID_ARGUMENT:
        return CM_HIR_METADATA_ARTIFACT_INVALID_ARGUMENT;
    case CM_HIR_METADATA_LIMIT_EXCEEDED:
    case CM_HIR_METADATA_LENGTH_OVERFLOW:
        return CM_HIR_METADATA_ARTIFACT_LIMIT_EXCEEDED;
    case CM_HIR_METADATA_DONE:
    case CM_HIR_METADATA_TRUNCATED:
    case CM_HIR_METADATA_TRAILING_BYTES:
    case CM_HIR_METADATA_WRONG_MAGIC:
    case CM_HIR_METADATA_UNSUPPORTED_VERSION:
    case CM_HIR_METADATA_UNSUPPORTED_FLAGS:
    case CM_HIR_METADATA_INVALID_HEADER_LENGTH:
    case CM_HIR_METADATA_NONZERO_RESERVED:
    case CM_HIR_METADATA_CRC_MISMATCH:
        return CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT;
    }
    return CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT;
}

static uint8_t cm_meta_edition_to_wire(CmHirEdition edition)
{
    switch (edition) {
    case CM_HIR_EDITION_2015: return UINT8_C(1);
    case CM_HIR_EDITION_2018: return UINT8_C(2);
    case CM_HIR_EDITION_2021: return UINT8_C(3);
    case CM_HIR_EDITION_2024: return UINT8_C(4);
    }
    return UINT8_C(0);
}

static int cm_meta_edition_from_wire(uint8_t wire, CmHirEdition *out_edition)
{
    if (out_edition == NULL) return 0;
    switch (wire) {
    case UINT8_C(1): *out_edition = CM_HIR_EDITION_2015; return 1;
    case UINT8_C(2): *out_edition = CM_HIR_EDITION_2018; return 1;
    case UINT8_C(3): *out_edition = CM_HIR_EDITION_2021; return 1;
    case UINT8_C(4): *out_edition = CM_HIR_EDITION_2024; return 1;
    default: return 0;
    }
}

static uint8_t cm_meta_primitive_to_wire(CmHirPrimitiveKind primitive)
{
    switch (primitive) {
    case CM_HIR_PRIMITIVE_BOOL: return UINT8_C(1);
    case CM_HIR_PRIMITIVE_CHAR: return UINT8_C(2);
    case CM_HIR_PRIMITIVE_STR: return UINT8_C(3);
    case CM_HIR_PRIMITIVE_I8: return UINT8_C(4);
    case CM_HIR_PRIMITIVE_I16: return UINT8_C(5);
    case CM_HIR_PRIMITIVE_I32: return UINT8_C(6);
    case CM_HIR_PRIMITIVE_I64: return UINT8_C(7);
    case CM_HIR_PRIMITIVE_I128: return UINT8_C(8);
    case CM_HIR_PRIMITIVE_ISIZE: return UINT8_C(9);
    case CM_HIR_PRIMITIVE_U8: return UINT8_C(10);
    case CM_HIR_PRIMITIVE_U16: return UINT8_C(11);
    case CM_HIR_PRIMITIVE_U32: return UINT8_C(12);
    case CM_HIR_PRIMITIVE_U64: return UINT8_C(13);
    case CM_HIR_PRIMITIVE_U128: return UINT8_C(14);
    case CM_HIR_PRIMITIVE_USIZE: return UINT8_C(15);
    case CM_HIR_PRIMITIVE_F16: return UINT8_C(16);
    case CM_HIR_PRIMITIVE_F32: return UINT8_C(17);
    case CM_HIR_PRIMITIVE_F64: return UINT8_C(18);
    case CM_HIR_PRIMITIVE_F128: return UINT8_C(19);
    case CM_HIR_PRIMITIVE_NONE: return UINT8_C(0);
    }
    return UINT8_C(0);
}

static int cm_meta_primitive_from_wire(uint32_t wire,
    CmHirPrimitiveKind *out_primitive)
{
    if (out_primitive == NULL) return 0;
    switch (wire) {
    case UINT32_C(1): *out_primitive = CM_HIR_PRIMITIVE_BOOL; return 1;
    case UINT32_C(2): *out_primitive = CM_HIR_PRIMITIVE_CHAR; return 1;
    case UINT32_C(3): *out_primitive = CM_HIR_PRIMITIVE_STR; return 1;
    case UINT32_C(4): *out_primitive = CM_HIR_PRIMITIVE_I8; return 1;
    case UINT32_C(5): *out_primitive = CM_HIR_PRIMITIVE_I16; return 1;
    case UINT32_C(6): *out_primitive = CM_HIR_PRIMITIVE_I32; return 1;
    case UINT32_C(7): *out_primitive = CM_HIR_PRIMITIVE_I64; return 1;
    case UINT32_C(8): *out_primitive = CM_HIR_PRIMITIVE_I128; return 1;
    case UINT32_C(9): *out_primitive = CM_HIR_PRIMITIVE_ISIZE; return 1;
    case UINT32_C(10): *out_primitive = CM_HIR_PRIMITIVE_U8; return 1;
    case UINT32_C(11): *out_primitive = CM_HIR_PRIMITIVE_U16; return 1;
    case UINT32_C(12): *out_primitive = CM_HIR_PRIMITIVE_U32; return 1;
    case UINT32_C(13): *out_primitive = CM_HIR_PRIMITIVE_U64; return 1;
    case UINT32_C(14): *out_primitive = CM_HIR_PRIMITIVE_U128; return 1;
    case UINT32_C(15): *out_primitive = CM_HIR_PRIMITIVE_USIZE; return 1;
    case UINT32_C(16): *out_primitive = CM_HIR_PRIMITIVE_F16; return 1;
    case UINT32_C(17): *out_primitive = CM_HIR_PRIMITIVE_F32; return 1;
    case UINT32_C(18): *out_primitive = CM_HIR_PRIMITIVE_F64; return 1;
    case UINT32_C(19): *out_primitive = CM_HIR_PRIMITIVE_F128; return 1;
    default: return 0;
    }
}

static uint8_t cm_meta_item_kind_to_wire(CmHirItemKind kind)
{
    switch (kind) {
    case CM_HIR_ITEM_EXTERN_TYPE: return CM_META_ITEM_EXTERN_TYPE;
    case CM_HIR_ITEM_STRUCT: return CM_META_ITEM_STRUCT;
    case CM_HIR_ITEM_UNION: return CM_META_ITEM_UNION;
    case CM_HIR_ITEM_ENUM: return CM_META_ITEM_ENUM;
    case CM_HIR_ITEM_TYPE_ALIAS: return CM_META_ITEM_ALIAS;
    default: return UINT8_C(0);
    }
}

static int cm_meta_item_kind_from_wire(uint8_t wire,
    CmHirItemKind *out_kind)
{
    if (out_kind == NULL) return 0;
    switch (wire) {
    case CM_META_ITEM_EXTERN_TYPE:
        *out_kind = CM_HIR_ITEM_EXTERN_TYPE; return 1;
    case CM_META_ITEM_STRUCT:
        *out_kind = CM_HIR_ITEM_STRUCT; return 1;
    case CM_META_ITEM_UNION:
        *out_kind = CM_HIR_ITEM_UNION; return 1;
    case CM_META_ITEM_ENUM:
        *out_kind = CM_HIR_ITEM_ENUM; return 1;
    case CM_META_ITEM_ALIAS:
        *out_kind = CM_HIR_ITEM_TYPE_ALIAS; return 1;
    default: return 0;
    }
}

static CmHirTypeKind cm_meta_item_type_kind(uint8_t item_kind)
{
    if (item_kind == CM_META_ITEM_EXTERN_TYPE)
        return CM_HIR_TYPE_FOREIGN_KIND;
    if (item_kind == CM_META_ITEM_ALIAS)
        return CM_HIR_TYPE_ALIAS_APPLICATION_KIND;
    return CM_HIR_TYPE_ADT_KIND;
}

static uint8_t cm_meta_value_kind_to_wire(CmHirLibraryValueKind kind)
{
    switch (kind) {
    case CM_HIR_LIBRARY_VALUE_FUNCTION: return CM_META_VALUE_FUNCTION;
    case CM_HIR_LIBRARY_VALUE_CONST: return CM_META_VALUE_CONST;
    case CM_HIR_LIBRARY_VALUE_STATIC: return CM_META_VALUE_STATIC;
    case CM_HIR_LIBRARY_VALUE_NONE: return UINT8_C(0);
    }
    return UINT8_C(0);
}

static int cm_meta_value_kind_from_wire(uint8_t wire,
    CmHirLibraryValueKind *out_kind)
{
    if (out_kind == NULL) return 0;
    switch (wire) {
    case CM_META_VALUE_FUNCTION:
        *out_kind = CM_HIR_LIBRARY_VALUE_FUNCTION; return 1;
    case CM_META_VALUE_CONST:
        *out_kind = CM_HIR_LIBRARY_VALUE_CONST; return 1;
    case CM_META_VALUE_STATIC:
        *out_kind = CM_HIR_LIBRARY_VALUE_STATIC; return 1;
    default:
        return 0;
    }
}

static uint8_t cm_meta_integer_to_wire(CmHirIntType kind)
{
    switch (kind) {
    case CM_HIR_INT_I8: return UINT8_C(1);
    case CM_HIR_INT_I16: return UINT8_C(2);
    case CM_HIR_INT_I32: return UINT8_C(3);
    case CM_HIR_INT_I64: return UINT8_C(4);
    case CM_HIR_INT_I128: return UINT8_C(5);
    case CM_HIR_INT_ISIZE: return UINT8_C(6);
    case CM_HIR_INT_U8: return UINT8_C(7);
    case CM_HIR_INT_U16: return UINT8_C(8);
    case CM_HIR_INT_U32: return UINT8_C(9);
    case CM_HIR_INT_U64: return UINT8_C(10);
    case CM_HIR_INT_U128: return UINT8_C(11);
    case CM_HIR_INT_USIZE: return UINT8_C(12);
    }
    return UINT8_C(0);
}

static int cm_meta_integer_from_wire(uint8_t wire, CmHirIntType *out_kind)
{
    if (out_kind == NULL) return 0;
    switch (wire) {
    case UINT8_C(1): *out_kind = CM_HIR_INT_I8; return 1;
    case UINT8_C(2): *out_kind = CM_HIR_INT_I16; return 1;
    case UINT8_C(3): *out_kind = CM_HIR_INT_I32; return 1;
    case UINT8_C(4): *out_kind = CM_HIR_INT_I64; return 1;
    case UINT8_C(5): *out_kind = CM_HIR_INT_I128; return 1;
    case UINT8_C(6): *out_kind = CM_HIR_INT_ISIZE; return 1;
    case UINT8_C(7): *out_kind = CM_HIR_INT_U8; return 1;
    case UINT8_C(8): *out_kind = CM_HIR_INT_U16; return 1;
    case UINT8_C(9): *out_kind = CM_HIR_INT_U32; return 1;
    case UINT8_C(10): *out_kind = CM_HIR_INT_U64; return 1;
    case UINT8_C(11): *out_kind = CM_HIR_INT_U128; return 1;
    case UINT8_C(12): *out_kind = CM_HIR_INT_USIZE; return 1;
    default: return 0;
    }
}

static uint8_t cm_meta_float_to_wire(CmHirFloatType kind)
{
    switch (kind) {
    case CM_HIR_FLOAT_F16: return UINT8_C(1);
    case CM_HIR_FLOAT_F32: return UINT8_C(2);
    case CM_HIR_FLOAT_F64: return UINT8_C(3);
    case CM_HIR_FLOAT_F128: return UINT8_C(4);
    }
    return UINT8_C(0);
}

static int cm_meta_float_from_wire(uint8_t wire,
    CmHirFloatType *out_kind)
{
    if (out_kind == NULL) return 0;
    switch (wire) {
    case UINT8_C(1): *out_kind = CM_HIR_FLOAT_F16; return 1;
    case UINT8_C(2): *out_kind = CM_HIR_FLOAT_F32; return 1;
    case UINT8_C(3): *out_kind = CM_HIR_FLOAT_F64; return 1;
    case UINT8_C(4): *out_kind = CM_HIR_FLOAT_F128; return 1;
    default: return 0;
    }
}

static uint8_t cm_meta_form_to_wire(CmHirAggregateForm form)
{
    switch (form) {
    case CM_HIR_AGGREGATE_UNIT: return CM_META_FORM_UNIT;
    case CM_HIR_AGGREGATE_TUPLE: return CM_META_FORM_TUPLE;
    case CM_HIR_AGGREGATE_NAMED: return CM_META_FORM_NAMED;
    }
    return UINT8_C(0);
}

static int cm_meta_form_from_wire(uint8_t wire,
    CmHirAggregateForm *out_form)
{
    if (out_form == NULL) return 0;
    switch (wire) {
    case CM_META_FORM_UNIT: *out_form = CM_HIR_AGGREGATE_UNIT; return 1;
    case CM_META_FORM_TUPLE: *out_form = CM_HIR_AGGREGATE_TUPLE; return 1;
    case CM_META_FORM_NAMED: *out_form = CM_HIR_AGGREGATE_NAMED; return 1;
    default: return 0;
    }
}

static int cm_meta_bytes_compare(const unsigned char *left,
    size_t left_length, const unsigned char *right, size_t right_length)
{
    size_t common;
    int compared;

    common = left_length < right_length ? left_length : right_length;
    compared = common == 0u ? 0 : memcmp(left, right, common);
    if (compared != 0) return compared;
    if (left_length < right_length) return -1;
    if (left_length > right_length) return 1;
    return 0;
}

static int cm_meta_name_compare(const CmInternedString *left,
    const CmInternedString *right)
{
    if (left == NULL) return right == NULL ? 0 : -1;
    if (right == NULL) return 1;
    return cm_meta_bytes_compare(left->bytes, left->len, right->bytes,
        right->len);
}

static int cm_meta_owned_has_module(const CmHirLibraryOwnedData *owned,
    CmHirDefId definition)
{
    size_t index;

    for (index = 0u; index < owned->modules.len; ++index) {
        const CmHirLibraryOwnedModule *module;

        module = (const CmHirLibraryOwnedModule *)cm_vec_at_const(
            &owned->modules, index);
        if (module != NULL
            && cm_hir_def_id_equal(module->definition, definition)) return 1;
    }
    return 0;
}

static uint32_t cm_meta_module_local(const CmVec *modules,
    CmHirDefId definition)
{
    size_t index;

    for (index = 0u; index < modules->len; ++index) {
        const CmMetaEncodeModule *module;

        module = (const CmMetaEncodeModule *)cm_vec_at_const(modules,
            index);
        if (module != NULL
            && cm_hir_def_id_equal(module->definition, definition)) {
            return (uint32_t)(index + 1u);
        }
    }
    return UINT32_C(0);
}

static uint32_t cm_meta_item_local(const CmVec *items,
    CmHirDefId definition)
{
    size_t index;

    for (index = 0u; index < items->len; ++index) {
        const CmMetaEncodeItem *item;

        item = (const CmMetaEncodeItem *)cm_vec_at_const(items, index);
        if (item != NULL
            && cm_hir_def_id_equal(item->definition, definition)) {
            return (uint32_t)(index + 1u);
        }
    }
    return UINT32_C(0);
}

static uint32_t cm_meta_trait_local(const CmVec *traits,
    CmHirDefId definition)
{
    size_t index;

    for (index = 0u; index < traits->len; ++index) {
        const CmMetaEncodeTrait *trait_value;

        trait_value = (const CmMetaEncodeTrait *)cm_vec_at_const(traits,
            index);
        if (trait_value != NULL
            && cm_hir_def_id_equal(trait_value->definition, definition)) {
            return (uint32_t)(index + 1u);
        }
    }
    return UINT32_C(0);
}

static uint32_t cm_meta_value_local(const CmVec *values,
    CmHirDefId definition)
{
    size_t index;

    for (index = 0u; index < values->len; ++index) {
        const CmMetaEncodeValue *value;

        value = (const CmMetaEncodeValue *)cm_vec_at_const(values, index);
        if (value != NULL
            && cm_hir_def_id_equal(value->definition, definition)) {
            return (uint32_t)(index + 1u);
        }
    }
    return UINT32_C(0);
}

static int cm_meta_module_compare(const void *left_value,
    const void *right_value)
{
    const CmMetaEncodeModule *left;
    const CmMetaEncodeModule *right;

    left = (const CmMetaEncodeModule *)left_value;
    right = (const CmMetaEncodeModule *)right_value;
    return cm_meta_bytes_compare(left->path.data, left->path.len,
        right->path.data, right->path.len);
}

static int cm_meta_item_compare(const void *left_value,
    const void *right_value)
{
    const CmMetaEncodeItem *left;
    const CmMetaEncodeItem *right;

    left = (const CmMetaEncodeItem *)left_value;
    right = (const CmMetaEncodeItem *)right_value;
    if (left->owner < right->owner) return -1;
    if (left->owner > right->owner) return 1;
    {
        int names;

        names = cm_meta_name_compare(left->name, right->name);
        if (names != 0) return names;
    }
    if (left->kind < right->kind) return -1;
    if (left->kind > right->kind) return 1;
    return 0;
}

static int cm_meta_trait_compare(const void *left_value,
    const void *right_value)
{
    const CmMetaEncodeTrait *left;
    const CmMetaEncodeTrait *right;
    int names;

    left = (const CmMetaEncodeTrait *)left_value;
    right = (const CmMetaEncodeTrait *)right_value;
    if (left->owner < right->owner) return -1;
    if (left->owner > right->owner) return 1;
    names = cm_meta_name_compare(left->name, right->name);
    if (names != 0) return names;
    if (left->definition.index < right->definition.index) return -1;
    if (left->definition.index > right->definition.index) return 1;
    return 0;
}

static int cm_meta_value_compare(const void *left_value,
    const void *right_value)
{
    const CmMetaEncodeValue *left;
    const CmMetaEncodeValue *right;
    int names;

    left = (const CmMetaEncodeValue *)left_value;
    right = (const CmMetaEncodeValue *)right_value;
    if (left->canonical_module < right->canonical_module) return -1;
    if (left->canonical_module > right->canonical_module) return 1;
    names = cm_meta_name_compare(left->canonical_name,
        right->canonical_name);
    if (names != 0) return names;
    if (left->kind < right->kind) return -1;
    if (left->kind > right->kind) return 1;
    return 0;
}

static int cm_meta_impl_compare(const void *left_value,
    const void *right_value)
{
    const CmMetaEncodeImpl *left;
    const CmMetaEncodeImpl *right;

    left = (const CmMetaEncodeImpl *)left_value;
    right = (const CmMetaEncodeImpl *)right_value;
    if (left->trait_local < right->trait_local) return -1;
    if (left->trait_local > right->trait_local) return 1;
    if (left->definition.index < right->definition.index) return -1;
    if (left->definition.index > right->definition.index) return 1;
    return 0;
}

static int cm_meta_entry_compare(const void *left_value,
    const void *right_value)
{
    const CmMetaEncodeEntry *left;
    const CmMetaEncodeEntry *right;
    int names;

    left = (const CmMetaEncodeEntry *)left_value;
    right = (const CmMetaEncodeEntry *)right_value;
    if (left->module < right->module) return -1;
    if (left->module > right->module) return 1;
    names = cm_meta_name_compare(left->name, right->name);
    if (names != 0) return names;
    if (left->kind < right->kind) return -1;
    if (left->kind > right->kind) return 1;
    if (left->target < right->target) return -1;
    if (left->target > right->target) return 1;
    return 0;
}

static int cm_meta_build_module_path(
    const CmHirLibraryArtifactIdentity *identity,
    const CmHirLibraryOwnedData *owned, const CmHirModule *module,
    CmByteBuf *path)
{
    CmVec reversed;
    const CmHirModule *current;
    size_t depth;
    size_t index;
    int valid;

    cm_vec_init(&reversed, sizeof(const CmHirModule *));
    current = module;
    depth = 0u;
    valid = 1;
    while (current != NULL && !cm_hir_def_id_equal(current->definition,
            identity->root_definition)) {
        const CmHirModule *stored;

        if (current->crate_id != identity->crate_id
            || !cm_meta_owned_has_module(owned, current->definition)
            || current->parent == CM_HIR_MODULE_NONE
            || depth++ >= owned->modules.len) {
            valid = 0;
            break;
        }
        stored = current;
        (void)cm_vec_push(&reversed, &stored);
        current = cm_hir_get_module(identity->context, current->parent);
    }
    if (current == NULL || current->crate_id != identity->crate_id
        || !cm_meta_owned_has_module(owned, identity->root_definition)) {
        valid = 0;
    }
    for (index = reversed.len; valid && index != 0u; --index) {
        const CmHirModule *const *component;
        const CmInternedString *name;

        component = (const CmHirModule *const *)cm_vec_at_const(&reversed,
            index - 1u);
        name = component == NULL ? NULL : cm_interner_get(
            &identity->context->strings, (*component)->name);
        if (name == NULL || !cm_meta_identifier_bytes_valid(name->bytes,
                name->len)) {
            valid = 0;
            break;
        }
        cm_byte_buf_push(path, (unsigned char)'/');
        cm_byte_buf_append(path, name->bytes, name->len);
    }
    cm_vec_destroy(&reversed);
    return valid;
}

static void cm_meta_encode_modules_destroy(CmVec *modules)
{
    size_t index;

    for (index = 0u; index < modules->len; ++index) {
        CmMetaEncodeModule *module;

        module = (CmMetaEncodeModule *)cm_vec_at(modules, index);
        if (module != NULL) cm_byte_buf_destroy(&module->path);
    }
    cm_vec_destroy(modules);
}

static int cm_meta_collect_modules(
    const CmHirLibraryArtifactIdentity *identity,
    const CmHirLibraryOwnedData *owned, CmVec *modules)
{
    size_t index;

    for (index = 0u; index < owned->modules.len; ++index) {
        const CmHirLibraryOwnedModule *owned_module;
        const CmHirDefinition *definition;
        CmMetaEncodeModule module;

        owned_module = (const CmHirLibraryOwnedModule *)cm_vec_at_const(
            &owned->modules, index);
        definition = owned_module == NULL ? NULL
            : cm_hir_lookup_definition(identity->context,
                owned_module->definition);
        memset(&module, 0, sizeof(module));
        module.definition = owned_module == NULL
            ? cm_hir_def_id_none() : owned_module->definition;
        module.module = definition == NULL
                || definition->kind != CM_HIR_DEFINITION_MODULE
                || definition->state != CM_HIR_DEFINITION_BOUND
            ? NULL : cm_hir_get_module(identity->context,
                definition->entity.module_id);
        cm_byte_buf_init(&module.path);
        if (module.module == NULL
            || !cm_meta_build_module_path(identity, owned, module.module,
                &module.path)) {
            cm_byte_buf_destroy(&module.path);
            return 0;
        }
        (void)cm_vec_push(modules, &module);
    }
    if (modules->len > 1u) {
        qsort(modules->data, modules->len, sizeof(CmMetaEncodeModule),
            cm_meta_module_compare);
    }
    for (index = 0u; index < modules->len; ++index) {
        const CmMetaEncodeModule *module;

        module = (const CmMetaEncodeModule *)cm_vec_at_const(modules,
            index);
        if (module == NULL
            || (index == 0u
                && (!cm_hir_def_id_equal(module->definition,
                        identity->root_definition)
                    || module->path.len != 0u))
            || (index != 0u && module->path.len == 0u)) return 0;
        if (index != 0u) {
            const CmMetaEncodeModule *prior;

            prior = (const CmMetaEncodeModule *)cm_vec_at_const(modules,
                index - 1u);
            if (prior == NULL || cm_meta_module_compare(prior, module) == 0)
                return 0;
        }
    }
    return 1;
}

static int cm_meta_write_name(CmHirMetadataWriter *writer,
    const CmInternedString *name)
{
    if (name == NULL || !cm_meta_identifier_bytes_valid(name->bytes,
            name->len) || name->len > (size_t)UINT32_MAX) return 0;
    return cm_hir_metadata_write_u32(writer, (uint32_t)name->len)
            == CM_HIR_METADATA_OK
        && cm_hir_metadata_write_bytes(writer, name->bytes, name->len)
            == CM_HIR_METADATA_OK;
}

static int cm_meta_write_string(CmHirMetadataWriter *writer,
    const CmInternedString *string)
{
    if (string == NULL || string->len == 0u
        || string->len > (size_t)CM_META_MAX_STRING
        || string->len > (size_t)UINT32_MAX) return 0;
    return cm_hir_metadata_write_u32(writer, (uint32_t)string->len)
            == CM_HIR_METADATA_OK
        && cm_hir_metadata_write_bytes(writer, string->bytes, string->len)
            == CM_HIR_METADATA_OK;
}

static int cm_meta_write_generic_name(CmHirMetadataWriter *writer,
    const CmInternedString *name, uint8_t kind)
{
    if (name == NULL || !cm_meta_generic_name_bytes_valid(name->bytes,
            name->len, kind) || name->len > (size_t)UINT32_MAX) return 0;
    return cm_hir_metadata_write_u32(writer, (uint32_t)name->len)
            == CM_HIR_METADATA_OK
        && cm_hir_metadata_write_bytes(writer, name->bytes, name->len)
            == CM_HIR_METADATA_OK;
}

static int cm_meta_collect_items(const CmHirLibraryArtifactIdentity *identity,
    const CmVec *modules, CmVec *items, int semantic, int declaration)
{
    size_t item_index;

    for (item_index = 0u; item_index < identity->context->items.len;
            ++item_index) {
        const CmHirItem *item;
        const CmHirModule *owner;
        CmMetaEncodeItem collected;

        item = (const CmHirItem *)cm_vec_at_const(&identity->context->items,
            item_index);
        if (item == NULL) return 0;
        if (item->definition.crate_id != identity->crate_id) continue;
        if (item->kind == CM_HIR_ITEM_MODULE) continue;
        if (declaration && (item->kind == CM_HIR_ITEM_FUNCTION
                || item->kind == CM_HIR_ITEM_CONST
                || item->kind == CM_HIR_ITEM_STATIC)) continue;
        if (semantic && (item->kind == CM_HIR_ITEM_TRAIT
                || item->kind == CM_HIR_ITEM_IMPL)) continue;
        /*
         * Associated items ride with their owning trait rather than being
         * captured separately; attributes and where-clause predicates are
         * outside the declaration slice and stay unconsumed here.
         */
        if (!cm_hir_def_id_is_none(item->parent_definition)) continue;
        memset(&collected, 0, sizeof(collected));
        collected.kind = cm_meta_item_kind_to_wire(item->kind);
        owner = cm_hir_get_module(identity->context, item->owner_module);
        /*
         * Trait- and value-bearing kinds require the semantic or
         * declaration-v2 boundaries; plain cmhir-meta-v1 rejects them so
         * a consumer never mistakes an omitted family for completeness.
         */
        if (collected.kind == 0u || owner == NULL) return 0;
        if (item->kind == CM_HIR_ITEM_TYPE_ALIAS
            && (item->data.type_alias_item.target == CM_HIR_TYPE_NONE
                || !cm_hir_def_id_is_none(
                    item->data.type_alias_item.trait_item_definition))) {
            return 0;
        }
        collected.definition = item->definition;
        collected.item = item;
        collected.owner = cm_meta_module_local(modules, owner->definition);
        collected.name = cm_interner_get(&identity->context->strings,
            item->name);
        if (collected.owner == 0u || collected.name == NULL) return 0;
        (void)cm_vec_push(items, &collected);
        if (items->len > (size_t)CM_META_MAX_ITEMS) return 0;
    }
    if (items->len > 1u) {
        qsort(items->data, items->len, sizeof(CmMetaEncodeItem),
            cm_meta_item_compare);
    }
    for (item_index = 1u; item_index < items->len; ++item_index) {
        const CmMetaEncodeItem *prior;
        const CmMetaEncodeItem *item;

        prior = (const CmMetaEncodeItem *)cm_vec_at_const(items,
            item_index - 1u);
        item = (const CmMetaEncodeItem *)cm_vec_at_const(items,
            item_index);
        if (prior == NULL || item == NULL
            || (prior->owner == item->owner
                && prior->kind == item->kind
                && cm_meta_name_compare(prior->name, item->name) == 0)) {
            return 0;
        }
    }
    return 1;
}

static int cm_meta_collect_trait_universe(
    const CmHirLibraryArtifactIdentity *identity, const CmVec *modules,
    CmVec *traits, CmVec *impls)
{
    size_t item_index;

    for (item_index = 0u; item_index < identity->context->items.len;
            ++item_index) {
        const CmHirItem *item;
        const CmHirModule *owner;

        item = (const CmHirItem *)cm_vec_at_const(&identity->context->items,
            item_index);
        if (item == NULL) return 0;
        if (item->definition.crate_id != identity->crate_id
            || (item->kind != CM_HIR_ITEM_TRAIT
                && item->kind != CM_HIR_ITEM_IMPL)) continue;
        owner = cm_hir_get_module(identity->context, item->owner_module);
        if (owner == NULL || owner->crate_id != identity->crate_id
            || !cm_hir_def_id_is_none(item->parent_definition)
            || item->attribute_count != 0u
            || item->generic_parameter_count != 0u
            || item->predicate_scope_count != 0u
            || item->predicate_count != 0u
            || item->outlives_predicate_count != 0u) return 0;
        if (item->kind == CM_HIR_ITEM_TRAIT) {
            CmMetaEncodeTrait trait_value;

            if (item->data.trait_item.supertrait_count != 0u) return 0;
            memset(&trait_value, 0, sizeof(trait_value));
            trait_value.definition = item->definition;
            trait_value.item = item;
            trait_value.name = cm_interner_get(&identity->context->strings,
                item->name);
            trait_value.owner = cm_meta_module_local(modules,
                owner->definition);
            if (trait_value.name == NULL || trait_value.owner == 0u)
                return 0;
            (void)cm_vec_push(traits, &trait_value);
            if (traits->len > (size_t)CM_META_MAX_TRAITS) return 0;
        }
    }
    if (traits->len > 1u) {
        qsort(traits->data, traits->len, sizeof(CmMetaEncodeTrait),
            cm_meta_trait_compare);
    }
    for (item_index = 1u; item_index < traits->len; ++item_index) {
        const CmMetaEncodeTrait *prior;
        const CmMetaEncodeTrait *trait_value;

        prior = (const CmMetaEncodeTrait *)cm_vec_at_const(traits,
            item_index - 1u);
        trait_value = (const CmMetaEncodeTrait *)cm_vec_at_const(traits,
            item_index);
        if (prior == NULL || trait_value == NULL
            || (prior->owner == trait_value->owner
                && cm_meta_name_compare(prior->name,
                    trait_value->name) == 0)) return 0;
    }
    for (item_index = 0u; item_index < identity->context->items.len;
            ++item_index) {
        const CmHirItem *item;
        const CmHirModule *owner;
        CmMetaEncodeImpl impl_value;

        item = (const CmHirItem *)cm_vec_at_const(&identity->context->items,
            item_index);
        if (item == NULL) return 0;
        if (item->definition.crate_id != identity->crate_id
            || item->kind != CM_HIR_ITEM_IMPL) continue;
        if (!item->data.impl_item.has_trait) continue;
        owner = cm_hir_get_module(identity->context, item->owner_module);
        memset(&impl_value, 0, sizeof(impl_value));
        impl_value.definition = item->definition;
        impl_value.item = item;
        impl_value.owner = owner == NULL ? UINT32_C(0)
            : cm_meta_module_local(modules, owner->definition);
        impl_value.trait_local = cm_meta_trait_local(traits,
            item->data.impl_item.trait_type.definition);
        if (owner == NULL || owner->crate_id != identity->crate_id
            || impl_value.owner == 0u || impl_value.trait_local == 0u
            || !cm_hir_def_id_is_none(item->parent_definition)
            || item->attribute_count != 0u
            || item->generic_parameter_count != 0u
            || item->predicate_scope_count != 0u
            || item->predicate_count != 0u
            || item->outlives_predicate_count != 0u
            || item->data.impl_item.trait_type.argument_count != 0u
            || item->data.impl_item.trait_type.arguments != NULL) return 0;
        (void)cm_vec_push(impls, &impl_value);
        if (impls->len > (size_t)CM_META_MAX_IMPLS) return 0;
    }
    if (impls->len > 1u) {
        qsort(impls->data, impls->len, sizeof(CmMetaEncodeImpl),
            cm_meta_impl_compare);
    }
    return 1;
}

static int cm_meta_collect_values(const CmHirLibraryOwnedData *owned,
    const CmVec *modules, CmVec *values)
{
    size_t value_index;

    if (owned == NULL || modules == NULL || values == NULL
        || owned->values.elem_size != sizeof(CmHirLibraryOwnedValue)) {
        return 0;
    }
    for (value_index = 0u; value_index < owned->values.len; ++value_index) {
        const CmHirLibraryOwnedValue *owned_value;
        CmMetaEncodeValue collected;
        size_t module_index;

        owned_value = (const CmHirLibraryOwnedValue *)cm_vec_at_const(
            &owned->values, value_index);
        memset(&collected, 0, sizeof(collected));
        if (owned_value == NULL) return 0;
        collected.definition = owned_value->declaration.definition;
        collected.value = owned_value;
        collected.kind = cm_meta_value_kind_to_wire(
            owned_value->declaration.kind);
        if (collected.kind == 0u) return 0;
        for (module_index = 0u; module_index < owned->modules.len;
                ++module_index) {
            const CmHirLibraryOwnedModule *module;
            uint32_t module_local;
            size_t entry_index;

            module = (const CmHirLibraryOwnedModule *)cm_vec_at_const(
                &owned->modules, module_index);
            module_local = module == NULL ? UINT32_C(0)
                : cm_meta_module_local(modules, module->definition);
            if (module == NULL || module_local == 0u) return 0;
            for (entry_index = 0u; entry_index < module->entries.len;
                    ++entry_index) {
                const CmHirLibraryOwnedEntry *entry;
                const CmInternedString *name;

                entry = (const CmHirLibraryOwnedEntry *)cm_vec_at_const(
                    &module->entries, entry_index);
                if (entry == NULL
                    || entry->kind != CM_HIR_LIBRARY_BINDING_VALUE
                    || !cm_hir_def_id_equal(entry->target,
                        collected.definition)
                    || entry->value_kind
                        != owned_value->declaration.kind) continue;
                name = cm_interner_get(&owned->names, entry->name);
                if (name == NULL) return 0;
                if (collected.canonical_module == 0u
                    || module_local < collected.canonical_module
                    || (module_local == collected.canonical_module
                        && cm_meta_name_compare(name,
                            collected.canonical_name) < 0)) {
                    collected.canonical_module = module_local;
                    collected.canonical_name = name;
                }
            }
        }
        if (collected.canonical_module == 0u
            || collected.canonical_name == NULL) return 0;
        (void)cm_vec_push(values, &collected);
        if (values->len > (size_t)CM_META_MAX_VALUES) return 0;
    }
    if (values->len > 1u) {
        qsort(values->data, values->len, sizeof(CmMetaEncodeValue),
            cm_meta_value_compare);
    }
    for (value_index = 1u; value_index < values->len; ++value_index) {
        const CmMetaEncodeValue *prior;
        const CmMetaEncodeValue *value;

        prior = (const CmMetaEncodeValue *)cm_vec_at_const(values,
            value_index - 1u);
        value = (const CmMetaEncodeValue *)cm_vec_at_const(values,
            value_index);
        if (prior == NULL || value == NULL
            || (prior->canonical_module == value->canonical_module
                && cm_meta_name_compare(prior->canonical_name,
                    value->canonical_name) == 0)) return 0;
    }
    return 1;
}

static int cm_meta_collect_entries(const CmHirLibraryOwnedData *owned,
    const CmVec *modules, const CmVec *items, const CmVec *traits,
    const CmVec *values, CmVec *entries,
    size_t *out_public_entry_count)
{
    size_t module_index;
    size_t public_entry_count;

    public_entry_count = 0u;

    for (module_index = 0u; module_index < owned->modules.len;
            ++module_index) {
        const CmHirLibraryOwnedModule *owned_module;
        uint32_t owner;
        size_t entry_index;

        owned_module = (const CmHirLibraryOwnedModule *)cm_vec_at_const(
            &owned->modules, module_index);
        owner = owned_module == NULL ? UINT32_C(0)
            : cm_meta_module_local(modules, owned_module->definition);
        if (owner == 0u) return 0;
        for (entry_index = 0u; entry_index < owned_module->entries.len;
                ++entry_index) {
            const CmHirLibraryOwnedEntry *entry;
            CmMetaEncodeEntry collected;

            entry = (const CmHirLibraryOwnedEntry *)cm_vec_at_const(
                &owned_module->entries, entry_index);
            memset(&collected, 0, sizeof(collected));
            collected.module = owner;
            collected.name = entry == NULL ? NULL
                : cm_interner_get(&owned->names, entry->name);
            if (entry != NULL
                && entry->kind == CM_HIR_LIBRARY_BINDING_MODULE) {
                collected.kind = CM_META_BINDING_MODULE;
                collected.target = cm_meta_module_local(modules,
                    entry->target);
            } else if (entry != NULL
                && entry->kind == CM_HIR_LIBRARY_BINDING_TYPE
                && entry->primitive_kind == CM_HIR_PRIMITIVE_NONE) {
                const CmMetaEncodeItem *target_item;

                collected.kind = CM_META_BINDING_TYPE;
                collected.target = cm_meta_item_local(items, entry->target);
                target_item = collected.target == 0u ? NULL
                    : (const CmMetaEncodeItem *)cm_vec_at_const(items,
                        (size_t)(collected.target - 1u));
                if (target_item == NULL || entry->type_kind
                        != cm_meta_item_type_kind(target_item->kind)) {
                    return 0;
                }
            } else if (entry != NULL
                && entry->kind == CM_HIR_LIBRARY_BINDING_PRIMITIVE) {
                collected.kind = CM_META_BINDING_PRIMITIVE;
                collected.target = (uint32_t)cm_meta_primitive_to_wire(
                    entry->primitive_kind);
            } else if (entry != NULL && traits != NULL
                && entry->kind == CM_HIR_LIBRARY_BINDING_TRAIT) {
                collected.kind = CM_META_BINDING_TRAIT;
                collected.target = cm_meta_trait_local(traits,
                    entry->target);
            } else if (entry != NULL && values != NULL
                && entry->kind == CM_HIR_LIBRARY_BINDING_VALUE) {
                const CmMetaEncodeValue *target_value;

                collected.kind = CM_META_BINDING_VALUE;
                collected.target = cm_meta_value_local(values,
                    entry->target);
                target_value = collected.target == 0u ? NULL
                    : (const CmMetaEncodeValue *)cm_vec_at_const(values,
                        (size_t)(collected.target - 1u));
                if (target_value == NULL
                    || target_value->value->declaration.kind
                        != entry->value_kind) return 0;
            }
            if (collected.name == NULL || collected.kind == 0u
                || collected.target == 0u) return 0;
            if (collected.kind != CM_META_BINDING_MODULE) {
                if (public_entry_count >= (size_t)CM_META_MAX_ENTRIES)
                    return 0;
                public_entry_count += 1u;
            }
            (void)cm_vec_push(entries, &collected);
            if (entries->len > (size_t)CM_META_MAX_ENTRIES) return 0;
        }
    }
    if (entries->len > 1u) {
        qsort(entries->data, entries->len, sizeof(CmMetaEncodeEntry),
            cm_meta_entry_compare);
    }
    for (module_index = 1u; module_index < entries->len; ++module_index) {
        const CmMetaEncodeEntry *prior;
        const CmMetaEncodeEntry *entry;

        prior = (const CmMetaEncodeEntry *)cm_vec_at_const(entries,
            module_index - 1u);
        entry = (const CmMetaEncodeEntry *)cm_vec_at_const(entries,
            module_index);
        if (prior == NULL || entry == NULL
            || (prior->module == entry->module
                && cm_meta_name_compare(prior->name, entry->name) == 0
                && (prior->kind == CM_META_BINDING_VALUE)
                    == (entry->kind == CM_META_BINDING_VALUE))) {
            return 0;
        }
    }
    *out_public_entry_count = public_entry_count;
    return 1;
}

static uint32_t cm_meta_generic_local(const CmVec *generics,
    CmHirGenericParamId id)
{
    size_t index;

    for (index = 0u; index < generics->len; ++index) {
        const CmMetaEncodeGeneric *generic;

        generic = (const CmMetaEncodeGeneric *)cm_vec_at_const(generics,
            index);
        if (generic != NULL && generic->id == id)
            return (uint32_t)(index + 1u);
    }
    return UINT32_C(0);
}

static uint32_t cm_meta_type_local(const CmVec *types, CmHirTypeId id)
{
    size_t index;

    for (index = 0u; index < types->len; ++index) {
        const CmMetaEncodeType *type;

        type = (const CmMetaEncodeType *)cm_vec_at_const(types, index);
        if (type != NULL && type->id == id)
            return (uint32_t)(index + 1u);
    }
    return UINT32_C(0);
}

static int cm_meta_collect_generic_range(
    const CmHirLibraryArtifactIdentity *identity, CmVec *generics,
    CmHirDefId owner_definition, CmHirGenericParamId start, uint32_t count,
    uint8_t owner_kind, uint32_t owner_local)
{
    uint32_t parameter_index;

    if ((count == 0u && start != CM_HIR_GENERIC_PARAM_NONE)
        || (count != 0u && start == CM_HIR_GENERIC_PARAM_NONE)
        || (owner_kind != CM_META_GENERIC_OWNER_ITEM
            && owner_kind != CM_META_GENERIC_OWNER_VALUE)
        || owner_local == 0u) return 0;
    for (parameter_index = 0u; parameter_index < count;
            ++parameter_index) {
        CmHirGenericParamId id;
        const CmHirGenericParam *parameter;
        const CmInternedString *name;
        CmMetaEncodeGeneric encoded;

        id = start + parameter_index;
        if (id < start) return 0;
        parameter = cm_hir_get_generic_param(identity->context, id);
        name = parameter == NULL ? NULL : cm_interner_get(
            &identity->context->strings, parameter->name);
        if (parameter == NULL || name == NULL
            || !cm_meta_generic_name_bytes_valid(name->bytes, name->len,
                parameter->kind == CM_HIR_GENERIC_LIFETIME
                    ? CM_META_GENERIC_LIFETIME : CM_META_GENERIC_TYPE)
            || !cm_hir_def_id_equal(parameter->owner, owner_definition)
            || parameter->index != parameter_index
            || (parameter->kind == CM_HIR_GENERIC_CONST
                ? parameter->declared_type == CM_HIR_TYPE_NONE
                : parameter->declared_type != CM_HIR_TYPE_NONE)
            || (parameter->kind != CM_HIR_GENERIC_LIFETIME
                && parameter->kind != CM_HIR_GENERIC_TYPE
                && parameter->kind != CM_HIR_GENERIC_CONST)
            || (parameter->kind == CM_HIR_GENERIC_LIFETIME
                && (parameter->is_relaxed_sized || parameter->has_default))
            || (parameter->has_default
                && parameter->default_argument.kind
                    != CM_HIR_GENERIC_ARG_TYPE)
            || (parameter->kind == CM_HIR_GENERIC_CONST
                && parameter->has_default)) return 0;
        memset(&encoded, 0, sizeof(encoded));
        encoded.id = id;
        encoded.parameter = parameter;
        encoded.owner_kind = owner_kind;
        encoded.owner = owner_local;
        (void)cm_vec_push(generics, &encoded);
        if (generics->len > (size_t)CM_META_MAX_GENERICS) return 0;
    }
    return 1;
}

static int cm_meta_collect_generics(
    const CmHirLibraryArtifactIdentity *identity, const CmVec *items,
    const CmVec *values, CmVec *generics)
{
    size_t owner_index;

    for (owner_index = 0u; owner_index < items->len; ++owner_index) {
        const CmMetaEncodeItem *encoded_item;
        const CmHirItem *item;

        encoded_item = (const CmMetaEncodeItem *)cm_vec_at_const(items,
            owner_index);
        item = encoded_item == NULL ? NULL : encoded_item->item;
        if (item == NULL || !cm_meta_collect_generic_range(identity,
                generics, item->definition, item->generic_parameter_start,
                item->generic_parameter_count, CM_META_GENERIC_OWNER_ITEM,
                (uint32_t)(owner_index + 1u))) return 0;
    }
    if (values == NULL) return 1;
    for (owner_index = 0u; owner_index < values->len; ++owner_index) {
        const CmMetaEncodeValue *encoded_value;
        const CmHirLibraryValue *value;

        encoded_value = (const CmMetaEncodeValue *)cm_vec_at_const(values,
            owner_index);
        value = encoded_value == NULL || encoded_value->value == NULL ? NULL
            : &encoded_value->value->declaration;
        if (value == NULL) return 0;
        if (value->kind != CM_HIR_LIBRARY_VALUE_FUNCTION) {
            continue;
        }
        if (!cm_meta_collect_generic_range(identity, generics,
                value->definition,
                value->data.function.generic_parameter_start,
                value->data.function.generic_parameter_count,
                CM_META_GENERIC_OWNER_VALUE,
                (uint32_t)(owner_index + 1u))) return 0;
    }
    return 1;
}

static int cm_meta_region_supported(const CmHirRegion *region,
    const CmVec *generics)
{
    const CmMetaEncodeGeneric *generic;
    uint32_t local;

    if (region->kind == CM_HIR_REGION_STATIC) return 1;
    if (region->kind != CM_HIR_REGION_EARLY_BOUND) return 0;
    local = cm_meta_generic_local(generics, region->data.parameter);
    generic = local == 0u ? NULL
        : (const CmMetaEncodeGeneric *)cm_vec_at_const(generics,
            (size_t)(local - 1u));
    return generic != NULL
        && generic->parameter->kind == CM_HIR_GENERIC_LIFETIME;
}

static int cm_meta_collect_type(const CmHirLibraryArtifactIdentity *identity,
    const CmVec *items, const CmVec *generics, CmVec *types,
    unsigned char *states, CmHirTypeId id);

static int cm_meta_scalar_const_type_equal(const CmHirContext *context,
    CmHirTypeId left_id, CmHirTypeId right_id)
{
    const CmHirType *left;
    const CmHirType *right;

    left = cm_hir_get_type(context, left_id);
    right = cm_hir_get_type(context, right_id);
    if (left == NULL || right == NULL || left->kind != right->kind) return 0;
    if (left->kind == CM_HIR_TYPE_BOOL_KIND
        || left->kind == CM_HIR_TYPE_CHAR_KIND) return 1;
    return left->kind == CM_HIR_TYPE_INTEGER_KIND
        && left->data.integer_type.kind == right->data.integer_type.kind;
}

static int cm_meta_collect_const(
    const CmHirLibraryArtifactIdentity *identity, const CmVec *items,
    const CmVec *generics, CmVec *types, unsigned char *states,
    const CmHirConstArg *constant, CmHirTypeId expected_type)
{
    const CmHirGenericParam *parameter;

    if (constant == NULL
        || (constant->kind != CM_HIR_CONST_VALUE
            && constant->kind != CM_HIR_CONST_PARAMETER)
        || !cm_meta_scalar_const_type_equal(identity->context,
            constant->type, expected_type)
        || !cm_meta_collect_type(identity, items, generics, types, states,
            constant->type)) return 0;
    if (constant->kind == CM_HIR_CONST_VALUE) return 1;
    parameter = cm_hir_get_generic_param(identity->context,
        constant->data.parameter);
    return parameter != NULL && parameter->kind == CM_HIR_GENERIC_CONST
        && cm_meta_generic_local(generics, constant->data.parameter) != 0u
        && cm_meta_scalar_const_type_equal(identity->context,
            constant->type, parameter->declared_type);
}

static int cm_meta_collect_named(
    const CmHirLibraryArtifactIdentity *identity, const CmVec *items,
    const CmVec *generics, CmVec *types, unsigned char *states,
    const CmHirNamedType *named, uint8_t expected_item_kind)
{
    uint32_t item_local;
    const CmMetaEncodeItem *target;
    uint32_t index;

    item_local = cm_meta_item_local(items, named->definition);
    target = item_local == 0u ? NULL
        : (const CmMetaEncodeItem *)cm_vec_at_const(items,
            (size_t)(item_local - 1u));
    if (target == NULL || target->kind != expected_item_kind
        || named->argument_count != target->item->generic_parameter_count
        || (named->argument_count != 0u && named->arguments == NULL)) return 0;
    for (index = 0u; index < named->argument_count; ++index) {
        const CmHirGenericArg *argument;
        const CmHirGenericParam *parameter;

        argument = &named->arguments[index];
        parameter = cm_hir_get_generic_param(identity->context,
            target->item->generic_parameter_start + index);
        if (parameter == NULL) return 0;
        if (argument->kind == CM_HIR_GENERIC_ARG_LIFETIME) {
            if (parameter->kind != CM_HIR_GENERIC_LIFETIME
                || !cm_meta_region_supported(&argument->data.lifetime,
                    generics)) return 0;
        } else if (argument->kind == CM_HIR_GENERIC_ARG_TYPE) {
            if (parameter->kind != CM_HIR_GENERIC_TYPE
                || !cm_meta_collect_type(identity, items, generics, types,
                    states, argument->data.type)) return 0;
        } else if (argument->kind == CM_HIR_GENERIC_ARG_CONST) {
            if (parameter->kind != CM_HIR_GENERIC_CONST
                || !cm_meta_collect_const(identity, items, generics, types,
                    states, &argument->data.constant,
                    parameter->declared_type)) return 0;
        } else {
            return 0;
        }
    }
    return 1;
}

static int cm_meta_collect_type(const CmHirLibraryArtifactIdentity *identity,
    const CmVec *items, const CmVec *generics, CmVec *types,
    unsigned char *states, CmHirTypeId id)
{
    const CmHirType *type;
    CmMetaEncodeType encoded;
    size_t state_index;
    uint32_t index;
    uint8_t named_kind;

    if (id == CM_HIR_TYPE_NONE || (size_t)id > identity->context->types.len)
        return 0;
    if (cm_meta_type_local(types, id) != 0u) return 1;
    state_index = (size_t)id - 1u;
    if (states[state_index] != 0u) return 0;
    states[state_index] = UINT8_C(1);
    type = cm_hir_get_type(identity->context, id);
    if (type == NULL) return 0;
    switch (type->kind) {
    case CM_HIR_TYPE_NEVER_KIND:
    case CM_HIR_TYPE_UNIT_KIND:
    case CM_HIR_TYPE_BOOL_KIND:
    case CM_HIR_TYPE_CHAR_KIND:
    case CM_HIR_TYPE_STR_KIND:
        break;
    case CM_HIR_TYPE_INTEGER_KIND:
        if (cm_meta_integer_to_wire(type->data.integer_type.kind) == 0u)
            return 0;
        break;
    case CM_HIR_TYPE_FLOAT_KIND:
        if (cm_meta_float_to_wire(type->data.float_type.kind) == 0u)
            return 0;
        break;
    case CM_HIR_TYPE_REFERENCE_KIND:
        if (!cm_meta_region_supported(&type->data.reference_type.region,
                generics)
            || (type->data.reference_type.mutability != CM_HIR_IMMUTABLE
                && type->data.reference_type.mutability != CM_HIR_MUTABLE)
            || !cm_meta_collect_type(identity, items, generics, types, states,
                type->data.reference_type.pointee)) return 0;
        break;
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        if ((type->data.raw_pointer_type.mutability != CM_HIR_IMMUTABLE
                && type->data.raw_pointer_type.mutability != CM_HIR_MUTABLE)
            || !cm_meta_collect_type(identity, items, generics, types, states,
                type->data.raw_pointer_type.pointee)) return 0;
        break;
    case CM_HIR_TYPE_TUPLE_KIND:
        if (type->data.tuple_type.element_count != 0u
            && type->data.tuple_type.elements == NULL) return 0;
        for (index = 0u; index < type->data.tuple_type.element_count;
                ++index) {
            if (!cm_meta_collect_type(identity, items, generics, types, states,
                    type->data.tuple_type.elements[index])) return 0;
        }
        break;
    case CM_HIR_TYPE_ARRAY_KIND:
        if (!cm_meta_collect_type(identity, items, generics, types, states,
                type->data.array_type.element)
            || !cm_meta_collect_const(identity, items, generics, types,
                states, &type->data.array_type.length,
                type->data.array_type.length.type)) return 0;
        break;
    case CM_HIR_TYPE_SLICE_KIND:
        if (!cm_meta_collect_type(identity, items, generics, types, states,
                type->data.slice_type.element)) return 0;
        break;
    case CM_HIR_TYPE_ADT_KIND:
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
        named_kind = type->kind == CM_HIR_TYPE_ALIAS_APPLICATION_KIND
            ? CM_META_ITEM_ALIAS : (type->kind == CM_HIR_TYPE_FOREIGN_KIND
                ? CM_META_ITEM_EXTERN_TYPE : UINT8_C(0));
        if (type->kind == CM_HIR_TYPE_ADT_KIND) {
            uint32_t local;
            const CmMetaEncodeItem *target;

            local = cm_meta_item_local(items,
                type->data.named_type.definition);
            target = local == 0u ? NULL
                : (const CmMetaEncodeItem *)cm_vec_at_const(items,
                    (size_t)(local - 1u));
            if (target == NULL || (target->kind != CM_META_ITEM_STRUCT
                    && target->kind != CM_META_ITEM_UNION
                    && target->kind != CM_META_ITEM_ENUM)) return 0;
            named_kind = target->kind;
        }
        if (!cm_meta_collect_named(identity, items, generics, types, states,
                &type->data.named_type, named_kind)) return 0;
        break;
    case CM_HIR_TYPE_PARAMETER_KIND:
    {
        uint32_t local;
        const CmMetaEncodeGeneric *generic;

        local = cm_meta_generic_local(generics,
            type->data.parameter_type.parameter);
        generic = local == 0u ? NULL
            : (const CmMetaEncodeGeneric *)cm_vec_at_const(generics,
                (size_t)(local - 1u));
        if (generic == NULL
            || generic->parameter->kind != CM_HIR_GENERIC_TYPE) return 0;
        break;
    }
    default:
        return 0;
    }
    memset(&encoded, 0, sizeof(encoded));
    encoded.id = id;
    encoded.type = type;
    (void)cm_vec_push(types, &encoded);
    if (types->len > (size_t)CM_META_MAX_TYPES) return 0;
    states[state_index] = UINT8_C(2);
    return 1;
}

static int cm_meta_collect_types(
    const CmHirLibraryArtifactIdentity *identity, const CmVec *items,
    const CmVec *generics, const CmVec *impls, const CmVec *values,
    CmVec *types)
{
    unsigned char *states;
    size_t index;
    int valid;

    states = (unsigned char *)cm_alloc_zeroed(identity->context->types.len,
        sizeof(unsigned char));
    valid = 1;
    for (index = 0u; valid && index < generics->len; ++index) {
        const CmMetaEncodeGeneric *generic;

        generic = (const CmMetaEncodeGeneric *)cm_vec_at_const(generics,
            index);
        if (generic == NULL) valid = 0;
        else if (generic->parameter->has_default) {
            valid = cm_meta_collect_type(identity, items, generics, types,
                states, generic->parameter->default_argument.data.type);
        } else if (generic->parameter->kind == CM_HIR_GENERIC_CONST) {
            valid = cm_meta_collect_type(identity, items, generics, types,
                states, generic->parameter->declared_type);
        }
    }
    for (index = 0u; valid && index < items->len; ++index) {
        const CmMetaEncodeItem *encoded_item;
        const CmHirItem *item;
        uint32_t child;

        encoded_item = (const CmMetaEncodeItem *)cm_vec_at_const(items,
            index);
        item = encoded_item == NULL ? NULL : encoded_item->item;
        if (item == NULL) {
            valid = 0;
            break;
        }
        if (item->kind == CM_HIR_ITEM_STRUCT
            || item->kind == CM_HIR_ITEM_UNION) {
            for (child = 0u; valid
                    && child < item->data.aggregate_item.field_count;
                    ++child) {
                valid = cm_meta_collect_type(identity, items, generics, types,
                    states, item->data.aggregate_item.fields[child].type);
            }
        } else if (item->kind == CM_HIR_ITEM_ENUM) {
            for (child = 0u; valid
                    && child < item->data.enum_item.variant_count; ++child) {
                const CmHirVariant *variant;
                uint32_t field;

                variant = &item->data.enum_item.variants[child];
                for (field = 0u; valid && field < variant->field_count;
                        ++field) {
                    valid = cm_meta_collect_type(identity, items, generics,
                        types, states, variant->fields[field].type);
                }
                if (valid && variant->has_discriminant) {
                    valid = variant->discriminant.kind == CM_HIR_CONST_VALUE
                        && cm_meta_collect_type(identity, items, generics,
                            types, states, variant->discriminant.type);
                }
            }
        } else if (item->kind == CM_HIR_ITEM_TYPE_ALIAS) {
            valid = cm_meta_collect_type(identity, items, generics, types,
                states, item->data.type_alias_item.target);
        }
    }
    for (index = 0u; valid && impls != NULL && index < impls->len; ++index) {
        const CmMetaEncodeImpl *impl_value;

        impl_value = (const CmMetaEncodeImpl *)cm_vec_at_const(impls,
            index);
        valid = impl_value != NULL
            && cm_meta_collect_type(identity, items, generics, types,
                states, impl_value->item->data.impl_item.self_type);
    }
    for (index = 0u; valid && values != NULL && index < values->len;
            ++index) {
        const CmMetaEncodeValue *encoded_value;
        const CmHirLibraryValue *value;
        uint32_t parameter;

        encoded_value = (const CmMetaEncodeValue *)cm_vec_at_const(values,
            index);
        value = encoded_value == NULL ? NULL
            : &encoded_value->value->declaration;
        if (value == NULL) {
            valid = 0;
            break;
        }
        if (value->kind == CM_HIR_LIBRARY_VALUE_FUNCTION) {
            for (parameter = 0u; valid
                    && parameter < value->data.function.parameter_count;
                    ++parameter) {
                valid = cm_meta_collect_type(identity, items, generics,
                    types, states,
                    value->data.function.parameter_types[parameter]);
            }
            if (valid) valid = cm_meta_collect_type(identity, items,
                generics, types, states,
                value->data.function.return_type);
        } else {
            valid = cm_meta_collect_type(identity, items, generics, types,
                states, value->data.value.type);
        }
    }
    cm_free(states);
    return valid;
}

static int cm_meta_encode_alias_acyclic(
    const CmHirLibraryArtifactIdentity *identity, uint32_t item_local,
    const CmVec *items, unsigned char *states);

static int cm_meta_encode_alias_type_acyclic(
    const CmHirLibraryArtifactIdentity *identity, CmHirTypeId type_id,
    const CmVec *items, unsigned char *states, size_t depth)
{
    const CmHirType *type;
    uint32_t index;

    if (depth > identity->context->types.len + items->len) return 0;
    type = cm_hir_get_type(identity->context, type_id);
    if (type == NULL) return 0;
    switch (type->kind) {
    case CM_HIR_TYPE_REFERENCE_KIND:
        return cm_meta_encode_alias_type_acyclic(identity,
            type->data.reference_type.pointee, items, states, depth + 1u);
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        return cm_meta_encode_alias_type_acyclic(identity,
            type->data.raw_pointer_type.pointee, items, states, depth + 1u);
    case CM_HIR_TYPE_TUPLE_KIND:
        for (index = 0u; index < type->data.tuple_type.element_count;
                ++index) {
            if (!cm_meta_encode_alias_type_acyclic(identity,
                    type->data.tuple_type.elements[index], items, states,
                    depth + 1u)) return 0;
        }
        return 1;
    case CM_HIR_TYPE_ARRAY_KIND:
        return cm_meta_encode_alias_type_acyclic(identity,
                type->data.array_type.element, items, states, depth + 1u)
            && cm_meta_encode_alias_type_acyclic(identity,
                type->data.array_type.length.type, items, states,
                depth + 1u);
    case CM_HIR_TYPE_SLICE_KIND:
        return cm_meta_encode_alias_type_acyclic(identity,
            type->data.slice_type.element, items, states, depth + 1u);
    case CM_HIR_TYPE_ADT_KIND:
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
        for (index = 0u; index < type->data.named_type.argument_count;
                ++index) {
            const CmHirGenericArg *argument;

            argument = &type->data.named_type.arguments[index];
            if (argument->kind == CM_HIR_GENERIC_ARG_TYPE
                && !cm_meta_encode_alias_type_acyclic(identity,
                    argument->data.type, items, states, depth + 1u)) return 0;
        }
        if (type->kind == CM_HIR_TYPE_ALIAS_APPLICATION_KIND) {
            uint32_t local;

            local = cm_meta_item_local(items,
                type->data.named_type.definition);
            return local != 0u && cm_meta_encode_alias_acyclic(identity,
                local, items, states);
        }
        return 1;
    default:
        return 1;
    }
}

static int cm_meta_encode_alias_acyclic(
    const CmHirLibraryArtifactIdentity *identity, uint32_t item_local,
    const CmVec *items, unsigned char *states)
{
    const CmMetaEncodeItem *item;

    item = item_local == 0u || (size_t)item_local > items->len ? NULL
        : (const CmMetaEncodeItem *)cm_vec_at_const(items,
            (size_t)(item_local - 1u));
    if (item == NULL || item->kind != CM_META_ITEM_ALIAS) return 0;
    if (states[item_local - 1u] == UINT8_C(1)) return 0;
    if (states[item_local - 1u] == UINT8_C(2)) return 1;
    states[item_local - 1u] = UINT8_C(1);
    if (!cm_meta_encode_alias_type_acyclic(identity,
            item->item->data.type_alias_item.target, items, states, 0u)) {
        return 0;
    }
    states[item_local - 1u] = UINT8_C(2);
    return 1;
}

static int cm_meta_encode_aliases_acyclic(
    const CmHirLibraryArtifactIdentity *identity, const CmVec *items)
{
    unsigned char *states;
    size_t index;
    int valid;

    states = (unsigned char *)cm_alloc_zeroed(items->len,
        sizeof(unsigned char));
    valid = 1;
    for (index = 0u; valid && index < items->len; ++index) {
        const CmMetaEncodeItem *item;

        item = (const CmMetaEncodeItem *)cm_vec_at_const(items, index);
        if (item != NULL && item->kind == CM_META_ITEM_ALIAS)
            valid = cm_meta_encode_alias_acyclic(identity,
                (uint32_t)(index + 1u), items, states);
    }
    cm_free(states);
    return valid;
}

static uint8_t cm_meta_visibility_kind_to_wire(CmHirVisibilityKind kind)
{
    switch (kind) {
    case CM_HIR_VIS_PRIVATE: return CM_META_VIS_PRIVATE;
    case CM_HIR_VIS_PUBLIC: return CM_META_VIS_PUBLIC;
    case CM_HIR_VIS_CRATE: return CM_META_VIS_CRATE;
    case CM_HIR_VIS_RESTRICTED: return CM_META_VIS_RESTRICTED;
    }
    return UINT8_C(0);
}

static int cm_meta_write_visibility(CmHirMetadataWriter *writer,
    const CmHirVisibility *visibility, const CmVec *modules)
{
    uint8_t kind;
    uint32_t restriction;

    kind = cm_meta_visibility_kind_to_wire(visibility->kind);
    restriction = visibility->kind == CM_HIR_VIS_RESTRICTED
        ? cm_meta_module_local(modules, visibility->restriction)
        : UINT32_C(0);
    return kind != 0u
        && ((kind == CM_META_VIS_RESTRICTED && restriction != 0u)
            || (kind != CM_META_VIS_RESTRICTED
                && cm_hir_def_id_is_none(visibility->restriction)))
        && cm_hir_metadata_write_u8(writer, kind) == CM_HIR_METADATA_OK
        && cm_hir_metadata_write_u32(writer, restriction)
            == CM_HIR_METADATA_OK;
}

static int cm_meta_write_region(CmHirMetadataWriter *writer,
    const CmHirRegion *region, const CmVec *generics)
{
    uint8_t kind;
    uint32_t parameter;

    if (region->kind == CM_HIR_REGION_STATIC) {
        kind = CM_META_REGION_STATIC;
        parameter = UINT32_C(0);
    } else if (region->kind == CM_HIR_REGION_EARLY_BOUND) {
        kind = CM_META_REGION_EARLY_BOUND;
        parameter = cm_meta_generic_local(generics, region->data.parameter);
        if (parameter == 0u) return 0;
    } else {
        return 0;
    }
    return cm_hir_metadata_write_u8(writer, kind) == CM_HIR_METADATA_OK
        && cm_hir_metadata_write_u32(writer, parameter)
            == CM_HIR_METADATA_OK;
}

static int cm_meta_write_const(CmHirMetadataWriter *writer,
    const CmHirConstArg *constant, const CmVec *generics,
    const CmVec *types)
{
    uint8_t kind;
    uint32_t type;
    uint32_t parameter;

    if (constant->kind == CM_HIR_CONST_VALUE) {
        kind = CM_META_CONST_VALUE;
    } else if (constant->kind == CM_HIR_CONST_PARAMETER) {
        kind = CM_META_CONST_PARAMETER;
    } else {
        return 0;
    }
    type = cm_meta_type_local(types, constant->type);
    if (type == 0u
        || cm_hir_metadata_write_u8(writer, kind) != CM_HIR_METADATA_OK
        || cm_hir_metadata_write_u32(writer, type) != CM_HIR_METADATA_OK) {
        return 0;
    }
    if (constant->kind == CM_HIR_CONST_VALUE) {
        return cm_hir_metadata_write_u64(writer,
                    constant->data.value.low_bits) == CM_HIR_METADATA_OK
            && cm_hir_metadata_write_u64(writer,
                    constant->data.value.high_bits) == CM_HIR_METADATA_OK;
    }
    parameter = cm_meta_generic_local(generics,
        constant->data.parameter);
    return parameter != 0u
        && cm_hir_metadata_write_u32(writer, parameter)
            == CM_HIR_METADATA_OK;
}

static int cm_meta_write_named(CmHirMetadataWriter *writer,
    const CmHirNamedType *named, const CmVec *items, const CmVec *generics,
    const CmVec *types)
{
    uint32_t item;
    uint32_t index;

    item = cm_meta_item_local(items, named->definition);
    if (item == 0u
        || cm_hir_metadata_write_u32(writer, item) != CM_HIR_METADATA_OK
        || cm_hir_metadata_write_u32(writer, named->argument_count)
            != CM_HIR_METADATA_OK) return 0;
    for (index = 0u; index < named->argument_count; ++index) {
        const CmHirGenericArg *argument;

        argument = &named->arguments[index];
        if (argument->kind == CM_HIR_GENERIC_ARG_LIFETIME) {
            if (cm_hir_metadata_write_u8(writer, CM_META_ARG_LIFETIME)
                    != CM_HIR_METADATA_OK
                || !cm_meta_write_region(writer, &argument->data.lifetime,
                    generics)) return 0;
        } else if (argument->kind == CM_HIR_GENERIC_ARG_TYPE) {
            uint32_t type;

            type = cm_meta_type_local(types, argument->data.type);
            if (type == 0u
                || cm_hir_metadata_write_u8(writer, CM_META_ARG_TYPE)
                    != CM_HIR_METADATA_OK
                || cm_hir_metadata_write_u32(writer, type)
                    != CM_HIR_METADATA_OK) return 0;
        } else if (argument->kind == CM_HIR_GENERIC_ARG_CONST) {
            if (cm_hir_metadata_write_u8(writer, CM_META_ARG_CONST)
                    != CM_HIR_METADATA_OK
                || !cm_meta_write_const(writer, &argument->data.constant,
                    generics, types)) return 0;
        } else {
            return 0;
        }
    }
    return 1;
}

static uint8_t cm_meta_type_kind_to_wire(CmHirTypeKind kind)
{
    switch (kind) {
    case CM_HIR_TYPE_NEVER_KIND: return CM_META_TYPE_NEVER;
    case CM_HIR_TYPE_UNIT_KIND: return CM_META_TYPE_UNIT;
    case CM_HIR_TYPE_BOOL_KIND: return CM_META_TYPE_BOOL;
    case CM_HIR_TYPE_CHAR_KIND: return CM_META_TYPE_CHAR;
    case CM_HIR_TYPE_STR_KIND: return CM_META_TYPE_STR;
    case CM_HIR_TYPE_INTEGER_KIND: return CM_META_TYPE_INTEGER;
    case CM_HIR_TYPE_FLOAT_KIND: return CM_META_TYPE_FLOAT;
    case CM_HIR_TYPE_REFERENCE_KIND: return CM_META_TYPE_REFERENCE;
    case CM_HIR_TYPE_RAW_POINTER_KIND: return CM_META_TYPE_RAW_POINTER;
    case CM_HIR_TYPE_TUPLE_KIND: return CM_META_TYPE_TUPLE;
    case CM_HIR_TYPE_ARRAY_KIND: return CM_META_TYPE_ARRAY;
    case CM_HIR_TYPE_SLICE_KIND: return CM_META_TYPE_SLICE;
    case CM_HIR_TYPE_ADT_KIND: return CM_META_TYPE_ADT;
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND: return CM_META_TYPE_ALIAS;
    case CM_HIR_TYPE_PARAMETER_KIND: return CM_META_TYPE_PARAMETER;
    case CM_HIR_TYPE_FOREIGN_KIND: return CM_META_TYPE_FOREIGN;
    default: return UINT8_C(0);
    }
}

static uint8_t cm_meta_mutability_to_wire(CmHirMutability mutability)
{
    if (mutability == CM_HIR_IMMUTABLE) return CM_META_MUT_IMMUTABLE;
    if (mutability == CM_HIR_MUTABLE) return CM_META_MUT_MUTABLE;
    return UINT8_C(0);
}

static int cm_meta_write_type(CmHirMetadataWriter *writer,
    const CmMetaEncodeType *encoded, const CmVec *items,
    const CmVec *generics, const CmVec *types)
{
    const CmHirType *type;
    uint8_t kind;
    uint32_t local;
    uint32_t index;

    type = encoded->type;
    kind = cm_meta_type_kind_to_wire(type->kind);
    if (kind == 0u
        || cm_hir_metadata_write_u8(writer, kind) != CM_HIR_METADATA_OK)
        return 0;
    switch (type->kind) {
    case CM_HIR_TYPE_NEVER_KIND:
    case CM_HIR_TYPE_UNIT_KIND:
    case CM_HIR_TYPE_BOOL_KIND:
    case CM_HIR_TYPE_CHAR_KIND:
    case CM_HIR_TYPE_STR_KIND:
        return 1;
    case CM_HIR_TYPE_INTEGER_KIND:
        return cm_hir_metadata_write_u8(writer,
            cm_meta_integer_to_wire(type->data.integer_type.kind))
            == CM_HIR_METADATA_OK;
    case CM_HIR_TYPE_FLOAT_KIND:
        return cm_hir_metadata_write_u8(writer,
            cm_meta_float_to_wire(type->data.float_type.kind))
            == CM_HIR_METADATA_OK;
    case CM_HIR_TYPE_REFERENCE_KIND:
        local = cm_meta_type_local(types,
            type->data.reference_type.pointee);
        return local != 0u
            && cm_meta_write_region(writer,
                &type->data.reference_type.region, generics)
            && cm_hir_metadata_write_u32(writer, local)
                == CM_HIR_METADATA_OK
            && cm_hir_metadata_write_u8(writer, cm_meta_mutability_to_wire(
                type->data.reference_type.mutability)) == CM_HIR_METADATA_OK;
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        local = cm_meta_type_local(types,
            type->data.raw_pointer_type.pointee);
        return local != 0u
            && cm_hir_metadata_write_u32(writer, local)
                == CM_HIR_METADATA_OK
            && cm_hir_metadata_write_u8(writer, cm_meta_mutability_to_wire(
                type->data.raw_pointer_type.mutability))
                == CM_HIR_METADATA_OK;
    case CM_HIR_TYPE_TUPLE_KIND:
        if (cm_hir_metadata_write_u32(writer,
                type->data.tuple_type.element_count) != CM_HIR_METADATA_OK)
            return 0;
        for (index = 0u; index < type->data.tuple_type.element_count;
                ++index) {
            local = cm_meta_type_local(types,
                type->data.tuple_type.elements[index]);
            if (local == 0u || cm_hir_metadata_write_u32(writer, local)
                    != CM_HIR_METADATA_OK) return 0;
        }
        return 1;
    case CM_HIR_TYPE_ARRAY_KIND:
        local = cm_meta_type_local(types, type->data.array_type.element);
        if (local == 0u
            || cm_hir_metadata_write_u32(writer, local)
                != CM_HIR_METADATA_OK) return 0;
        return cm_meta_write_const(writer, &type->data.array_type.length,
            generics, types);
    case CM_HIR_TYPE_SLICE_KIND:
        local = cm_meta_type_local(types, type->data.slice_type.element);
        return local != 0u
            && cm_hir_metadata_write_u32(writer, local)
                == CM_HIR_METADATA_OK;
    case CM_HIR_TYPE_ADT_KIND:
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
        return cm_meta_write_named(writer, &type->data.named_type, items,
            generics, types);
    case CM_HIR_TYPE_PARAMETER_KIND:
        local = cm_meta_generic_local(generics,
            type->data.parameter_type.parameter);
        return local != 0u
            && cm_hir_metadata_write_u32(writer, local)
                == CM_HIR_METADATA_OK;
    default:
        return 0;
    }
}

static int cm_meta_write_field(CmHirMetadataWriter *writer,
    const CmHirLibraryArtifactIdentity *identity, const CmHirField *field,
    CmHirAggregateForm form, const CmVec *modules, const CmVec *types)
{
    uint32_t type;

    if (form == CM_HIR_AGGREGATE_NAMED) {
        if (!cm_meta_write_name(writer, cm_interner_get(
                &identity->context->strings, field->name))) return 0;
    } else if (field->name != CM_INTERN_ID_NONE
        || cm_hir_metadata_write_u32(writer, UINT32_C(0))
            != CM_HIR_METADATA_OK) {
        return 0;
    }
    type = cm_meta_type_local(types, field->type);
    return type != 0u
        && cm_hir_metadata_write_u32(writer, type) == CM_HIR_METADATA_OK
        && cm_meta_write_visibility(writer, &field->visibility, modules);
}

static int cm_meta_write_item(CmHirMetadataWriter *writer,
    const CmHirLibraryArtifactIdentity *identity,
    const CmMetaEncodeItem *encoded, const CmVec *modules,
    const CmVec *generics, const CmVec *types)
{
    const CmHirItem *item;
    uint32_t generic_start;
    uint32_t index;

    item = encoded->item;
    generic_start = item->generic_parameter_count == 0u ? UINT32_C(0)
        : cm_meta_generic_local(generics, item->generic_parameter_start);
    if (cm_hir_metadata_write_u8(writer, encoded->kind)
            != CM_HIR_METADATA_OK
        || cm_hir_metadata_write_u32(writer, encoded->owner)
            != CM_HIR_METADATA_OK
        || !cm_meta_write_name(writer, encoded->name)
        || !cm_meta_write_visibility(writer, &item->visibility, modules)
        || (item->generic_parameter_count != 0u && generic_start == 0u)
        || cm_hir_metadata_write_u32(writer, generic_start)
            != CM_HIR_METADATA_OK
        || cm_hir_metadata_write_u32(writer, item->generic_parameter_count)
            != CM_HIR_METADATA_OK) return 0;
    if (item->kind == CM_HIR_ITEM_EXTERN_TYPE) return 1;
    if (item->kind == CM_HIR_ITEM_STRUCT
        || item->kind == CM_HIR_ITEM_UNION) {
        uint8_t form;

        form = cm_meta_form_to_wire(item->data.aggregate_item.form);
        if (form == 0u
            || cm_hir_metadata_write_u8(writer, form) != CM_HIR_METADATA_OK
            || cm_hir_metadata_write_u32(writer,
                item->data.aggregate_item.field_count)
                != CM_HIR_METADATA_OK) return 0;
        for (index = 0u; index < item->data.aggregate_item.field_count;
                ++index) {
            if (!cm_meta_write_field(writer, identity,
                    &item->data.aggregate_item.fields[index],
                    item->data.aggregate_item.form, modules, types)) return 0;
        }
        return 1;
    }
    if (item->kind == CM_HIR_ITEM_ENUM) {
        if (cm_hir_metadata_write_u32(writer,
                item->data.enum_item.variant_count) != CM_HIR_METADATA_OK)
            return 0;
        for (index = 0u; index < item->data.enum_item.variant_count;
                ++index) {
            const CmHirVariant *variant;
            uint8_t form;
            uint32_t field;

            variant = &item->data.enum_item.variants[index];
            form = cm_meta_form_to_wire(variant->form);
            if (form == 0u
                || !cm_meta_write_name(writer, cm_interner_get(
                    &identity->context->strings, variant->name))
                || cm_hir_metadata_write_u8(writer, form)
                    != CM_HIR_METADATA_OK
                || cm_hir_metadata_write_u32(writer, variant->field_count)
                    != CM_HIR_METADATA_OK) return 0;
            for (field = 0u; field < variant->field_count; ++field) {
                if (!cm_meta_write_field(writer, identity,
                        &variant->fields[field], variant->form, modules,
                        types)) return 0;
            }
            if (cm_hir_metadata_write_u8(writer,
                    variant->has_discriminant ? UINT8_C(1) : UINT8_C(0))
                    != CM_HIR_METADATA_OK) return 0;
            if (variant->has_discriminant) {
                uint32_t discriminant_type;

                if (variant->discriminant.kind != CM_HIR_CONST_VALUE)
                    return 0;
                discriminant_type = cm_meta_type_local(types,
                    variant->discriminant.type);
                if (discriminant_type == 0u
                    || cm_hir_metadata_write_u32(writer, discriminant_type)
                        != CM_HIR_METADATA_OK
                    || cm_hir_metadata_write_u64(writer,
                        variant->discriminant.data.value.low_bits)
                        != CM_HIR_METADATA_OK
                    || cm_hir_metadata_write_u64(writer,
                        variant->discriminant.data.value.high_bits)
                        != CM_HIR_METADATA_OK) return 0;
            }
        }
        return 1;
    }
    if (item->kind == CM_HIR_ITEM_TYPE_ALIAS) {
        uint32_t target;

        target = cm_meta_type_local(types, item->data.type_alias_item.target);
        return target != 0u
            && cm_hir_metadata_write_u32(writer, target)
                == CM_HIR_METADATA_OK;
    }
    return 0;
}

static int cm_meta_write_value(CmHirMetadataWriter *writer,
    const CmHirLibraryArtifactIdentity *identity,
    const CmMetaEncodeValue *encoded, const CmVec *generics,
    const CmVec *types)
{
    const CmHirLibraryValue *value;

    if (writer == NULL || identity == NULL || encoded == NULL
        || encoded->value == NULL || types == NULL) return 0;
    value = &encoded->value->declaration;
    if (cm_hir_metadata_write_u8(writer, encoded->kind)
            != CM_HIR_METADATA_OK) return 0;
    if (value->kind == CM_HIR_LIBRARY_VALUE_FUNCTION) {
        uint32_t index;
        uint32_t local;
        uint32_t generic_start;

        if (cm_hir_metadata_write_u32(writer,
                value->data.function.parameter_count)
                != CM_HIR_METADATA_OK) return 0;
        for (index = 0u; index < value->data.function.parameter_count;
                ++index) {
            local = cm_meta_type_local(types,
                value->data.function.parameter_types[index]);
            if (local == 0u || cm_hir_metadata_write_u32(writer, local)
                    != CM_HIR_METADATA_OK) return 0;
        }
        local = cm_meta_type_local(types, value->data.function.return_type);
        generic_start = value->data.function.generic_parameter_count == 0u
            ? UINT32_C(0) : cm_meta_generic_local(generics,
                value->data.function.generic_parameter_start);
        return local != 0u
            && cm_hir_metadata_write_u32(writer, local)
                == CM_HIR_METADATA_OK
            && (value->data.function.generic_parameter_count == 0u
                || generic_start != 0u)
            && cm_hir_metadata_write_u32(writer, generic_start)
                == CM_HIR_METADATA_OK
            && cm_hir_metadata_write_u32(writer,
                value->data.function.generic_parameter_count)
                == CM_HIR_METADATA_OK
            && cm_meta_write_string(writer, cm_interner_get(
                &identity->context->strings, value->data.function.abi))
            && cm_hir_metadata_write_u8(writer,
                (uint8_t)value->data.function.safety)
                == CM_HIR_METADATA_OK
            && cm_hir_metadata_write_u8(writer,
                value->data.function.is_const ? UINT8_C(1) : UINT8_C(0))
                == CM_HIR_METADATA_OK
            && cm_hir_metadata_write_u8(writer,
                value->data.function.is_async ? UINT8_C(1) : UINT8_C(0))
                == CM_HIR_METADATA_OK
            && cm_hir_metadata_write_u8(writer,
                value->data.function.is_variadic
                    ? UINT8_C(1) : UINT8_C(0)) == CM_HIR_METADATA_OK;
    }
    {
        uint32_t local;
        uint8_t mutability;

        local = cm_meta_type_local(types, value->data.value.type);
        mutability = cm_meta_mutability_to_wire(
            value->data.value.mutability);
        return local != 0u && mutability != 0u
            && cm_hir_metadata_write_u32(writer, local)
                == CM_HIR_METADATA_OK
            && cm_hir_metadata_write_u8(writer, mutability)
                == CM_HIR_METADATA_OK;
    }
}

static CmHirMetadataArtifactResult cm_meta_encode_artifact(
    CmByteBuf *output, const CmHirLibraryArtifact *artifact, int semantic,
    int declaration)
{
    CmHirMetadataArtifactResult result;
    CmHirLibraryArtifactIdentity identity;
    const CmHirLibraryOwnedData *owned;
    const CmHirCrate *crate_value;
    CmVec modules;
    CmVec items;
    CmVec generics;
    CmVec types;
    CmVec entries;
    CmVec traits;
    CmVec impls;
    CmVec values;
    CmByteBuf crate_section;
    CmByteBuf module_section;
    CmByteBuf generic_section;
    CmByteBuf type_section;
    CmByteBuf item_section;
    CmByteBuf namespace_section;
    CmByteBuf trait_universe_section;
    CmByteBuf value_section;
    CmByteBuf payload;
    CmHirMetadataWriter writer;
    CmHirMetadataWriter payload_writer;
    CmHirMetadataStatus codec_status;
    size_t module_index;
    size_t public_entry_count;

    result = cm_meta_result(CM_HIR_METADATA_ARTIFACT_INVALID_ARGUMENT);
    if (output == NULL || artifact == NULL || (semantic && declaration)
        || !cm_hir_library_artifact_identity(artifact, &identity)
        || (owned = cm_hir_library_artifact_owned_data_const(artifact))
            == NULL
        || owned->modules.len == 0u
        || owned->modules.len > (size_t)CM_META_MAX_MODULES
        || (crate_value = cm_hir_get_crate(identity.context,
            identity.crate_id)) == NULL) return result;
    if (semantic) {
        size_t item_index;

        for (item_index = 0u;
             item_index < identity.context->items.len; ++item_index) {
            const CmHirItem *item;

            item = (const CmHirItem *)cm_vec_at_const(
                &identity.context->items, item_index);
            if (item == NULL) {
                result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR;
                return result;
            }
            if (item->definition.crate_id == identity.crate_id
                && item->is_specializable) {
                /* v1.1 carries the impl header but has no item-defaultness
                 * field.  Reject instead of publishing a falsely-final impl. */
                result.status = CM_HIR_METADATA_ARTIFACT_UNSUPPORTED_HIR;
                return result;
            }
        }
    }

    cm_vec_init(&modules, sizeof(CmMetaEncodeModule));
    cm_vec_init(&items, sizeof(CmMetaEncodeItem));
    cm_vec_init(&generics, sizeof(CmMetaEncodeGeneric));
    cm_vec_init(&types, sizeof(CmMetaEncodeType));
    cm_vec_init(&entries, sizeof(CmMetaEncodeEntry));
    cm_vec_init(&traits, sizeof(CmMetaEncodeTrait));
    cm_vec_init(&impls, sizeof(CmMetaEncodeImpl));
    cm_vec_init(&values, sizeof(CmMetaEncodeValue));
    if (!cm_meta_collect_modules(&identity, owned, &modules)) {
        result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR;
        goto cleanup_views;
    }
    if (!cm_meta_collect_items(&identity, &modules, &items, semantic,
            declaration)
        || (semantic && !cm_meta_collect_trait_universe(&identity,
            &modules, &traits, &impls))
        || (declaration && !cm_meta_collect_values(owned, &modules,
            &values))
        || !cm_meta_collect_generics(&identity, &items,
            declaration ? &values : NULL, &generics)
        || !cm_meta_collect_types(&identity, &items, &generics,
            semantic ? &impls : NULL, declaration ? &values : NULL, &types)
        || !cm_meta_encode_aliases_acyclic(&identity, &items)) {
        result.status = CM_HIR_METADATA_ARTIFACT_UNSUPPORTED_HIR;
        goto cleanup_views;
    }
    if (!cm_meta_collect_entries(owned, &modules, &items,
            semantic ? &traits : NULL, declaration ? &values : NULL,
            &entries,
            &public_entry_count)) {
        result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR;
        goto cleanup_views;
    }
    cm_byte_buf_init(&crate_section);
    cm_byte_buf_init(&module_section);
    cm_byte_buf_init(&generic_section);
    cm_byte_buf_init(&type_section);
    cm_byte_buf_init(&item_section);
    cm_byte_buf_init(&namespace_section);
    cm_byte_buf_init(&trait_universe_section);
    cm_byte_buf_init(&value_section);
    cm_byte_buf_init(&payload);

    cm_hir_metadata_writer_init(&writer, &crate_section,
        CM_HIR_METADATA_MAX_PAYLOAD_SIZE);
    if (!cm_meta_write_name(&writer, cm_interner_get(
            &identity.context->strings, crate_value->name))
        || cm_hir_metadata_write_u8(&writer,
            cm_meta_edition_to_wire(crate_value->edition))
            != CM_HIR_METADATA_OK
        || cm_meta_edition_to_wire(crate_value->edition) == 0u) {
        result.status = CM_HIR_METADATA_ARTIFACT_UNSUPPORTED_HIR;
        goto cleanup_encode;
    }

    cm_hir_metadata_writer_init(&writer, &module_section,
        CM_HIR_METADATA_MAX_PAYLOAD_SIZE);
    if (cm_hir_metadata_write_u32(&writer, (uint32_t)modules.len)
            != CM_HIR_METADATA_OK) goto encode_limit;
    for (module_index = 0u; module_index < modules.len;
            ++module_index) {
        const CmMetaEncodeModule *encoded_module;
        const CmHirModule *module;
        uint32_t parent;

        encoded_module = (const CmMetaEncodeModule *)cm_vec_at_const(
            &modules, module_index);
        module = encoded_module == NULL ? NULL : encoded_module->module;
        if (module == NULL || module->crate_id != identity.crate_id) {
            result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR;
            goto cleanup_encode;
        }
        if (cm_hir_def_id_equal(module->definition,
                identity.root_definition)) {
            parent = UINT32_C(0);
        } else {
            const CmHirModule *parent_module;

            parent_module = cm_hir_get_module(identity.context,
                module->parent);
            parent = parent_module == NULL ? UINT32_C(0)
                : cm_meta_module_local(&modules,
                    parent_module->definition);
            if (parent == 0u) {
                result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR;
                goto cleanup_encode;
            }
        }
        if (cm_hir_metadata_write_u32(&writer, parent)
                != CM_HIR_METADATA_OK
            || !cm_meta_write_name(&writer, cm_interner_get(
                &identity.context->strings, module->name))) {
            goto encode_limit;
        }
    }

    cm_hir_metadata_writer_init(&writer, &generic_section,
        CM_HIR_METADATA_MAX_PAYLOAD_SIZE);
    if (cm_hir_metadata_write_u32(&writer, (uint32_t)generics.len)
            != CM_HIR_METADATA_OK) goto encode_limit;
    for (module_index = 0u; module_index < generics.len; ++module_index) {
        const CmMetaEncodeGeneric *generic;
        const CmHirGenericParam *parameter;
        const CmInternedString *name;
        uint8_t kind;
        uint32_t default_type;

        generic = (const CmMetaEncodeGeneric *)cm_vec_at_const(&generics,
            module_index);
        parameter = generic == NULL ? NULL : generic->parameter;
        name = parameter == NULL ? NULL : cm_interner_get(
            &identity.context->strings, parameter->name);
        kind = parameter == NULL ? UINT8_C(0)
            : (parameter->kind == CM_HIR_GENERIC_LIFETIME
                ? CM_META_GENERIC_LIFETIME
                : (parameter->kind == CM_HIR_GENERIC_CONST
                    ? CM_META_GENERIC_CONST : CM_META_GENERIC_TYPE));
        default_type = parameter != NULL && parameter->has_default
            ? cm_meta_type_local(&types,
                parameter->default_argument.data.type)
            : (parameter != NULL
                && parameter->kind == CM_HIR_GENERIC_CONST
                ? cm_meta_type_local(&types, parameter->declared_type)
                : UINT32_C(0));
        if (generic == NULL || kind == 0u || name == NULL
            || (parameter->has_default && default_type == 0u)
            || (parameter != NULL
                && parameter->kind == CM_HIR_GENERIC_CONST
                && default_type == 0u)
            || (declaration
                && cm_hir_metadata_write_u8(&writer,
                    generic->owner_kind) != CM_HIR_METADATA_OK)
            || cm_hir_metadata_write_u32(&writer, generic->owner)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_write_u32(&writer, parameter->index)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_write_u8(&writer, kind)
                != CM_HIR_METADATA_OK
            || !cm_meta_write_generic_name(&writer, name, kind)
            || cm_hir_metadata_write_u8(&writer,
                parameter->is_relaxed_sized ? UINT8_C(1) : UINT8_C(0))
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_write_u8(&writer,
                parameter->has_default ? UINT8_C(1) : UINT8_C(0))
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_write_u32(&writer, default_type)
                != CM_HIR_METADATA_OK) {
            result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR;
            goto cleanup_encode;
        }
    }

    cm_hir_metadata_writer_init(&writer, &type_section,
        CM_HIR_METADATA_MAX_PAYLOAD_SIZE);
    if (cm_hir_metadata_write_u32(&writer, (uint32_t)types.len)
            != CM_HIR_METADATA_OK) goto encode_limit;
    for (module_index = 0u; module_index < types.len; ++module_index) {
        const CmMetaEncodeType *type;

        type = (const CmMetaEncodeType *)cm_vec_at_const(&types,
            module_index);
        if (type == NULL || !cm_meta_write_type(&writer, type, &items,
                &generics, &types)) {
            result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR;
            goto cleanup_encode;
        }
    }

    cm_hir_metadata_writer_init(&writer, &item_section,
        CM_HIR_METADATA_MAX_PAYLOAD_SIZE);
    if (cm_hir_metadata_write_u32(&writer, (uint32_t)items.len)
            != CM_HIR_METADATA_OK) goto encode_limit;
    for (module_index = 0u; module_index < items.len; ++module_index) {
        const CmMetaEncodeItem *encoded_item;
        encoded_item = (const CmMetaEncodeItem *)cm_vec_at_const(&items,
            module_index);
        if (encoded_item == NULL || !cm_meta_write_item(&writer, &identity,
                encoded_item, &modules, &generics, &types)) {
            result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR;
            goto cleanup_encode;
        }
    }

    if (declaration) {
        cm_hir_metadata_writer_init(&writer, &value_section,
            CM_HIR_METADATA_MAX_PAYLOAD_SIZE);
        if (cm_hir_metadata_write_u32(&writer, (uint32_t)values.len)
                != CM_HIR_METADATA_OK) goto encode_limit;
        for (module_index = 0u; module_index < values.len; ++module_index) {
            const CmMetaEncodeValue *value;

            value = (const CmMetaEncodeValue *)cm_vec_at_const(&values,
                module_index);
            if (value == NULL || !cm_meta_write_value(&writer, &identity,
                    value, &generics, &types)) {
                result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR;
                goto cleanup_encode;
            }
        }
    }

    cm_hir_metadata_writer_init(&writer, &namespace_section,
        CM_HIR_METADATA_MAX_PAYLOAD_SIZE);
    if (cm_hir_metadata_write_u32(&writer, (uint32_t)entries.len)
            != CM_HIR_METADATA_OK) goto encode_limit;
    for (module_index = 0u; module_index < entries.len; ++module_index) {
        const CmMetaEncodeEntry *entry;

        entry = (const CmMetaEncodeEntry *)cm_vec_at_const(&entries,
            module_index);
        if (entry == NULL
            || cm_hir_metadata_write_u32(&writer, entry->module)
                != CM_HIR_METADATA_OK
            || !cm_meta_write_name(&writer, entry->name)
            || cm_hir_metadata_write_u8(&writer, entry->kind)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_write_u32(&writer, entry->target)
                != CM_HIR_METADATA_OK) {
            result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR;
            goto cleanup_encode;
        }
    }

    if (semantic) {
        cm_hir_metadata_writer_init(&writer, &trait_universe_section,
            CM_HIR_METADATA_MAX_PAYLOAD_SIZE);
        if (cm_hir_metadata_write_u8(&writer, CM_META_UNIVERSE_OPEN)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_write_u32(&writer, (uint32_t)traits.len)
                != CM_HIR_METADATA_OK) goto encode_limit;
        for (module_index = 0u; module_index < traits.len; ++module_index) {
            const CmMetaEncodeTrait *trait_value;

            trait_value = (const CmMetaEncodeTrait *)cm_vec_at_const(
                &traits, module_index);
            if (trait_value == NULL
                || (unsigned int)trait_value->item->data.trait_item.safety
                    > (unsigned int)CM_HIR_UNSAFE
                || cm_hir_metadata_write_u32(&writer, trait_value->owner)
                    != CM_HIR_METADATA_OK
                || !cm_meta_write_name(&writer, trait_value->name)
                || !cm_meta_write_visibility(&writer,
                    &trait_value->item->visibility, &modules)
                || cm_hir_metadata_write_u8(&writer,
                    (uint8_t)trait_value->item->data.trait_item.safety)
                    != CM_HIR_METADATA_OK
                || cm_hir_metadata_write_u8(&writer,
                    trait_value->item->data.trait_item.is_auto
                        ? UINT8_C(1) : UINT8_C(0))
                    != CM_HIR_METADATA_OK) {
                result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR;
                goto cleanup_encode;
            }
        }
        if (cm_hir_metadata_write_u32(&writer, (uint32_t)impls.len)
                != CM_HIR_METADATA_OK) goto encode_limit;
        for (module_index = 0u; module_index < impls.len; ++module_index) {
            const CmMetaEncodeImpl *impl_value;
            uint32_t self_type;

            impl_value = (const CmMetaEncodeImpl *)cm_vec_at_const(&impls,
                module_index);
            self_type = impl_value == NULL ? UINT32_C(0)
                : cm_meta_type_local(&types,
                    impl_value->item->data.impl_item.self_type);
            if (impl_value == NULL || self_type == 0u
                || (unsigned int)impl_value->item->data.impl_item.safety
                    > (unsigned int)CM_HIR_UNSAFE
                || cm_hir_metadata_write_u32(&writer, impl_value->owner)
                    != CM_HIR_METADATA_OK
                || cm_hir_metadata_write_u32(&writer,
                    impl_value->trait_local) != CM_HIR_METADATA_OK
                || cm_hir_metadata_write_u32(&writer, self_type)
                    != CM_HIR_METADATA_OK
                || cm_hir_metadata_write_u8(&writer,
                    (uint8_t)impl_value->item->data.impl_item.safety)
                    != CM_HIR_METADATA_OK
                || cm_hir_metadata_write_u8(&writer,
                    impl_value->item->data.impl_item.is_negative
                        ? UINT8_C(1) : UINT8_C(0))
                    != CM_HIR_METADATA_OK) {
                result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR;
                goto cleanup_encode;
            }
        }
    }

    cm_hir_metadata_writer_init(&payload_writer, &payload,
        CM_HIR_METADATA_MAX_PAYLOAD_SIZE);
    codec_status = cm_hir_metadata_write_section(&payload_writer,
        cm_meta_tag_crate, crate_section.data, crate_section.len);
    if (codec_status == CM_HIR_METADATA_OK)
        codec_status = cm_hir_metadata_write_section(&payload_writer,
            cm_meta_tag_modules, module_section.data, module_section.len);
    if (codec_status == CM_HIR_METADATA_OK)
        codec_status = cm_hir_metadata_write_section(&payload_writer,
            cm_meta_tag_generics, generic_section.data,
            generic_section.len);
    if (codec_status == CM_HIR_METADATA_OK)
        codec_status = cm_hir_metadata_write_section(&payload_writer,
            cm_meta_tag_types, type_section.data, type_section.len);
    if (codec_status == CM_HIR_METADATA_OK)
        codec_status = cm_hir_metadata_write_section(&payload_writer,
            cm_meta_tag_items, item_section.data, item_section.len);
    if (codec_status == CM_HIR_METADATA_OK && declaration)
        codec_status = cm_hir_metadata_write_section(&payload_writer,
            cm_meta_tag_values, value_section.data, value_section.len);
    if (codec_status == CM_HIR_METADATA_OK)
        codec_status = cm_hir_metadata_write_section(&payload_writer,
            cm_meta_tag_namespace, namespace_section.data,
            namespace_section.len);
    if (codec_status == CM_HIR_METADATA_OK && semantic)
        codec_status = cm_hir_metadata_write_section(&payload_writer,
            cm_meta_tag_trait_universe, trait_universe_section.data,
            trait_universe_section.len);
    if (codec_status != CM_HIR_METADATA_OK) {
        result.status = cm_meta_codec_status(codec_status);
        goto cleanup_encode;
    }
    codec_status = cm_hir_metadata_encode_envelope_version(output,
        (uint16_t)(declaration ? CM_HIR_METADATA_DECLARATION_MAJOR
            : CM_HIR_METADATA_MAJOR),
        (uint16_t)(declaration ? CM_HIR_METADATA_DECLARATION_MINOR
            : (semantic ? CM_HIR_METADATA_SEMANTIC_MINOR
                : CM_HIR_METADATA_MINOR)), UINT32_C(0), payload.data,
        payload.len);
    if (codec_status != CM_HIR_METADATA_OK) {
        result.status = cm_meta_codec_status(codec_status);
        goto cleanup_encode;
    }
    result.status = CM_HIR_METADATA_ARTIFACT_OK;
    result.module_count = modules.len;
    result.public_entry_count = public_entry_count;
    goto cleanup_encode;

encode_limit:
    result.status = CM_HIR_METADATA_ARTIFACT_LIMIT_EXCEEDED;

cleanup_encode:
    cm_byte_buf_destroy(&payload);
    cm_byte_buf_destroy(&value_section);
    cm_byte_buf_destroy(&trait_universe_section);
    cm_byte_buf_destroy(&namespace_section);
    cm_byte_buf_destroy(&item_section);
    cm_byte_buf_destroy(&type_section);
    cm_byte_buf_destroy(&generic_section);
    cm_byte_buf_destroy(&module_section);
    cm_byte_buf_destroy(&crate_section);
cleanup_views:
    cm_vec_destroy(&values);
    cm_vec_destroy(&impls);
    cm_vec_destroy(&traits);
    cm_vec_destroy(&entries);
    cm_vec_destroy(&types);
    cm_vec_destroy(&generics);
    cm_vec_destroy(&items);
    cm_meta_encode_modules_destroy(&modules);
    return result;
}

CmHirMetadataArtifactResult cm_hir_metadata_encode_artifact(
    CmByteBuf *output, const CmHirLibraryArtifact *artifact)
{
    return cm_meta_encode_artifact(output, artifact, 0, 0);
}

CmHirMetadataArtifactResult cm_hir_metadata_encode_semantic_artifact(
    CmByteBuf *output, const CmHirLibraryArtifact *artifact)
{
    return cm_meta_encode_artifact(output, artifact, 1, 0);
}

CmHirMetadataArtifactResult cm_hir_metadata_encode_declaration_artifact(
    CmByteBuf *output, const CmHirLibraryArtifact *artifact)
{
    return cm_meta_encode_artifact(output, artifact, 0, 1);
}

static int cm_meta_read_name(CmHirMetadataReader *reader,
    CmMetaWireName *out_name)
{
    uint32_t length;
    const unsigned char *bytes;

    if (cm_hir_metadata_read_u32(reader, &length) != CM_HIR_METADATA_OK
        || length == 0u || length > CM_META_MAX_STRING
        || cm_hir_metadata_read_bytes(reader, (size_t)length, &bytes)
            != CM_HIR_METADATA_OK
        || !cm_meta_identifier_bytes_valid(bytes, (size_t)length)) return 0;
    out_name->bytes = bytes;
    out_name->length = (size_t)length;
    return 1;
}

static int cm_meta_read_string(CmHirMetadataReader *reader,
    CmMetaWireName *out_string)
{
    uint32_t length;
    const unsigned char *bytes;

    if (reader == NULL || out_string == NULL
        || cm_hir_metadata_read_u32(reader, &length) != CM_HIR_METADATA_OK
        || length == 0u || length > CM_META_MAX_STRING
        || cm_hir_metadata_read_bytes(reader, (size_t)length, &bytes)
            != CM_HIR_METADATA_OK) return 0;
    out_string->bytes = bytes;
    out_string->length = (size_t)length;
    return 1;
}

static int cm_meta_read_generic_name(CmHirMetadataReader *reader,
    uint8_t kind, CmMetaWireName *out_name)
{
    uint32_t length;
    const unsigned char *bytes;

    if (cm_hir_metadata_read_u32(reader, &length) != CM_HIR_METADATA_OK
        || length == 0u || length > CM_META_MAX_STRING
        || cm_hir_metadata_read_bytes(reader, (size_t)length, &bytes)
            != CM_HIR_METADATA_OK
        || !cm_meta_generic_name_bytes_valid(bytes, (size_t)length, kind)) {
        return 0;
    }
    out_name->bytes = bytes;
    out_name->length = (size_t)length;
    return 1;
}

static int cm_meta_decode_crate(const CmHirMetadataSection *section,
    CmMetaWireName *out_name, CmHirEdition *out_edition)
{
    CmHirMetadataReader reader;
    uint8_t edition;

    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    return cm_meta_read_name(&reader, out_name)
        && cm_hir_metadata_read_u8(&reader, &edition) == CM_HIR_METADATA_OK
        && cm_meta_edition_from_wire(edition, out_edition)
        && cm_hir_metadata_reader_finish(&reader) == CM_HIR_METADATA_OK;
}

static int cm_meta_decode_modules(const CmHirMetadataSection *section,
    CmVec *modules, uint32_t *out_root)
{
    CmHirMetadataReader reader;
    uint32_t count;
    uint32_t index;
    uint32_t root;

    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (cm_hir_metadata_read_u32(&reader, &count) != CM_HIR_METADATA_OK
        || count == 0u || count > CM_META_MAX_MODULES) return 0;
    root = UINT32_C(0);
    for (index = 0u; index < count; ++index) {
        CmMetaWireModule module;

        memset(&module, 0, sizeof(module));
        if (cm_hir_metadata_read_u32(&reader, &module.parent)
                != CM_HIR_METADATA_OK
            || module.parent > count || module.parent == index + 1u
            || !cm_meta_read_name(&reader, &module.name)) return 0;
        if (module.parent == 0u) {
            if (root != 0u) return 0;
            root = index + 1u;
        }
        (void)cm_vec_push(modules, &module);
    }
    if (root == 0u
        || cm_hir_metadata_reader_finish(&reader) != CM_HIR_METADATA_OK) {
        return 0;
    }
    for (index = 0u; index < count; ++index) {
        uint32_t cursor;
        uint32_t depth;

        cursor = index + 1u;
        depth = 0u;
        while (cursor != root) {
            const CmMetaWireModule *module;

            if (cursor == 0u || cursor > count || depth++ >= count)
                return 0;
            module = (const CmMetaWireModule *)cm_vec_at_const(modules,
                (size_t)(cursor - 1u));
            cursor = module == NULL ? UINT32_C(0) : module->parent;
        }
    }
    *out_root = root;
    return 1;
}

static int cm_meta_read_optional_name(CmHirMetadataReader *reader,
    CmMetaWireName *out_name)
{
    uint32_t length;
    const unsigned char *bytes;

    if (cm_hir_metadata_read_u32(reader, &length) != CM_HIR_METADATA_OK
        || length > CM_META_MAX_STRING) return 0;
    if (length == 0u) {
        out_name->bytes = NULL;
        out_name->length = 0u;
        return 1;
    }
    if (cm_hir_metadata_read_bytes(reader, (size_t)length, &bytes)
            != CM_HIR_METADATA_OK
        || !cm_meta_identifier_bytes_valid(bytes, (size_t)length)) return 0;
    out_name->bytes = bytes;
    out_name->length = (size_t)length;
    return 1;
}

static int cm_meta_section_count(const CmHirMetadataSection *section,
    uint32_t maximum, uint32_t *out_count)
{
    CmHirMetadataReader reader;
    uint32_t count;

    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (cm_hir_metadata_read_u32(&reader, &count) != CM_HIR_METADATA_OK
        || count > maximum) return 0;
    *out_count = count;
    return 1;
}

static int cm_meta_read_visibility(CmHirMetadataReader *reader,
    uint32_t module_count, CmMetaWireVisibility *out_visibility)
{
    if (cm_hir_metadata_read_u8(reader, &out_visibility->kind)
            != CM_HIR_METADATA_OK
        || cm_hir_metadata_read_u32(reader, &out_visibility->restriction)
            != CM_HIR_METADATA_OK) return 0;
    if (out_visibility->kind == CM_META_VIS_RESTRICTED)
        return out_visibility->restriction != 0u
            && out_visibility->restriction <= module_count;
    return (out_visibility->kind == CM_META_VIS_PRIVATE
            || out_visibility->kind == CM_META_VIS_PUBLIC
            || out_visibility->kind == CM_META_VIS_CRATE)
        && out_visibility->restriction == 0u;
}

static int cm_meta_decode_generics(const CmHirMetadataSection *section,
    uint32_t item_count, uint32_t value_count, int declaration,
    CmVec *generics)
{
    CmHirMetadataReader reader;
    uint32_t count;
    uint32_t index;

    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (cm_hir_metadata_read_u32(&reader, &count) != CM_HIR_METADATA_OK
        || count > CM_META_MAX_GENERICS) return 0;
    for (index = 0u; index < count; ++index) {
        CmMetaWireGeneric generic;
        uint8_t relaxed;
        uint8_t has_default;

        memset(&generic, 0, sizeof(generic));
        generic.owner_kind = CM_META_GENERIC_OWNER_ITEM;
        if ((declaration
                && (cm_hir_metadata_read_u8(&reader, &generic.owner_kind)
                        != CM_HIR_METADATA_OK
                    || (generic.owner_kind != CM_META_GENERIC_OWNER_ITEM
                        && generic.owner_kind
                            != CM_META_GENERIC_OWNER_VALUE)))
            || cm_hir_metadata_read_u32(&reader, &generic.owner)
                != CM_HIR_METADATA_OK
            || generic.owner == 0u
            || (generic.owner_kind == CM_META_GENERIC_OWNER_ITEM
                ? generic.owner > item_count : generic.owner > value_count)
            || cm_hir_metadata_read_u32(&reader, &generic.index)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u8(&reader, &generic.kind)
                != CM_HIR_METADATA_OK
            || (generic.kind != CM_META_GENERIC_LIFETIME
                && generic.kind != CM_META_GENERIC_TYPE
                && generic.kind != CM_META_GENERIC_CONST)
            || !cm_meta_read_generic_name(&reader, generic.kind,
                &generic.name)
            || cm_hir_metadata_read_u8(&reader, &relaxed)
                != CM_HIR_METADATA_OK
            || relaxed > 1u
            || cm_hir_metadata_read_u8(&reader, &has_default)
                != CM_HIR_METADATA_OK
            || has_default > 1u
            || cm_hir_metadata_read_u32(&reader, &generic.default_type)
                != CM_HIR_METADATA_OK) return 0;
        generic.is_relaxed_sized = relaxed != 0u;
        generic.has_default = has_default != 0u;
        if ((generic.kind == CM_META_GENERIC_LIFETIME
                && (generic.is_relaxed_sized || generic.has_default))
            || (generic.kind != CM_META_GENERIC_CONST
                && !generic.has_default && generic.default_type != 0u)
            || (generic.kind == CM_META_GENERIC_CONST
                && (generic.has_default || generic.default_type == 0u)))
            return 0;
        (void)cm_vec_push(generics, &generic);
    }
    return cm_hir_metadata_reader_finish(&reader) == CM_HIR_METADATA_OK;
}

static int cm_meta_read_region(CmHirMetadataReader *reader,
    const CmVec *generics, CmMetaWireRegion *out_region)
{
    const CmMetaWireGeneric *generic;

    if (cm_hir_metadata_read_u8(reader, &out_region->kind)
            != CM_HIR_METADATA_OK
        || cm_hir_metadata_read_u32(reader, &out_region->parameter)
            != CM_HIR_METADATA_OK) return 0;
    if (out_region->kind == CM_META_REGION_STATIC)
        return out_region->parameter == 0u;
    if (out_region->kind != CM_META_REGION_EARLY_BOUND
        || out_region->parameter == 0u
        || (size_t)out_region->parameter > generics->len) return 0;
    generic = (const CmMetaWireGeneric *)cm_vec_at_const(generics,
        (size_t)(out_region->parameter - 1u));
    return generic != NULL && generic->kind == CM_META_GENERIC_LIFETIME;
}

static int cm_meta_read_const(CmHirMetadataReader *reader,
    const CmVec *generics, uint32_t current_type,
    CmMetaWireConst *out_constant)
{
    const CmMetaWireGeneric *generic;

    if (cm_hir_metadata_read_u8(reader, &out_constant->kind)
            != CM_HIR_METADATA_OK
        || (out_constant->kind != CM_META_CONST_VALUE
            && out_constant->kind != CM_META_CONST_PARAMETER)
        || cm_hir_metadata_read_u32(reader, &out_constant->type)
            != CM_HIR_METADATA_OK
        || out_constant->type == 0u
        || out_constant->type >= current_type) return 0;
    if (out_constant->kind == CM_META_CONST_VALUE) {
        return cm_hir_metadata_read_u64(reader,
                    &out_constant->data.value.low_bits) == CM_HIR_METADATA_OK
            && cm_hir_metadata_read_u64(reader,
                    &out_constant->data.value.high_bits)
                == CM_HIR_METADATA_OK;
    }
    if (cm_hir_metadata_read_u32(reader, &out_constant->data.parameter)
            != CM_HIR_METADATA_OK
        || out_constant->data.parameter == 0u
        || (size_t)out_constant->data.parameter > generics->len) return 0;
    generic = (const CmMetaWireGeneric *)cm_vec_at_const(generics,
        (size_t)(out_constant->data.parameter - 1u));
    return generic != NULL && generic->kind == CM_META_GENERIC_CONST;
}

static int cm_meta_read_named(CmHirMetadataReader *reader,
    uint32_t item_count, const CmVec *generics, uint32_t current_type,
    CmMetaWireNamed *out_named)
{
    uint32_t index;

    if (cm_hir_metadata_read_u32(reader, &out_named->item)
            != CM_HIR_METADATA_OK
        || out_named->item == 0u || out_named->item > item_count
        || cm_hir_metadata_read_u32(reader, &out_named->argument_count)
            != CM_HIR_METADATA_OK
        || out_named->argument_count > CM_META_MAX_GENERICS) return 0;
    out_named->arguments = (CmMetaWireArg *)cm_alloc_zeroed(
        (size_t)out_named->argument_count, sizeof(CmMetaWireArg));
    for (index = 0u; index < out_named->argument_count; ++index) {
        CmMetaWireArg *argument;

        argument = &out_named->arguments[index];
        if (cm_hir_metadata_read_u8(reader, &argument->kind)
                != CM_HIR_METADATA_OK) goto invalid;
        if (argument->kind == CM_META_ARG_LIFETIME) {
            if (!cm_meta_read_region(reader, generics,
                    &argument->data.lifetime)) goto invalid;
        } else if (argument->kind == CM_META_ARG_TYPE) {
            if (cm_hir_metadata_read_u32(reader, &argument->data.type)
                    != CM_HIR_METADATA_OK
                || argument->data.type == 0u
                || argument->data.type >= current_type) goto invalid;
        } else if (argument->kind == CM_META_ARG_CONST) {
            if (!cm_meta_read_const(reader, generics, current_type,
                    &argument->data.constant)) goto invalid;
        } else {
            goto invalid;
        }
    }
    return 1;

invalid:
    cm_free(out_named->arguments);
    out_named->arguments = NULL;
    out_named->argument_count = UINT32_C(0);
    return 0;
}

static void cm_meta_wire_types_destroy(CmVec *types)
{
    size_t index;

    for (index = 0u; index < types->len; ++index) {
        CmMetaWireType *type;

        type = (CmMetaWireType *)cm_vec_at(types, index);
        if (type == NULL) continue;
        if (type->kind == CM_META_TYPE_TUPLE)
            cm_free(type->data.tuple_type.elements);
        else if (type->kind == CM_META_TYPE_ADT
            || type->kind == CM_META_TYPE_ALIAS
            || type->kind == CM_META_TYPE_FOREIGN)
            cm_free(type->data.named_type.arguments);
    }
    cm_vec_destroy(types);
}

static int cm_meta_decode_types(const CmHirMetadataSection *section,
    uint32_t item_count, const CmVec *generics, CmVec *types)
{
    CmHirMetadataReader reader;
    uint32_t count;
    uint32_t index;

    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (cm_hir_metadata_read_u32(&reader, &count) != CM_HIR_METADATA_OK
        || count > CM_META_MAX_TYPES) return 0;
    for (index = 0u; index < count; ++index) {
        CmMetaWireType type;
        uint32_t current;
        uint32_t child;

        memset(&type, 0, sizeof(type));
        current = index + 1u;
        if (cm_hir_metadata_read_u8(&reader, &type.kind)
                != CM_HIR_METADATA_OK) return 0;
        switch (type.kind) {
        case CM_META_TYPE_NEVER:
        case CM_META_TYPE_UNIT:
        case CM_META_TYPE_BOOL:
        case CM_META_TYPE_CHAR:
        case CM_META_TYPE_STR:
            break;
        case CM_META_TYPE_INTEGER:
        {
            CmHirIntType integer_kind;

            if (cm_hir_metadata_read_u8(&reader, &type.data.scalar_kind)
                    != CM_HIR_METADATA_OK
                || !cm_meta_integer_from_wire(type.data.scalar_kind,
                    &integer_kind)) return 0;
            break;
        }
        case CM_META_TYPE_FLOAT:
        {
            CmHirFloatType float_kind;

            if (cm_hir_metadata_read_u8(&reader, &type.data.scalar_kind)
                    != CM_HIR_METADATA_OK
                || !cm_meta_float_from_wire(type.data.scalar_kind,
                    &float_kind)) return 0;
            break;
        }
        case CM_META_TYPE_REFERENCE:
            if (!cm_meta_read_region(&reader, generics,
                    &type.data.reference_type.region)
                || cm_hir_metadata_read_u32(&reader,
                    &type.data.reference_type.pointee) != CM_HIR_METADATA_OK
                || type.data.reference_type.pointee == 0u
                || type.data.reference_type.pointee >= current
                || cm_hir_metadata_read_u8(&reader,
                    &type.data.reference_type.mutability)
                    != CM_HIR_METADATA_OK
                || (type.data.reference_type.mutability
                        != CM_META_MUT_IMMUTABLE
                    && type.data.reference_type.mutability
                        != CM_META_MUT_MUTABLE)) return 0;
            break;
        case CM_META_TYPE_RAW_POINTER:
            if (cm_hir_metadata_read_u32(&reader,
                    &type.data.raw_pointer_type.pointee)
                    != CM_HIR_METADATA_OK
                || type.data.raw_pointer_type.pointee == 0u
                || type.data.raw_pointer_type.pointee >= current
                || cm_hir_metadata_read_u8(&reader,
                    &type.data.raw_pointer_type.mutability)
                    != CM_HIR_METADATA_OK
                || (type.data.raw_pointer_type.mutability
                        != CM_META_MUT_IMMUTABLE
                    && type.data.raw_pointer_type.mutability
                        != CM_META_MUT_MUTABLE)) return 0;
            break;
        case CM_META_TYPE_TUPLE:
            if (cm_hir_metadata_read_u32(&reader,
                    &type.data.tuple_type.element_count)
                    != CM_HIR_METADATA_OK
                || type.data.tuple_type.element_count > CM_META_MAX_TYPES)
                return 0;
            type.data.tuple_type.elements = (uint32_t *)cm_alloc_zeroed(
                (size_t)type.data.tuple_type.element_count,
                sizeof(uint32_t));
            for (child = 0u; child < type.data.tuple_type.element_count;
                    ++child) {
                if (cm_hir_metadata_read_u32(&reader,
                        &type.data.tuple_type.elements[child])
                        != CM_HIR_METADATA_OK
                    || type.data.tuple_type.elements[child] == 0u
                    || type.data.tuple_type.elements[child] >= current) {
                    cm_free(type.data.tuple_type.elements);
                    return 0;
                }
            }
            break;
        case CM_META_TYPE_ARRAY:
            if (cm_hir_metadata_read_u32(&reader,
                    &type.data.array_type.element) != CM_HIR_METADATA_OK
                || type.data.array_type.element == 0u
                || type.data.array_type.element >= current
                || !cm_meta_read_const(&reader, generics, current,
                    &type.data.array_type.length))
                return 0;
            break;
        case CM_META_TYPE_SLICE:
            if (cm_hir_metadata_read_u32(&reader,
                    &type.data.slice_type.element) != CM_HIR_METADATA_OK
                || type.data.slice_type.element == 0u
                || type.data.slice_type.element >= current) return 0;
            break;
        case CM_META_TYPE_ADT:
        case CM_META_TYPE_ALIAS:
        case CM_META_TYPE_FOREIGN:
            if (!cm_meta_read_named(&reader, item_count, generics, current,
                    &type.data.named_type)) return 0;
            break;
        case CM_META_TYPE_PARAMETER:
        {
            const CmMetaWireGeneric *generic;

            if (cm_hir_metadata_read_u32(&reader,
                    &type.data.parameter_type.parameter)
                    != CM_HIR_METADATA_OK
                || type.data.parameter_type.parameter == 0u
                || (size_t)type.data.parameter_type.parameter
                    > generics->len) return 0;
            generic = (const CmMetaWireGeneric *)cm_vec_at_const(generics,
                (size_t)(type.data.parameter_type.parameter - 1u));
            if (generic == NULL || generic->kind != CM_META_GENERIC_TYPE)
                return 0;
            break;
        }
        default:
            return 0;
        }
        (void)cm_vec_push(types, &type);
    }
    return cm_hir_metadata_reader_finish(&reader) == CM_HIR_METADATA_OK;
}

static void cm_meta_wire_items_destroy(CmVec *items)
{
    size_t index;

    for (index = 0u; index < items->len; ++index) {
        CmMetaWireItem *item;
        uint32_t child;

        item = (CmMetaWireItem *)cm_vec_at(items, index);
        if (item == NULL) continue;
        if (item->kind == CM_META_ITEM_STRUCT
            || item->kind == CM_META_ITEM_UNION) {
            cm_free(item->data.aggregate_item.fields);
        } else if (item->kind == CM_META_ITEM_ENUM) {
            for (child = 0u; child < item->data.enum_item.variant_count;
                    ++child) {
                cm_free(item->data.enum_item.variants[child].fields);
            }
            cm_free(item->data.enum_item.variants);
        }
    }
    cm_vec_destroy(items);
}

static int cm_meta_read_field(CmHirMetadataReader *reader,
    uint32_t module_count, uint32_t type_count, uint8_t form,
    CmMetaWireField *field)
{
    if (!cm_meta_read_optional_name(reader, &field->name)
        || (form == CM_META_FORM_NAMED && field->name.length == 0u)
        || (form != CM_META_FORM_NAMED && field->name.length != 0u)
        || cm_hir_metadata_read_u32(reader, &field->type)
            != CM_HIR_METADATA_OK
        || field->type == 0u || field->type > type_count
        || !cm_meta_read_visibility(reader, module_count,
            &field->visibility)) return 0;
    return 1;
}

static int cm_meta_decode_items(const CmHirMetadataSection *section,
    uint32_t module_count, uint32_t generic_count, uint32_t type_count,
    CmVec *items)
{
    CmHirMetadataReader reader;
    uint32_t count;
    uint32_t index;

    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (cm_hir_metadata_read_u32(&reader, &count) != CM_HIR_METADATA_OK
        || count > CM_META_MAX_ITEMS) return 0;
    for (index = 0u; index < count; ++index) {
        CmMetaWireItem item;
        uint32_t child;

        memset(&item, 0, sizeof(item));
        if (cm_hir_metadata_read_u8(&reader, &item.kind)
                != CM_HIR_METADATA_OK
            || !cm_meta_item_kind_from_wire(item.kind,
                &(CmHirItemKind){ CM_HIR_ITEM_EXTERN_TYPE })
            || cm_hir_metadata_read_u32(&reader, &item.owner)
                != CM_HIR_METADATA_OK
            || item.owner == 0u || item.owner > module_count
            || !cm_meta_read_name(&reader, &item.name)
            || !cm_meta_read_visibility(&reader, module_count,
                &item.visibility)
            || cm_hir_metadata_read_u32(&reader, &item.generic_start)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader, &item.generic_count)
                != CM_HIR_METADATA_OK
            || (item.generic_count == 0u && item.generic_start != 0u)
            || (item.generic_count != 0u
                && (item.generic_start == 0u
                    || item.generic_start > generic_count
                    || item.generic_count
                        > generic_count - item.generic_start + 1u))) return 0;
        if (item.kind == CM_META_ITEM_STRUCT
            || item.kind == CM_META_ITEM_UNION) {
            if (cm_hir_metadata_read_u8(&reader,
                    &item.data.aggregate_item.form) != CM_HIR_METADATA_OK
                || (item.data.aggregate_item.form != CM_META_FORM_UNIT
                    && item.data.aggregate_item.form != CM_META_FORM_TUPLE
                    && item.data.aggregate_item.form != CM_META_FORM_NAMED)
                || cm_hir_metadata_read_u32(&reader,
                    &item.data.aggregate_item.field_count)
                    != CM_HIR_METADATA_OK
                || item.data.aggregate_item.field_count > CM_META_MAX_TYPES
                || (item.data.aggregate_item.form == CM_META_FORM_UNIT
                    && item.data.aggregate_item.field_count != 0u)) return 0;
            item.data.aggregate_item.fields = (CmMetaWireField *)
                cm_alloc_zeroed(
                    (size_t)item.data.aggregate_item.field_count,
                    sizeof(CmMetaWireField));
            for (child = 0u;
                    child < item.data.aggregate_item.field_count; ++child) {
                if (!cm_meta_read_field(&reader, module_count, type_count,
                        item.data.aggregate_item.form,
                        &item.data.aggregate_item.fields[child])) return 0;
            }
        } else if (item.kind == CM_META_ITEM_ENUM) {
            if (cm_hir_metadata_read_u32(&reader,
                    &item.data.enum_item.variant_count)
                    != CM_HIR_METADATA_OK
                || item.data.enum_item.variant_count > CM_META_MAX_ITEMS)
                return 0;
            item.data.enum_item.variants = (CmMetaWireVariant *)
                cm_alloc_zeroed((size_t)item.data.enum_item.variant_count,
                    sizeof(CmMetaWireVariant));
            for (child = 0u; child < item.data.enum_item.variant_count;
                    ++child) {
                CmMetaWireVariant *variant;
                uint32_t field;
                uint8_t has_discriminant;

                variant = &item.data.enum_item.variants[child];
                if (!cm_meta_read_name(&reader, &variant->name)
                    || cm_hir_metadata_read_u8(&reader, &variant->form)
                        != CM_HIR_METADATA_OK
                    || (variant->form != CM_META_FORM_UNIT
                        && variant->form != CM_META_FORM_TUPLE
                        && variant->form != CM_META_FORM_NAMED)
                    || cm_hir_metadata_read_u32(&reader,
                        &variant->field_count) != CM_HIR_METADATA_OK
                    || variant->field_count > CM_META_MAX_TYPES
                    || (variant->form == CM_META_FORM_UNIT
                        && variant->field_count != 0u)) return 0;
                variant->fields = (CmMetaWireField *)cm_alloc_zeroed(
                    (size_t)variant->field_count, sizeof(CmMetaWireField));
                for (field = 0u; field < variant->field_count; ++field) {
                    if (!cm_meta_read_field(&reader, module_count,
                            type_count, variant->form,
                            &variant->fields[field])) return 0;
                }
                if (cm_hir_metadata_read_u8(&reader, &has_discriminant)
                        != CM_HIR_METADATA_OK
                    || has_discriminant > 1u) return 0;
                variant->has_discriminant = has_discriminant != 0u;
                if (variant->has_discriminant
                    && (cm_hir_metadata_read_u32(&reader,
                            &variant->discriminant_type)
                            != CM_HIR_METADATA_OK
                        || variant->discriminant_type == 0u
                        || variant->discriminant_type > type_count
                        || cm_hir_metadata_read_u64(&reader,
                            &variant->discriminant_low)
                            != CM_HIR_METADATA_OK
                        || cm_hir_metadata_read_u64(&reader,
                            &variant->discriminant_high)
                            != CM_HIR_METADATA_OK)) return 0;
            }
        } else if (item.kind == CM_META_ITEM_ALIAS) {
            if (cm_hir_metadata_read_u32(&reader,
                    &item.data.alias_item.target) != CM_HIR_METADATA_OK
                || item.data.alias_item.target == 0u
                || item.data.alias_item.target > type_count) return 0;
        }
        (void)cm_vec_push(items, &item);
    }
    return cm_hir_metadata_reader_finish(&reader) == CM_HIR_METADATA_OK;
}

static void cm_meta_wire_values_destroy(CmVec *values)
{
    size_t index;

    for (index = 0u; index < values->len; ++index) {
        CmMetaWireValue *value;

        value = (CmMetaWireValue *)cm_vec_at(values, index);
        if (value != NULL && value->kind == CM_META_VALUE_FUNCTION)
            cm_free(value->data.function.parameter_types);
    }
    cm_vec_destroy(values);
}

static int cm_meta_decode_values(const CmHirMetadataSection *section,
    uint32_t type_count, uint32_t generic_count, CmVec *values)
{
    CmHirMetadataReader reader;
    uint32_t count;
    uint32_t index;

    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (cm_hir_metadata_read_u32(&reader, &count) != CM_HIR_METADATA_OK
        || count > CM_META_MAX_VALUES) return 0;
    for (index = 0u; index < count; ++index) {
        CmMetaWireValue value;

        memset(&value, 0, sizeof(value));
        if (cm_hir_metadata_read_u8(&reader, &value.kind)
                != CM_HIR_METADATA_OK) return 0;
        if (value.kind == CM_META_VALUE_FUNCTION) {
            uint32_t parameter;
            uint8_t safety;
            uint8_t is_const;
            uint8_t is_async;
            uint8_t is_variadic;

            if (cm_hir_metadata_read_u32(&reader,
                    &value.data.function.parameter_count)
                    != CM_HIR_METADATA_OK
                || value.data.function.parameter_count > CM_META_MAX_TYPES) {
                return 0;
            }
            value.data.function.parameter_types = (uint32_t *)
                cm_alloc_zeroed(
                    (size_t)value.data.function.parameter_count,
                    sizeof(uint32_t));
            for (parameter = 0u;
                    parameter < value.data.function.parameter_count;
                    ++parameter) {
                if (cm_hir_metadata_read_u32(&reader,
                        &value.data.function.parameter_types[parameter])
                        != CM_HIR_METADATA_OK
                    || value.data.function.parameter_types[parameter] == 0u
                    || value.data.function.parameter_types[parameter]
                        > type_count) {
                    cm_free(value.data.function.parameter_types);
                    return 0;
                }
            }
            if (cm_hir_metadata_read_u32(&reader,
                    &value.data.function.return_type)
                    != CM_HIR_METADATA_OK
                || value.data.function.return_type == 0u
                || value.data.function.return_type > type_count
                || cm_hir_metadata_read_u32(&reader,
                    &value.data.function.generic_start)
                    != CM_HIR_METADATA_OK
                || cm_hir_metadata_read_u32(&reader,
                    &value.data.function.generic_count)
                    != CM_HIR_METADATA_OK
                || (value.data.function.generic_count == 0u
                    ? value.data.function.generic_start != 0u
                    : (value.data.function.generic_start == 0u
                        || value.data.function.generic_start > generic_count
                        || value.data.function.generic_count
                            > generic_count
                                - value.data.function.generic_start + 1u))
                || !cm_meta_read_string(&reader,
                    &value.data.function.abi)
                || cm_hir_metadata_read_u8(&reader, &safety)
                    != CM_HIR_METADATA_OK
                || safety > (uint8_t)CM_HIR_UNSAFE
                || cm_hir_metadata_read_u8(&reader, &is_const)
                    != CM_HIR_METADATA_OK || is_const > 1u
                || cm_hir_metadata_read_u8(&reader, &is_async)
                    != CM_HIR_METADATA_OK || is_async > 1u
                || cm_hir_metadata_read_u8(&reader, &is_variadic)
                    != CM_HIR_METADATA_OK || is_variadic > 1u) {
                cm_free(value.data.function.parameter_types);
                return 0;
            }
            value.data.function.safety = safety;
            value.data.function.is_const = is_const != 0u;
            value.data.function.is_async = is_async != 0u;
            value.data.function.is_variadic = is_variadic != 0u;
        } else if (value.kind == CM_META_VALUE_CONST
                || value.kind == CM_META_VALUE_STATIC) {
            if (cm_hir_metadata_read_u32(&reader,
                    &value.data.value.type) != CM_HIR_METADATA_OK
                || value.data.value.type == 0u
                || value.data.value.type > type_count
                || cm_hir_metadata_read_u8(&reader,
                    &value.data.value.mutability) != CM_HIR_METADATA_OK
                || (value.data.value.mutability != CM_META_MUT_IMMUTABLE
                    && value.data.value.mutability
                        != CM_META_MUT_MUTABLE)
                || (value.kind == CM_META_VALUE_CONST
                    && value.data.value.mutability
                        != CM_META_MUT_IMMUTABLE)) return 0;
        } else {
            return 0;
        }
        (void)cm_vec_push(values, &value);
    }
    return cm_hir_metadata_reader_finish(&reader) == CM_HIR_METADATA_OK;
}

static int cm_meta_decode_entries(const CmHirMetadataSection *section,
    uint32_t module_count, uint32_t item_count, uint32_t trait_count,
    uint32_t value_count, CmVec *entries)
{
    CmHirMetadataReader reader;
    uint32_t count;
    uint32_t index;

    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (cm_hir_metadata_read_u32(&reader, &count) != CM_HIR_METADATA_OK
        || count > CM_META_MAX_ENTRIES) return 0;
    for (index = 0u; index < count; ++index) {
        CmMetaWireEntry entry;
        CmHirPrimitiveKind primitive;

        memset(&entry, 0, sizeof(entry));
        if (cm_hir_metadata_read_u32(&reader, &entry.module)
                != CM_HIR_METADATA_OK
            || entry.module == 0u || entry.module > module_count
            || !cm_meta_read_name(&reader, &entry.name)
            || cm_hir_metadata_read_u8(&reader, &entry.kind)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader, &entry.target)
                != CM_HIR_METADATA_OK) return 0;
        if ((entry.kind == CM_META_BINDING_MODULE
                && (entry.target == 0u || entry.target > module_count))
            || (entry.kind == CM_META_BINDING_TYPE
                && (entry.target == 0u || entry.target > item_count))
            || (entry.kind == CM_META_BINDING_PRIMITIVE
                && !cm_meta_primitive_from_wire(entry.target, &primitive))
            || (entry.kind == CM_META_BINDING_TRAIT
                && (entry.target == 0u || entry.target > trait_count))
            || (entry.kind == CM_META_BINDING_VALUE
                && (entry.target == 0u || entry.target > value_count))
            || (entry.kind != CM_META_BINDING_MODULE
                && entry.kind != CM_META_BINDING_TYPE
                && entry.kind != CM_META_BINDING_PRIMITIVE
                && entry.kind != CM_META_BINDING_TRAIT
                && entry.kind != CM_META_BINDING_VALUE)) return 0;
        (void)cm_vec_push(entries, &entry);
    }
    return cm_hir_metadata_reader_finish(&reader) == CM_HIR_METADATA_OK;
}

static int cm_meta_decode_trait_universe(
    const CmHirMetadataSection *section, uint32_t module_count,
    uint32_t type_count, CmVec *traits, CmVec *impls)
{
    CmHirMetadataReader reader;
    uint8_t universe;
    uint32_t count;
    uint32_t index;

    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (cm_hir_metadata_read_u8(&reader, &universe) != CM_HIR_METADATA_OK
        || universe != CM_META_UNIVERSE_OPEN
        || cm_hir_metadata_read_u32(&reader, &count) != CM_HIR_METADATA_OK
        || count > CM_META_MAX_TRAITS) return 0;
    for (index = 0u; index < count; ++index) {
        CmMetaWireTrait trait_value;
        uint8_t safety;
        uint8_t is_auto;

        memset(&trait_value, 0, sizeof(trait_value));
        if (cm_hir_metadata_read_u32(&reader, &trait_value.owner)
                != CM_HIR_METADATA_OK
            || trait_value.owner == 0u || trait_value.owner > module_count
            || !cm_meta_read_name(&reader, &trait_value.name)
            || !cm_meta_read_visibility(&reader, module_count,
                &trait_value.visibility)
            || cm_hir_metadata_read_u8(&reader, &safety)
                != CM_HIR_METADATA_OK
            || safety > (uint8_t)CM_HIR_UNSAFE
            || cm_hir_metadata_read_u8(&reader, &is_auto)
                != CM_HIR_METADATA_OK
            || is_auto > 1u) return 0;
        trait_value.safety = safety;
        trait_value.is_auto = is_auto != 0u;
        (void)cm_vec_push(traits, &trait_value);
    }
    if (cm_hir_metadata_read_u32(&reader, &count) != CM_HIR_METADATA_OK
        || count > CM_META_MAX_IMPLS) return 0;
    for (index = 0u; index < count; ++index) {
        CmMetaWireImpl impl_value;
        uint8_t safety;
        uint8_t is_negative;

        memset(&impl_value, 0, sizeof(impl_value));
        if (cm_hir_metadata_read_u32(&reader, &impl_value.owner)
                != CM_HIR_METADATA_OK
            || impl_value.owner == 0u || impl_value.owner > module_count
            || cm_hir_metadata_read_u32(&reader,
                &impl_value.trait_local) != CM_HIR_METADATA_OK
            || impl_value.trait_local == 0u
            || (size_t)impl_value.trait_local > traits->len
            || cm_hir_metadata_read_u32(&reader, &impl_value.self_type)
                != CM_HIR_METADATA_OK
            || impl_value.self_type == 0u
            || impl_value.self_type > type_count
            || cm_hir_metadata_read_u8(&reader, &safety)
                != CM_HIR_METADATA_OK
            || safety > (uint8_t)CM_HIR_UNSAFE
            || cm_hir_metadata_read_u8(&reader, &is_negative)
                != CM_HIR_METADATA_OK
            || is_negative > 1u) return 0;
        impl_value.safety = safety;
        impl_value.is_negative = is_negative != 0u;
        if (impl_value.is_negative) {
            const CmMetaWireTrait *trait_value;

            trait_value = (const CmMetaWireTrait *)cm_vec_at_const(traits,
                (size_t)(impl_value.trait_local - 1u));
            if (trait_value == NULL || !trait_value->is_auto
                || impl_value.safety != (uint8_t)CM_HIR_SAFE) return 0;
        } else {
            const CmMetaWireTrait *trait_value;

            trait_value = (const CmMetaWireTrait *)cm_vec_at_const(traits,
                (size_t)(impl_value.trait_local - 1u));
            if (trait_value == NULL
                || impl_value.safety != trait_value->safety) return 0;
        }
        (void)cm_vec_push(impls, &impl_value);
    }
    return cm_hir_metadata_reader_finish(&reader) == CM_HIR_METADATA_OK;
}

static int cm_meta_wire_scalar_type_equal(const CmVec *types,
    uint32_t left_local, uint32_t right_local)
{
    const CmMetaWireType *left;
    const CmMetaWireType *right;

    left = left_local == 0u || (size_t)left_local > types->len ? NULL
        : (const CmMetaWireType *)cm_vec_at_const(types,
            (size_t)(left_local - 1u));
    right = right_local == 0u || (size_t)right_local > types->len ? NULL
        : (const CmMetaWireType *)cm_vec_at_const(types,
            (size_t)(right_local - 1u));
    if (left == NULL || right == NULL || left->kind != right->kind) return 0;
    if (left->kind == CM_META_TYPE_BOOL
        || left->kind == CM_META_TYPE_CHAR) return 1;
    return left->kind == CM_META_TYPE_INTEGER
        && left->data.scalar_kind == right->data.scalar_kind;
}

static int cm_meta_wire_const_valid(const CmMetaWireConst *constant,
    uint32_t expected_type, const CmVec *generics, const CmVec *types)
{
    const CmMetaWireGeneric *parameter;

    if (constant == NULL
        || (constant->kind != CM_META_CONST_VALUE
            && constant->kind != CM_META_CONST_PARAMETER)
        || !cm_meta_wire_scalar_type_equal(types, constant->type,
            expected_type)) return 0;
    if (constant->kind == CM_META_CONST_VALUE) return 1;
    parameter = constant->data.parameter == 0u
            || (size_t)constant->data.parameter > generics->len ? NULL
        : (const CmMetaWireGeneric *)cm_vec_at_const(generics,
            (size_t)(constant->data.parameter - 1u));
    return parameter != NULL && parameter->kind == CM_META_GENERIC_CONST
        && cm_meta_wire_scalar_type_equal(types, constant->type,
            parameter->default_type);
}

static int cm_meta_wire_named_valid(const CmMetaWireNamed *named,
    uint8_t type_kind, const CmVec *items, const CmVec *generics,
    const CmVec *types)
{
    const CmMetaWireItem *item;
    uint32_t index;

    item = named->item == 0u || (size_t)named->item > items->len ? NULL
        : (const CmMetaWireItem *)cm_vec_at_const(items,
            (size_t)(named->item - 1u));
    if (item == NULL || named->argument_count != item->generic_count)
        return 0;
    if ((type_kind == CM_META_TYPE_ALIAS
            && item->kind != CM_META_ITEM_ALIAS)
        || (type_kind == CM_META_TYPE_FOREIGN
            && item->kind != CM_META_ITEM_EXTERN_TYPE)
        || (type_kind == CM_META_TYPE_ADT
            && item->kind != CM_META_ITEM_STRUCT
            && item->kind != CM_META_ITEM_UNION
            && item->kind != CM_META_ITEM_ENUM)) return 0;
    for (index = 0u; index < named->argument_count; ++index) {
        const CmMetaWireGeneric *generic;
        const CmMetaWireArg *argument;

        generic = (const CmMetaWireGeneric *)cm_vec_at_const(generics,
            (size_t)(item->generic_start - 1u + index));
        argument = &named->arguments[index];
        if (generic == NULL
            || (generic->kind == CM_META_GENERIC_LIFETIME
                && argument->kind != CM_META_ARG_LIFETIME)
            || (generic->kind == CM_META_GENERIC_TYPE
                && argument->kind != CM_META_ARG_TYPE)
            || (generic->kind == CM_META_GENERIC_CONST
                && (argument->kind != CM_META_ARG_CONST
                    || !cm_meta_wire_const_valid(&argument->data.constant,
                        generic->default_type, generics, types)))) return 0;
    }
    return 1;
}

static int cm_meta_alias_acyclic(uint32_t item_local, const CmVec *items,
    const CmVec *types, unsigned char *states);

static int cm_meta_alias_type_acyclic(uint32_t type_local,
    const CmVec *items, const CmVec *types, unsigned char *states)
{
    const CmMetaWireType *type;
    uint32_t index;

    type = type_local == 0u || (size_t)type_local > types->len ? NULL
        : (const CmMetaWireType *)cm_vec_at_const(types,
            (size_t)(type_local - 1u));
    if (type == NULL) return 0;
    switch (type->kind) {
    case CM_META_TYPE_REFERENCE:
        return cm_meta_alias_type_acyclic(
            type->data.reference_type.pointee, items, types, states);
    case CM_META_TYPE_RAW_POINTER:
        return cm_meta_alias_type_acyclic(
            type->data.raw_pointer_type.pointee, items, types, states);
    case CM_META_TYPE_TUPLE:
        for (index = 0u; index < type->data.tuple_type.element_count;
                ++index) {
            if (!cm_meta_alias_type_acyclic(
                    type->data.tuple_type.elements[index], items, types,
                    states)) return 0;
        }
        return 1;
    case CM_META_TYPE_ARRAY:
        return cm_meta_alias_type_acyclic(type->data.array_type.element,
                items, types, states)
            && cm_meta_alias_type_acyclic(
                type->data.array_type.length.type, items, types, states);
    case CM_META_TYPE_SLICE:
        return cm_meta_alias_type_acyclic(type->data.slice_type.element,
            items, types, states);
    case CM_META_TYPE_ADT:
    case CM_META_TYPE_ALIAS:
    case CM_META_TYPE_FOREIGN:
        for (index = 0u; index < type->data.named_type.argument_count;
                ++index) {
            const CmMetaWireArg *argument;

            argument = &type->data.named_type.arguments[index];
            if (argument->kind == CM_META_ARG_TYPE
                && !cm_meta_alias_type_acyclic(argument->data.type, items,
                    types, states)) return 0;
            if (argument->kind == CM_META_ARG_CONST
                && !cm_meta_alias_type_acyclic(
                    argument->data.constant.type, items, types, states)) {
                return 0;
            }
        }
        return type->kind != CM_META_TYPE_ALIAS
            || cm_meta_alias_acyclic(type->data.named_type.item, items,
                types, states);
    default:
        return 1;
    }
}

static int cm_meta_alias_acyclic(uint32_t item_local, const CmVec *items,
    const CmVec *types, unsigned char *states)
{
    const CmMetaWireItem *item;

    item = item_local == 0u || (size_t)item_local > items->len ? NULL
        : (const CmMetaWireItem *)cm_vec_at_const(items,
            (size_t)(item_local - 1u));
    if (item == NULL || item->kind != CM_META_ITEM_ALIAS) return 0;
    if (states[item_local - 1u] == UINT8_C(1)) return 0;
    if (states[item_local - 1u] == UINT8_C(2)) return 1;
    states[item_local - 1u] = UINT8_C(1);
    if (!cm_meta_alias_type_acyclic(item->data.alias_item.target, items,
            types, states)) return 0;
    states[item_local - 1u] = UINT8_C(2);
    return 1;
}

static int cm_meta_wire_modules_canonical(const CmVec *modules)
{
    CmByteBuf *paths;
    size_t index;
    int valid;

    if (modules->len == 0u) return 0;
    paths = (CmByteBuf *)cm_alloc_zeroed(modules->len, sizeof(CmByteBuf));
    for (index = 0u; index < modules->len; ++index)
        cm_byte_buf_init(&paths[index]);
    valid = 1;
    for (index = 0u; valid && index < modules->len; ++index) {
        const CmMetaWireModule *module;

        module = (const CmMetaWireModule *)cm_vec_at_const(modules, index);
        if (module == NULL
            || (index == 0u && module->parent != 0u)
            || (index != 0u
                && (module->parent == 0u
                    || (size_t)module->parent > index))) {
            valid = 0;
            break;
        }
        if (index != 0u) {
            cm_byte_buf_append(&paths[index],
                paths[module->parent - 1u].data,
                paths[module->parent - 1u].len);
            cm_byte_buf_push(&paths[index], (unsigned char)'/');
            cm_byte_buf_append(&paths[index], module->name.bytes,
                module->name.length);
            if (cm_meta_bytes_compare(paths[index - 1u].data,
                    paths[index - 1u].len, paths[index].data,
                    paths[index].len) >= 0) valid = 0;
        }
    }
    for (index = 0u; index < modules->len; ++index)
        cm_byte_buf_destroy(&paths[index]);
    cm_free(paths);
    return valid;
}

static int cm_meta_wire_trait_universe_canonical(const CmVec *traits,
    const CmVec *impls)
{
    size_t index;

    for (index = 1u; index < traits->len; ++index) {
        const CmMetaWireTrait *prior;
        const CmMetaWireTrait *trait_value;
        int names;

        prior = (const CmMetaWireTrait *)cm_vec_at_const(traits,
            index - 1u);
        trait_value = (const CmMetaWireTrait *)cm_vec_at_const(traits,
            index);
        if (prior == NULL || trait_value == NULL
            || prior->owner > trait_value->owner) return 0;
        names = cm_meta_bytes_compare(prior->name.bytes,
            prior->name.length, trait_value->name.bytes,
            trait_value->name.length);
        if (prior->owner == trait_value->owner && names >= 0) return 0;
    }
    for (index = 1u; index < impls->len; ++index) {
        const CmMetaWireImpl *prior;
        const CmMetaWireImpl *impl_value;

        prior = (const CmMetaWireImpl *)cm_vec_at_const(impls, index - 1u);
        impl_value = (const CmMetaWireImpl *)cm_vec_at_const(impls, index);
        if (prior == NULL || impl_value == NULL
            || prior->trait_local > impl_value->trait_local) return 0;
    }
    return 1;
}

static int cm_meta_wire_valid(const CmVec *modules, const CmVec *generics,
    const CmVec *types, const CmVec *items, const CmVec *values,
    const CmVec *entries)
{
    size_t index;
    unsigned char *alias_states;
    int valid;

    if (!cm_meta_wire_modules_canonical(modules)) return 0;
    for (index = 0u; index < items->len; ++index) {
        const CmMetaWireItem *item;
        uint32_t parameter;

        item = (const CmMetaWireItem *)cm_vec_at_const(items, index);
        if (item == NULL) return 0;
        if (index != 0u) {
            const CmMetaWireItem *prior;

            prior = (const CmMetaWireItem *)cm_vec_at_const(items,
                index - 1u);
            if (prior == NULL || prior->owner > item->owner
                || (prior->owner == item->owner
                    && cm_meta_bytes_compare(prior->name.bytes,
                        prior->name.length, item->name.bytes,
                        item->name.length) >= 0)) return 0;
        }
        for (parameter = 0u; parameter < item->generic_count;
                ++parameter) {
            const CmMetaWireGeneric *generic;

            generic = (const CmMetaWireGeneric *)cm_vec_at_const(generics,
                (size_t)(item->generic_start - 1u + parameter));
            if (generic == NULL
                || generic->owner_kind != CM_META_GENERIC_OWNER_ITEM
                || generic->owner != index + 1u
                || generic->index != parameter) return 0;
        }
    }
    if (values != NULL) {
        for (index = 0u; index < values->len; ++index) {
            const CmMetaWireValue *value;
            uint32_t parameter;

            value = (const CmMetaWireValue *)cm_vec_at_const(values,
                index);
            if (value == NULL) return 0;
            if (value->kind != CM_META_VALUE_FUNCTION) continue;
            for (parameter = 0u;
                    parameter < value->data.function.generic_count;
                    ++parameter) {
                const CmMetaWireGeneric *generic;

                generic = (const CmMetaWireGeneric *)cm_vec_at_const(
                    generics, (size_t)(value->data.function.generic_start
                        - 1u + parameter));
                if (generic == NULL
                    || generic->owner_kind != CM_META_GENERIC_OWNER_VALUE
                    || generic->owner != index + 1u
                    || generic->index != parameter) return 0;
            }
        }
    }
    for (index = 0u; index < generics->len; ++index) {
        const CmMetaWireGeneric *generic;
        uint32_t owner_start;
        uint32_t owner_count;

        generic = (const CmMetaWireGeneric *)cm_vec_at_const(generics,
            index);
        owner_start = UINT32_C(0);
        owner_count = UINT32_C(0);
        if (generic != NULL
            && generic->owner_kind == CM_META_GENERIC_OWNER_ITEM
            && generic->owner != 0u
            && (size_t)generic->owner <= items->len) {
            const CmMetaWireItem *owner;

            owner = (const CmMetaWireItem *)cm_vec_at_const(items,
                (size_t)(generic->owner - 1u));
            if (owner != NULL) {
                owner_start = owner->generic_start;
                owner_count = owner->generic_count;
            }
        } else if (generic != NULL
            && generic->owner_kind == CM_META_GENERIC_OWNER_VALUE
            && values != NULL && generic->owner != 0u
            && (size_t)generic->owner <= values->len) {
            const CmMetaWireValue *owner;

            owner = (const CmMetaWireValue *)cm_vec_at_const(values,
                (size_t)(generic->owner - 1u));
            if (owner != NULL && owner->kind == CM_META_VALUE_FUNCTION) {
                owner_start = owner->data.function.generic_start;
                owner_count = owner->data.function.generic_count;
            }
        }
        if (owner_start == 0u
            || index + 1u < (size_t)owner_start
            || index + 1u >= (size_t)owner_start + (size_t)owner_count
            || ((generic->has_default
                    || generic->kind == CM_META_GENERIC_CONST)
                && (generic->default_type == 0u
                    || (size_t)generic->default_type > types->len))) return 0;
    }
    for (index = 0u; index < types->len; ++index) {
        const CmMetaWireType *type;

        type = (const CmMetaWireType *)cm_vec_at_const(types, index);
        if (type == NULL) return 0;
        if ((type->kind == CM_META_TYPE_ADT
                || type->kind == CM_META_TYPE_ALIAS
                || type->kind == CM_META_TYPE_FOREIGN)
            && !cm_meta_wire_named_valid(&type->data.named_type, type->kind,
                items, generics, types)) return 0;
        if (type->kind == CM_META_TYPE_ARRAY
            && !cm_meta_wire_const_valid(&type->data.array_type.length,
                type->data.array_type.length.type, generics, types)) {
            return 0;
        }
    }
    for (index = 1u; index < entries->len; ++index) {
        const CmMetaWireEntry *prior;
        const CmMetaWireEntry *entry;

        prior = (const CmMetaWireEntry *)cm_vec_at_const(entries, index - 1u);
        entry = (const CmMetaWireEntry *)cm_vec_at_const(entries, index);
        if (prior == NULL || entry == NULL
            || prior->module > entry->module
            || (prior->module == entry->module
                && (cm_meta_bytes_compare(prior->name.bytes,
                        prior->name.length, entry->name.bytes,
                        entry->name.length) > 0
                    || (cm_meta_name_equal(prior->name, entry->name)
                        && ((prior->kind == CM_META_BINDING_VALUE)
                                == (entry->kind
                                    == CM_META_BINDING_VALUE)
                            || prior->kind
                                == CM_META_BINDING_VALUE))))) return 0;
    }
    if (values != NULL) {
        unsigned char *seen;
        uint32_t next;

        seen = (unsigned char *)cm_alloc_zeroed(values->len,
            sizeof(unsigned char));
        next = UINT32_C(1);
        for (index = 0u; index < entries->len; ++index) {
            const CmMetaWireEntry *entry;

            entry = (const CmMetaWireEntry *)cm_vec_at_const(entries,
                index);
            if (entry == NULL) {
                cm_free(seen);
                return 0;
            }
            if (entry->kind != CM_META_BINDING_VALUE
                || seen[entry->target - 1u] != 0u) continue;
            if (entry->target != next) {
                cm_free(seen);
                return 0;
            }
            seen[entry->target - 1u] = UINT8_C(1);
            next += 1u;
        }
        cm_free(seen);
        if ((size_t)(next - 1u) != values->len) return 0;
    }
    alias_states = (unsigned char *)cm_alloc_zeroed(items->len,
        sizeof(unsigned char));
    valid = 1;
    for (index = 0u; valid && index < items->len; ++index) {
        const CmMetaWireItem *item;

        item = (const CmMetaWireItem *)cm_vec_at_const(items, index);
        if (item != NULL && item->kind == CM_META_ITEM_ALIAS)
            valid = cm_meta_alias_acyclic((uint32_t)(index + 1u), items,
                types, alias_states);
    }
    cm_free(alias_states);
    (void)modules;
    return valid;
}

static CmInternId cm_meta_intern_name(CmHirContext *context,
    CmMetaWireName name)
{
    return cm_interner_intern(&context->strings, name.bytes, name.length);
}

static int cm_meta_visibility_from_wire(const CmMetaWireVisibility *wire,
    const CmHirModuleId *runtime_modules, CmHirContext *context,
    CmHirVisibility *out_visibility)
{
    const CmHirModule *module;

    out_visibility->restriction = cm_hir_def_id_none();
    switch (wire->kind) {
    case CM_META_VIS_PRIVATE:
        out_visibility->kind = CM_HIR_VIS_PRIVATE;
        return 1;
    case CM_META_VIS_PUBLIC:
        out_visibility->kind = CM_HIR_VIS_PUBLIC;
        return 1;
    case CM_META_VIS_CRATE:
        out_visibility->kind = CM_HIR_VIS_CRATE;
        return 1;
    case CM_META_VIS_RESTRICTED:
        module = cm_hir_get_module(context,
            runtime_modules[wire->restriction - 1u]);
        if (module == NULL) return 0;
        out_visibility->kind = CM_HIR_VIS_RESTRICTED;
        out_visibility->restriction = module->definition;
        return 1;
    default:
        return 0;
    }
}

static int cm_meta_region_from_wire(const CmMetaWireRegion *wire,
    const CmHirGenericParamId *runtime_generics, CmHirRegion *out_region)
{
    memset(out_region, 0, sizeof(*out_region));
    if (wire->kind == CM_META_REGION_STATIC) {
        out_region->kind = CM_HIR_REGION_STATIC;
        return 1;
    }
    if (wire->kind != CM_META_REGION_EARLY_BOUND
        || wire->parameter == 0u) return 0;
    out_region->kind = CM_HIR_REGION_EARLY_BOUND;
    out_region->data.parameter = runtime_generics[wire->parameter - 1u];
    return out_region->data.parameter != CM_HIR_GENERIC_PARAM_NONE;
}

static int cm_meta_const_from_wire(const CmMetaWireConst *wire,
    const CmHirGenericParamId *runtime_generics,
    const CmHirTypeId *runtime_types, CmHirConstArg *out_constant)
{
    memset(out_constant, 0, sizeof(*out_constant));
    out_constant->type = runtime_types[wire->type - 1u];
    if (out_constant->type == CM_HIR_TYPE_NONE) return 0;
    if (wire->kind == CM_META_CONST_VALUE) {
        out_constant->kind = CM_HIR_CONST_VALUE;
        out_constant->data.value.low_bits = wire->data.value.low_bits;
        out_constant->data.value.high_bits = wire->data.value.high_bits;
        return 1;
    }
    if (wire->kind != CM_META_CONST_PARAMETER
        || wire->data.parameter == 0u) return 0;
    out_constant->kind = CM_HIR_CONST_PARAMETER;
    out_constant->data.parameter =
        runtime_generics[wire->data.parameter - 1u];
    return out_constant->data.parameter != CM_HIR_GENERIC_PARAM_NONE;
}

static int cm_meta_named_from_wire(const CmMetaWireNamed *wire,
    const CmHirDefId *runtime_items,
    const CmHirGenericParamId *runtime_generics,
    const CmHirTypeId *runtime_types, CmHirNamedType *out_named)
{
    uint32_t index;

    memset(out_named, 0, sizeof(*out_named));
    out_named->definition = runtime_items[wire->item - 1u];
    out_named->argument_count = wire->argument_count;
    out_named->arguments = (CmHirGenericArg *)cm_alloc_zeroed(
        (size_t)wire->argument_count, sizeof(CmHirGenericArg));
    for (index = 0u; index < wire->argument_count; ++index) {
        const CmMetaWireArg *argument;
        CmHirGenericArg *runtime;

        argument = &wire->arguments[index];
        runtime = &out_named->arguments[index];
        if (argument->kind == CM_META_ARG_LIFETIME) {
            runtime->kind = CM_HIR_GENERIC_ARG_LIFETIME;
            if (!cm_meta_region_from_wire(&argument->data.lifetime,
                    runtime_generics, &runtime->data.lifetime)) return 0;
        } else if (argument->kind == CM_META_ARG_TYPE) {
            runtime->kind = CM_HIR_GENERIC_ARG_TYPE;
            runtime->data.type = runtime_types[argument->data.type - 1u];
            if (runtime->data.type == CM_HIR_TYPE_NONE) return 0;
        } else if (argument->kind == CM_META_ARG_CONST) {
            runtime->kind = CM_HIR_GENERIC_ARG_CONST;
            if (!cm_meta_const_from_wire(&argument->data.constant,
                    runtime_generics, runtime_types,
                    &runtime->data.constant)) return 0;
        } else {
            return 0;
        }
    }
    return 1;
}

static CmHirMutability cm_meta_mutability_from_wire(uint8_t wire)
{
    return wire == CM_META_MUT_MUTABLE ? CM_HIR_MUTABLE : CM_HIR_IMMUTABLE;
}

static int cm_meta_add_runtime_type(CmHirContext *context,
    const CmMetaWireType *wire, const CmHirDefId *runtime_items,
    const CmHirGenericParamId *runtime_generics,
    const CmHirTypeId *runtime_types, CmSpan span, CmHirTypeId *out_type)
{
    CmHirType type;
    CmHirTypeId *tuple_elements;
    CmHirGenericArg *named_arguments;
    uint32_t index;
    int valid;

    memset(&type, 0, sizeof(type));
    type.span = span;
    tuple_elements = NULL;
    named_arguments = NULL;
    valid = 1;
    switch (wire->kind) {
    case CM_META_TYPE_NEVER: type.kind = CM_HIR_TYPE_NEVER_KIND; break;
    case CM_META_TYPE_UNIT: type.kind = CM_HIR_TYPE_UNIT_KIND; break;
    case CM_META_TYPE_BOOL: type.kind = CM_HIR_TYPE_BOOL_KIND; break;
    case CM_META_TYPE_CHAR: type.kind = CM_HIR_TYPE_CHAR_KIND; break;
    case CM_META_TYPE_STR: type.kind = CM_HIR_TYPE_STR_KIND; break;
    case CM_META_TYPE_INTEGER:
        type.kind = CM_HIR_TYPE_INTEGER_KIND;
        valid = cm_meta_integer_from_wire(wire->data.scalar_kind,
            &type.data.integer_type.kind);
        break;
    case CM_META_TYPE_FLOAT:
        type.kind = CM_HIR_TYPE_FLOAT_KIND;
        valid = cm_meta_float_from_wire(wire->data.scalar_kind,
            &type.data.float_type.kind);
        break;
    case CM_META_TYPE_REFERENCE:
        type.kind = CM_HIR_TYPE_REFERENCE_KIND;
        valid = cm_meta_region_from_wire(&wire->data.reference_type.region,
            runtime_generics, &type.data.reference_type.region);
        type.data.reference_type.pointee =
            runtime_types[wire->data.reference_type.pointee - 1u];
        type.data.reference_type.mutability = cm_meta_mutability_from_wire(
            wire->data.reference_type.mutability);
        break;
    case CM_META_TYPE_RAW_POINTER:
        type.kind = CM_HIR_TYPE_RAW_POINTER_KIND;
        type.data.raw_pointer_type.pointee =
            runtime_types[wire->data.raw_pointer_type.pointee - 1u];
        type.data.raw_pointer_type.mutability = cm_meta_mutability_from_wire(
            wire->data.raw_pointer_type.mutability);
        break;
    case CM_META_TYPE_TUPLE:
        type.kind = CM_HIR_TYPE_TUPLE_KIND;
        type.data.tuple_type.element_count =
            wire->data.tuple_type.element_count;
        tuple_elements = (CmHirTypeId *)cm_alloc_zeroed(
            (size_t)wire->data.tuple_type.element_count,
            sizeof(CmHirTypeId));
        type.data.tuple_type.elements = tuple_elements;
        for (index = 0u; index < wire->data.tuple_type.element_count;
                ++index) {
            tuple_elements[index] = runtime_types[
                wire->data.tuple_type.elements[index] - 1u];
        }
        break;
    case CM_META_TYPE_ARRAY:
        type.kind = CM_HIR_TYPE_ARRAY_KIND;
        type.data.array_type.element =
            runtime_types[wire->data.array_type.element - 1u];
        valid = cm_meta_const_from_wire(&wire->data.array_type.length,
            runtime_generics, runtime_types,
            &type.data.array_type.length);
        break;
    case CM_META_TYPE_SLICE:
        type.kind = CM_HIR_TYPE_SLICE_KIND;
        type.data.slice_type.element =
            runtime_types[wire->data.slice_type.element - 1u];
        break;
    case CM_META_TYPE_ADT:
    case CM_META_TYPE_ALIAS:
    case CM_META_TYPE_FOREIGN:
        type.kind = wire->kind == CM_META_TYPE_ADT ? CM_HIR_TYPE_ADT_KIND
            : (wire->kind == CM_META_TYPE_ALIAS
                ? CM_HIR_TYPE_ALIAS_APPLICATION_KIND
                : CM_HIR_TYPE_FOREIGN_KIND);
        valid = cm_meta_named_from_wire(&wire->data.named_type,
            runtime_items, runtime_generics, runtime_types,
            &type.data.named_type);
        named_arguments = type.data.named_type.arguments;
        break;
    case CM_META_TYPE_PARAMETER:
        type.kind = CM_HIR_TYPE_PARAMETER_KIND;
        type.data.parameter_type.parameter = runtime_generics[
            wire->data.parameter_type.parameter - 1u];
        break;
    default:
        valid = 0;
        break;
    }
    if (valid)
        valid = cm_hir_add_type(context, &type, out_type) == CM_HIR_OK;
    cm_free(named_arguments);
    cm_free(tuple_elements);
    return valid;
}

static int cm_meta_runtime_field(CmHirContext *context,
    const CmMetaWireField *wire, CmHirAggregateForm form,
    const CmHirModuleId *runtime_modules, const CmHirTypeId *runtime_types,
    CmSpan span, CmHirField *out_field)
{
    memset(out_field, 0, sizeof(*out_field));
    out_field->name = form == CM_HIR_AGGREGATE_NAMED
        ? cm_meta_intern_name(context, wire->name) : CM_INTERN_ID_NONE;
    out_field->type = runtime_types[wire->type - 1u];
    out_field->span = span;
    return cm_meta_visibility_from_wire(&wire->visibility, runtime_modules,
        context, &out_field->visibility);
}

static int cm_meta_bind_runtime_item(CmHirContext *context,
    CmMetaWireItem *wire, uint32_t item_local,
    const CmHirModuleId *runtime_modules,
    const CmHirDefId *runtime_items,
    const CmHirGenericParamId *runtime_generics,
    const CmHirTypeId *runtime_types, CmSpan span)
{
    CmHirItem item;
    CmHirItemId item_id;
    CmHirField *fields;
    CmHirVariant *variants;
    uint32_t index;
    int valid;

    memset(&item, 0, sizeof(item));
    item.definition = runtime_items[item_local - 1u];
    item.owner_module = runtime_modules[wire->owner - 1u];
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_meta_intern_name(context, wire->name);
    item.span = span;
    if (!cm_meta_visibility_from_wire(&wire->visibility, runtime_modules,
            context, &item.visibility)) return 0;
    item.generic_parameter_start = wire->generic_count == 0u
        ? CM_HIR_GENERIC_PARAM_NONE
        : runtime_generics[wire->generic_start - 1u];
    item.generic_parameter_count = wire->generic_count;
    fields = NULL;
    variants = NULL;
    valid = 1;
    if (!cm_meta_item_kind_from_wire(wire->kind, &item.kind)) return 0;
    if (wire->kind == CM_META_ITEM_STRUCT
        || wire->kind == CM_META_ITEM_UNION) {
        CmHirAggregateForm form;

        form = CM_HIR_AGGREGATE_UNIT;
        valid = cm_meta_form_from_wire(wire->data.aggregate_item.form,
            &form);
        item.data.aggregate_item.form = form;
        item.data.aggregate_item.field_count =
            wire->data.aggregate_item.field_count;
        fields = (CmHirField *)cm_alloc_zeroed(
            (size_t)wire->data.aggregate_item.field_count,
            sizeof(CmHirField));
        item.data.aggregate_item.fields = fields;
        for (index = 0u; valid
                && index < wire->data.aggregate_item.field_count; ++index) {
            valid = cm_meta_runtime_field(context,
                &wire->data.aggregate_item.fields[index], form,
                runtime_modules, runtime_types, span, &fields[index]);
        }
    } else if (wire->kind == CM_META_ITEM_ENUM) {
        item.data.enum_item.variant_count = wire->data.enum_item.variant_count;
        variants = (CmHirVariant *)cm_alloc_zeroed(
            (size_t)wire->data.enum_item.variant_count,
            sizeof(CmHirVariant));
        item.data.enum_item.variants = variants;
        for (index = 0u; valid
                && index < wire->data.enum_item.variant_count; ++index) {
            CmMetaWireVariant *wire_variant;
            CmHirVariant *variant;
            uint32_t field;

            wire_variant = &wire->data.enum_item.variants[index];
            variant = &variants[index];
            variant->definition = wire_variant->runtime_definition;
            variant->name = cm_meta_intern_name(context, wire_variant->name);
            variant->span = span;
            valid = cm_meta_form_from_wire(wire_variant->form,
                &variant->form);
            variant->field_count = wire_variant->field_count;
            variant->fields = (CmHirField *)cm_alloc_zeroed(
                (size_t)wire_variant->field_count, sizeof(CmHirField));
            for (field = 0u; valid && field < wire_variant->field_count;
                    ++field) {
                valid = cm_meta_runtime_field(context,
                    &wire_variant->fields[field], variant->form,
                    runtime_modules, runtime_types, span,
                    &variant->fields[field]);
            }
            variant->has_discriminant = wire_variant->has_discriminant;
            if (wire_variant->has_discriminant) {
                variant->discriminant.kind = CM_HIR_CONST_VALUE;
                variant->discriminant.type = runtime_types[
                    wire_variant->discriminant_type - 1u];
                variant->discriminant.data.value.low_bits =
                    wire_variant->discriminant_low;
                variant->discriminant.data.value.high_bits =
                    wire_variant->discriminant_high;
            }
        }
    } else if (wire->kind == CM_META_ITEM_ALIAS) {
        item.data.type_alias_item.target =
            runtime_types[wire->data.alias_item.target - 1u];
        item.data.type_alias_item.trait_item_definition =
            cm_hir_def_id_none();
    }
    if (valid)
        valid = cm_hir_add_item(context, &item, &item_id) == CM_HIR_OK;
    if (variants != NULL) {
        for (index = 0u; index < wire->data.enum_item.variant_count;
                ++index) cm_free(variants[index].fields);
    }
    cm_free(variants);
    cm_free(fields);
    return valid;
}

static int cm_meta_bind_runtime_trait(CmHirContext *context,
    const CmMetaWireTrait *wire, uint32_t trait_local,
    const CmHirModuleId *runtime_modules,
    const CmHirDefId *runtime_traits, CmSpan span)
{
    CmHirItem item;
    CmHirItemId item_id;

    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_TRAIT;
    item.definition = runtime_traits[trait_local - 1u];
    item.owner_module = runtime_modules[wire->owner - 1u];
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_meta_intern_name(context, wire->name);
    item.span = span;
    if (!cm_meta_visibility_from_wire(&wire->visibility, runtime_modules,
            context, &item.visibility)) return 0;
    item.data.trait_item.safety = (CmHirSafety)wire->safety;
    item.data.trait_item.is_auto = wire->is_auto;
    return cm_hir_add_item(context, &item, &item_id) == CM_HIR_OK;
}

static int cm_meta_bind_runtime_impl(CmHirContext *context,
    const CmMetaWireImpl *wire, uint32_t impl_local,
    const CmHirModuleId *runtime_modules,
    const CmHirDefId *runtime_traits, const CmHirDefId *runtime_impls,
    const CmHirTypeId *runtime_types, CmSpan span)
{
    CmHirItem item;
    CmHirItemId item_id;

    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_IMPL;
    item.definition = runtime_impls[impl_local - 1u];
    item.owner_module = runtime_modules[wire->owner - 1u];
    item.parent_definition = cm_hir_def_id_none();
    item.name = CM_INTERN_ID_NONE;
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = span;
    item.data.impl_item.self_type = runtime_types[wire->self_type - 1u];
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition =
        runtime_traits[wire->trait_local - 1u];
    item.data.impl_item.safety = (CmHirSafety)wire->safety;
    item.data.impl_item.is_negative = wire->is_negative;
    return cm_hir_add_item(context, &item, &item_id) == CM_HIR_OK;
}

static CmHirMetadataArtifactResult cm_meta_decode_artifact(
    CmHirContext *context, CmHirLibraryArtifact *artifact,
    const void *encoded, size_t encoded_length, const char *extern_name,
    CmSourceId metadata_source, int semantic, int declaration)
{
    CmHirMetadataArtifactResult result;
    CmHirMetadataEnvelope envelope;
    CmHirMetadataReader section_reader;
    CmHirMetadataSection sections[7];
    CmHirMetadataStatus codec_status;
    CmMetaWireName crate_name;
    CmHirEdition edition;
    CmVec modules;
    CmVec generics;
    CmVec types;
    CmVec items;
    CmVec entries;
    CmVec traits;
    CmVec impls;
    CmVec values;
    uint32_t item_count;
    uint32_t type_count;
    uint32_t value_count;
    uint32_t root_local;
    CmHirContextMark mark;
    int mark_active;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    CmHirModuleId *runtime_modules;
    CmHirDefId *runtime_items;
    CmHirDefId *runtime_traits;
    CmHirDefId *runtime_impls;
    CmHirDefId *runtime_values;
    CmHirGenericParamId *runtime_generics;
    CmHirTypeId *runtime_types;
    unsigned char *module_created;
    CmHirLibraryOwnedData owned;
    int owned_initialized;
    CmHirLibraryArtifact candidate_artifact;
    int candidate_initialized;
    CmSpan span;
    uint32_t index;
    size_t public_entry_count;

    result = cm_meta_result(CM_HIR_METADATA_ARTIFACT_INVALID_ARGUMENT);
    if (context == NULL || artifact == NULL || artifact->state == NULL
        || (semantic && declaration)
        || (encoded_length != 0u && encoded == NULL)
        || !cm_meta_identifier_c_str_valid(extern_name)) return result;
    memset(&envelope, 0, sizeof(envelope));
    codec_status = cm_hir_metadata_decode_envelope_version(encoded,
        encoded_length,
        (uint16_t)(declaration ? CM_HIR_METADATA_DECLARATION_MAJOR
            : CM_HIR_METADATA_MAJOR),
        (uint16_t)(declaration ? CM_HIR_METADATA_DECLARATION_MINOR
            : (semantic ? CM_HIR_METADATA_SEMANTIC_MINOR
                : CM_HIR_METADATA_MINOR)), &envelope);
    if (codec_status != CM_HIR_METADATA_OK) {
        result.status = cm_meta_codec_status(codec_status);
        return result;
    }
    cm_hir_metadata_reader_init(&section_reader, envelope.payload,
        envelope.payload_length);
    for (index = 0u; index < ((semantic || declaration) ? 7u : 6u);
            ++index) {
        codec_status = cm_hir_metadata_read_section(&section_reader,
            &sections[index]);
        if (codec_status != CM_HIR_METADATA_OK) {
            do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT; } while(0);
            return result;
        }
    }
    if (cm_hir_metadata_read_section(&section_reader, &sections[0])
            != CM_HIR_METADATA_DONE
        || !cm_meta_section_tag_is(&sections[0], cm_meta_tag_crate)
        || !cm_meta_section_tag_is(&sections[1], cm_meta_tag_modules)
        || !cm_meta_section_tag_is(&sections[2], cm_meta_tag_generics)
        || !cm_meta_section_tag_is(&sections[3], cm_meta_tag_types)
        || !cm_meta_section_tag_is(&sections[4], cm_meta_tag_items)
        || (!declaration && !cm_meta_section_tag_is(&sections[5],
            cm_meta_tag_namespace))
        || (declaration && (!cm_meta_section_tag_is(&sections[5],
                cm_meta_tag_values)
            || !cm_meta_section_tag_is(&sections[6],
                cm_meta_tag_namespace)))
        || (semantic && !cm_meta_section_tag_is(&sections[6],
            cm_meta_tag_trait_universe))) {
        do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT; } while(0);
        return result;
    }

    cm_vec_init(&modules, sizeof(CmMetaWireModule));
    cm_vec_init(&generics, sizeof(CmMetaWireGeneric));
    cm_vec_init(&types, sizeof(CmMetaWireType));
    cm_vec_init(&items, sizeof(CmMetaWireItem));
    cm_vec_init(&entries, sizeof(CmMetaWireEntry));
    cm_vec_init(&traits, sizeof(CmMetaWireTrait));
    cm_vec_init(&impls, sizeof(CmMetaWireImpl));
    cm_vec_init(&values, sizeof(CmMetaWireValue));
    memset(&crate_name, 0, sizeof(crate_name));
    root_local = UINT32_C(0);
    item_count = UINT32_C(0);
    type_count = UINT32_C(0);
    value_count = UINT32_C(0);
    if (!cm_meta_section_count(&sections[4], CM_META_MAX_ITEMS,
            &item_count)
        || !cm_meta_section_count(&sections[3], CM_META_MAX_TYPES,
            &type_count)
        || (declaration && !cm_meta_section_count(&sections[5],
            CM_META_MAX_VALUES, &value_count))
        || !cm_meta_decode_crate(&sections[0], &crate_name, &edition)
        || !cm_meta_decode_modules(&sections[1], &modules, &root_local)
        || !cm_meta_decode_generics(&sections[2], item_count,
            declaration ? value_count : UINT32_C(0), declaration,
            &generics)
        || !cm_meta_decode_types(&sections[3], item_count, &generics,
            &types)
        || types.len != (size_t)type_count
        || !cm_meta_decode_items(&sections[4], (uint32_t)modules.len,
            (uint32_t)generics.len, type_count, &items)
        || items.len != (size_t)item_count
        || (declaration && !cm_meta_decode_values(&sections[5], type_count,
            (uint32_t)generics.len, &values))
        || (declaration && values.len != (size_t)value_count)
        || (semantic && !cm_meta_decode_trait_universe(&sections[6],
            (uint32_t)modules.len, type_count, &traits, &impls))
        || !cm_meta_decode_entries(&sections[declaration ? 6u : 5u],
            (uint32_t)modules.len,
            item_count, semantic ? (uint32_t)traits.len : UINT32_C(0),
            declaration ? (uint32_t)values.len : UINT32_C(0), &entries)
        || !cm_meta_wire_valid(&modules, &generics, &types, &items,
            declaration ? &values : NULL, &entries)
        || (semantic && !cm_meta_wire_trait_universe_canonical(&traits,
            &impls))) {
        do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT; } while(0);
        goto cleanup_wire;
    }
    {
        const CmMetaWireModule *root_wire;

        root_wire = (const CmMetaWireModule *)cm_vec_at_const(&modules,
            (size_t)(root_local - 1u));
        if (root_wire == NULL
            || !cm_meta_name_equal(crate_name, root_wire->name)) {
            do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT; } while(0);
            goto cleanup_wire;
        }
    }

    runtime_modules = (CmHirModuleId *)cm_alloc_zeroed(modules.len,
        sizeof(CmHirModuleId));
    runtime_items = (CmHirDefId *)cm_alloc_zeroed(items.len,
        sizeof(CmHirDefId));
    runtime_traits = (CmHirDefId *)cm_alloc_zeroed(traits.len,
        sizeof(CmHirDefId));
    runtime_impls = (CmHirDefId *)cm_alloc_zeroed(impls.len,
        sizeof(CmHirDefId));
    runtime_values = (CmHirDefId *)cm_alloc_zeroed(values.len,
        sizeof(CmHirDefId));
    runtime_generics = (CmHirGenericParamId *)cm_alloc_zeroed(generics.len,
        sizeof(CmHirGenericParamId));
    runtime_types = (CmHirTypeId *)cm_alloc_zeroed(types.len,
        sizeof(CmHirTypeId));
    module_created = (unsigned char *)cm_alloc_zeroed(modules.len,
        sizeof(unsigned char));
    memset(&mark, 0, sizeof(mark));
    mark_active = 0;
    owned_initialized = 0;
    candidate_initialized = 0;
    memset(&candidate_artifact, 0, sizeof(candidate_artifact));
    span.source = metadata_source;
    span.start = 0u;
    span.end = 0u;
    if (cm_hir_context_mark(context, &mark) != CM_HIR_OK) {
        do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR; } while(0);
        goto cleanup_runtime;
    }
    mark_active = 1;
    if (cm_hir_create_crate(context, cm_meta_intern_name(context,
            crate_name), edition, span, &crate_id, &root_module)
            != CM_HIR_OK) {
        do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR; } while(0);
        goto rollback;
    }
    runtime_modules[root_local - 1u] = root_module;
    module_created[root_local - 1u] = UINT8_C(1);
    {
        size_t remaining;

        remaining = modules.len - 1u;
        while (remaining != 0u) {
            size_t before;

            before = remaining;
            for (index = 0u; index < (uint32_t)modules.len; ++index) {
                const CmMetaWireModule *module;

                if (module_created[index] != 0u) continue;
                module = (const CmMetaWireModule *)cm_vec_at_const(
                    &modules, index);
                if (module == NULL || module->parent == 0u
                    || module_created[module->parent - 1u] == 0u) continue;
                if (cm_hir_add_module(context, crate_id,
                        runtime_modules[module->parent - 1u],
                        cm_meta_intern_name(context, module->name), span,
                        &runtime_modules[index]) != CM_HIR_OK) {
                    do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR; } while(0);
                    goto rollback;
                }
                module_created[index] = UINT8_C(1);
                remaining -= 1u;
            }
            if (remaining == before) {
                do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT; } while(0);
                goto rollback;
            }
        }
    }

    for (index = 0u; index < (uint32_t)items.len; ++index) {
        const CmMetaWireItem *wire_item;
        CmHirItemKind item_kind;

        wire_item = (const CmMetaWireItem *)cm_vec_at_const(&items, index);
        if (wire_item == NULL
            || !cm_meta_item_kind_from_wire(wire_item->kind, &item_kind)
            || cm_hir_reserve_item_definition_as(context, crate_id,
                item_kind, span, &runtime_items[index])
                != CM_HIR_OK) {
            do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR; } while(0);
            goto rollback;
        }
        if (wire_item->kind == CM_META_ITEM_ENUM) {
            uint32_t variant;

            for (variant = 0u;
                    variant < wire_item->data.enum_item.variant_count;
                    ++variant) {
                CmMetaWireItem *mutable_item;

                mutable_item = (CmMetaWireItem *)cm_vec_at(&items, index);
                if (mutable_item == NULL
                    || cm_hir_reserve_enum_variant_definition(context,
                        crate_id, span,
                        &mutable_item->data.enum_item.variants[variant]
                            .runtime_definition) != CM_HIR_OK) {
                    do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR; } while(0);
                    goto rollback;
                }
            }
        }
    }

    for (index = 0u; index < (uint32_t)values.len; ++index) {
        const CmMetaWireValue *wire_value;
        CmHirLibraryValueKind value_kind;
        CmHirItemKind item_kind;

        wire_value = (const CmMetaWireValue *)cm_vec_at_const(&values,
            index);
        if (wire_value == NULL
            || !cm_meta_value_kind_from_wire(wire_value->kind,
                &value_kind)) {
            do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT; } while(0);
            goto rollback;
        }
        item_kind = value_kind == CM_HIR_LIBRARY_VALUE_FUNCTION
            ? CM_HIR_ITEM_FUNCTION
            : (value_kind == CM_HIR_LIBRARY_VALUE_CONST
                ? CM_HIR_ITEM_CONST : CM_HIR_ITEM_STATIC);
        if (cm_hir_reserve_item_definition_as(context, crate_id,
                item_kind, span, &runtime_values[index]) != CM_HIR_OK) {
            do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR; } while(0);
            goto rollback;
        }
    }

    for (index = 0u; index < (uint32_t)traits.len; ++index) {
        if (cm_hir_reserve_item_definition_as(context, crate_id,
                CM_HIR_ITEM_TRAIT, span, &runtime_traits[index])
                != CM_HIR_OK) {
            do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR; } while(0);
            goto rollback;
        }
    }
    for (index = 0u; index < (uint32_t)impls.len; ++index) {
        if (cm_hir_reserve_item_definition_as(context, crate_id,
                CM_HIR_ITEM_IMPL, span, &runtime_impls[index])
                != CM_HIR_OK) {
            do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR; } while(0);
            goto rollback;
        }
    }

    for (index = 0u; index < (uint32_t)generics.len; ++index) {
        const CmMetaWireGeneric *wire_generic;
        CmHirGenericParam parameter;

        wire_generic = (const CmMetaWireGeneric *)cm_vec_at_const(&generics,
            index);
        if (wire_generic == NULL) {
            do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR; } while(0);
            goto rollback;
        }
        memset(&parameter, 0, sizeof(parameter));
        parameter.kind = wire_generic->kind == CM_META_GENERIC_LIFETIME
            ? CM_HIR_GENERIC_LIFETIME
            : (wire_generic->kind == CM_META_GENERIC_CONST
                ? CM_HIR_GENERIC_CONST : CM_HIR_GENERIC_TYPE);
        parameter.owner = wire_generic->owner_kind
                == CM_META_GENERIC_OWNER_ITEM
            ? runtime_items[wire_generic->owner - 1u]
            : runtime_values[wire_generic->owner - 1u];
        parameter.index = wire_generic->index;
        parameter.name = cm_meta_intern_name(context, wire_generic->name);
        parameter.span = span;
        parameter.declared_type = CM_HIR_TYPE_NONE;
        parameter.is_relaxed_sized = wire_generic->is_relaxed_sized;
        if (cm_hir_add_generic_param(context, &parameter,
                &runtime_generics[index]) != CM_HIR_OK) {
            do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR; } while(0);
            goto rollback;
        }
    }

    for (index = 0u; index < (uint32_t)types.len; ++index) {
        const CmMetaWireType *wire_type;

        wire_type = (const CmMetaWireType *)cm_vec_at_const(&types, index);
        if (wire_type == NULL || !cm_meta_add_runtime_type(context, wire_type,
                runtime_items, runtime_generics, runtime_types, span,
                &runtime_types[index])) {
            do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR; } while(0);
            goto rollback;
        }
    }

    for (index = 0u; index < (uint32_t)generics.len; ++index) {
        const CmMetaWireGeneric *wire_generic;

        wire_generic = (const CmMetaWireGeneric *)cm_vec_at_const(&generics,
            index);
        if (wire_generic == NULL
            || wire_generic->kind != CM_META_GENERIC_CONST) continue;
        if (cm_hir_set_generic_param_declared_type(context,
                runtime_generics[index],
                runtime_types[wire_generic->default_type - 1u])
            != CM_HIR_OK) {
            result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR;
            goto rollback;
        }
    }

    for (index = 0u; index < (uint32_t)generics.len; ++index) {
        const CmMetaWireGeneric *wire_generic;
        CmHirGenericArg argument;

        wire_generic = (const CmMetaWireGeneric *)cm_vec_at_const(&generics,
            index);
        if (wire_generic == NULL || !wire_generic->has_default) continue;
        memset(&argument, 0, sizeof(argument));
        argument.kind = CM_HIR_GENERIC_ARG_TYPE;
        argument.data.type = runtime_types[wire_generic->default_type - 1u];
        if (cm_hir_set_generic_param_default(context,
                runtime_generics[index], &argument) != CM_HIR_OK) {
            do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR; } while(0);
            goto rollback;
        }
    }

    for (index = 0u; index < (uint32_t)items.len; ++index) {
        CmMetaWireItem *wire_item;

        wire_item = (CmMetaWireItem *)cm_vec_at(&items, index);
        if (wire_item == NULL || !cm_meta_bind_runtime_item(context,
                wire_item, index + 1u, runtime_modules, runtime_items,
                runtime_generics, runtime_types, span)) {
            do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR; } while(0);
            goto rollback;
        }
    }

    for (index = 0u; index < (uint32_t)traits.len; ++index) {
        const CmMetaWireTrait *wire_trait;

        wire_trait = (const CmMetaWireTrait *)cm_vec_at_const(&traits,
            index);
        if (wire_trait == NULL || !cm_meta_bind_runtime_trait(context,
                wire_trait, index + 1u, runtime_modules, runtime_traits,
                span)) {
            do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR; } while(0);
            goto rollback;
        }
    }
    for (index = 0u; index < (uint32_t)impls.len; ++index) {
        const CmMetaWireImpl *wire_impl;

        wire_impl = (const CmMetaWireImpl *)cm_vec_at_const(&impls, index);
        if (wire_impl == NULL || !cm_meta_bind_runtime_impl(context,
                wire_impl, index + 1u, runtime_modules, runtime_traits,
                runtime_impls, runtime_types, span)) {
            do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR; } while(0);
            goto rollback;
        }
    }

    cm_hir_library_owned_data_init(&owned);
    owned_initialized = 1;
    for (index = 0u; index < (uint32_t)modules.len; ++index) {
        const CmHirModule *module;
        size_t owned_index;

        module = cm_hir_get_module(context, runtime_modules[index]);
        if (module == NULL
            || cm_hir_library_owned_data_add_module(&owned,
                module->definition, &owned_index) != CM_HIR_LIBRARY_OK
            || owned_index != (size_t)index) {
            do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR; } while(0);
            goto rollback;
        }
    }

    for (index = 0u; index < (uint32_t)values.len; ++index) {
        const CmMetaWireValue *wire_value;
        CmHirLibraryValue value;
        CmHirTypeId *parameter_types;
        uint32_t parameter;

        wire_value = (const CmMetaWireValue *)cm_vec_at_const(&values,
            index);
        memset(&value, 0, sizeof(value));
        parameter_types = NULL;
        if (wire_value == NULL
            || !cm_meta_value_kind_from_wire(wire_value->kind,
                &value.kind)) {
            do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT; } while(0);
            goto rollback;
        }
        value.definition = runtime_values[index];
        if (value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION) {
            if (wire_value->data.function.parameter_count != 0u) {
                parameter_types = (CmHirTypeId *)cm_alloc(
                    (size_t)wire_value->data.function.parameter_count
                        * sizeof(CmHirTypeId));
                for (parameter = 0u;
                        parameter < wire_value->data.function.parameter_count;
                        ++parameter) {
                    parameter_types[parameter] = runtime_types[
                        wire_value->data.function.parameter_types[parameter]
                            - 1u];
                }
            }
            value.data.function.parameter_types = parameter_types;
            value.data.function.parameter_count =
                wire_value->data.function.parameter_count;
            value.data.function.return_type = runtime_types[
                wire_value->data.function.return_type - 1u];
            value.data.function.generic_parameter_start =
                wire_value->data.function.generic_count == 0u
                    ? CM_HIR_GENERIC_PARAM_NONE
                    : runtime_generics[
                        wire_value->data.function.generic_start - 1u];
            value.data.function.generic_parameter_count =
                wire_value->data.function.generic_count;
            value.data.function.abi = cm_meta_intern_name(context,
                wire_value->data.function.abi);
            value.data.function.safety =
                (CmHirSafety)wire_value->data.function.safety;
            value.data.function.is_const = wire_value->data.function.is_const;
            value.data.function.is_async = wire_value->data.function.is_async;
            value.data.function.is_variadic =
                wire_value->data.function.is_variadic;
        } else {
            value.data.value.type = runtime_types[
                wire_value->data.value.type - 1u];
            value.data.value.mutability = cm_meta_mutability_from_wire(
                wire_value->data.value.mutability);
        }
        if (cm_hir_library_owned_data_add_value(&owned, &value)
                != CM_HIR_LIBRARY_OK) {
            cm_free(parameter_types);
            do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR; } while(0);
            goto rollback;
        }
        cm_free(parameter_types);
    }

    public_entry_count = 0u;
    for (index = 0u; index < (uint32_t)entries.len; ++index) {
        const CmMetaWireEntry *wire_entry;
        CmHirLibraryBinding binding;
        CmHirPrimitiveKind primitive;

        wire_entry = (const CmMetaWireEntry *)cm_vec_at_const(&entries,
            index);
        if (wire_entry == NULL) {
            do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR; } while(0);
            goto rollback;
        }
        memset(&binding, 0, sizeof(binding));
        binding.type_kind = CM_HIR_TYPE_ERROR_KIND;
        binding.primitive_kind = CM_HIR_PRIMITIVE_NONE;
        binding.value_kind = CM_HIR_LIBRARY_VALUE_NONE;
        if (wire_entry->kind == CM_META_BINDING_MODULE) {
            const CmHirModule *target_module;

            target_module = cm_hir_get_module(context,
                runtime_modules[wire_entry->target - 1u]);
            if (target_module == NULL) {
                do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR; } while(0);
                goto rollback;
            }
            binding.kind = CM_HIR_LIBRARY_BINDING_MODULE;
            binding.definition = target_module->definition;
        } else if (wire_entry->kind == CM_META_BINDING_TYPE) {
            const CmMetaWireItem *target_item;

            target_item = (const CmMetaWireItem *)cm_vec_at_const(&items,
                (size_t)(wire_entry->target - 1u));
            if (target_item == NULL) {
                do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR; } while(0);
                goto rollback;
            }
            binding.kind = CM_HIR_LIBRARY_BINDING_TYPE;
            binding.definition = runtime_items[wire_entry->target - 1u];
            binding.type_kind = cm_meta_item_type_kind(target_item->kind);
            public_entry_count += 1u;
        } else if (wire_entry->kind == CM_META_BINDING_TRAIT) {
            if ((size_t)wire_entry->target > traits.len) {
                do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT; } while(0);
                goto rollback;
            }
            binding.kind = CM_HIR_LIBRARY_BINDING_TRAIT;
            binding.definition = runtime_traits[wire_entry->target - 1u];
            public_entry_count += 1u;
        } else if (wire_entry->kind == CM_META_BINDING_VALUE) {
            const CmMetaWireValue *target_value;

            target_value = (const CmMetaWireValue *)cm_vec_at_const(&values,
                (size_t)(wire_entry->target - 1u));
            if (target_value == NULL
                || !cm_meta_value_kind_from_wire(target_value->kind,
                    &binding.value_kind)) {
                do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT; } while(0);
                goto rollback;
            }
            binding.kind = CM_HIR_LIBRARY_BINDING_VALUE;
            binding.definition = runtime_values[wire_entry->target - 1u];
            public_entry_count += 1u;
        } else {
            if (!cm_meta_primitive_from_wire(wire_entry->target,
                    &primitive)) {
                do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT; } while(0);
                goto rollback;
            }
            binding.kind = CM_HIR_LIBRARY_BINDING_PRIMITIVE;
            binding.definition = cm_hir_def_id_none();
            binding.primitive_kind = primitive;
            public_entry_count += 1u;
        }
        if (cm_hir_library_owned_data_add_entry(&owned,
                (size_t)(wire_entry->module - 1u), wire_entry->name.bytes,
                wire_entry->name.length, &binding) != CM_HIR_LIBRARY_OK) {
            do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR; } while(0);
            goto rollback;
        }
    }

    cm_hir_library_artifact_init(&candidate_artifact);
    candidate_initialized = 1;
    {
        const CmHirModule *root_value;
        CmHirLibraryArtifactResult restore_result;

        root_value = cm_hir_get_module(context, root_module);
        if (root_value == NULL) {
            do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR; } while(0);
            goto rollback;
        }
        restore_result = cm_hir_library_artifact_restore_owned(
            &candidate_artifact, context, crate_id, root_value->definition,
            extern_name, &owned);
        if (restore_result.status != CM_HIR_LIBRARY_OK) {
            do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR; } while(0);
            goto rollback;
        }
    }
    if (cm_hir_context_commit(context, &mark) != CM_HIR_OK) {
        do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR; } while(0);
        goto rollback;
    }
    mark_active = 0;
    {
        CmHirLibraryArtifact previous;

        previous = *artifact;
        *artifact = candidate_artifact;
        candidate_artifact.state = NULL;
        candidate_initialized = 0;
        cm_hir_library_artifact_destroy(&previous);
    }
    result.status = CM_HIR_METADATA_ARTIFACT_OK;
    result.crate_id = crate_id;
    result.root_module = root_module;
    result.module_count = modules.len;
    result.public_entry_count = public_entry_count;
    goto cleanup_decoded;

rollback:
    if (candidate_initialized)
        cm_hir_library_artifact_destroy(&candidate_artifact);
    candidate_initialized = 0;
    if (mark_active) (void)cm_hir_context_rewind(context, &mark);
    mark_active = 0;

cleanup_decoded:
    if (owned_initialized) cm_hir_library_owned_data_destroy(&owned);

cleanup_runtime:
    cm_free(module_created);
    cm_free(runtime_types);
    cm_free(runtime_generics);
    cm_free(runtime_impls);
    cm_free(runtime_values);
    cm_free(runtime_traits);
    cm_free(runtime_items);
    cm_free(runtime_modules);

cleanup_wire:
    cm_meta_wire_values_destroy(&values);
    cm_vec_destroy(&impls);
    cm_vec_destroy(&traits);
    cm_vec_destroy(&entries);
    cm_meta_wire_items_destroy(&items);
    cm_meta_wire_types_destroy(&types);
    cm_vec_destroy(&generics);
    cm_vec_destroy(&modules);
    return result;
}

CmHirMetadataArtifactResult cm_hir_metadata_decode_artifact(
    CmHirContext *context, CmHirLibraryArtifact *artifact,
    const void *encoded, size_t encoded_length, const char *extern_name,
    CmSourceId metadata_source)
{
    return cm_meta_decode_artifact(context, artifact, encoded,
        encoded_length, extern_name, metadata_source, 0, 0);
}

CmHirMetadataArtifactResult cm_hir_metadata_decode_semantic_artifact(
    CmHirContext *context, CmHirLibraryArtifact *artifact,
    const void *encoded, size_t encoded_length, const char *extern_name,
    CmSourceId metadata_source)
{
    return cm_meta_decode_artifact(context, artifact, encoded,
        encoded_length, extern_name, metadata_source, 1, 0);
}

CmHirMetadataArtifactResult cm_hir_metadata_decode_declaration_artifact(
    CmHirContext *context, CmHirLibraryArtifact *artifact,
    const void *encoded, size_t encoded_length, const char *extern_name,
    CmSourceId metadata_source)
{
    return cm_meta_decode_artifact(context, artifact, encoded,
        encoded_length, extern_name, metadata_source, 0, 1);
}

const char *cm_hir_metadata_artifact_status_name(
    CmHirMetadataArtifactStatus status)
{
    switch (status) {
    case CM_HIR_METADATA_ARTIFACT_OK: return "ok";
    case CM_HIR_METADATA_ARTIFACT_INVALID_ARGUMENT:
        return "invalid argument";
    case CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT: return "invalid format";
    case CM_HIR_METADATA_ARTIFACT_LIMIT_EXCEEDED: return "limit exceeded";
    case CM_HIR_METADATA_ARTIFACT_UNSUPPORTED_HIR: return "unsupported HIR";
    case CM_HIR_METADATA_ARTIFACT_INVALID_HIR: return "invalid HIR";
    }
    return "unknown";
}
