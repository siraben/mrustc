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
#define CM_META_MAX_NOMINAL_REFERENCES UINT32_C(131072)
#define CM_META_MAX_PREDICATES UINT32_C(131072)
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
#define CM_META_REGION_LATE_BOUND UINT8_C(3)
#define CM_META_PREDICATE_REQUIRED UINT8_C(0)
#define CM_META_PREDICATE_CONST_IF_CONST UINT8_C(1)
#define CM_META_PREDICATE_CONST UINT8_C(2)

#define CM_META_NOMINAL_TRAIT UINT8_C(1)
#define CM_META_NOMINAL_TRAIT_ALIAS UINT8_C(2)
#define CM_META_NOMINAL_ASSOCIATED_TYPE UINT8_C(3)

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
static const unsigned char cm_meta_tag_nominal_references[4] = {
    (unsigned char)'N', (unsigned char)'R',
    (unsigned char)'E', (unsigned char)'F'
};
static const unsigned char cm_meta_tag_predicates[4] = {
    (unsigned char)'P', (unsigned char)'R',
    (unsigned char)'E', (unsigned char)'D'
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

typedef struct CmMetaEncodeTypeState {
    uint32_t binder_requirement;
    uint32_t wire_local;
    uint8_t collect_state;
    uint8_t binder_state;
} CmMetaEncodeTypeState;

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

typedef struct CmMetaEncodeNominal {
    const CmHirLibraryNominalReference *reference;
    uint32_t owner;
    uint32_t parent_owner;
    CmHirLibraryPathSegment parent_name;
} CmMetaEncodeNominal;

typedef struct CmMetaEncodeNominalLookup {
    CmHirDefId definition;
    uint32_t local;
} CmMetaEncodeNominalLookup;

typedef struct CmMetaEncodeEquality {
    const CmHirAssociatedTypeEquality *equality;
    uint32_t nominal_local;
} CmMetaEncodeEquality;

typedef struct CmMetaEncodePredicate {
    const CmHirTraitPredicate *predicate;
    uint32_t nominal_local;
    uint32_t subject_local;
} CmMetaEncodePredicate;

typedef struct CmMetaEncodeOutlives {
    const CmHirOutlivesPredicate *outlives;
    uint32_t type_local;
} CmMetaEncodeOutlives;

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

typedef struct CmMetaWireNominal {
    uint8_t kind;
    uint32_t owner;
    CmMetaWireName name;
    uint32_t declaring_trait;
    uint8_t *generic_kinds;
    uint32_t generic_count;
    CmHirDefId runtime_definition;
} CmMetaWireNominal;

typedef struct CmMetaWirePredicate {
    uint32_t subject;
    uint32_t trait_reference;
    uint8_t modifier;
    CmMetaWireName *binder_names;
    uint32_t binder_count;
    uint32_t *arguments;
    uint32_t argument_count;
    uint32_t *equality_associated;
    uint32_t *equality_values;
    uint32_t equality_count;
} CmMetaWirePredicate;

typedef struct CmMetaWireValuePredicates {
    uint32_t value_local;
    uint32_t *nominal_references;
    uint32_t nominal_reference_count;
    uint32_t *availability_traits;
    uint32_t *availability_associated;
    uint32_t availability_count;
    CmMetaWirePredicate *predicates;
    uint32_t predicate_count;
    uint32_t *outlives_subjects;
    uint32_t outlives_count;
} CmMetaWireValuePredicates;

static int cm_meta_predicate_modifier_to_wire(
    CmHirTraitPredicateModifier modifier, uint8_t *wire)
{
    uint8_t value;

    if (modifier == CM_HIR_PREDICATE_REQUIRED)
        value = CM_META_PREDICATE_REQUIRED;
    else if (modifier == CM_HIR_PREDICATE_CONST_IF_CONST)
        value = CM_META_PREDICATE_CONST_IF_CONST;
    else if (modifier == CM_HIR_PREDICATE_CONST)
        value = CM_META_PREDICATE_CONST;
    else return 0;
    if (wire != NULL) *wire = value;
    return 1;
}

static int cm_meta_predicate_modifier_from_wire(uint8_t wire,
    CmHirTraitPredicateModifier *modifier)
{
    CmHirTraitPredicateModifier value;

    if (wire == CM_META_PREDICATE_REQUIRED)
        value = CM_HIR_PREDICATE_REQUIRED;
    else if (wire == CM_META_PREDICATE_CONST_IF_CONST)
        value = CM_HIR_PREDICATE_CONST_IF_CONST;
    else if (wire == CM_META_PREDICATE_CONST)
        value = CM_HIR_PREDICATE_CONST;
    else return 0;
    if (modifier != NULL) *modifier = value;
    return 1;
}

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

static int cm_meta_wire_name_compare_qsort(const void *left_ptr,
    const void *right_ptr)
{
    const CmMetaWireName *left;
    const CmMetaWireName *right;

    left = (const CmMetaWireName *)left_ptr;
    right = (const CmMetaWireName *)right_ptr;
    return cm_meta_bytes_compare(left->bytes, left->length, right->bytes,
        right->length);
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

static int cm_meta_definition_compare(CmHirDefId left, CmHirDefId right)
{
    if (left.crate_id < right.crate_id) return -1;
    if (left.crate_id > right.crate_id) return 1;
    return left.index < right.index ? -1 : (left.index > right.index ? 1 : 0);
}

static uint32_t cm_meta_nominal_local(const CmVec *nominal_lookup,
    CmHirDefId definition)
{
    size_t lower;
    size_t upper;

    lower = 0u;
    upper = nominal_lookup->len;
    while (lower < upper) {
        size_t middle;
        const CmMetaEncodeNominalLookup *entry;
        int order;

        middle = lower + (upper - lower) / 2u;
        entry = (const CmMetaEncodeNominalLookup *)cm_vec_at_const(
            nominal_lookup, middle);
        if (entry == NULL) return UINT32_C(0);
        order = cm_meta_definition_compare(entry->definition, definition);
        if (order < 0) lower = middle + 1u;
        else upper = middle;
    }
    if (lower >= nominal_lookup->len) return UINT32_C(0);
    {
        const CmMetaEncodeNominalLookup *entry;

        entry = (const CmMetaEncodeNominalLookup *)cm_vec_at_const(
            nominal_lookup, lower);
        return entry != NULL && cm_hir_def_id_equal(entry->definition,
            definition) ? entry->local : UINT32_C(0);
    }
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

static int cm_meta_encode_equality_compare(const void *left_value,
    const void *right_value)
{
    const CmMetaEncodeEquality *left;
    const CmMetaEncodeEquality *right;

    left = (const CmMetaEncodeEquality *)left_value;
    right = (const CmMetaEncodeEquality *)right_value;
    return left->nominal_local < right->nominal_local ? -1
        : (left->nominal_local > right->nominal_local ? 1 : 0);
}

static int cm_meta_encode_predicate_compare(const void *left_value,
    const void *right_value)
{
    const CmMetaEncodePredicate *left;
    const CmMetaEncodePredicate *right;

    left = (const CmMetaEncodePredicate *)left_value;
    right = (const CmMetaEncodePredicate *)right_value;
    if (left->nominal_local != right->nominal_local)
        return left->nominal_local < right->nominal_local ? -1 : 1;
    return left->subject_local < right->subject_local ? -1
        : (left->subject_local > right->subject_local ? 1 : 0);
}

static int cm_meta_encode_outlives_compare(const void *left_value,
    const void *right_value)
{
    const CmMetaEncodeOutlives *left;
    const CmMetaEncodeOutlives *right;

    left = (const CmMetaEncodeOutlives *)left_value;
    right = (const CmMetaEncodeOutlives *)right_value;
    return left->type_local < right->type_local ? -1
        : (left->type_local > right->type_local ? 1 : 0);
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

static int cm_meta_write_bytes_string(CmHirMetadataWriter *writer,
    const unsigned char *bytes, size_t length)
{
    return bytes != NULL && length != 0u
        && length <= (size_t)CM_META_MAX_STRING
        && length <= (size_t)UINT32_MAX
        && cm_hir_metadata_write_u32(writer, (uint32_t)length)
            == CM_HIR_METADATA_OK
        && cm_hir_metadata_write_bytes(writer, bytes, length)
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
    const CmVec *modules, const CmVec *nominal_lookup, CmVec *items,
    int semantic, int declaration)
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
        if (declaration && (item->kind == CM_HIR_ITEM_TRAIT
                || item->kind == CM_HIR_ITEM_TRAIT_ALIAS)
            && nominal_lookup != NULL
            && cm_meta_nominal_local(nominal_lookup,
                item->definition) != 0u)
            continue;
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

            if (item->data.trait_item.is_const
                || item->data.trait_item.supertrait_count != 0u) return 0;
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
            || item->data.impl_item.is_const
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
    const CmVec *modules, CmVec *values, int declaration_v24)
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
        /* Declaration v2.3 has no predicate wire payload. */
        if (owned_value->declaration.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
            && (owned_value->declaration.data.function.predicate_scope_count
                    != 0u
                || owned_value->declaration.data.function.predicate_count
                    != 0u
                || owned_value->declaration.data.function
                    .outlives_predicate_count != 0u
                || owned_value->declaration.data.function
                    .nominal_reference_count != 0u
                || owned_value->declaration.data.function
                    .associated_availability_count != 0u)
            && !declaration_v24) return 0;
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

static int cm_meta_nominal_compare(const void *left_value,
    const void *right_value)
{
    const CmMetaEncodeNominal *left;
    const CmMetaEncodeNominal *right;
    int names;

    left = (const CmMetaEncodeNominal *)left_value;
    right = (const CmMetaEncodeNominal *)right_value;
    if (left->reference->kind
            == CM_HIR_LIBRARY_NOMINAL_ASSOCIATED_TYPE
        || right->reference->kind
            == CM_HIR_LIBRARY_NOMINAL_ASSOCIATED_TYPE) {
        if (left->reference->kind
                != CM_HIR_LIBRARY_NOMINAL_ASSOCIATED_TYPE) return -1;
        if (right->reference->kind
                != CM_HIR_LIBRARY_NOMINAL_ASSOCIATED_TYPE) return 1;
        if (left->parent_owner < right->parent_owner) return -1;
        if (left->parent_owner > right->parent_owner) return 1;
        names = cm_meta_bytes_compare(left->parent_name.bytes,
            left->parent_name.length, right->parent_name.bytes,
            right->parent_name.length);
        if (names != 0) return names;
    } else {
        if (left->owner < right->owner) return -1;
        if (left->owner > right->owner) return 1;
        names = cm_meta_bytes_compare(left->reference->name.bytes,
            left->reference->name.length, right->reference->name.bytes,
            right->reference->name.length);
        return names;
    }
    return cm_meta_bytes_compare(left->reference->name.bytes,
        left->reference->name.length, right->reference->name.bytes,
        right->reference->name.length);
}

static int cm_meta_nominal_fact_equal(
    const CmHirLibraryNominalReference *left,
    const CmHirLibraryNominalReference *right)
{
    uint32_t index;

    if (!cm_hir_def_id_equal(left->definition, right->definition)
        || !cm_hir_def_id_equal(left->owner_module, right->owner_module)
        || left->name.length != right->name.length
        || memcmp(left->name.bytes, right->name.bytes, left->name.length) != 0
        || left->use != right->use || left->kind != right->kind
        || !cm_hir_def_id_equal(left->declaring_trait,
            right->declaring_trait)
        || left->generic_parameter_count
            != right->generic_parameter_count) return 0;
    for (index = 0u; index < left->generic_parameter_count; ++index) {
        if (left->generic_parameter_kinds[index]
                != right->generic_parameter_kinds[index]) return 0;
    }
    return 1;
}

static int cm_meta_nominal_definition_compare(const void *left_value,
    const void *right_value)
{
    const CmMetaEncodeNominal *left;
    const CmMetaEncodeNominal *right;

    left = (const CmMetaEncodeNominal *)left_value;
    right = (const CmMetaEncodeNominal *)right_value;
    return cm_meta_definition_compare(left->reference->definition,
        right->reference->definition);
}

static uint32_t cm_meta_nominal_definition_local(const CmVec *nominals,
    CmHirDefId definition)
{
    size_t lower;
    size_t upper;

    lower = 0u;
    upper = nominals->len;
    while (lower < upper) {
        size_t middle;
        const CmMetaEncodeNominal *nominal;
        int order;

        middle = lower + (upper - lower) / 2u;
        nominal = (const CmMetaEncodeNominal *)cm_vec_at_const(nominals,
            middle);
        if (nominal == NULL) return UINT32_C(0);
        order = cm_meta_definition_compare(nominal->reference->definition,
            definition);
        if (order < 0) lower = middle + 1u;
        else upper = middle;
    }
    if (lower >= nominals->len) return UINT32_C(0);
    {
        const CmMetaEncodeNominal *nominal;

        nominal = (const CmMetaEncodeNominal *)cm_vec_at_const(nominals,
            lower);
        return nominal != NULL && cm_hir_def_id_equal(
            nominal->reference->definition, definition)
            ? (uint32_t)(lower + 1u) : UINT32_C(0);
    }
}

static int cm_meta_intern_id_compare(const void *left_value,
    const void *right_value)
{
    CmInternId left;
    CmInternId right;

    left = *(const CmInternId *)left_value;
    right = *(const CmInternId *)right_value;
    return left < right ? -1 : (left > right ? 1 : 0);
}

static int cm_meta_u32_compare(const void *left, const void *right);

static const CmHirLibraryNominalReference *
cm_meta_function_nominal_reference(
    const CmHirLibraryFunctionSignature *function, CmHirDefId definition,
    uint32_t *out_index)
{
    size_t lower;
    size_t upper;

    lower = 0u;
    upper = function->nominal_reference_count;
    while (lower < upper) {
        size_t middle;
        const CmHirLibraryNominalReference *reference;
        int order;

        middle = lower + (upper - lower) / 2u;
        reference = &function->nominal_references[middle];
        order = cm_meta_definition_compare(reference->definition, definition);
        if (order < 0) lower = middle + 1u;
        else upper = middle;
    }
    if (lower >= function->nominal_reference_count
        || !cm_hir_def_id_equal(function->nominal_references[lower].definition,
            definition)) return NULL;
    if (out_index != NULL) *out_index = (uint32_t)lower;
    return &function->nominal_references[lower];
}

static int cm_meta_collect_nominals(const CmVec *values,
    const CmVec *modules, CmVec *nominals)
{
    size_t value_index;
    size_t total_generic_count;

    total_generic_count = 0u;

    for (value_index = 0u; value_index < values->len; ++value_index) {
        const CmMetaEncodeValue *value;
        const CmHirLibraryFunctionSignature *function;
        uint32_t reference_index;

        value = (const CmMetaEncodeValue *)cm_vec_at_const(values,
            value_index);
        if (value == NULL) return 0;
        if (value->value->declaration.kind
                != CM_HIR_LIBRARY_VALUE_FUNCTION) continue;
        function = &value->value->declaration.data.function;
        if (function->predicate_scope_count != 0u) return 0;
        for (reference_index = 0u;
                reference_index < function->nominal_reference_count;
                ++reference_index) {
            const CmHirLibraryNominalReference *reference;
            CmMetaEncodeNominal nominal;

            reference = &function->nominal_references[reference_index];
            if (reference->use != CM_HIR_LIBRARY_REFERENCE_ONLY
                || reference->name.bytes == NULL
                || !cm_meta_identifier_bytes_valid(reference->name.bytes,
                    reference->name.length)
                || (reference->kind != CM_HIR_LIBRARY_NOMINAL_TRAIT
                    && reference->kind
                        != CM_HIR_LIBRARY_NOMINAL_TRAIT_ALIAS
                    && reference->kind
                        != CM_HIR_LIBRARY_NOMINAL_ASSOCIATED_TYPE)
                || (reference_index != 0u
                    && cm_meta_definition_compare(function
                            ->nominal_references[reference_index - 1u]
                                .definition,
                        reference->definition) >= 0)) return 0;
            memset(&nominal, 0, sizeof(nominal));
            nominal.reference = reference;
            nominal.owner = cm_meta_module_local(modules,
                reference->owner_module);
            if (nominal.owner == 0u) return 0;
            (void)cm_vec_push(nominals, &nominal);
            if (nominals->len > (size_t)CM_META_MAX_NOMINAL_REFERENCES)
                return 0;
        }
        for (reference_index = 0u;
                reference_index < function->predicate_count;
                ++reference_index) {
            const CmHirTraitPredicate *predicate;
            const CmHirLibraryNominalReference *direct;
            uint32_t child;

            predicate = &function->predicates[reference_index];
            direct = cm_meta_function_nominal_reference(function,
                predicate->trait_type.definition, NULL);
            if (direct == NULL
                || (direct->kind != CM_HIR_LIBRARY_NOMINAL_TRAIT
                    && direct->kind
                        != CM_HIR_LIBRARY_NOMINAL_TRAIT_ALIAS)
                || (direct->kind == CM_HIR_LIBRARY_NOMINAL_TRAIT_ALIAS
                    && predicate->equality_count != 0u)
                || predicate->scope != CM_HIR_PREDICATE_SCOPE_NONE
                || !cm_meta_predicate_modifier_to_wire(
                    predicate->modifier, NULL)
                || predicate->trait_type.argument_count
                    != direct->generic_parameter_count) return 0;
            for (child = 0u; child < direct->generic_parameter_count;
                    ++child) {
                if (direct->generic_parameter_kinds[child]
                        != CM_HIR_GENERIC_TYPE) return 0;
            }
            if (predicate->binder.lifetime_count > 1u) {
                CmInternId *binder_names;

                binder_names = (CmInternId *)cm_alloc((size_t)predicate
                    ->binder.lifetime_count * sizeof(CmInternId));
                memcpy(binder_names, predicate->binder.lifetimes,
                    (size_t)predicate->binder.lifetime_count
                        * sizeof(CmInternId));
                qsort(binder_names, predicate->binder.lifetime_count,
                    sizeof(CmInternId), cm_meta_intern_id_compare);
                for (child = 1u; child < predicate->binder.lifetime_count;
                        ++child) {
                    if (binder_names[child - 1u] == binder_names[child]) {
                        cm_free(binder_names);
                        return 0;
                    }
                }
                cm_free(binder_names);
            }
            {
                uint32_t *equality_references;

                equality_references = predicate->equality_count == 0u ? NULL
                    : (uint32_t *)cm_alloc((size_t)predicate->equality_count
                        * sizeof(uint32_t));
            for (child = 0u; child < predicate->equality_count; ++child) {
                const CmHirLibraryNominalReference *associated;
                uint32_t function_reference;

                associated = cm_meta_function_nominal_reference(function,
                    predicate->equalities[child].associated_type,
                    &function_reference);
                if (associated == NULL || associated->kind
                        != CM_HIR_LIBRARY_NOMINAL_ASSOCIATED_TYPE
                    || associated->generic_parameter_count != 0u) {
                    cm_free(equality_references);
                    return 0;
                }
                equality_references[child] = function_reference;
            }
                if (predicate->equality_count > 1u) {
                    qsort(equality_references, predicate->equality_count,
                        sizeof(uint32_t), cm_meta_u32_compare);
                    for (child = 1u; child < predicate->equality_count;
                            ++child) {
                        if (equality_references[child - 1u]
                                == equality_references[child]) {
                            cm_free(equality_references);
                            return 0;
                        }
                    }
                }
                cm_free(equality_references);
            }
        }
    }
    if (nominals->len > 1u) qsort(nominals->data, nominals->len,
        sizeof(CmMetaEncodeNominal), cm_meta_nominal_definition_compare);
    if (nominals->len > 1u) {
        size_t read_index;
        size_t write_index;

        write_index = 1u;
        for (read_index = 1u; read_index < nominals->len; ++read_index) {
            CmMetaEncodeNominal *prior;
            CmMetaEncodeNominal *current;

            prior = (CmMetaEncodeNominal *)cm_vec_at(nominals,
                write_index - 1u);
            current = (CmMetaEncodeNominal *)cm_vec_at(nominals, read_index);
            if (prior == NULL || current == NULL) return 0;
            if (cm_hir_def_id_equal(prior->reference->definition,
                    current->reference->definition)) {
                if (!cm_meta_nominal_fact_equal(prior->reference,
                        current->reference)) return 0;
                continue;
            }
            if (write_index != read_index) {
                CmMetaEncodeNominal *target;

                target = (CmMetaEncodeNominal *)cm_vec_at(nominals,
                    write_index);
                if (target == NULL) return 0;
                *target = *current;
            }
            write_index += 1u;
        }
        nominals->len = write_index;
    }
    for (value_index = 0u; value_index < nominals->len; ++value_index) {
        CmMetaEncodeNominal *nominal;

        nominal = (CmMetaEncodeNominal *)cm_vec_at(nominals, value_index);
        if (nominal != NULL && nominal->reference->kind
                == CM_HIR_LIBRARY_NOMINAL_ASSOCIATED_TYPE) {
            uint32_t parent_local;
            const CmMetaEncodeNominal *parent;

            parent_local = cm_meta_nominal_definition_local(nominals,
                nominal->reference->declaring_trait);
            parent = parent_local == 0u ? NULL
                : (const CmMetaEncodeNominal *)cm_vec_at_const(nominals,
                    parent_local - 1u);
            if (parent == NULL || parent->reference->kind
                    != CM_HIR_LIBRARY_NOMINAL_TRAIT
                || nominal->owner != parent->owner) return 0;
            nominal->parent_owner = parent->owner;
            nominal->parent_name = parent->reference->name;
        }
    }
    if (nominals->len > 1u) qsort(nominals->data, nominals->len,
        sizeof(CmMetaEncodeNominal), cm_meta_nominal_compare);
    for (value_index = 0u; value_index < nominals->len; ++value_index) {
        const CmMetaEncodeNominal *nominal;
        const CmHirLibraryNominalReference *reference;
        uint32_t index;

        nominal = (const CmMetaEncodeNominal *)cm_vec_at_const(nominals,
            value_index);
        reference = nominal == NULL ? NULL : nominal->reference;
        if (value_index != 0u) {
            const CmMetaEncodeNominal *prior;

            prior = (const CmMetaEncodeNominal *)cm_vec_at_const(nominals,
                value_index - 1u);
            if (prior == NULL || cm_meta_nominal_compare(prior, nominal) >= 0)
                return 0;
        }
        if (reference == NULL || (reference->kind
                == CM_HIR_LIBRARY_NOMINAL_ASSOCIATED_TYPE
                ? nominal->parent_owner == 0u
                : !cm_hir_def_id_is_none(reference->declaring_trait))) {
            return 0;
        }
        for (index = 0u; index < reference->generic_parameter_count;
                ++index) {
            if ((unsigned int)reference->generic_parameter_kinds[index]
                    > (unsigned int)CM_HIR_GENERIC_CONST) return 0;
        }
        if (!cm_size_add(total_generic_count,
                (size_t)reference->generic_parameter_count,
                &total_generic_count)
            || total_generic_count > (size_t)CM_META_MAX_GENERICS) return 0;
    }
    return 1;
}

static int cm_meta_nominal_lookup_compare(const void *left_value,
    const void *right_value)
{
    const CmMetaEncodeNominalLookup *left;
    const CmMetaEncodeNominalLookup *right;

    left = (const CmMetaEncodeNominalLookup *)left_value;
    right = (const CmMetaEncodeNominalLookup *)right_value;
    return cm_meta_definition_compare(left->definition, right->definition);
}

static int cm_meta_build_nominal_lookup(const CmVec *nominals,
    CmVec *lookup)
{
    size_t index;

    for (index = 0u; index < nominals->len; ++index) {
        const CmMetaEncodeNominal *nominal;
        CmMetaEncodeNominalLookup entry;

        nominal = (const CmMetaEncodeNominal *)cm_vec_at_const(nominals,
            index);
        if (nominal == NULL || index >= (size_t)UINT32_MAX) return 0;
        entry.definition = nominal->reference->definition;
        entry.local = (uint32_t)(index + 1u);
        (void)cm_vec_push(lookup, &entry);
    }
    if (lookup->len > 1u) qsort(lookup->data, lookup->len,
        sizeof(CmMetaEncodeNominalLookup), cm_meta_nominal_lookup_compare);
    for (index = 1u; index < lookup->len; ++index) {
        const CmMetaEncodeNominalLookup *prior;
        const CmMetaEncodeNominalLookup *entry;

        prior = (const CmMetaEncodeNominalLookup *)cm_vec_at_const(lookup,
            index - 1u);
        entry = (const CmMetaEncodeNominalLookup *)cm_vec_at_const(lookup,
            index);
        if (prior == NULL || entry == NULL
            || cm_meta_definition_compare(prior->definition,
                entry->definition) >= 0) return 0;
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

static uint32_t cm_meta_type_local(const uint32_t *type_locals,
    size_t type_local_count, CmHirTypeId id)
{
    return id == CM_HIR_TYPE_NONE || (size_t)id > type_local_count
        ? UINT32_C(0) : type_locals[id - 1u];
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
    const CmVec *generics, uint32_t late_bound_count)
{
    const CmMetaEncodeGeneric *generic;
    uint32_t local;

    if (region->kind == CM_HIR_REGION_STATIC) return 1;
    if (region->kind == CM_HIR_REGION_LATE_BOUND)
        return region->data.binder_index < late_bound_count;
    if (region->kind != CM_HIR_REGION_EARLY_BOUND) return 0;
    local = cm_meta_generic_local(generics, region->data.parameter);
    generic = local == 0u ? NULL
        : (const CmMetaEncodeGeneric *)cm_vec_at_const(generics,
            (size_t)(local - 1u));
    return generic != NULL
        && generic->parameter->kind == CM_HIR_GENERIC_LIFETIME;
}

static int cm_meta_type_late_bound_requirement_cached(
    const CmHirContext *context, CmHirTypeId id, size_t depth,
    CmMetaEncodeTypeState *states,
    uint32_t *out_requirement)
{
    const CmHirType *type;

    (void)depth;
    (void)states;
    if (context == NULL || out_requirement == NULL) return 0;
    type = cm_hir_get_type(context, id);
    if (type == NULL) return 0;
    *out_requirement = type->late_bound_requirement;
    return 1;
}

static int cm_meta_function_type_generics_supported_cached(
    const CmHirContext *context, CmHirTypeId id, CmHirDefId owner,
    CmHirGenericParamId start, uint32_t count, size_t depth,
    int predicate_root, uint32_t *marks, uint32_t generation)
{
    const CmHirType *type;
    uint32_t index;

    if (id == CM_HIR_TYPE_NONE
        || depth >= (size_t)CM_META_MAX_TYPE_NESTING) return 0;
    if ((size_t)id > context->types.len) return 0;
    if (marks[id - 1u] == generation) return 1;
    marks[id - 1u] = generation;
    type = cm_hir_get_type(context, id);
    if (type == NULL) return 0;
#define CM_META_PREDICATE_CHILD(child_id) \
    cm_meta_function_type_generics_supported_cached(context, (child_id), \
        owner, start, count, depth + 1u, predicate_root, marks, generation)
#define CM_META_FUNCTION_GENERIC(parameter_id, expected_kind) do { \
        const CmHirGenericParam *owned_parameter; \
        CmHirGenericParamId owned_id; \
        owned_id = (parameter_id); \
        owned_parameter = cm_hir_get_generic_param(context, owned_id); \
        if (owned_parameter == NULL \
            || owned_parameter->kind != (expected_kind) \
            || !cm_hir_def_id_equal(owned_parameter->owner, owner) \
            || count == 0u || owned_id < start \
            || owned_id - start >= count \
            || owned_parameter->index != owned_id - start) return 0; \
    } while (0)
    if (type->kind == CM_HIR_TYPE_PARAMETER_KIND) {
        const CmHirGenericParam *parameter;
        CmHirGenericParamId parameter_id;

        parameter_id = type->data.parameter_type.parameter;
        parameter = cm_hir_get_generic_param(context, parameter_id);
        return parameter != NULL && parameter->kind == CM_HIR_GENERIC_TYPE
            && cm_hir_def_id_equal(parameter->owner, owner)
            && count != 0u && parameter_id >= start
            && parameter_id - start < count
            && parameter->index == parameter_id - start;
    }
    if (type->kind == CM_HIR_TYPE_REFERENCE_KIND) {
        if (predicate_root && type->data.reference_type.region.kind
                == CM_HIR_REGION_EARLY_BOUND) return 0;
        if (type->data.reference_type.region.kind
                == CM_HIR_REGION_EARLY_BOUND) {
            CM_META_FUNCTION_GENERIC(
                type->data.reference_type.region.data.parameter,
                CM_HIR_GENERIC_LIFETIME);
        }
        return CM_META_PREDICATE_CHILD(type->data.reference_type.pointee);
    }
    if (type->kind == CM_HIR_TYPE_RAW_POINTER_KIND)
        return CM_META_PREDICATE_CHILD(type->data.raw_pointer_type.pointee);
    if (type->kind == CM_HIR_TYPE_TUPLE_KIND) {
        for (index = 0u; index < type->data.tuple_type.element_count; ++index)
            if (!CM_META_PREDICATE_CHILD(
                    type->data.tuple_type.elements[index])) return 0;
    } else if (type->kind == CM_HIR_TYPE_ARRAY_KIND) {
        if (predicate_root && type->data.array_type.length.kind
                == CM_HIR_CONST_PARAMETER) return 0;
        if (type->data.array_type.length.kind == CM_HIR_CONST_PARAMETER) {
            CM_META_FUNCTION_GENERIC(
                type->data.array_type.length.data.parameter,
                CM_HIR_GENERIC_CONST);
        }
        if (!CM_META_PREDICATE_CHILD(type->data.array_type.element)
            || !CM_META_PREDICATE_CHILD(type->data.array_type.length.type))
            return 0;
    } else if (type->kind == CM_HIR_TYPE_SLICE_KIND) {
        if (!CM_META_PREDICATE_CHILD(type->data.slice_type.element)) return 0;
    } else if (type->kind == CM_HIR_TYPE_FN_POINTER_KIND) {
        for (index = 0u;
             index < type->data.fn_pointer_type.parameter_count; ++index) {
            if (!CM_META_PREDICATE_CHILD(
                    type->data.fn_pointer_type.parameters[index])) return 0;
        }
        if (!CM_META_PREDICATE_CHILD(
                type->data.fn_pointer_type.return_type)) return 0;
    } else if (type->kind == CM_HIR_TYPE_ADT_KIND
            || type->kind == CM_HIR_TYPE_ALIAS_APPLICATION_KIND
            || type->kind == CM_HIR_TYPE_FOREIGN_KIND) {
        for (index = 0u; index < type->data.named_type.argument_count;
                ++index) {
            const CmHirGenericArg *argument;

            argument = &type->data.named_type.arguments[index];
            if (predicate_root
                && ((argument->kind == CM_HIR_GENERIC_ARG_LIFETIME
                    && argument->data.lifetime.kind
                        == CM_HIR_REGION_EARLY_BOUND)
                || (argument->kind == CM_HIR_GENERIC_ARG_CONST
                    && argument->data.constant.kind
                        == CM_HIR_CONST_PARAMETER)))
                return 0;
            if (argument->kind == CM_HIR_GENERIC_ARG_LIFETIME
                && argument->data.lifetime.kind == CM_HIR_REGION_EARLY_BOUND) {
                CM_META_FUNCTION_GENERIC(argument->data.lifetime.data.parameter,
                    CM_HIR_GENERIC_LIFETIME);
            }
            if (argument->kind == CM_HIR_GENERIC_ARG_CONST
                && argument->data.constant.kind == CM_HIR_CONST_PARAMETER) {
                CM_META_FUNCTION_GENERIC(argument->data.constant.data.parameter,
                    CM_HIR_GENERIC_CONST);
            }
            if (argument->kind == CM_HIR_GENERIC_ARG_TYPE
                && !CM_META_PREDICATE_CHILD(argument->data.type)) return 0;
            if (argument->kind == CM_HIR_GENERIC_ARG_CONST
                && !CM_META_PREDICATE_CHILD(argument->data.constant.type))
                return 0;
        }
    }
#undef CM_META_PREDICATE_CHILD
#undef CM_META_FUNCTION_GENERIC
    return 1;
}

static int cm_meta_collect_type(const CmHirLibraryArtifactIdentity *identity,
    const CmVec *items, const CmVec *generics, CmVec *types,
    CmMetaEncodeTypeState *states, CmHirTypeId id,
    uint32_t late_bound_count);

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
    const CmVec *generics, CmVec *types, CmMetaEncodeTypeState *states,
    const CmHirConstArg *constant, CmHirTypeId expected_type,
    uint32_t late_bound_count)
{
    const CmHirGenericParam *parameter;

    if (constant == NULL
        || (constant->kind != CM_HIR_CONST_VALUE
            && constant->kind != CM_HIR_CONST_PARAMETER)
        || !cm_meta_scalar_const_type_equal(identity->context,
            constant->type, expected_type)
        || !cm_meta_collect_type(identity, items, generics, types, states,
            constant->type, late_bound_count)) return 0;
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
    const CmVec *generics, CmVec *types, CmMetaEncodeTypeState *states,
    const CmHirNamedType *named, uint8_t expected_item_kind,
    uint32_t late_bound_count)
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
                    generics, late_bound_count)) return 0;
        } else if (argument->kind == CM_HIR_GENERIC_ARG_TYPE) {
            if (parameter->kind != CM_HIR_GENERIC_TYPE
                || !cm_meta_collect_type(identity, items, generics, types,
                    states, argument->data.type, late_bound_count)) return 0;
        } else if (argument->kind == CM_HIR_GENERIC_ARG_CONST) {
            if (parameter->kind != CM_HIR_GENERIC_CONST
                || !cm_meta_collect_const(identity, items, generics, types,
                    states, &argument->data.constant,
                    parameter->declared_type, late_bound_count)) return 0;
        } else {
            return 0;
        }
    }
    return 1;
}

static int cm_meta_collect_type(const CmHirLibraryArtifactIdentity *identity,
    const CmVec *items, const CmVec *generics, CmVec *types,
    CmMetaEncodeTypeState *states, CmHirTypeId id,
    uint32_t late_bound_count)
{
    const CmHirType *type;
    CmMetaEncodeType encoded;
    size_t state_index;
    uint32_t index;
    uint8_t named_kind;
    uint32_t required_binders;

    if (id == CM_HIR_TYPE_NONE || (size_t)id > identity->context->types.len)
        return 0;
    state_index = (size_t)id - 1u;
    if (!cm_meta_type_late_bound_requirement_cached(identity->context, id,
            0u, states, &required_binders)
        || required_binders > late_bound_count) return 0;
    if (states[state_index].wire_local != 0u) return 1;
    if (states[state_index].collect_state != 0u) return 0;
    states[state_index].collect_state = UINT8_C(1);
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
                generics, late_bound_count)
            || (type->data.reference_type.mutability != CM_HIR_IMMUTABLE
                && type->data.reference_type.mutability != CM_HIR_MUTABLE)
            || !cm_meta_collect_type(identity, items, generics, types, states,
                type->data.reference_type.pointee, late_bound_count)) return 0;
        break;
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        if ((type->data.raw_pointer_type.mutability != CM_HIR_IMMUTABLE
                && type->data.raw_pointer_type.mutability != CM_HIR_MUTABLE)
            || !cm_meta_collect_type(identity, items, generics, types, states,
                type->data.raw_pointer_type.pointee, late_bound_count)) return 0;
        break;
    case CM_HIR_TYPE_TUPLE_KIND:
        if (type->data.tuple_type.element_count != 0u
            && type->data.tuple_type.elements == NULL) return 0;
        for (index = 0u; index < type->data.tuple_type.element_count;
                ++index) {
            if (!cm_meta_collect_type(identity, items, generics, types, states,
                    type->data.tuple_type.elements[index], late_bound_count))
                return 0;
        }
        break;
    case CM_HIR_TYPE_ARRAY_KIND:
        if (!cm_meta_collect_type(identity, items, generics, types, states,
                type->data.array_type.element, late_bound_count)
            || !cm_meta_collect_const(identity, items, generics, types,
                states, &type->data.array_type.length,
                type->data.array_type.length.type, late_bound_count)) return 0;
        break;
    case CM_HIR_TYPE_SLICE_KIND:
        if (!cm_meta_collect_type(identity, items, generics, types, states,
                type->data.slice_type.element, late_bound_count)) return 0;
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
                &type->data.named_type, named_kind, late_bound_count)) return 0;
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
    states[state_index].wire_local = (uint32_t)types->len;
    states[state_index].collect_state = UINT8_C(2);
    return 1;
}

static int cm_meta_collect_types(
    const CmHirLibraryArtifactIdentity *identity, const CmVec *items,
    const CmVec *generics, const CmVec *impls, const CmVec *values,
    const CmVec *nominal_lookup, CmVec *types, uint32_t **out_type_locals)
{
    CmMetaEncodeTypeState *states;
    uint32_t *provenance_marks;
    uint32_t provenance_generation;
    size_t index;
    int valid;

    if (out_type_locals == NULL) return 0;
    *out_type_locals = NULL;
    states = (CmMetaEncodeTypeState *)cm_alloc_zeroed(
        identity->context->types.len, sizeof(CmMetaEncodeTypeState));
    provenance_marks = (uint32_t *)cm_alloc_zeroed(
        identity->context->types.len, sizeof(uint32_t));
    provenance_generation = 0u;
    valid = 1;
    for (index = 0u; valid && index < generics->len; ++index) {
        const CmMetaEncodeGeneric *generic;

        generic = (const CmMetaEncodeGeneric *)cm_vec_at_const(generics,
            index);
        if (generic == NULL) valid = 0;
        else if (generic->parameter->has_default) {
            valid = cm_meta_collect_type(identity, items, generics, types,
                states, generic->parameter->default_argument.data.type, 0u);
        } else if (generic->parameter->kind == CM_HIR_GENERIC_CONST) {
            valid = cm_meta_collect_type(identity, items, generics, types,
                states, generic->parameter->declared_type, 0u);
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
                    states, item->data.aggregate_item.fields[child].type, 0u);
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
                        types, states, variant->fields[field].type, 0u);
                }
                if (valid && variant->has_discriminant) {
                    valid = variant->discriminant.kind == CM_HIR_CONST_VALUE
                        && cm_meta_collect_type(identity, items, generics,
                            types, states, variant->discriminant.type, 0u);
                }
            }
        } else if (item->kind == CM_HIR_ITEM_TYPE_ALIAS) {
            valid = cm_meta_collect_type(identity, items, generics, types,
                states, item->data.type_alias_item.target, 0u);
        }
    }
    for (index = 0u; valid && impls != NULL && index < impls->len; ++index) {
        const CmMetaEncodeImpl *impl_value;

        impl_value = (const CmMetaEncodeImpl *)cm_vec_at_const(impls,
            index);
        valid = impl_value != NULL
            && cm_meta_collect_type(identity, items, generics, types,
                states, impl_value->item->data.impl_item.self_type, 0u);
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
            uint32_t predicate_generation;
            uint32_t signature_generation;

            signature_generation = ++provenance_generation;
            predicate_generation = ++provenance_generation;
            for (parameter = 0u; valid
                    && parameter < value->data.function.parameter_count;
                    ++parameter) {
                valid = cm_meta_collect_type(identity, items, generics,
                    types, states,
                    value->data.function.parameter_types[parameter], 0u);
                if (valid) valid =
                    cm_meta_function_type_generics_supported_cached(
                    identity->context,
                    value->data.function.parameter_types[parameter],
                    value->definition,
                    value->data.function.generic_parameter_start,
                    value->data.function.generic_parameter_count, 0u, 0,
                    provenance_marks, signature_generation);
            }
            if (valid) valid = cm_meta_collect_type(identity, items,
                generics, types, states,
                value->data.function.return_type, 0u);
            if (valid) valid = cm_meta_function_type_generics_supported_cached(
                identity->context, value->data.function.return_type,
                value->definition,
                value->data.function.generic_parameter_start,
                value->data.function.generic_parameter_count, 0u, 0,
                provenance_marks, signature_generation);
            {
                CmMetaEncodePredicate *predicates;

                predicates = value->data.function.predicate_count == 0u
                    ? NULL : (CmMetaEncodePredicate *)cm_alloc(
                        (size_t)value->data.function.predicate_count
                            * sizeof(CmMetaEncodePredicate));
                for (parameter = 0u;
                        parameter < value->data.function.predicate_count;
                        ++parameter) {
                    const CmHirType *subject_type;

                    predicates[parameter].predicate =
                        &value->data.function.predicates[parameter];
                    predicates[parameter].nominal_local =
                        cm_meta_nominal_local(nominal_lookup,
                            predicates[parameter]
                            .predicate->trait_type.definition);
                    subject_type = cm_hir_get_type(identity->context,
                        predicates[parameter].predicate->subject);
                    predicates[parameter].subject_local = subject_type != NULL
                            && subject_type->kind
                                == CM_HIR_TYPE_PARAMETER_KIND
                        ? cm_meta_generic_local(generics,
                            subject_type->data.parameter_type.parameter)
                        : 0u;
                    if (predicates[parameter].nominal_local == 0u
                        || predicates[parameter].subject_local == 0u)
                        valid = 0;
                }
                if (value->data.function.predicate_count > 1u)
                    qsort(predicates, value->data.function.predicate_count,
                        sizeof(CmMetaEncodePredicate),
                        cm_meta_encode_predicate_compare);
            for (parameter = 0u; valid
                    && parameter < value->data.function.predicate_count;
                    ++parameter) {
                const CmHirTraitPredicate *predicate;
                uint32_t argument;

                predicate = predicates[parameter].predicate;
                if ((parameter != 0u
                        && cm_meta_encode_predicate_compare(
                            &predicates[parameter - 1u],
                            &predicates[parameter]) >= 0)
                    || predicate->scope != CM_HIR_PREDICATE_SCOPE_NONE
                    || !cm_meta_predicate_modifier_to_wire(
                        predicate->modifier, NULL)) {
                    valid = 0;
                    break;
                }
                valid = cm_meta_collect_type(identity, items, generics,
                    types, states, predicate->subject, 0u);
                if (valid) valid =
                    cm_meta_function_type_generics_supported_cached(
                    identity->context, predicate->subject,
                    value->definition,
                    value->data.function.generic_parameter_start,
                    value->data.function.generic_parameter_count, 0u, 1,
                    provenance_marks, predicate_generation);
                for (argument = 0u; valid
                        && argument < predicate->trait_type.argument_count;
                        ++argument) {
                    if (predicate->trait_type.arguments[argument].kind
                            != CM_HIR_GENERIC_ARG_TYPE) valid = 0;
                    else valid = cm_meta_collect_type(identity, items,
                        generics, types, states,
                        predicate->trait_type.arguments[argument].data.type,
                        predicate->binder.lifetime_count);
                    if (valid) valid =
                        cm_meta_function_type_generics_supported_cached(
                            identity->context,
                            predicate->trait_type.arguments[argument]
                                .data.type,
                            value->definition,
                            value->data.function.generic_parameter_start,
                            value->data.function.generic_parameter_count, 0u,
                            1, provenance_marks, predicate_generation);
                }
                {
                    CmMetaEncodeEquality *equalities;
                    uint32_t position;

                    equalities = predicate->equality_count == 0u ? NULL
                        : (CmMetaEncodeEquality *)cm_alloc(
                            (size_t)predicate->equality_count
                                * sizeof(CmMetaEncodeEquality));
                    for (position = 0u; position < predicate->equality_count;
                            ++position) {
                        equalities[position].equality =
                            &predicate->equalities[position];
                        equalities[position].nominal_local =
                            cm_meta_nominal_local(nominal_lookup,
                                predicate->equalities[position]
                                    .associated_type);
                        if (equalities[position].nominal_local == 0u)
                            valid = 0;
                    }
                    if (predicate->equality_count > 1u)
                        qsort(equalities, predicate->equality_count,
                            sizeof(CmMetaEncodeEquality),
                            cm_meta_encode_equality_compare);
                    for (position = 0u; valid
                            && position < predicate->equality_count;
                            ++position) {
                        const CmHirAssociatedTypeEquality *selected;

                        selected = equalities[position].equality;
                        if (position != 0u
                            && equalities[position - 1u].nominal_local
                                >= equalities[position].nominal_local) {
                            valid = 0;
                            break;
                        }
                        valid = cm_meta_collect_type(identity, items,
                            generics, types, states, selected->value,
                            predicate->binder.lifetime_count)
                            && cm_meta_function_type_generics_supported_cached(
                                identity->context, selected->value,
                                value->definition,
                                value->data.function.generic_parameter_start,
                                value->data.function.generic_parameter_count,
                                0u, 1, provenance_marks,
                                predicate_generation);
                    }
                    cm_free(equalities);
                }
            }
                cm_free(predicates);
            }
            {
                CmMetaEncodeOutlives *outlives_values;

                outlives_values = value->data.function
                        .outlives_predicate_count == 0u ? NULL
                    : (CmMetaEncodeOutlives *)cm_alloc((size_t)value->data
                        .function.outlives_predicate_count
                        * sizeof(CmMetaEncodeOutlives));
                for (parameter = 0u; parameter < value->data.function
                        .outlives_predicate_count; ++parameter) {
                    CmHirTypeId subject;
                    const CmHirType *subject_type;

                    outlives_values[parameter].outlives = &value->data
                        .function.outlives_predicates[parameter];
                    subject = outlives_values[parameter].outlives
                            ->subject_kind == CM_HIR_OUTLIVES_TYPE
                        ? outlives_values[parameter].outlives->subject.type
                        : CM_HIR_TYPE_NONE;
                    subject_type = cm_hir_get_type(identity->context,
                        subject);
                    outlives_values[parameter].type_local = subject_type
                                == NULL || subject_type->kind
                                    != CM_HIR_TYPE_PARAMETER_KIND
                        ? UINT32_C(0) : cm_meta_generic_local(generics,
                            subject_type->data.parameter_type.parameter);
                    if (outlives_values[parameter].type_local == 0u)
                        valid = 0;
                }
                if (value->data.function.outlives_predicate_count > 1u)
                    qsort(outlives_values, value->data.function
                        .outlives_predicate_count,
                        sizeof(CmMetaEncodeOutlives),
                        cm_meta_encode_outlives_compare);
            for (parameter = 0u; valid && parameter
                    < value->data.function.outlives_predicate_count;
                    ++parameter) {
                const CmHirOutlivesPredicate *outlives;

                outlives = outlives_values[parameter].outlives;
                valid = (parameter == 0u
                        || outlives_values[parameter - 1u].type_local
                            < outlives_values[parameter].type_local)
                    && outlives->scope == CM_HIR_PREDICATE_SCOPE_NONE
                    && outlives->subject_kind == CM_HIR_OUTLIVES_TYPE
                    && outlives->bound.kind == CM_HIR_REGION_STATIC
                    && outlives_values[parameter].type_local != 0u
                    && cm_meta_collect_type(identity, items, generics,
                        types, states, outlives->subject.type, 0u);
                if (valid) valid =
                    cm_meta_function_type_generics_supported_cached(
                    identity->context, outlives->subject.type,
                    value->definition,
                    value->data.function.generic_parameter_start,
                    value->data.function.generic_parameter_count, 0u, 1,
                    provenance_marks, predicate_generation);
            }
                cm_free(outlives_values);
            }
        } else {
            valid = cm_meta_collect_type(identity, items, generics, types,
                states, value->data.value.type, 0u);
        }
    }
    if (valid) {
        uint32_t *type_locals;

        type_locals = (uint32_t *)cm_alloc(identity->context->types.len
            * sizeof(uint32_t));
        for (index = 0u; index < identity->context->types.len; ++index)
            type_locals[index] = states[index].wire_local;
        *out_type_locals = type_locals;
    }
    cm_free(provenance_marks);
    cm_free(states);
    return valid;
}

static int cm_meta_encode_alias_acyclic(
    const CmHirLibraryArtifactIdentity *identity, uint32_t item_local,
    const CmVec *items, unsigned char *item_states,
    uint32_t *item_heights, unsigned char *type_states,
    uint32_t *type_heights, size_t traversal_depth);

static int cm_meta_encode_alias_type_acyclic(
    const CmHirLibraryArtifactIdentity *identity, CmHirTypeId type_id,
    const CmVec *items, unsigned char *item_states,
    uint32_t *item_heights, unsigned char *type_states,
    uint32_t *type_heights, size_t traversal_depth)
{
    const CmHirType *type;
    size_t state_index;
    uint32_t height;
    uint32_t index;

    if (type_id == CM_HIR_TYPE_NONE
        || (size_t)type_id > identity->context->types.len
        || traversal_depth >= (size_t)CM_META_MAX_TYPE_NESTING) return 0;
    state_index = (size_t)type_id - 1u;
    if (type_states[state_index] == UINT8_C(1)) return 0;
    if (type_states[state_index] == UINT8_C(2)) {
        return (size_t)type_heights[state_index]
            < (size_t)CM_META_MAX_TYPE_NESTING - traversal_depth;
    }
    type_states[state_index] = UINT8_C(1);
    type = cm_hir_get_type(identity->context, type_id);
    if (type == NULL) return 0;
    height = 0u;
#define CM_META_ENCODE_ALIAS_CHILD(child_id) do { \
        CmHirTypeId cm_child_id; \
        uint32_t cm_child_height; \
        cm_child_id = (child_id); \
        if (!cm_meta_encode_alias_type_acyclic(identity, cm_child_id, items, \
                item_states, item_heights, type_states, type_heights, \
                traversal_depth + 1u)) \
            return 0; \
        cm_child_height = type_heights[(size_t)cm_child_id - 1u]; \
        if (cm_child_height == UINT32_MAX) return 0; \
        cm_child_height += 1u; \
        if (cm_child_height > height) height = cm_child_height; \
    } while (0)
    switch (type->kind) {
    case CM_HIR_TYPE_REFERENCE_KIND:
        CM_META_ENCODE_ALIAS_CHILD(type->data.reference_type.pointee);
        break;
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        CM_META_ENCODE_ALIAS_CHILD(type->data.raw_pointer_type.pointee);
        break;
    case CM_HIR_TYPE_TUPLE_KIND:
        for (index = 0u; index < type->data.tuple_type.element_count;
                ++index) {
            CM_META_ENCODE_ALIAS_CHILD(type->data.tuple_type.elements[index]);
        }
        break;
    case CM_HIR_TYPE_ARRAY_KIND:
        CM_META_ENCODE_ALIAS_CHILD(type->data.array_type.element);
        CM_META_ENCODE_ALIAS_CHILD(type->data.array_type.length.type);
        break;
    case CM_HIR_TYPE_SLICE_KIND:
        CM_META_ENCODE_ALIAS_CHILD(type->data.slice_type.element);
        break;
    case CM_HIR_TYPE_ADT_KIND:
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
        for (index = 0u; index < type->data.named_type.argument_count;
                ++index) {
            const CmHirGenericArg *argument;

            argument = &type->data.named_type.arguments[index];
            if (argument->kind == CM_HIR_GENERIC_ARG_TYPE)
                CM_META_ENCODE_ALIAS_CHILD(argument->data.type);
            if (argument->kind == CM_HIR_GENERIC_ARG_CONST)
                CM_META_ENCODE_ALIAS_CHILD(argument->data.constant.type);
        }
        if (type->kind == CM_HIR_TYPE_ALIAS_APPLICATION_KIND) {
            uint32_t local;

            local = cm_meta_item_local(items,
                type->data.named_type.definition);
            if (local == 0u || !cm_meta_encode_alias_acyclic(identity,
                    local, items, item_states, item_heights, type_states,
                    type_heights, traversal_depth + 1u))
                return 0;
            if (item_heights[local - 1u] == UINT32_MAX) return 0;
            {
                uint32_t nested_height;

                nested_height = item_heights[local - 1u] + 1u;
                if (nested_height > height) height = nested_height;
            }
        }
        break;
    default:
        break;
    }
#undef CM_META_ENCODE_ALIAS_CHILD
    type_heights[state_index] = height;
    type_states[state_index] = UINT8_C(2);
    return 1;
}

static int cm_meta_encode_alias_acyclic(
    const CmHirLibraryArtifactIdentity *identity, uint32_t item_local,
    const CmVec *items, unsigned char *item_states,
    uint32_t *item_heights, unsigned char *type_states,
    uint32_t *type_heights, size_t traversal_depth)
{
    const CmMetaEncodeItem *item;

    item = item_local == 0u || (size_t)item_local > items->len
            || traversal_depth >= (size_t)CM_META_MAX_TYPE_NESTING ? NULL
        : (const CmMetaEncodeItem *)cm_vec_at_const(items,
            (size_t)(item_local - 1u));
    if (item == NULL || item->kind != CM_META_ITEM_ALIAS) return 0;
    if (item_states[item_local - 1u] == UINT8_C(1)) return 0;
    if (item_states[item_local - 1u] == UINT8_C(2)) {
        return (size_t)item_heights[item_local - 1u]
            < (size_t)CM_META_MAX_TYPE_NESTING - traversal_depth;
    }
    item_states[item_local - 1u] = UINT8_C(1);
    if (!cm_meta_encode_alias_type_acyclic(identity,
            item->item->data.type_alias_item.target, items, item_states,
            item_heights, type_states, type_heights,
            traversal_depth + 1u)) {
        return 0;
    }
    if (type_heights[item->item->data.type_alias_item.target - 1u]
            == UINT32_MAX) return 0;
    item_heights[item_local - 1u] = type_heights[
        item->item->data.type_alias_item.target - 1u] + 1u;
    item_states[item_local - 1u] = UINT8_C(2);
    return 1;
}

static int cm_meta_encode_aliases_acyclic(
    const CmHirLibraryArtifactIdentity *identity, const CmVec *items)
{
    unsigned char *item_states;
    uint32_t *item_heights;
    unsigned char *type_states;
    uint32_t *type_heights;
    size_t index;
    int valid;

    item_states = (unsigned char *)cm_alloc_zeroed(items->len,
        sizeof(unsigned char));
    item_heights = (uint32_t *)cm_alloc_zeroed(items->len,
        sizeof(uint32_t));
    type_states = (unsigned char *)cm_alloc_zeroed(
        identity->context->types.len, sizeof(unsigned char));
    type_heights = (uint32_t *)cm_alloc_zeroed(
        identity->context->types.len, sizeof(uint32_t));
    valid = 1;
    for (index = 0u; valid && index < items->len; ++index) {
        const CmMetaEncodeItem *item;

        item = (const CmMetaEncodeItem *)cm_vec_at_const(items, index);
        if (item != NULL && item->kind == CM_META_ITEM_ALIAS)
            valid = cm_meta_encode_alias_acyclic(identity,
                (uint32_t)(index + 1u), items, item_states,
                item_heights, type_states, type_heights, 0u);
    }
    cm_free(type_heights);
    cm_free(type_states);
    cm_free(item_heights);
    cm_free(item_states);
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
    } else if (region->kind == CM_HIR_REGION_LATE_BOUND) {
        kind = CM_META_REGION_LATE_BOUND;
        parameter = region->data.binder_index;
    } else {
        return 0;
    }
    return cm_hir_metadata_write_u8(writer, kind) == CM_HIR_METADATA_OK
        && cm_hir_metadata_write_u32(writer, parameter)
            == CM_HIR_METADATA_OK;
}

static int cm_meta_write_const(CmHirMetadataWriter *writer,
    const CmHirConstArg *constant, const CmVec *generics,
    const uint32_t *type_locals, size_t type_local_count)
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
    type = cm_meta_type_local(type_locals, type_local_count, constant->type);
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
    const uint32_t *type_locals, size_t type_local_count)
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

            type = cm_meta_type_local(type_locals, type_local_count,
                argument->data.type);
            if (type == 0u
                || cm_hir_metadata_write_u8(writer, CM_META_ARG_TYPE)
                    != CM_HIR_METADATA_OK
                || cm_hir_metadata_write_u32(writer, type)
                    != CM_HIR_METADATA_OK) return 0;
        } else if (argument->kind == CM_HIR_GENERIC_ARG_CONST) {
            if (cm_hir_metadata_write_u8(writer, CM_META_ARG_CONST)
                    != CM_HIR_METADATA_OK
                || !cm_meta_write_const(writer, &argument->data.constant,
                    generics, type_locals, type_local_count)) return 0;
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
    const CmVec *generics, const uint32_t *type_locals,
    size_t type_local_count)
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
        local = cm_meta_type_local(type_locals, type_local_count,
            type->data.reference_type.pointee);
        return local != 0u
            && cm_meta_write_region(writer,
                &type->data.reference_type.region, generics)
            && cm_hir_metadata_write_u32(writer, local)
                == CM_HIR_METADATA_OK
            && cm_hir_metadata_write_u8(writer, cm_meta_mutability_to_wire(
                type->data.reference_type.mutability)) == CM_HIR_METADATA_OK;
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        local = cm_meta_type_local(type_locals, type_local_count,
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
            local = cm_meta_type_local(type_locals, type_local_count,
                type->data.tuple_type.elements[index]);
            if (local == 0u || cm_hir_metadata_write_u32(writer, local)
                    != CM_HIR_METADATA_OK) return 0;
        }
        return 1;
    case CM_HIR_TYPE_ARRAY_KIND:
        local = cm_meta_type_local(type_locals, type_local_count,
            type->data.array_type.element);
        if (local == 0u
            || cm_hir_metadata_write_u32(writer, local)
                != CM_HIR_METADATA_OK) return 0;
        return cm_meta_write_const(writer, &type->data.array_type.length,
            generics, type_locals, type_local_count);
    case CM_HIR_TYPE_SLICE_KIND:
        local = cm_meta_type_local(type_locals, type_local_count,
            type->data.slice_type.element);
        return local != 0u
            && cm_hir_metadata_write_u32(writer, local)
                == CM_HIR_METADATA_OK;
    case CM_HIR_TYPE_ADT_KIND:
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
        return cm_meta_write_named(writer, &type->data.named_type, items,
            generics, type_locals, type_local_count);
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
    CmHirAggregateForm form, const CmVec *modules,
    const uint32_t *type_locals, size_t type_local_count)
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
    type = cm_meta_type_local(type_locals, type_local_count, field->type);
    return type != 0u
        && cm_hir_metadata_write_u32(writer, type) == CM_HIR_METADATA_OK
        && cm_meta_write_visibility(writer, &field->visibility, modules);
}

static int cm_meta_write_item(CmHirMetadataWriter *writer,
    const CmHirLibraryArtifactIdentity *identity,
    const CmMetaEncodeItem *encoded, const CmVec *modules,
    const CmVec *generics, const uint32_t *type_locals,
    size_t type_local_count)
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
                    item->data.aggregate_item.form, modules, type_locals,
                    type_local_count)) return 0;
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
                        type_locals, type_local_count)) return 0;
            }
            if (cm_hir_metadata_write_u8(writer,
                    variant->has_discriminant ? UINT8_C(1) : UINT8_C(0))
                    != CM_HIR_METADATA_OK) return 0;
            if (variant->has_discriminant) {
                uint32_t discriminant_type;

                if (variant->discriminant.kind != CM_HIR_CONST_VALUE)
                    return 0;
                discriminant_type = cm_meta_type_local(type_locals,
                    type_local_count,
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

        target = cm_meta_type_local(type_locals, type_local_count,
            item->data.type_alias_item.target);
        return target != 0u
            && cm_hir_metadata_write_u32(writer, target)
                == CM_HIR_METADATA_OK;
    }
    return 0;
}

static int cm_meta_write_value(CmHirMetadataWriter *writer,
    const CmHirLibraryArtifactIdentity *identity,
    const CmMetaEncodeValue *encoded, const CmVec *generics,
    const uint32_t *type_locals, size_t type_local_count)
{
    const CmHirLibraryValue *value;

    if (writer == NULL || identity == NULL || encoded == NULL
        || encoded->value == NULL || type_locals == NULL) return 0;
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
            local = cm_meta_type_local(type_locals, type_local_count,
                value->data.function.parameter_types[index]);
            if (local == 0u || cm_hir_metadata_write_u32(writer, local)
                    != CM_HIR_METADATA_OK) return 0;
        }
        local = cm_meta_type_local(type_locals, type_local_count,
            value->data.function.return_type);
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

        local = cm_meta_type_local(type_locals, type_local_count,
            value->data.value.type);
        mutability = cm_meta_mutability_to_wire(
            value->data.value.mutability);
        return local != 0u && mutability != 0u
            && cm_hir_metadata_write_u32(writer, local)
                == CM_HIR_METADATA_OK
            && cm_hir_metadata_write_u8(writer, mutability)
                == CM_HIR_METADATA_OK;
    }
}

static uint8_t cm_meta_generic_kind_to_wire(CmHirGenericParamKind kind)
{
    if (kind == CM_HIR_GENERIC_LIFETIME) return CM_META_GENERIC_LIFETIME;
    if (kind == CM_HIR_GENERIC_TYPE) return CM_META_GENERIC_TYPE;
    if (kind == CM_HIR_GENERIC_CONST) return CM_META_GENERIC_CONST;
    return UINT8_C(0);
}

static uint8_t cm_meta_nominal_kind_to_wire(
    CmHirLibraryNominalReferenceKind kind)
{
    if (kind == CM_HIR_LIBRARY_NOMINAL_TRAIT) return CM_META_NOMINAL_TRAIT;
    if (kind == CM_HIR_LIBRARY_NOMINAL_TRAIT_ALIAS)
        return CM_META_NOMINAL_TRAIT_ALIAS;
    if (kind == CM_HIR_LIBRARY_NOMINAL_ASSOCIATED_TYPE)
        return CM_META_NOMINAL_ASSOCIATED_TYPE;
    return UINT8_C(0);
}

static int cm_meta_u32_compare(const void *left, const void *right)
{
    uint32_t l;
    uint32_t r;

    l = *(const uint32_t *)left;
    r = *(const uint32_t *)right;
    return l < r ? -1 : (l > r ? 1 : 0);
}

typedef struct CmMetaLocalPair {
    uint32_t first;
    uint32_t second;
} CmMetaLocalPair;

static int cm_meta_local_pair_compare(const void *left_value,
    const void *right_value)
{
    const CmMetaLocalPair *left;
    const CmMetaLocalPair *right;

    left = (const CmMetaLocalPair *)left_value;
    right = (const CmMetaLocalPair *)right_value;
    if (left->first < right->first) return -1;
    if (left->first > right->first) return 1;
    return left->second < right->second ? -1
        : (left->second > right->second ? 1 : 0);
}

static int cm_meta_write_nominals(CmHirMetadataWriter *writer,
    const CmVec *nominals, const CmVec *nominal_lookup)
{
    size_t index;

    if (cm_hir_metadata_write_u32(writer, (uint32_t)nominals->len)
            != CM_HIR_METADATA_OK) return 0;
    for (index = 0u; index < nominals->len; ++index) {
        const CmMetaEncodeNominal *nominal;
        const CmHirLibraryNominalReference *reference;
        uint32_t parent;
        uint32_t generic;
        uint8_t kind;

        nominal = (const CmMetaEncodeNominal *)cm_vec_at_const(nominals,
            index);
        reference = nominal == NULL ? NULL : nominal->reference;
        kind = reference == NULL ? UINT8_C(0)
            : cm_meta_nominal_kind_to_wire(reference->kind);
        parent = reference != NULL && reference->kind
                == CM_HIR_LIBRARY_NOMINAL_ASSOCIATED_TYPE
            ? cm_meta_nominal_local(nominal_lookup,
                reference->declaring_trait)
            : UINT32_C(0);
        if (reference == NULL || kind == 0u || nominal->owner == 0u
            || (reference->kind
                    == CM_HIR_LIBRARY_NOMINAL_ASSOCIATED_TYPE
                ? parent == 0u : parent != 0u)
            || cm_hir_metadata_write_u8(writer, kind) != CM_HIR_METADATA_OK
            || cm_hir_metadata_write_u32(writer, nominal->owner)
                != CM_HIR_METADATA_OK
            || !cm_meta_write_bytes_string(writer, reference->name.bytes,
                reference->name.length)
            || cm_hir_metadata_write_u32(writer, parent)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_write_u32(writer,
                reference->generic_parameter_count)
                != CM_HIR_METADATA_OK) return 0;
        for (generic = 0u; generic < reference->generic_parameter_count;
                ++generic) {
            uint8_t generic_kind;

            generic_kind = cm_meta_generic_kind_to_wire(
                reference->generic_parameter_kinds[generic]);
            if (generic_kind == 0u || cm_hir_metadata_write_u8(writer,
                    generic_kind) != CM_HIR_METADATA_OK) return 0;
        }
    }
    return 1;
}

static int cm_meta_write_value_predicates(CmHirMetadataWriter *writer,
    const CmHirLibraryArtifactIdentity *identity, const CmVec *values,
    const CmVec *nominal_lookup, const uint32_t *type_locals,
    size_t type_local_count)
{
    size_t value_index;
    size_t total_nominal_memberships;
    size_t total_availability;
    size_t total_predicates;
    size_t total_binders;
    size_t total_arguments;
    size_t total_equalities;
    size_t total_outlives;
    uint32_t group_count;

    group_count = 0u;
    total_nominal_memberships = 0u;
    total_availability = 0u;
    total_predicates = 0u;
    total_binders = 0u;
    total_arguments = 0u;
    total_equalities = 0u;
    total_outlives = 0u;
    for (value_index = 0u; value_index < values->len; ++value_index) {
        const CmMetaEncodeValue *encoded;
        const CmHirLibraryValue *value;

        encoded = (const CmMetaEncodeValue *)cm_vec_at_const(values,
            value_index);
        value = encoded == NULL ? NULL : &encoded->value->declaration;
        if (value != NULL && value->kind == CM_HIR_LIBRARY_VALUE_FUNCTION
            && (value->data.function.nominal_reference_count != 0u
                || value->data.function.associated_availability_count != 0u
                || value->data.function.predicate_count != 0u
                || value->data.function.outlives_predicate_count != 0u)) {
            uint32_t predicate_index;

            if (!cm_size_add(total_nominal_memberships,
                    value->data.function.nominal_reference_count,
                    &total_nominal_memberships)
                || !cm_size_add(total_availability,
                    value->data.function.associated_availability_count,
                    &total_availability)
                || !cm_size_add(total_predicates,
                    value->data.function.predicate_count, &total_predicates)
                || !cm_size_add(total_outlives,
                    value->data.function.outlives_predicate_count,
                    &total_outlives)) return 0;
            for (predicate_index = 0u;
                    predicate_index < value->data.function.predicate_count;
                    ++predicate_index) {
                const CmHirTraitPredicate *predicate;

                predicate = &value->data.function.predicates[predicate_index];
                if (!cm_size_add(total_binders,
                        predicate->binder.lifetime_count, &total_binders)
                    || !cm_size_add(total_arguments,
                        predicate->trait_type.argument_count,
                        &total_arguments)
                    || !cm_size_add(total_equalities,
                        predicate->equality_count, &total_equalities))
                    return 0;
            }
            group_count += 1u;
        }
    }
    if (total_nominal_memberships > CM_META_MAX_NOMINAL_REFERENCES
        || total_availability > CM_META_MAX_PREDICATES
        || total_predicates > CM_META_MAX_PREDICATES
        || total_binders > CM_META_MAX_GENERICS
        || total_arguments > CM_META_MAX_GENERICS
        || total_equalities > CM_META_MAX_PREDICATES
        || total_outlives > CM_META_MAX_PREDICATES) return 0;
    if (cm_hir_metadata_write_u32(writer, group_count)
            != CM_HIR_METADATA_OK) return 0;
    for (value_index = 0u; value_index < values->len; ++value_index) {
        const CmMetaEncodeValue *encoded;
        const CmHirLibraryValue *value;
        const CmHirLibraryFunctionSignature *function;
        uint32_t index;
        uint32_t *reference_locals;

        encoded = (const CmMetaEncodeValue *)cm_vec_at_const(values,
            value_index);
        value = encoded == NULL ? NULL : &encoded->value->declaration;
        if (value == NULL) return 0;
        if (value->kind != CM_HIR_LIBRARY_VALUE_FUNCTION
            || (value->data.function.nominal_reference_count == 0u
                && value->data.function.associated_availability_count == 0u
                && value->data.function.predicate_count == 0u
                && value->data.function.outlives_predicate_count == 0u))
            continue;
        function = &value->data.function;
        if (function->predicate_scope_count != 0u) return 0;
        if (cm_hir_metadata_write_u32(writer, (uint32_t)value_index + 1u)
                != CM_HIR_METADATA_OK) return 0;
        reference_locals = function->nominal_reference_count == 0u ? NULL
            : (uint32_t *)cm_alloc((size_t)function->nominal_reference_count
                * sizeof(uint32_t));
        for (index = 0u; index < function->nominal_reference_count; ++index) {
            reference_locals[index] = cm_meta_nominal_local(nominal_lookup,
                function->nominal_references[index].definition);
            if (reference_locals[index] == 0u) {
                cm_free(reference_locals);
                return 0;
            }
        }
        if (function->nominal_reference_count > 1u) qsort(reference_locals,
            function->nominal_reference_count, sizeof(uint32_t),
            cm_meta_u32_compare);
        if (cm_hir_metadata_write_u32(writer,
                function->nominal_reference_count) != CM_HIR_METADATA_OK) {
            cm_free(reference_locals);
            return 0;
        }
        for (index = 0u; index < function->nominal_reference_count; ++index) {
            if ((index != 0u
                    && reference_locals[index - 1u] >= reference_locals[index])
                || cm_hir_metadata_write_u32(writer, reference_locals[index])
                    != CM_HIR_METADATA_OK) {
                cm_free(reference_locals);
                return 0;
            }
        }
        cm_free(reference_locals);
        if (cm_hir_metadata_write_u32(writer,
                function->associated_availability_count)
                != CM_HIR_METADATA_OK) return 0;
        {
            CmMetaLocalPair *pairs;

            pairs = function->associated_availability_count == 0u ? NULL
                : (CmMetaLocalPair *)cm_alloc((size_t)function
                    ->associated_availability_count
                    * sizeof(CmMetaLocalPair));
            for (index = 0u;
                    index < function->associated_availability_count;
                    ++index) {
                pairs[index].first = cm_meta_nominal_local(nominal_lookup,
                    function->associated_availability[index].direct_trait);
                pairs[index].second = cm_meta_nominal_local(nominal_lookup,
                    function->associated_availability[index].associated_type);
                if (pairs[index].first == 0u || pairs[index].second == 0u) {
                    cm_free(pairs);
                    return 0;
                }
            }
            if (function->associated_availability_count > 1u) qsort(pairs,
                function->associated_availability_count,
                sizeof(CmMetaLocalPair), cm_meta_local_pair_compare);
            for (index = 0u;
                    index < function->associated_availability_count;
                    ++index) {
                if ((index != 0u && cm_meta_local_pair_compare(
                        &pairs[index - 1u], &pairs[index]) >= 0)
                    || cm_hir_metadata_write_u32(writer, pairs[index].first)
                        != CM_HIR_METADATA_OK
                    || cm_hir_metadata_write_u32(writer, pairs[index].second)
                        != CM_HIR_METADATA_OK) {
                    cm_free(pairs);
                    return 0;
                }
            }
            cm_free(pairs);
        }
        {
        CmMetaEncodePredicate *predicates;

        predicates = function->predicate_count == 0u ? NULL
            : (CmMetaEncodePredicate *)cm_alloc(
                (size_t)function->predicate_count
                    * sizeof(CmMetaEncodePredicate));
        for (index = 0u; index < function->predicate_count; ++index) {
            predicates[index].predicate = &function->predicates[index];
            predicates[index].nominal_local = cm_meta_nominal_local(
                nominal_lookup,
                function->predicates[index].trait_type.definition);
            predicates[index].subject_local = cm_meta_type_local(type_locals,
                type_local_count, function->predicates[index].subject);
            if (predicates[index].nominal_local == 0u
                || predicates[index].subject_local == 0u) {
                cm_free(predicates);
                return 0;
            }
        }
        if (function->predicate_count > 1u) qsort(predicates,
            function->predicate_count, sizeof(CmMetaEncodePredicate),
            cm_meta_encode_predicate_compare);
        if (cm_hir_metadata_write_u32(writer, function->predicate_count)
                != CM_HIR_METADATA_OK) {
            cm_free(predicates);
            return 0;
        }
#define CM_META_PREDICATE_WRITE_FAIL() do { \
            cm_free(predicates); \
            return 0; \
        } while (0)
        for (index = 0u; index < function->predicate_count; ++index) {
            const CmHirTraitPredicate *predicate;
            uint32_t child;
            uint32_t local;
            uint32_t trait_local;
            uint8_t modifier;

            predicate = predicates[index].predicate;
            local = cm_meta_type_local(type_locals, type_local_count,
                predicate->subject);
            trait_local = predicates[index].nominal_local;
            if (local == 0u || trait_local == 0u
                || local != predicates[index].subject_local
                || (index != 0u && cm_meta_encode_predicate_compare(
                    &predicates[index - 1u], &predicates[index]) >= 0)
                || predicate->scope != CM_HIR_PREDICATE_SCOPE_NONE
                || !cm_meta_predicate_modifier_to_wire(
                    predicate->modifier, &modifier)
                || cm_hir_metadata_write_u32(writer, local)
                    != CM_HIR_METADATA_OK
                || cm_hir_metadata_write_u32(writer, trait_local)
                    != CM_HIR_METADATA_OK
                || cm_hir_metadata_write_u8(writer, modifier)
                    != CM_HIR_METADATA_OK
                || cm_hir_metadata_write_u32(writer,
                    predicate->binder.lifetime_count)
                    != CM_HIR_METADATA_OK) CM_META_PREDICATE_WRITE_FAIL();
            for (child = 0u; child < predicate->binder.lifetime_count;
                    ++child) {
                const CmInternedString *name;

                name = cm_interner_get(&identity->context->strings,
                    predicate->binder.lifetimes[child]);
                if (name == NULL || !cm_meta_write_bytes_string(writer,
                        name->bytes, name->len))
                    CM_META_PREDICATE_WRITE_FAIL();
            }
            if (cm_hir_metadata_write_u32(writer,
                    predicate->trait_type.argument_count)
                    != CM_HIR_METADATA_OK) CM_META_PREDICATE_WRITE_FAIL();
            for (child = 0u; child < predicate->trait_type.argument_count;
                    ++child) {
                if (predicate->trait_type.arguments[child].kind
                        != CM_HIR_GENERIC_ARG_TYPE)
                    CM_META_PREDICATE_WRITE_FAIL();
                local = cm_meta_type_local(type_locals, type_local_count,
                    predicate->trait_type.arguments[child].data.type);
                if (local == 0u || cm_hir_metadata_write_u32(writer, local)
                        != CM_HIR_METADATA_OK)
                    CM_META_PREDICATE_WRITE_FAIL();
            }
            if (cm_hir_metadata_write_u32(writer, predicate->equality_count)
                    != CM_HIR_METADATA_OK) CM_META_PREDICATE_WRITE_FAIL();
            {
                CmMetaLocalPair *pairs;

                pairs = predicate->equality_count == 0u ? NULL
                    : (CmMetaLocalPair *)cm_alloc(
                        (size_t)predicate->equality_count
                            * sizeof(CmMetaLocalPair));
                for (child = 0u; child < predicate->equality_count; ++child) {
                    pairs[child].first = cm_meta_nominal_local(nominal_lookup,
                        predicate->equalities[child].associated_type);
                    pairs[child].second = cm_meta_type_local(type_locals,
                        type_local_count,
                        predicate->equalities[child].value);
                    if (pairs[child].first == 0u
                        || pairs[child].second == 0u) {
                        cm_free(pairs);
                        CM_META_PREDICATE_WRITE_FAIL();
                    }
                }
                if (predicate->equality_count > 1u) qsort(pairs,
                    predicate->equality_count, sizeof(CmMetaLocalPair),
                    cm_meta_local_pair_compare);
                for (child = 0u; child < predicate->equality_count; ++child) {
                    if ((child != 0u && pairs[child - 1u].first
                            >= pairs[child].first)
                        || cm_hir_metadata_write_u32(writer,
                            pairs[child].first) != CM_HIR_METADATA_OK
                        || cm_hir_metadata_write_u32(writer,
                            pairs[child].second) != CM_HIR_METADATA_OK) {
                        cm_free(pairs);
                        CM_META_PREDICATE_WRITE_FAIL();
                    }
                }
                cm_free(pairs);
            }
        }
        cm_free(predicates);
#undef CM_META_PREDICATE_WRITE_FAIL
        }
        if (cm_hir_metadata_write_u32(writer,
                function->outlives_predicate_count)
                != CM_HIR_METADATA_OK) return 0;
        {
        CmMetaEncodeOutlives *outlives_values;

        outlives_values = function->outlives_predicate_count == 0u ? NULL
            : (CmMetaEncodeOutlives *)cm_alloc(
                (size_t)function->outlives_predicate_count
                    * sizeof(CmMetaEncodeOutlives));
        for (index = 0u; index < function->outlives_predicate_count;
                ++index) {
            outlives_values[index].outlives =
                &function->outlives_predicates[index];
            outlives_values[index].type_local =
                function->outlives_predicates[index].subject_kind
                    == CM_HIR_OUTLIVES_TYPE
                ? cm_meta_type_local(type_locals, type_local_count,
                    function->outlives_predicates[index].subject.type)
                : UINT32_C(0);
        }
        if (function->outlives_predicate_count > 1u) qsort(outlives_values,
            function->outlives_predicate_count, sizeof(CmMetaEncodeOutlives),
            cm_meta_encode_outlives_compare);
        for (index = 0u; index < function->outlives_predicate_count;
                ++index) {
            const CmHirOutlivesPredicate *outlives;
            uint32_t subject;

            outlives = outlives_values[index].outlives;
            subject = outlives_values[index].type_local;
            if (subject == 0u
                || (index != 0u
                    && outlives_values[index - 1u].type_local >= subject)
                || outlives->scope != CM_HIR_PREDICATE_SCOPE_NONE
                || outlives->subject_kind != CM_HIR_OUTLIVES_TYPE
                || outlives->bound.kind != CM_HIR_REGION_STATIC
                || cm_hir_metadata_write_u32(writer, subject)
                    != CM_HIR_METADATA_OK) {
                cm_free(outlives_values);
                return 0;
            }
        }
        cm_free(outlives_values);
        }
    }
    return 1;
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
    CmVec nominals;
    CmVec nominal_lookup;
    CmByteBuf crate_section;
    CmByteBuf module_section;
    CmByteBuf generic_section;
    CmByteBuf type_section;
    CmByteBuf item_section;
    CmByteBuf namespace_section;
    CmByteBuf trait_universe_section;
    CmByteBuf value_section;
    CmByteBuf nominal_section;
    CmByteBuf predicate_section;
    CmByteBuf payload;
    CmHirMetadataWriter writer;
    CmHirMetadataWriter payload_writer;
    CmHirMetadataStatus codec_status;
    uint32_t *type_locals;
    size_t type_local_count;
    size_t module_index;
    size_t public_entry_count;
    int declaration_v24;

    result = cm_meta_result(CM_HIR_METADATA_ARTIFACT_INVALID_ARGUMENT);
    if (output == NULL || artifact == NULL || (semantic && declaration)
        || !cm_hir_library_artifact_identity(artifact, &identity)
        || (owned = cm_hir_library_artifact_owned_data_const(artifact))
            == NULL
        || owned->modules.len == 0u
        || owned->modules.len > (size_t)CM_META_MAX_MODULES
        || (crate_value = cm_hir_get_crate(identity.context,
            identity.crate_id)) == NULL) return result;
    /* The current declaration encoder always emits the exact v2.6 family. */
    declaration_v24 = declaration;
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
            if (item->definition.crate_id == identity.crate_id
                && ((item->kind == CM_HIR_ITEM_FUNCTION
                        && item->data.function_item.has_default_body)
                    || (item->kind == CM_HIR_ITEM_CONST
                        && item->data.value_item.has_default_body))) {
                /*
                 * Semantic v1.1 carries neither associated declarations nor
                 * their default promises.  Reject rather than publish a
                 * trait contract weaker than the producer's live HIR.
                 */
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
    cm_vec_init(&nominals, sizeof(CmMetaEncodeNominal));
    cm_vec_init(&nominal_lookup, sizeof(CmMetaEncodeNominalLookup));
    type_locals = NULL;
    type_local_count = identity.context->types.len;
    if (!cm_meta_collect_modules(&identity, owned, &modules)) {
        result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR;
        goto cleanup_views;
    }
    if ((declaration && !cm_meta_collect_values(owned, &modules,
            &values, declaration_v24))
        || (declaration_v24 && !cm_meta_collect_nominals(&values, &modules,
            &nominals))
        || (declaration_v24 && !cm_meta_build_nominal_lookup(&nominals,
            &nominal_lookup))
        || !cm_meta_collect_items(&identity, &modules,
            declaration_v24 ? &nominal_lookup : NULL, &items, semantic,
            declaration)
        || (semantic && !cm_meta_collect_trait_universe(&identity,
            &modules, &traits, &impls))
        || !cm_meta_collect_generics(&identity, &items,
            declaration ? &values : NULL, &generics)
        || !cm_meta_collect_types(&identity, &items, &generics,
            semantic ? &impls : NULL, declaration ? &values : NULL,
            declaration_v24 ? &nominal_lookup : NULL, &types, &type_locals)
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
    cm_byte_buf_init(&nominal_section);
    cm_byte_buf_init(&predicate_section);
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
            ? cm_meta_type_local(type_locals, type_local_count,
                parameter->default_argument.data.type)
            : (parameter != NULL
                && parameter->kind == CM_HIR_GENERIC_CONST
                ? cm_meta_type_local(type_locals, type_local_count,
                    parameter->declared_type)
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
                &generics, type_locals, type_local_count)) {
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
                encoded_item, &modules, &generics, type_locals,
                type_local_count)) {
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
                    value, &generics, type_locals, type_local_count)) {
                result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR;
                goto cleanup_encode;
            }
        }
    }

    if (declaration_v24) {
        cm_hir_metadata_writer_init(&writer, &nominal_section,
            CM_HIR_METADATA_MAX_PAYLOAD_SIZE);
        if (!cm_meta_write_nominals(&writer, &nominals, &nominal_lookup)) {
            result.status = CM_HIR_METADATA_ARTIFACT_UNSUPPORTED_HIR;
            goto cleanup_encode;
        }
        cm_hir_metadata_writer_init(&writer, &predicate_section,
            CM_HIR_METADATA_MAX_PAYLOAD_SIZE);
        if (!cm_meta_write_value_predicates(&writer, &identity, &values,
                &nominal_lookup, type_locals, type_local_count)) {
            result.status = CM_HIR_METADATA_ARTIFACT_UNSUPPORTED_HIR;
            goto cleanup_encode;
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
                : cm_meta_type_local(type_locals, type_local_count,
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
    if (codec_status == CM_HIR_METADATA_OK && declaration_v24)
        codec_status = cm_hir_metadata_write_section(&payload_writer,
            cm_meta_tag_nominal_references, nominal_section.data,
            nominal_section.len);
    if (codec_status == CM_HIR_METADATA_OK && declaration_v24)
        codec_status = cm_hir_metadata_write_section(&payload_writer,
            cm_meta_tag_predicates, predicate_section.data,
            predicate_section.len);
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
        (uint16_t)(declaration ? (declaration_v24
                ? CM_HIR_METADATA_DECLARATION_MINOR
                : CM_HIR_METADATA_DECLARATION_LEGACY_MINOR)
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
    cm_byte_buf_destroy(&predicate_section);
    cm_byte_buf_destroy(&nominal_section);
    cm_byte_buf_destroy(&value_section);
    cm_byte_buf_destroy(&trait_universe_section);
    cm_byte_buf_destroy(&namespace_section);
    cm_byte_buf_destroy(&item_section);
    cm_byte_buf_destroy(&type_section);
    cm_byte_buf_destroy(&generic_section);
    cm_byte_buf_destroy(&module_section);
    cm_byte_buf_destroy(&crate_section);
cleanup_views:
    cm_free(type_locals);
    cm_vec_destroy(&nominal_lookup);
    cm_vec_destroy(&nominals);
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
    const CmVec *generics, CmMetaWireRegion *out_region, int allow_late)
{
    const CmMetaWireGeneric *generic;

    if (cm_hir_metadata_read_u8(reader, &out_region->kind)
            != CM_HIR_METADATA_OK
        || cm_hir_metadata_read_u32(reader, &out_region->parameter)
            != CM_HIR_METADATA_OK) return 0;
    if (out_region->kind == CM_META_REGION_STATIC)
        return out_region->parameter == 0u;
    if (out_region->kind == CM_META_REGION_LATE_BOUND)
        return allow_late;
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
    CmMetaWireNamed *out_named, int allow_late)
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
                    &argument->data.lifetime, allow_late)) goto invalid;
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
    uint32_t item_count, const CmVec *generics, CmVec *types,
    int allow_late)
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
                    &type.data.reference_type.region, allow_late)
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
                    &type.data.named_type, allow_late)) return 0;
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

static void cm_meta_wire_nominals_destroy(CmVec *nominals)
{
    size_t index;

    for (index = 0u; index < nominals->len; ++index) {
        CmMetaWireNominal *nominal;

        nominal = (CmMetaWireNominal *)cm_vec_at(nominals, index);
        if (nominal != NULL) cm_free(nominal->generic_kinds);
    }
    cm_vec_destroy(nominals);
}

static int cm_meta_wire_nominal_compare(const CmVec *nominals,
    const CmMetaWireNominal *left, const CmMetaWireNominal *right)
{
    int names;

    if (left->kind == CM_META_NOMINAL_ASSOCIATED_TYPE
        || right->kind == CM_META_NOMINAL_ASSOCIATED_TYPE) {
        const CmMetaWireNominal *left_parent;
        const CmMetaWireNominal *right_parent;

        if (left->kind != CM_META_NOMINAL_ASSOCIATED_TYPE) return -1;
        if (right->kind != CM_META_NOMINAL_ASSOCIATED_TYPE) return 1;
        left_parent = (const CmMetaWireNominal *)cm_vec_at_const(nominals,
            left->declaring_trait - 1u);
        right_parent = (const CmMetaWireNominal *)cm_vec_at_const(nominals,
            right->declaring_trait - 1u);
        if (left_parent->owner < right_parent->owner) return -1;
        if (left_parent->owner > right_parent->owner) return 1;
        names = cm_meta_bytes_compare(left_parent->name.bytes,
            left_parent->name.length, right_parent->name.bytes,
            right_parent->name.length);
        if (names != 0) return names;
    } else {
        if (left->owner < right->owner) return -1;
        if (left->owner > right->owner) return 1;
        return cm_meta_bytes_compare(left->name.bytes, left->name.length,
            right->name.bytes, right->name.length);
    }
    return cm_meta_bytes_compare(left->name.bytes, left->name.length,
        right->name.bytes, right->name.length);
}

static int cm_meta_decode_nominals(const CmHirMetadataSection *section,
    uint32_t module_count, CmVec *nominals)
{
    CmHirMetadataReader reader;
    uint32_t count;
    uint32_t index;
    size_t total_generic_count;

    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (cm_hir_metadata_read_u32(&reader, &count) != CM_HIR_METADATA_OK
        || count > CM_META_MAX_NOMINAL_REFERENCES) return 0;
    total_generic_count = 0u;
    for (index = 0u; index < count; ++index) {
        CmMetaWireNominal nominal;
        uint32_t generic;

        memset(&nominal, 0, sizeof(nominal));
        if (cm_hir_metadata_read_u8(&reader, &nominal.kind)
                != CM_HIR_METADATA_OK
            || (nominal.kind != CM_META_NOMINAL_TRAIT
                && nominal.kind != CM_META_NOMINAL_TRAIT_ALIAS
                && nominal.kind != CM_META_NOMINAL_ASSOCIATED_TYPE)
            || cm_hir_metadata_read_u32(&reader, &nominal.owner)
                != CM_HIR_METADATA_OK
            || nominal.owner == 0u || nominal.owner > module_count
            || !cm_meta_read_name(&reader, &nominal.name)
            || cm_hir_metadata_read_u32(&reader,
                &nominal.declaring_trait) != CM_HIR_METADATA_OK
            || (nominal.kind == CM_META_NOMINAL_ASSOCIATED_TYPE
                ? (nominal.declaring_trait == 0u
                    || nominal.declaring_trait > count)
                : nominal.declaring_trait != 0u)
            || cm_hir_metadata_read_u32(&reader, &nominal.generic_count)
                != CM_HIR_METADATA_OK
            || nominal.generic_count > CM_META_MAX_GENERICS
            || !cm_size_add(total_generic_count,
                (size_t)nominal.generic_count, &total_generic_count)
            || total_generic_count > (size_t)CM_META_MAX_GENERICS) return 0;
        nominal.generic_kinds = nominal.generic_count == 0u ? NULL
            : (uint8_t *)cm_alloc((size_t)nominal.generic_count);
        for (generic = 0u; generic < nominal.generic_count; ++generic) {
            if (cm_hir_metadata_read_u8(&reader,
                    &nominal.generic_kinds[generic]) != CM_HIR_METADATA_OK
                || (nominal.generic_kinds[generic] != CM_META_GENERIC_LIFETIME
                    && nominal.generic_kinds[generic]
                        != CM_META_GENERIC_TYPE
                    && nominal.generic_kinds[generic]
                        != CM_META_GENERIC_CONST)) {
                cm_free(nominal.generic_kinds);
                return 0;
            }
        }
        (void)cm_vec_push(nominals, &nominal);
    }
    for (index = 0u; index < count; ++index) {
        const CmMetaWireNominal *nominal;
        const CmMetaWireNominal *parent;

        nominal = (const CmMetaWireNominal *)cm_vec_at_const(nominals,
            index);
        if (nominal == NULL
            || nominal->kind != CM_META_NOMINAL_ASSOCIATED_TYPE) continue;
        parent = (const CmMetaWireNominal *)cm_vec_at_const(nominals,
            (size_t)(nominal->declaring_trait - 1u));
        if (parent == NULL || parent->kind != CM_META_NOMINAL_TRAIT
            || nominal->declaring_trait >= index + 1u
            || nominal->owner != parent->owner) return 0;
        if (index != 0u) {
            const CmMetaWireNominal *prior;

            prior = (const CmMetaWireNominal *)cm_vec_at_const(nominals,
                index - 1u);
            if (prior == NULL || cm_meta_wire_nominal_compare(nominals,
                    prior, nominal) >= 0) return 0;
        }
    }
    for (index = 1u; index < count; ++index) {
        const CmMetaWireNominal *prior;
        const CmMetaWireNominal *nominal;

        prior = (const CmMetaWireNominal *)cm_vec_at_const(nominals,
            index - 1u);
        nominal = (const CmMetaWireNominal *)cm_vec_at_const(nominals, index);
        if (prior == NULL || nominal == NULL
            || cm_meta_wire_nominal_compare(nominals, prior, nominal) >= 0)
            return 0;
    }
    return cm_hir_metadata_reader_finish(&reader) == CM_HIR_METADATA_OK;
}

static void cm_meta_wire_value_predicates_destroy(CmVec *payloads)
{
    size_t value_index;

    for (value_index = 0u; value_index < payloads->len; ++value_index) {
        CmMetaWireValuePredicates *payload;
        uint32_t predicate_index;

        payload = (CmMetaWireValuePredicates *)cm_vec_at(payloads,
            value_index);
        if (payload == NULL) continue;
        for (predicate_index = 0u;
                predicate_index < payload->predicate_count;
                ++predicate_index) {
            CmMetaWirePredicate *predicate;

            predicate = &payload->predicates[predicate_index];
            cm_free(predicate->binder_names);
            cm_free(predicate->arguments);
            cm_free(predicate->equality_associated);
            cm_free(predicate->equality_values);
        }
        cm_free(payload->nominal_references);
        cm_free(payload->availability_traits);
        cm_free(payload->availability_associated);
        cm_free(payload->predicates);
        cm_free(payload->outlives_subjects);
    }
    cm_vec_destroy(payloads);
}

static const CmMetaWireValuePredicates *cm_meta_wire_value_payload(
    const CmVec *payloads, uint32_t value_local)
{
    size_t low;
    size_t high;

    low = 0u;
    high = payloads->len;
    while (low < high) {
        size_t middle;
        const CmMetaWireValuePredicates *payload;

        middle = low + (high - low) / 2u;
        payload = (const CmMetaWireValuePredicates *)cm_vec_at_const(
            payloads, middle);
        if (payload != NULL && payload->value_local == value_local)
            return payload;
        if (payload == NULL || payload->value_local > value_local)
            high = middle;
        else
            low = middle + 1u;
    }
    return NULL;
}

static int cm_meta_wire_local_present(const uint32_t *locals,
    uint32_t count, uint32_t local)
{
    uint32_t low;
    uint32_t high;

    low = 0u;
    high = count;
    while (low < high) {
        uint32_t middle;

        middle = low + (high - low) / 2u;
        if (locals[middle] == local) return 1;
        if (locals[middle] > local) high = middle;
        else low = middle + 1u;
    }
    return 0;
}

static int cm_meta_wire_pair_present(const uint32_t *left_values,
    const uint32_t *right_values, uint32_t count, uint32_t left,
    uint32_t right)
{
    uint32_t low;
    uint32_t high;

    low = 0u;
    high = count;
    while (low < high) {
        uint32_t middle;

        middle = low + (high - low) / 2u;
        if (left_values[middle] == left
            && right_values[middle] == right) return 1;
        if (left_values[middle] > left
            || (left_values[middle] == left
                && right_values[middle] > right)) high = middle;
        else low = middle + 1u;
    }
    return 0;
}

static int cm_meta_wire_type_requirements(const CmVec *types,
    uint32_t *requirements)
{
    uint32_t *heights;
    size_t local_index;
    int valid;

    heights = (uint32_t *)cm_alloc_zeroed(types->len, sizeof(uint32_t));
    valid = 1;
    for (local_index = 0u; valid && local_index < types->len; ++local_index) {
        const CmMetaWireType *type;
        uint32_t requirement;
        uint32_t height;
        uint32_t index;

        type = (const CmMetaWireType *)cm_vec_at_const(types, local_index);
        requirement = 0u;
        height = 1u;
        if (type == NULL) {
            valid = 0;
            break;
        }
#define CM_META_WIRE_REQUIRE(child_local) do { \
            uint32_t child; \
            child = (child_local); \
            if (child == 0u || (size_t)child > local_index) { \
                valid = 0; \
            } else { \
                uint32_t child_height; \
                child_height = heights[child - 1u] + 1u; \
                if (requirements[child - 1u] > requirement) \
                    requirement = requirements[child - 1u]; \
                if (child_height > height) height = child_height; \
            } \
        } while (0)
        if (type->kind == CM_META_TYPE_REFERENCE) {
            if (type->data.reference_type.region.kind
                    == CM_META_REGION_LATE_BOUND) {
                if (type->data.reference_type.region.parameter == UINT32_MAX) {
                    valid = 0;
                } else {
                    requirement = type->data.reference_type.region.parameter
                        + 1u;
                }
            }
            CM_META_WIRE_REQUIRE(type->data.reference_type.pointee);
        } else if (type->kind == CM_META_TYPE_RAW_POINTER) {
            CM_META_WIRE_REQUIRE(type->data.raw_pointer_type.pointee);
        } else if (type->kind == CM_META_TYPE_TUPLE) {
            for (index = 0u; valid
                    && index < type->data.tuple_type.element_count; ++index)
                CM_META_WIRE_REQUIRE(type->data.tuple_type.elements[index]);
        } else if (type->kind == CM_META_TYPE_ARRAY) {
            CM_META_WIRE_REQUIRE(type->data.array_type.element);
            CM_META_WIRE_REQUIRE(type->data.array_type.length.type);
        } else if (type->kind == CM_META_TYPE_SLICE) {
            CM_META_WIRE_REQUIRE(type->data.slice_type.element);
        } else if (type->kind == CM_META_TYPE_ADT
                || type->kind == CM_META_TYPE_ALIAS
                || type->kind == CM_META_TYPE_FOREIGN) {
            for (index = 0u; valid
                    && index < type->data.named_type.argument_count; ++index) {
                const CmMetaWireArg *argument;

                argument = &type->data.named_type.arguments[index];
                if (argument->kind == CM_META_ARG_LIFETIME
                    && argument->data.lifetime.kind
                        == CM_META_REGION_LATE_BOUND) {
                    uint32_t needed;

                    if (argument->data.lifetime.parameter == UINT32_MAX) {
                        valid = 0;
                    } else {
                        needed = argument->data.lifetime.parameter + 1u;
                        if (needed > requirement) requirement = needed;
                    }
                } else if (argument->kind == CM_META_ARG_TYPE) {
                    CM_META_WIRE_REQUIRE(argument->data.type);
                } else if (argument->kind == CM_META_ARG_CONST) {
                    CM_META_WIRE_REQUIRE(argument->data.constant.type);
                }
            }
        }
#undef CM_META_WIRE_REQUIRE
        if (height > (uint32_t)CM_META_MAX_TYPE_NESTING) valid = 0;
        requirements[local_index] = requirement;
        heights[local_index] = height;
    }
    cm_free(heights);
    return valid;
}

static int cm_meta_wire_function_type_generics_valid_cached(
    const CmVec *types,
    const CmVec *generics, uint32_t type_local, uint32_t value_local,
    uint32_t generic_start, uint32_t generic_count, size_t depth,
    int predicate_root, uint32_t *marks, uint32_t generation)
{
    const CmMetaWireType *type;
    uint32_t index;

    if (type_local == 0u || type_local > types->len
        || depth >= (size_t)CM_META_MAX_TYPE_NESTING) return 0;
    if (marks[type_local - 1u] == generation) return 1;
    marks[type_local - 1u] = generation;
    type = (const CmMetaWireType *)cm_vec_at_const(types, type_local - 1u);
    if (type == NULL) return 0;
#define CM_META_WIRE_FUNCTION_CHILD(child_local) \
    cm_meta_wire_function_type_generics_valid_cached(types, generics, \
        (child_local), value_local, generic_start, generic_count, \
        depth + 1u, predicate_root, marks, generation)
#define CM_META_WIRE_FUNCTION_GENERIC(local, expected_kind) do { \
        const CmMetaWireGeneric *owned_generic; \
        uint32_t owned_local; \
        owned_local = (local); \
        owned_generic = owned_local == 0u ? NULL \
            : (const CmMetaWireGeneric *)cm_vec_at_const(generics, \
                owned_local - 1u); \
        if (owned_generic == NULL \
            || owned_generic->owner_kind != CM_META_GENERIC_OWNER_VALUE \
            || owned_generic->owner != value_local \
            || owned_generic->kind != (expected_kind) \
            || generic_count == 0u || owned_local < generic_start \
            || owned_local - generic_start >= generic_count \
            || owned_generic->index != owned_local - generic_start) return 0; \
    } while (0)
    if (type->kind == CM_META_TYPE_PARAMETER) {
        CM_META_WIRE_FUNCTION_GENERIC(type->data.parameter_type.parameter,
            CM_META_GENERIC_TYPE);
        return 1;
    }
    if (type->kind == CM_META_TYPE_REFERENCE) {
        if (predicate_root && type->data.reference_type.region.kind
                == CM_META_REGION_EARLY_BOUND) return 0;
        if (type->data.reference_type.region.kind
                == CM_META_REGION_EARLY_BOUND) {
            CM_META_WIRE_FUNCTION_GENERIC(
                type->data.reference_type.region.parameter,
                CM_META_GENERIC_LIFETIME);
        }
        return CM_META_WIRE_FUNCTION_CHILD(
            type->data.reference_type.pointee);
    }
    if (type->kind == CM_META_TYPE_RAW_POINTER)
        return CM_META_WIRE_FUNCTION_CHILD(
            type->data.raw_pointer_type.pointee);
    if (type->kind == CM_META_TYPE_TUPLE) {
        for (index = 0u; index < type->data.tuple_type.element_count; ++index)
            if (!CM_META_WIRE_FUNCTION_CHILD(
                    type->data.tuple_type.elements[index])) return 0;
    } else if (type->kind == CM_META_TYPE_ARRAY) {
        if (predicate_root && type->data.array_type.length.kind
                == CM_META_CONST_PARAMETER) return 0;
        if (type->data.array_type.length.kind == CM_META_CONST_PARAMETER) {
            CM_META_WIRE_FUNCTION_GENERIC(
                type->data.array_type.length.data.parameter,
                CM_META_GENERIC_CONST);
        }
        if (!CM_META_WIRE_FUNCTION_CHILD(type->data.array_type.element)
            || !CM_META_WIRE_FUNCTION_CHILD(type->data.array_type.length.type))
            return 0;
    } else if (type->kind == CM_META_TYPE_SLICE) {
        if (!CM_META_WIRE_FUNCTION_CHILD(type->data.slice_type.element))
            return 0;
    } else if (type->kind == CM_META_TYPE_ADT
            || type->kind == CM_META_TYPE_ALIAS
            || type->kind == CM_META_TYPE_FOREIGN) {
        for (index = 0u; index < type->data.named_type.argument_count;
                ++index) {
            const CmMetaWireArg *argument;

            argument = &type->data.named_type.arguments[index];
            if (predicate_root
                && ((argument->kind == CM_META_ARG_LIFETIME
                        && argument->data.lifetime.kind
                            == CM_META_REGION_EARLY_BOUND)
                    || (argument->kind == CM_META_ARG_CONST
                        && argument->data.constant.kind
                            == CM_META_CONST_PARAMETER))) return 0;
            if (argument->kind == CM_META_ARG_LIFETIME
                && argument->data.lifetime.kind
                    == CM_META_REGION_EARLY_BOUND) {
                CM_META_WIRE_FUNCTION_GENERIC(
                    argument->data.lifetime.parameter,
                    CM_META_GENERIC_LIFETIME);
            } else if (argument->kind == CM_META_ARG_TYPE) {
                if (!CM_META_WIRE_FUNCTION_CHILD(argument->data.type))
                    return 0;
            } else if (argument->kind == CM_META_ARG_CONST) {
                if (argument->data.constant.kind == CM_META_CONST_PARAMETER) {
                    CM_META_WIRE_FUNCTION_GENERIC(
                        argument->data.constant.data.parameter,
                        CM_META_GENERIC_CONST);
                }
                if (!CM_META_WIRE_FUNCTION_CHILD(argument->data.constant.type))
                    return 0;
            }
        }
    }
#undef CM_META_WIRE_FUNCTION_CHILD
#undef CM_META_WIRE_FUNCTION_GENERIC
    return 1;
}

static int cm_meta_wire_nonpredicate_roots_valid(const CmVec *generics,
    const CmVec *items, const uint32_t *requirements)
{
    size_t index;

    for (index = 0u; index < generics->len; ++index) {
        const CmMetaWireGeneric *generic;

        generic = (const CmMetaWireGeneric *)cm_vec_at_const(generics,
            index);
        if (generic == NULL) return 0;
        if (generic->default_type != 0u
            && requirements[generic->default_type - 1u] != 0u) return 0;
    }
    for (index = 0u; index < items->len; ++index) {
        const CmMetaWireItem *item;
        uint32_t child;

        item = (const CmMetaWireItem *)cm_vec_at_const(items, index);
        if (item == NULL) return 0;
        if (item->kind == CM_META_ITEM_STRUCT
                || item->kind == CM_META_ITEM_UNION) {
            for (child = 0u; child < item->data.aggregate_item.field_count;
                    ++child) {
                if (requirements[item->data.aggregate_item.fields[child].type
                        - 1u] != 0u) return 0;
            }
        } else if (item->kind == CM_META_ITEM_ENUM) {
            for (child = 0u; child < item->data.enum_item.variant_count;
                    ++child) {
                const CmMetaWireVariant *variant;
                uint32_t field;

                variant = &item->data.enum_item.variants[child];
                for (field = 0u; field < variant->field_count; ++field) {
                    if (requirements[variant->fields[field].type - 1u]
                            != 0u) return 0;
                }
                if (variant->has_discriminant
                    && requirements[variant->discriminant_type - 1u] != 0u)
                    return 0;
            }
        } else if (item->kind == CM_META_ITEM_ALIAS
            && requirements[item->data.alias_item.target - 1u] != 0u)
            return 0;
    }
    return 1;
}

static int cm_meta_wire_mark_type_reachable(const CmVec *types,
    uint32_t local, unsigned char *reachable, size_t depth)
{
    const CmMetaWireType *type;
    uint32_t index;

    if (local == 0u || local > types->len
        || depth >= (size_t)CM_META_MAX_TYPE_NESTING) return 0;
    if (reachable[local - 1u] != 0u) return 1;
    reachable[local - 1u] = UINT8_C(1);
    type = (const CmMetaWireType *)cm_vec_at_const(types, local - 1u);
    if (type == NULL) return 0;
#define CM_META_WIRE_MARK(child_local) \
    cm_meta_wire_mark_type_reachable(types, (child_local), reachable, \
        depth + 1u)
    if (type->kind == CM_META_TYPE_REFERENCE) {
        if (!CM_META_WIRE_MARK(type->data.reference_type.pointee)) return 0;
    } else if (type->kind == CM_META_TYPE_RAW_POINTER) {
        if (!CM_META_WIRE_MARK(type->data.raw_pointer_type.pointee)) return 0;
    } else if (type->kind == CM_META_TYPE_TUPLE) {
        for (index = 0u; index < type->data.tuple_type.element_count; ++index)
            if (!CM_META_WIRE_MARK(type->data.tuple_type.elements[index]))
                return 0;
    } else if (type->kind == CM_META_TYPE_ARRAY) {
        if (!CM_META_WIRE_MARK(type->data.array_type.element)
            || !CM_META_WIRE_MARK(type->data.array_type.length.type))
            return 0;
    } else if (type->kind == CM_META_TYPE_SLICE) {
        if (!CM_META_WIRE_MARK(type->data.slice_type.element)) return 0;
    } else if (type->kind == CM_META_TYPE_ADT
            || type->kind == CM_META_TYPE_ALIAS
            || type->kind == CM_META_TYPE_FOREIGN) {
        for (index = 0u; index < type->data.named_type.argument_count;
                ++index) {
            const CmMetaWireArg *argument;

            argument = &type->data.named_type.arguments[index];
            if (argument->kind == CM_META_ARG_TYPE) {
                if (!CM_META_WIRE_MARK(argument->data.type)) return 0;
            } else if (argument->kind == CM_META_ARG_CONST
                && !CM_META_WIRE_MARK(argument->data.constant.type)) {
                return 0;
            }
        }
    }
#undef CM_META_WIRE_MARK
    return 1;
}

static int cm_meta_wire_types_reachable(const CmVec *generics,
    const CmVec *types, const CmVec *items, const CmVec *values,
    const CmVec *payloads)
{
    unsigned char *reachable;
    size_t index;
    int valid;

    reachable = (unsigned char *)cm_alloc_zeroed(types->len,
        sizeof(unsigned char));
    valid = 1;
#define CM_META_WIRE_ROOT(type_local) do { \
        if (!cm_meta_wire_mark_type_reachable(types, (type_local), \
                reachable, 0u)) valid = 0; \
    } while (0)
    for (index = 0u; valid && index < generics->len; ++index) {
        const CmMetaWireGeneric *generic;

        generic = (const CmMetaWireGeneric *)cm_vec_at_const(generics,
            index);
        if (generic == NULL) valid = 0;
        else if (generic->default_type != 0u)
            CM_META_WIRE_ROOT(generic->default_type);
    }
    for (index = 0u; valid && index < items->len; ++index) {
        const CmMetaWireItem *item;
        uint32_t child;

        item = (const CmMetaWireItem *)cm_vec_at_const(items, index);
        if (item == NULL) {
            valid = 0;
        } else if (item->kind == CM_META_ITEM_STRUCT
                || item->kind == CM_META_ITEM_UNION) {
            for (child = 0u; valid
                    && child < item->data.aggregate_item.field_count; ++child)
                CM_META_WIRE_ROOT(
                    item->data.aggregate_item.fields[child].type);
        } else if (item->kind == CM_META_ITEM_ENUM) {
            for (child = 0u; valid
                    && child < item->data.enum_item.variant_count; ++child) {
                const CmMetaWireVariant *variant;
                uint32_t field;

                variant = &item->data.enum_item.variants[child];
                for (field = 0u; valid && field < variant->field_count;
                        ++field)
                    CM_META_WIRE_ROOT(variant->fields[field].type);
                if (valid && variant->has_discriminant)
                    CM_META_WIRE_ROOT(variant->discriminant_type);
            }
        } else if (item->kind == CM_META_ITEM_ALIAS) {
            CM_META_WIRE_ROOT(item->data.alias_item.target);
        }
    }
    for (index = 0u; valid && index < values->len; ++index) {
        const CmMetaWireValue *value;
        uint32_t child;

        value = (const CmMetaWireValue *)cm_vec_at_const(values, index);
        if (value == NULL) {
            valid = 0;
        } else if (value->kind == CM_META_VALUE_FUNCTION) {
            for (child = 0u; valid
                    && child < value->data.function.parameter_count; ++child)
                CM_META_WIRE_ROOT(
                    value->data.function.parameter_types[child]);
            if (valid) CM_META_WIRE_ROOT(value->data.function.return_type);
        } else {
            CM_META_WIRE_ROOT(value->data.value.type);
        }
    }
    for (index = 0u; valid && index < payloads->len; ++index) {
        const CmMetaWireValuePredicates *payload;
        uint32_t child;

        payload = (const CmMetaWireValuePredicates *)cm_vec_at_const(
            payloads, index);
        if (payload == NULL) {
            valid = 0;
            break;
        }
        for (child = 0u; valid && child < payload->predicate_count; ++child) {
            const CmMetaWirePredicate *predicate;
            uint32_t nested;

            predicate = &payload->predicates[child];
            CM_META_WIRE_ROOT(predicate->subject);
            for (nested = 0u; valid
                    && nested < predicate->argument_count; ++nested)
                CM_META_WIRE_ROOT(predicate->arguments[nested]);
            for (nested = 0u; valid
                    && nested < predicate->equality_count; ++nested)
                CM_META_WIRE_ROOT(predicate->equality_values[nested]);
        }
        for (child = 0u; valid && child < payload->outlives_count; ++child)
            CM_META_WIRE_ROOT(payload->outlives_subjects[child]);
    }
    for (index = 0u; valid && index < types->len; ++index)
        if (reachable[index] == 0u) {
            valid = 0;
        }
#undef CM_META_WIRE_ROOT
    cm_free(reachable);
    return valid;
}

static int cm_meta_wire_predicates_canonical(const CmVec *generics,
    const CmVec *types, const CmVec *items, const CmVec *values,
    const CmVec *nominals, const CmVec *payloads,
    int allow_trait_alias_predicates)
{
    unsigned char *used;
    uint32_t *requirements;
    uint32_t *provenance_marks;
    uint32_t provenance_generation;
    size_t total_predicates;
    size_t total_availability;
    size_t total_nominal_memberships;
    size_t total_binders;
    size_t total_arguments;
    size_t total_equalities;
    size_t total_outlives;
    size_t group_index;
    int valid;

    requirements = (uint32_t *)cm_alloc_zeroed(types->len,
        sizeof(uint32_t));
    if (!cm_meta_wire_type_requirements(types, requirements)) {
        cm_free(requirements); return 0;
    }
    if (!cm_meta_wire_nonpredicate_roots_valid(generics, items,
            requirements)) {
        cm_free(requirements); return 0;
    }
    if (!cm_meta_wire_types_reachable(generics, types, items, values,
            payloads)) {
        cm_free(requirements); return 0;
    }
    used = (unsigned char *)cm_alloc_zeroed(nominals->len,
        sizeof(unsigned char));
    provenance_marks = (uint32_t *)cm_alloc_zeroed(types->len,
        sizeof(uint32_t));
    provenance_generation = 0u;
    total_predicates = 0u;
    total_availability = 0u;
    total_nominal_memberships = 0u;
    total_binders = 0u;
    total_arguments = 0u;
    total_equalities = 0u;
    total_outlives = 0u;
    valid = 1;
    for (group_index = 0u; valid && group_index < values->len;
            ++group_index) {
        const CmMetaWireValue *value;
        uint32_t index;

        value = (const CmMetaWireValue *)cm_vec_at_const(values,
            group_index);
        if (value == NULL) valid = 0;
        else if (value->kind == CM_META_VALUE_FUNCTION) {
            provenance_generation += 1u;
            for (index = 0u; valid
                    && index < value->data.function.parameter_count; ++index) {
                if (requirements[value->data.function.parameter_types[index]
                            - 1u] != 0u
                    || !cm_meta_wire_function_type_generics_valid_cached(
                        types,
                        generics,
                        value->data.function.parameter_types[index],
                        (uint32_t)group_index + 1u,
                        value->data.function.generic_start,
                        value->data.function.generic_count, 0u, 0,
                        provenance_marks, provenance_generation))
                    valid = 0;
            }
            if (valid && (requirements[
                    value->data.function.return_type - 1u] != 0u
                || !cm_meta_wire_function_type_generics_valid_cached(types,
                    generics, value->data.function.return_type,
                    (uint32_t)group_index + 1u,
                    value->data.function.generic_start,
                    value->data.function.generic_count, 0u, 0,
                    provenance_marks, provenance_generation)))
                valid = 0;
        } else if (requirements[value->data.value.type - 1u] != 0u) {
            valid = 0;
        }
    }
    for (group_index = 0u; valid && group_index < payloads->len;
            ++group_index) {
        const CmMetaWireValuePredicates *payload;
        const CmMetaWireValue *value;
        uint32_t index;

        payload = (const CmMetaWireValuePredicates *)cm_vec_at_const(
            payloads, group_index);
        value = payload == NULL ? NULL : (const CmMetaWireValue *)
            cm_vec_at_const(values, payload->value_local - 1u);
        if (payload == NULL || value == NULL
            || value->kind != CM_META_VALUE_FUNCTION
            || !cm_size_add(total_predicates, payload->predicate_count,
                &total_predicates)
            || !cm_size_add(total_availability, payload->availability_count,
                &total_availability)
            || !cm_size_add(total_nominal_memberships,
                payload->nominal_reference_count,
                &total_nominal_memberships)
            || !cm_size_add(total_outlives, payload->outlives_count,
                &total_outlives)
            || total_predicates > CM_META_MAX_PREDICATES
            || total_availability > CM_META_MAX_PREDICATES
            || total_nominal_memberships
                > CM_META_MAX_NOMINAL_REFERENCES
            || total_outlives > CM_META_MAX_PREDICATES) {
            valid = 0;
            break;
        }
        provenance_generation += 1u;
        for (index = 0u; valid
                && index < payload->nominal_reference_count; ++index)
            used[payload->nominal_references[index] - 1u] = UINT8_C(1);
        for (index = 0u; valid && index < payload->availability_count;
                ++index) {
            if (!cm_meta_wire_local_present(payload->nominal_references,
                    payload->nominal_reference_count,
                    payload->availability_traits[index])
                || !cm_meta_wire_local_present(payload->nominal_references,
                    payload->nominal_reference_count,
                    payload->availability_associated[index])
                || (index != 0u
                    && (payload->availability_traits[index - 1u]
                            > payload->availability_traits[index]
                        || (payload->availability_traits[index - 1u]
                                == payload->availability_traits[index]
                            && payload->availability_associated[index - 1u]
                                >= payload->availability_associated[index]))))
                valid = 0;
        }
        for (index = 0u; valid && index < payload->predicate_count; ++index) {
            const CmMetaWirePredicate *predicate;
            const CmMetaWireNominal *direct;
            uint32_t child;

            predicate = &payload->predicates[index];
            if (!cm_size_add(total_binders, predicate->binder_count,
                    &total_binders)
                || !cm_size_add(total_arguments, predicate->argument_count,
                    &total_arguments)
                || !cm_size_add(total_equalities, predicate->equality_count,
                    &total_equalities)
                || total_binders > CM_META_MAX_GENERICS
                || total_arguments > CM_META_MAX_GENERICS
                || total_equalities > CM_META_MAX_PREDICATES) {
                valid = 0;
                break;
            }
            direct = (const CmMetaWireNominal *)cm_vec_at_const(nominals,
                predicate->trait_reference - 1u);
            if (direct == NULL
                || (direct->kind != CM_META_NOMINAL_TRAIT
                    && (!allow_trait_alias_predicates
                        || direct->kind != CM_META_NOMINAL_TRAIT_ALIAS))
                || (direct->kind == CM_META_NOMINAL_TRAIT_ALIAS
                    && predicate->equality_count != 0u)
                || !cm_meta_predicate_modifier_from_wire(predicate->modifier,
                    NULL)
                || (index != 0u
                    && (payload->predicates[index - 1u].trait_reference
                            > predicate->trait_reference
                        || (payload->predicates[index - 1u]
                                .trait_reference
                                == predicate->trait_reference
                            && payload->predicates[index - 1u].subject
                                >= predicate->subject)))
                || !cm_meta_wire_local_present(payload->nominal_references,
                    payload->nominal_reference_count,
                    predicate->trait_reference)
                || requirements[predicate->subject - 1u] != 0u
                || !cm_meta_wire_function_type_generics_valid_cached(types,
                    generics, predicate->subject, payload->value_local,
                    value->data.function.generic_start,
                    value->data.function.generic_count, 0u, 1,
                    provenance_marks, provenance_generation))
                valid = 0;
            for (child = 0u; valid && child < direct->generic_count; ++child)
                if (direct->generic_kinds[child] != CM_META_GENERIC_TYPE)
                    valid = 0;
            if (valid && predicate->binder_count > 1u) {
                CmMetaWireName *sorted_names;

                sorted_names = (CmMetaWireName *)cm_alloc(
                    (size_t)predicate->binder_count
                        * sizeof(CmMetaWireName));
                memcpy(sorted_names, predicate->binder_names,
                    (size_t)predicate->binder_count
                        * sizeof(CmMetaWireName));
                qsort(sorted_names, predicate->binder_count,
                    sizeof(CmMetaWireName), cm_meta_wire_name_compare_qsort);
                for (child = 1u; valid && child < predicate->binder_count;
                        ++child)
                    if (cm_meta_name_equal(sorted_names[child - 1u],
                            sorted_names[child])) valid = 0;
                cm_free(sorted_names);
            }
            for (child = 0u; valid && child < predicate->argument_count;
                    ++child) {
                if (requirements[predicate->arguments[child] - 1u]
                        > predicate->binder_count
                    || !cm_meta_wire_function_type_generics_valid_cached(
                        types,
                        generics, predicate->arguments[child],
                        payload->value_local,
                        value->data.function.generic_start,
                        value->data.function.generic_count, 0u, 1,
                        provenance_marks, provenance_generation))
                    valid = 0;
            }
            for (child = 0u; valid && child < predicate->equality_count;
                    ++child) {
                if (!cm_meta_wire_local_present(payload->nominal_references,
                        payload->nominal_reference_count,
                        predicate->equality_associated[child])
                    || (child != 0u
                        && predicate->equality_associated[child - 1u]
                            >= predicate->equality_associated[child])
                    || requirements[predicate->equality_values[child] - 1u]
                        > predicate->binder_count
                    || !cm_meta_wire_function_type_generics_valid_cached(
                        types,
                        generics, predicate->equality_values[child],
                        payload->value_local,
                        value->data.function.generic_start,
                        value->data.function.generic_count, 0u, 1,
                        provenance_marks, provenance_generation))
                    valid = 0;
                if (!cm_meta_wire_pair_present(payload->availability_traits,
                        payload->availability_associated,
                        payload->availability_count,
                        predicate->trait_reference,
                        predicate->equality_associated[child])) valid = 0;
            }
        }
        for (index = 0u; valid && index < payload->outlives_count; ++index) {
            const CmMetaWireType *subject_type;

            subject_type = (const CmMetaWireType *)cm_vec_at_const(types,
                payload->outlives_subjects[index] - 1u);
            if ((index != 0u
                    && payload->outlives_subjects[index - 1u]
                        >= payload->outlives_subjects[index])
                || subject_type == NULL
                || subject_type->kind != CM_META_TYPE_PARAMETER
                || requirements[payload->outlives_subjects[index] - 1u]
                    != 0u
                || !cm_meta_wire_function_type_generics_valid_cached(types,
                    generics, payload->outlives_subjects[index],
                    payload->value_local,
                    value->data.function.generic_start,
                    value->data.function.generic_count, 0u, 1,
                    provenance_marks, provenance_generation))
                valid = 0;
        }
    }
    for (group_index = 0u; valid && group_index < nominals->len;
            ++group_index)
        if (used[group_index] == 0u) valid = 0;
    cm_free(used);
    cm_free(provenance_marks);
    cm_free(requirements);
    return valid;
}

static int cm_meta_decode_value_predicates(
    const CmHirMetadataSection *section, const CmVec *values,
    const CmVec *nominals, uint32_t type_count, int has_modifiers,
    int allow_trait_alias_predicates, CmVec *payloads)
{
    CmHirMetadataReader reader;
    uint32_t count;
    uint32_t value_index;
    size_t total_nominal_memberships;
    size_t total_availability;
    size_t total_predicates;
    size_t total_binders;
    size_t total_arguments;
    size_t total_equalities;
    size_t total_outlives;

    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (cm_hir_metadata_read_u32(&reader, &count) != CM_HIR_METADATA_OK
        || count > (uint32_t)values->len) return 0;
    total_nominal_memberships = 0u;
    total_availability = 0u;
    total_predicates = 0u;
    total_binders = 0u;
    total_arguments = 0u;
    total_equalities = 0u;
    total_outlives = 0u;
    for (value_index = 0u; value_index < count; ++value_index) {
        const CmMetaWireValue *value;
        CmMetaWireValuePredicates payload;
        uint32_t index;

        memset(&payload, 0, sizeof(payload));
        if (cm_hir_metadata_read_u32(&reader, &payload.value_local)
                != CM_HIR_METADATA_OK
            || payload.value_local == 0u
            || payload.value_local > values->len
            || (value_index != 0u && ((const CmMetaWireValuePredicates *)
                cm_vec_at_const(payloads, value_index - 1u))->value_local
                    >= payload.value_local)) return 0;
        value = (const CmMetaWireValue *)cm_vec_at_const(values,
            payload.value_local - 1u);
#define CM_META_READ_COUNT(field, limit) \
        (cm_hir_metadata_read_u32(&reader, &(field)) == CM_HIR_METADATA_OK \
            && (field) <= (limit))
        if (!CM_META_READ_COUNT(payload.nominal_reference_count,
                CM_META_MAX_NOMINAL_REFERENCES)
            || !cm_size_add(total_nominal_memberships,
                payload.nominal_reference_count, &total_nominal_memberships)
            || total_nominal_memberships
                > CM_META_MAX_NOMINAL_REFERENCES) return 0;
        payload.nominal_references = payload.nominal_reference_count == 0u
            ? NULL : (uint32_t *)cm_alloc((size_t)
                payload.nominal_reference_count * sizeof(uint32_t));
        for (index = 0u; index < payload.nominal_reference_count; ++index) {
            if (cm_hir_metadata_read_u32(&reader,
                    &payload.nominal_references[index]) != CM_HIR_METADATA_OK
                || payload.nominal_references[index] == 0u
                || payload.nominal_references[index] > nominals->len
                || (index != 0u && payload.nominal_references[index - 1u]
                    >= payload.nominal_references[index])) goto invalid;
        }
        if (!CM_META_READ_COUNT(payload.availability_count,
                CM_META_MAX_PREDICATES)
            || !cm_size_add(total_availability, payload.availability_count,
                &total_availability)
            || total_availability > CM_META_MAX_PREDICATES) goto invalid;
        payload.availability_traits = payload.availability_count == 0u
            ? NULL : (uint32_t *)cm_alloc((size_t)payload.availability_count
                * sizeof(uint32_t));
        payload.availability_associated = payload.availability_count == 0u
            ? NULL : (uint32_t *)cm_alloc((size_t)payload.availability_count
                * sizeof(uint32_t));
        for (index = 0u; index < payload.availability_count; ++index) {
            const CmMetaWireNominal *direct;
            const CmMetaWireNominal *associated;

            if (cm_hir_metadata_read_u32(&reader,
                    &payload.availability_traits[index])
                    != CM_HIR_METADATA_OK
                || cm_hir_metadata_read_u32(&reader,
                    &payload.availability_associated[index])
                    != CM_HIR_METADATA_OK
                || payload.availability_traits[index] == 0u
                || payload.availability_traits[index] > nominals->len
                || payload.availability_associated[index] == 0u
                || payload.availability_associated[index] > nominals->len)
                goto invalid;
            direct = (const CmMetaWireNominal *)cm_vec_at_const(nominals,
                payload.availability_traits[index] - 1u);
            associated = (const CmMetaWireNominal *)cm_vec_at_const(nominals,
                payload.availability_associated[index] - 1u);
            if (direct == NULL || direct->kind != CM_META_NOMINAL_TRAIT
                || associated == NULL
                || associated->kind
                    != CM_META_NOMINAL_ASSOCIATED_TYPE) goto invalid;
        }
        if (!CM_META_READ_COUNT(payload.predicate_count,
                CM_META_MAX_PREDICATES)
            || !cm_size_add(total_predicates, payload.predicate_count,
                &total_predicates)
            || total_predicates > CM_META_MAX_PREDICATES) goto invalid;
        payload.predicates = payload.predicate_count == 0u ? NULL
            : (CmMetaWirePredicate *)cm_alloc_zeroed(
                (size_t)payload.predicate_count,
                sizeof(CmMetaWirePredicate));
        for (index = 0u; index < payload.predicate_count; ++index) {
            CmMetaWirePredicate *predicate;
            const CmMetaWireNominal *direct;
            uint32_t child;

            predicate = &payload.predicates[index];
            if (cm_hir_metadata_read_u32(&reader, &predicate->subject)
                    != CM_HIR_METADATA_OK
                || predicate->subject == 0u || predicate->subject > type_count
                || cm_hir_metadata_read_u32(&reader,
                    &predicate->trait_reference) != CM_HIR_METADATA_OK
                || predicate->trait_reference == 0u
                || predicate->trait_reference > nominals->len)
                goto invalid;
            direct = (const CmMetaWireNominal *)cm_vec_at_const(nominals,
                predicate->trait_reference - 1u);
            if (direct == NULL
                || (direct->kind != CM_META_NOMINAL_TRAIT
                    && (!allow_trait_alias_predicates
                        || direct->kind != CM_META_NOMINAL_TRAIT_ALIAS)))
                goto invalid;
            predicate->modifier = CM_META_PREDICATE_REQUIRED;
            if (has_modifiers
                && (cm_hir_metadata_read_u8(&reader, &predicate->modifier)
                        != CM_HIR_METADATA_OK
                    || !cm_meta_predicate_modifier_from_wire(
                        predicate->modifier, NULL)))
                goto invalid;
            if (!CM_META_READ_COUNT(predicate->binder_count,
                    CM_META_MAX_GENERICS)
                || !cm_size_add(total_binders, predicate->binder_count,
                    &total_binders)
                || total_binders > CM_META_MAX_GENERICS) goto invalid;
            predicate->binder_names = predicate->binder_count == 0u ? NULL
                : (CmMetaWireName *)cm_alloc_zeroed(
                    (size_t)predicate->binder_count,
                    sizeof(CmMetaWireName));
            for (child = 0u; child < predicate->binder_count; ++child) {
                if (!cm_meta_read_string(&reader,
                        &predicate->binder_names[child])) goto invalid;
            }
            if (!CM_META_READ_COUNT(predicate->argument_count,
                    CM_META_MAX_GENERICS)
                || !cm_size_add(total_arguments, predicate->argument_count,
                    &total_arguments)
                || total_arguments > CM_META_MAX_GENERICS) goto invalid;
            predicate->arguments = predicate->argument_count == 0u ? NULL
                : (uint32_t *)cm_alloc((size_t)predicate->argument_count
                    * sizeof(uint32_t));
            for (child = 0u; child < predicate->argument_count; ++child) {
                if (cm_hir_metadata_read_u32(&reader,
                        &predicate->arguments[child]) != CM_HIR_METADATA_OK
                    || predicate->arguments[child] == 0u
                    || predicate->arguments[child] > type_count) goto invalid;
            }
            if (predicate->argument_count != direct->generic_count
                || !CM_META_READ_COUNT(predicate->equality_count,
                    CM_META_MAX_PREDICATES)
                || !cm_size_add(total_equalities, predicate->equality_count,
                    &total_equalities)
                || total_equalities > CM_META_MAX_PREDICATES
                || (direct->kind == CM_META_NOMINAL_TRAIT_ALIAS
                    && predicate->equality_count != 0u)) goto invalid;
            predicate->equality_associated = predicate->equality_count == 0u
                ? NULL : (uint32_t *)cm_alloc((size_t)
                    predicate->equality_count * sizeof(uint32_t));
            predicate->equality_values = predicate->equality_count == 0u
                ? NULL : (uint32_t *)cm_alloc((size_t)
                    predicate->equality_count * sizeof(uint32_t));
            for (child = 0u; child < predicate->equality_count; ++child) {
                const CmMetaWireNominal *associated;

                if (cm_hir_metadata_read_u32(&reader,
                        &predicate->equality_associated[child])
                        != CM_HIR_METADATA_OK
                    || cm_hir_metadata_read_u32(&reader,
                        &predicate->equality_values[child])
                        != CM_HIR_METADATA_OK
                    || predicate->equality_associated[child] == 0u
                    || predicate->equality_associated[child] > nominals->len
                    || predicate->equality_values[child] == 0u
                    || predicate->equality_values[child] > type_count)
                    goto invalid;
                associated = (const CmMetaWireNominal *)cm_vec_at_const(
                    nominals, predicate->equality_associated[child] - 1u);
                if (associated == NULL || associated->kind
                        != CM_META_NOMINAL_ASSOCIATED_TYPE
                    || associated->generic_count != 0u) goto invalid;
            }
        }
        if (!CM_META_READ_COUNT(payload.outlives_count,
                CM_META_MAX_PREDICATES)
            || !cm_size_add(total_outlives, payload.outlives_count,
                &total_outlives)
            || total_outlives > CM_META_MAX_PREDICATES) goto invalid;
        payload.outlives_subjects = payload.outlives_count == 0u ? NULL
            : (uint32_t *)cm_alloc((size_t)payload.outlives_count
                * sizeof(uint32_t));
        for (index = 0u; index < payload.outlives_count; ++index) {
            if (cm_hir_metadata_read_u32(&reader,
                    &payload.outlives_subjects[index]) != CM_HIR_METADATA_OK
                || payload.outlives_subjects[index] == 0u
                || payload.outlives_subjects[index] > type_count) goto invalid;
        }
        if (value == NULL || value->kind != CM_META_VALUE_FUNCTION
            || (payload.nominal_reference_count == 0u
                && payload.availability_count == 0u
                && payload.predicate_count == 0u
                && payload.outlives_count == 0u)) goto invalid;
        (void)cm_vec_push(payloads, &payload);
        continue;
invalid:
        {
            CmVec one;
            cm_vec_init(&one, sizeof(payload));
            (void)cm_vec_push(&one, &payload);
            cm_meta_wire_value_predicates_destroy(&one);
        }
        return 0;
#undef CM_META_READ_COUNT
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
    const CmVec *types, unsigned char *item_states,
    uint32_t *item_heights, unsigned char *type_states,
    uint32_t *type_heights, size_t traversal_depth);

static int cm_meta_alias_type_acyclic(uint32_t type_local,
    const CmVec *items, const CmVec *types, unsigned char *item_states,
    uint32_t *item_heights, unsigned char *type_states,
    uint32_t *type_heights, size_t traversal_depth)
{
    const CmMetaWireType *type;
    size_t state_index;
    uint32_t height;
    uint32_t index;

    type = type_local == 0u || (size_t)type_local > types->len
        || traversal_depth >= (size_t)CM_META_MAX_TYPE_NESTING ? NULL
        : (const CmMetaWireType *)cm_vec_at_const(types,
            (size_t)(type_local - 1u));
    if (type == NULL) return 0;
    state_index = (size_t)type_local - 1u;
    if (type_states[state_index] == UINT8_C(1)) return 0;
    if (type_states[state_index] == UINT8_C(2)) {
        return (size_t)type_heights[state_index]
            < (size_t)CM_META_MAX_TYPE_NESTING - traversal_depth;
    }
    type_states[state_index] = UINT8_C(1);
    height = 0u;
#define CM_META_ALIAS_CHILD(child_local) do { \
        uint32_t cm_child_local; \
        uint32_t cm_child_height; \
        cm_child_local = (child_local); \
        if (!cm_meta_alias_type_acyclic(cm_child_local, items, types, \
                item_states, item_heights, type_states, type_heights, \
                traversal_depth + 1u)) \
            return 0; \
        cm_child_height = type_heights[(size_t)cm_child_local - 1u]; \
        if (cm_child_height == UINT32_MAX) return 0; \
        cm_child_height += 1u; \
        if (cm_child_height > height) height = cm_child_height; \
    } while (0)
    switch (type->kind) {
    case CM_META_TYPE_REFERENCE:
        CM_META_ALIAS_CHILD(type->data.reference_type.pointee);
        break;
    case CM_META_TYPE_RAW_POINTER:
        CM_META_ALIAS_CHILD(type->data.raw_pointer_type.pointee);
        break;
    case CM_META_TYPE_TUPLE:
        for (index = 0u; index < type->data.tuple_type.element_count;
                ++index) {
            CM_META_ALIAS_CHILD(type->data.tuple_type.elements[index]);
        }
        break;
    case CM_META_TYPE_ARRAY:
        CM_META_ALIAS_CHILD(type->data.array_type.element);
        CM_META_ALIAS_CHILD(type->data.array_type.length.type);
        break;
    case CM_META_TYPE_SLICE:
        CM_META_ALIAS_CHILD(type->data.slice_type.element);
        break;
    case CM_META_TYPE_ADT:
    case CM_META_TYPE_ALIAS:
    case CM_META_TYPE_FOREIGN:
        for (index = 0u; index < type->data.named_type.argument_count;
                ++index) {
            const CmMetaWireArg *argument;

            argument = &type->data.named_type.arguments[index];
            if (argument->kind == CM_META_ARG_TYPE)
                CM_META_ALIAS_CHILD(argument->data.type);
            if (argument->kind == CM_META_ARG_CONST)
                CM_META_ALIAS_CHILD(argument->data.constant.type);
        }
        if (type->kind == CM_META_TYPE_ALIAS
            && !cm_meta_alias_acyclic(type->data.named_type.item, items,
                types, item_states, item_heights, type_states,
                type_heights, traversal_depth + 1u)) return 0;
        if (type->kind == CM_META_TYPE_ALIAS) {
            uint32_t nested_height;

            nested_height = item_heights[type->data.named_type.item - 1u];
            if (nested_height == UINT32_MAX) return 0;
            nested_height += 1u;
            if (nested_height > height) height = nested_height;
        }
        break;
    default:
        break;
    }
#undef CM_META_ALIAS_CHILD
    type_heights[state_index] = height;
    type_states[state_index] = UINT8_C(2);
    return 1;
}

static int cm_meta_alias_acyclic(uint32_t item_local, const CmVec *items,
    const CmVec *types, unsigned char *item_states,
    uint32_t *item_heights, unsigned char *type_states,
    uint32_t *type_heights, size_t traversal_depth)
{
    const CmMetaWireItem *item;

    item = item_local == 0u || (size_t)item_local > items->len
            || traversal_depth >= (size_t)CM_META_MAX_TYPE_NESTING ? NULL
        : (const CmMetaWireItem *)cm_vec_at_const(items,
            (size_t)(item_local - 1u));
    if (item == NULL || item->kind != CM_META_ITEM_ALIAS) return 0;
    if (item_states[item_local - 1u] == UINT8_C(1)) return 0;
    if (item_states[item_local - 1u] == UINT8_C(2)) {
        return (size_t)item_heights[item_local - 1u]
            < (size_t)CM_META_MAX_TYPE_NESTING - traversal_depth;
    }
    item_states[item_local - 1u] = UINT8_C(1);
    if (!cm_meta_alias_type_acyclic(item->data.alias_item.target, items,
            types, item_states, item_heights, type_states, type_heights,
            traversal_depth + 1u)) return 0;
    if (type_heights[item->data.alias_item.target - 1u] == UINT32_MAX)
        return 0;
    item_heights[item_local - 1u] = type_heights[
        item->data.alias_item.target - 1u] + 1u;
    item_states[item_local - 1u] = UINT8_C(2);
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
    unsigned char *alias_item_states;
    uint32_t *alias_item_heights;
    unsigned char *alias_type_states;
    uint32_t *alias_type_heights;
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
    alias_item_states = (unsigned char *)cm_alloc_zeroed(items->len,
        sizeof(unsigned char));
    alias_item_heights = (uint32_t *)cm_alloc_zeroed(items->len,
        sizeof(uint32_t));
    alias_type_states = (unsigned char *)cm_alloc_zeroed(types->len,
        sizeof(unsigned char));
    alias_type_heights = (uint32_t *)cm_alloc_zeroed(types->len,
        sizeof(uint32_t));
    valid = 1;
    for (index = 0u; valid && index < items->len; ++index) {
        const CmMetaWireItem *item;

        item = (const CmMetaWireItem *)cm_vec_at_const(items, index);
        if (item != NULL && item->kind == CM_META_ITEM_ALIAS)
            valid = cm_meta_alias_acyclic((uint32_t)(index + 1u), items,
                types, alias_item_states, alias_item_heights,
                alias_type_states, alias_type_heights, 0u);
    }
    cm_free(alias_type_heights);
    cm_free(alias_type_states);
    cm_free(alias_item_heights);
    cm_free(alias_item_states);
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
    if (wire->kind == CM_META_REGION_LATE_BOUND) {
        out_region->kind = CM_HIR_REGION_LATE_BOUND;
        out_region->data.binder_index = wire->parameter;
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
    CmSourceId metadata_source, int semantic, int declaration,
    uint16_t declaration_minor)
{
    CmHirMetadataArtifactResult result;
    CmHirMetadataEnvelope envelope;
    CmHirMetadataReader section_reader;
    CmHirMetadataSection sections[9];
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
    CmVec nominals;
    CmVec value_predicates;
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
    CmHirDefId *runtime_nominals;
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
    int declaration_v24;

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
        (uint16_t)(declaration ? declaration_minor
            : (semantic ? CM_HIR_METADATA_SEMANTIC_MINOR
                : CM_HIR_METADATA_MINOR)), &envelope);
    if (codec_status != CM_HIR_METADATA_OK) {
        result.status = cm_meta_codec_status(codec_status);
        return result;
    }
    cm_hir_metadata_reader_init(&section_reader, envelope.payload,
        envelope.payload_length);
    declaration_v24 = declaration
        && (declaration_minor == CM_HIR_METADATA_DECLARATION_MINOR
            || declaration_minor
                == CM_HIR_METADATA_DECLARATION_MODIFIER_MINOR
            || declaration_minor
                == CM_HIR_METADATA_DECLARATION_PREDICATE_MINOR);
    for (index = 0u; index < (declaration_v24 ? 9u
            : ((semantic || declaration) ? 7u : 6u));
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
        || (declaration_v24
            && (!cm_meta_section_tag_is(&sections[7],
                    cm_meta_tag_nominal_references)
                || !cm_meta_section_tag_is(&sections[8],
                    cm_meta_tag_predicates)))
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
    cm_vec_init(&nominals, sizeof(CmMetaWireNominal));
    cm_vec_init(&value_predicates, sizeof(CmMetaWireValuePredicates));
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
            &types, declaration_v24)
        || types.len != (size_t)type_count
        || !cm_meta_decode_items(&sections[4], (uint32_t)modules.len,
            (uint32_t)generics.len, type_count, &items)
        || items.len != (size_t)item_count
        || (declaration && !cm_meta_decode_values(&sections[5], type_count,
            (uint32_t)generics.len, &values))
        || (declaration && values.len != (size_t)value_count)
        || (declaration_v24 && !cm_meta_decode_nominals(&sections[7],
            (uint32_t)modules.len, &nominals))
        || (declaration_v24 && !cm_meta_decode_value_predicates(&sections[8],
            &values, &nominals, type_count,
            declaration_minor == CM_HIR_METADATA_DECLARATION_MINOR
                || declaration_minor
                    == CM_HIR_METADATA_DECLARATION_MODIFIER_MINOR,
            declaration_minor == CM_HIR_METADATA_DECLARATION_MINOR,
            &value_predicates))
        || (declaration_v24 && !cm_meta_wire_predicates_canonical(&generics,
            &types, &items, &values, &nominals, &value_predicates,
            declaration_minor == CM_HIR_METADATA_DECLARATION_MINOR))
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
    runtime_nominals = (CmHirDefId *)cm_alloc_zeroed(nominals.len,
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

    for (index = 0u; index < (uint32_t)nominals.len; ++index) {
        CmMetaWireNominal *wire_nominal;
        CmHirItemKind item_kind;

        wire_nominal = (CmMetaWireNominal *)cm_vec_at(&nominals, index);
        item_kind = wire_nominal != NULL && wire_nominal->kind
                == CM_META_NOMINAL_TRAIT
            ? CM_HIR_ITEM_TRAIT
            : (wire_nominal != NULL && wire_nominal->kind
                    == CM_META_NOMINAL_TRAIT_ALIAS
                ? CM_HIR_ITEM_TRAIT_ALIAS : CM_HIR_ITEM_TYPE_ALIAS);
        if (wire_nominal == NULL
            || cm_hir_reserve_item_definition_as(context, crate_id,
                item_kind, span, &runtime_nominals[index]) != CM_HIR_OK) {
            result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR;
            goto rollback;
        }
        wire_nominal->runtime_definition = runtime_nominals[index];
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
        const CmMetaWireValuePredicates *wire_predicates;
        CmHirLibraryValue value;
        CmHirTypeId *parameter_types;
        CmHirLibraryNominalReference *nominal_references;
        CmHirGenericParamKind **nominal_schemas;
        CmHirLibraryAssociatedAvailability *availability;
        CmHirTraitPredicate *predicates;
        CmHirGenericArg **predicate_arguments;
        CmHirAssociatedTypeEquality **predicate_equalities;
        CmInternId **predicate_lifetimes;
        CmHirOutlivesPredicate *outlives;
        uint32_t parameter;

        wire_value = (const CmMetaWireValue *)cm_vec_at_const(&values,
            index);
        memset(&value, 0, sizeof(value));
        parameter_types = NULL;
        nominal_references = NULL;
        nominal_schemas = NULL;
        availability = NULL;
        predicates = NULL;
        predicate_arguments = NULL;
        predicate_equalities = NULL;
        predicate_lifetimes = NULL;
        outlives = NULL;
        wire_predicates = cm_meta_wire_value_payload(&value_predicates,
            index + 1u);
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
            if (wire_predicates != NULL) {
                nominal_references = wire_predicates
                        ->nominal_reference_count == 0u ? NULL
                    : (CmHirLibraryNominalReference *)cm_alloc_zeroed(
                        (size_t)wire_predicates->nominal_reference_count,
                        sizeof(CmHirLibraryNominalReference));
                nominal_schemas = wire_predicates
                        ->nominal_reference_count == 0u ? NULL
                    : (CmHirGenericParamKind **)cm_alloc_zeroed(
                        (size_t)wire_predicates->nominal_reference_count,
                        sizeof(CmHirGenericParamKind *));
                for (parameter = 0u; parameter
                        < wire_predicates->nominal_reference_count;
                        ++parameter) {
                    const CmMetaWireNominal *wire_nominal;
                    uint32_t generic;

                    wire_nominal = (const CmMetaWireNominal *)cm_vec_at_const(
                        &nominals, wire_predicates
                            ->nominal_references[parameter] - 1u);
                    nominal_references[parameter].definition =
                        wire_nominal->runtime_definition;
                    nominal_references[parameter].owner_module =
                        cm_hir_get_module(context,
                            runtime_modules[wire_nominal->owner - 1u])
                            ->definition;
                    nominal_references[parameter].name.bytes =
                        wire_nominal->name.bytes;
                    nominal_references[parameter].name.length =
                        wire_nominal->name.length;
                    nominal_references[parameter].use =
                        CM_HIR_LIBRARY_REFERENCE_ONLY;
                    nominal_references[parameter].kind = wire_nominal->kind
                            == CM_META_NOMINAL_TRAIT
                        ? CM_HIR_LIBRARY_NOMINAL_TRAIT
                        : (wire_nominal->kind
                                == CM_META_NOMINAL_TRAIT_ALIAS
                            ? CM_HIR_LIBRARY_NOMINAL_TRAIT_ALIAS
                            : CM_HIR_LIBRARY_NOMINAL_ASSOCIATED_TYPE);
                    nominal_references[parameter].declaring_trait =
                        wire_nominal->declaring_trait == 0u
                            ? cm_hir_def_id_none()
                            : runtime_nominals[
                                wire_nominal->declaring_trait - 1u];
                    nominal_schemas[parameter] = wire_nominal->generic_count
                            == 0u ? NULL
                        : (CmHirGenericParamKind *)cm_alloc(
                            (size_t)wire_nominal->generic_count
                                * sizeof(CmHirGenericParamKind));
                    for (generic = 0u; generic < wire_nominal->generic_count;
                            ++generic) {
                        nominal_schemas[parameter][generic] =
                            wire_nominal->generic_kinds[generic]
                                    == CM_META_GENERIC_LIFETIME
                                ? CM_HIR_GENERIC_LIFETIME
                                : (wire_nominal->generic_kinds[generic]
                                        == CM_META_GENERIC_CONST
                                    ? CM_HIR_GENERIC_CONST
                                    : CM_HIR_GENERIC_TYPE);
                    }
                    nominal_references[parameter].generic_parameter_kinds =
                        nominal_schemas[parameter];
                    nominal_references[parameter].generic_parameter_count =
                        wire_nominal->generic_count;
                }
                availability = wire_predicates->availability_count == 0u
                    ? NULL : (CmHirLibraryAssociatedAvailability *)
                        cm_alloc_zeroed((size_t)wire_predicates
                            ->availability_count,
                            sizeof(CmHirLibraryAssociatedAvailability));
                for (parameter = 0u; parameter
                        < wire_predicates->availability_count; ++parameter) {
                    availability[parameter].direct_trait = runtime_nominals[
                        wire_predicates->availability_traits[parameter] - 1u];
                    availability[parameter].associated_type =
                        runtime_nominals[wire_predicates
                            ->availability_associated[parameter] - 1u];
                }
                predicates = wire_predicates->predicate_count == 0u ? NULL
                    : (CmHirTraitPredicate *)cm_alloc_zeroed(
                        (size_t)wire_predicates->predicate_count,
                        sizeof(CmHirTraitPredicate));
                predicate_arguments = wire_predicates->predicate_count == 0u
                    ? NULL : (CmHirGenericArg **)cm_alloc_zeroed(
                        (size_t)wire_predicates->predicate_count,
                        sizeof(CmHirGenericArg *));
                predicate_equalities = wire_predicates->predicate_count == 0u
                    ? NULL : (CmHirAssociatedTypeEquality **)cm_alloc_zeroed(
                        (size_t)wire_predicates->predicate_count,
                        sizeof(CmHirAssociatedTypeEquality *));
                predicate_lifetimes = wire_predicates->predicate_count == 0u
                    ? NULL : (CmInternId **)cm_alloc_zeroed(
                        (size_t)wire_predicates->predicate_count,
                        sizeof(CmInternId *));
                for (parameter = 0u; parameter
                        < wire_predicates->predicate_count; ++parameter) {
                    const CmMetaWirePredicate *wire_predicate;
                    uint32_t child;

                    wire_predicate = &wire_predicates->predicates[parameter];
                    predicates[parameter].subject = runtime_types[
                        wire_predicate->subject - 1u];
                    predicates[parameter].trait_type.definition =
                        runtime_nominals[
                            wire_predicate->trait_reference - 1u];
                    predicate_arguments[parameter] = (CmHirGenericArg *)
                        (wire_predicate->argument_count == 0u ? NULL
                        : cm_alloc_zeroed(
                            (size_t)wire_predicate->argument_count,
                            sizeof(CmHirGenericArg)));
                    predicates[parameter].trait_type.arguments =
                        predicate_arguments[parameter];
                    predicates[parameter].trait_type.argument_count =
                        wire_predicate->argument_count;
                    for (child = 0u; child < wire_predicate->argument_count;
                            ++child) {
                        predicate_arguments[parameter][child].kind =
                            CM_HIR_GENERIC_ARG_TYPE;
                        predicate_arguments[parameter][child].data.type =
                            runtime_types[wire_predicate->arguments[child]
                                - 1u];
                    }
                    predicate_equalities[parameter] =
                        wire_predicate->equality_count == 0u ? NULL
                        : (CmHirAssociatedTypeEquality *)cm_alloc_zeroed(
                            (size_t)wire_predicate->equality_count,
                            sizeof(CmHirAssociatedTypeEquality));
                    predicates[parameter].equalities =
                        predicate_equalities[parameter];
                    predicates[parameter].equality_count =
                        wire_predicate->equality_count;
                    for (child = 0u; child < wire_predicate->equality_count;
                            ++child) {
                        predicate_equalities[parameter][child]
                            .associated_type = runtime_nominals[
                                wire_predicate->equality_associated[child]
                                    - 1u];
                        predicate_equalities[parameter][child].value =
                            runtime_types[wire_predicate
                                ->equality_values[child] - 1u];
                        predicate_equalities[parameter][child].span = span;
                        predicate_equalities[parameter][child].span.end = 1u;
                    }
                    predicate_lifetimes[parameter] =
                        wire_predicate->binder_count == 0u ? NULL
                        : (CmInternId *)cm_alloc(
                        (size_t)wire_predicate->binder_count
                            * sizeof(CmInternId));
                    predicates[parameter].binder.lifetimes =
                        predicate_lifetimes[parameter];
                    predicates[parameter].binder.lifetime_count =
                        wire_predicate->binder_count;
                    predicates[parameter].binder.span = span;
                    if (wire_predicate->binder_count == 0u)
                        memset(&predicates[parameter].binder.span, 0,
                            sizeof(CmSpan));
                    else predicates[parameter].binder.span.end = 1u;
                    for (child = 0u; child < wire_predicate->binder_count;
                            ++child) {
                        predicate_lifetimes[parameter][child] =
                            cm_meta_intern_name(context,
                                wire_predicate->binder_names[child]);
                    }
                    predicates[parameter].scope =
                        CM_HIR_PREDICATE_SCOPE_NONE;
                    predicates[parameter].span = span;
                    predicates[parameter].span.end = 1u;
                    (void)cm_meta_predicate_modifier_from_wire(
                        wire_predicate->modifier,
                        &predicates[parameter].modifier);
                }
                outlives = wire_predicates->outlives_count == 0u ? NULL
                    : (CmHirOutlivesPredicate *)cm_alloc_zeroed(
                        (size_t)wire_predicates->outlives_count,
                        sizeof(CmHirOutlivesPredicate));
                for (parameter = 0u;
                        parameter < wire_predicates->outlives_count;
                        ++parameter) {
                    outlives[parameter].subject_kind = CM_HIR_OUTLIVES_TYPE;
                    outlives[parameter].subject.type = runtime_types[
                        wire_predicates->outlives_subjects[parameter] - 1u];
                    outlives[parameter].bound.kind = CM_HIR_REGION_STATIC;
                    outlives[parameter].scope = CM_HIR_PREDICATE_SCOPE_NONE;
                    outlives[parameter].span = span;
                }
                value.data.function.predicates = predicates;
                value.data.function.predicate_count =
                    wire_predicates->predicate_count;
                value.data.function.outlives_predicates = outlives;
                value.data.function.outlives_predicate_count =
                    wire_predicates->outlives_count;
                value.data.function.nominal_references = nominal_references;
                value.data.function.nominal_reference_count =
                    wire_predicates->nominal_reference_count;
                value.data.function.associated_availability = availability;
                value.data.function.associated_availability_count =
                    wire_predicates->availability_count;
            }
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
        { CmHirLibraryStatus add_status = cm_hir_library_owned_data_add_value(&owned, &value);
        if (add_status != CM_HIR_LIBRARY_OK) {
            do { result.status = CM_HIR_METADATA_ARTIFACT_INVALID_HIR; } while(0);
        } else {
            result.status = CM_HIR_METADATA_ARTIFACT_OK;
        } }
        if (wire_predicates != NULL) {
            for (parameter = 0u;
                    parameter < wire_predicates->nominal_reference_count;
                    ++parameter) cm_free(nominal_schemas[parameter]);
            for (parameter = 0u;
                    parameter < wire_predicates->predicate_count;
                    ++parameter) {
                cm_free(predicate_arguments[parameter]);
                cm_free(predicate_equalities[parameter]);
                cm_free(predicate_lifetimes[parameter]);
            }
        }
        cm_free(outlives);
        cm_free(predicate_lifetimes);
        cm_free(predicate_equalities);
        cm_free(predicate_arguments);
        cm_free(predicates);
        cm_free(availability);
        cm_free(nominal_schemas);
        cm_free(nominal_references);
        cm_free(parameter_types);
        if (result.status != CM_HIR_METADATA_ARTIFACT_OK) goto rollback;
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
    cm_free(runtime_nominals);
    cm_free(runtime_values);
    cm_free(runtime_traits);
    cm_free(runtime_items);
    cm_free(runtime_modules);

cleanup_wire:
    cm_meta_wire_value_predicates_destroy(&value_predicates);
    cm_meta_wire_nominals_destroy(&nominals);
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
        encoded_length, extern_name, metadata_source, 0, 0, 0u);
}

CmHirMetadataArtifactResult cm_hir_metadata_decode_semantic_artifact(
    CmHirContext *context, CmHirLibraryArtifact *artifact,
    const void *encoded, size_t encoded_length, const char *extern_name,
    CmSourceId metadata_source)
{
    return cm_meta_decode_artifact(context, artifact, encoded,
        encoded_length, extern_name, metadata_source, 1, 0, 0u);
}

CmHirMetadataArtifactResult cm_hir_metadata_decode_declaration_artifact(
    CmHirContext *context, CmHirLibraryArtifact *artifact,
    const void *encoded, size_t encoded_length, const char *extern_name,
    CmSourceId metadata_source)
{
    CmHirMetadataEnvelope envelope;
    CmHirMetadataStatus status;
    uint16_t minor;

    memset(&envelope, 0, sizeof(envelope));
    status = cm_hir_metadata_decode_envelope_version(encoded, encoded_length,
        CM_HIR_METADATA_DECLARATION_MAJOR,
        CM_HIR_METADATA_DECLARATION_MINOR, &envelope);
    minor = CM_HIR_METADATA_DECLARATION_MINOR;
    if (status == CM_HIR_METADATA_UNSUPPORTED_VERSION) {
        status = cm_hir_metadata_decode_envelope_version(encoded,
            encoded_length, CM_HIR_METADATA_DECLARATION_MAJOR,
            CM_HIR_METADATA_DECLARATION_MODIFIER_MINOR, &envelope);
        minor = CM_HIR_METADATA_DECLARATION_MODIFIER_MINOR;
    }
    if (status == CM_HIR_METADATA_UNSUPPORTED_VERSION) {
        status = cm_hir_metadata_decode_envelope_version(encoded,
            encoded_length, CM_HIR_METADATA_DECLARATION_MAJOR,
            CM_HIR_METADATA_DECLARATION_PREDICATE_MINOR, &envelope);
        minor = CM_HIR_METADATA_DECLARATION_PREDICATE_MINOR;
    }
    if (status == CM_HIR_METADATA_UNSUPPORTED_VERSION) {
        status = cm_hir_metadata_decode_envelope_version(encoded,
            encoded_length, CM_HIR_METADATA_DECLARATION_MAJOR,
            CM_HIR_METADATA_DECLARATION_LEGACY_MINOR, &envelope);
        minor = CM_HIR_METADATA_DECLARATION_LEGACY_MINOR;
    }
    if (status != CM_HIR_METADATA_OK)
        return cm_meta_result(cm_meta_codec_status(status));
    return cm_meta_decode_artifact(context, artifact, encoded,
        encoded_length, extern_name, metadata_source, 0, 1, minor);
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
