#ifndef CMRUSTC_CM_HIR_DECLARATION_METADATA_H
#define CMRUSTC_CM_HIR_DECLARATION_METADATA_H

#include "cm/buf.h"

#include <stddef.h>
#include <stdint.h>

#define CM_HIR_DECL_METADATA_MAJOR UINT16_C(3)
#define CM_HIR_DECL_METADATA_MINOR UINT16_C(0)

/* Exact v3.0 family and aggregate limits from docs/METADATA_V3.md. */
#define CM_HIR_DECL_METADATA_MAX_CFGS ((size_t)131072u)
#define CM_HIR_DECL_METADATA_MAX_MODULES ((size_t)4096u)
#define CM_HIR_DECL_METADATA_MAX_ITEMS ((size_t)65536u)
#define CM_HIR_DECL_METADATA_MAX_NOMINALS ((size_t)65536u)
#define CM_HIR_DECL_METADATA_MAX_ASSOCIATED_ITEMS ((size_t)131072u)
#define CM_HIR_DECL_METADATA_MAX_GENERICS ((size_t)131072u)
#define CM_HIR_DECL_METADATA_MAX_TYPES ((size_t)262144u)
#define CM_HIR_DECL_METADATA_MAX_VALUES ((size_t)131072u)
#define CM_HIR_DECL_METADATA_MAX_PREDICATES ((size_t)131072u)
#define CM_HIR_DECL_METADATA_MAX_IMPLS ((size_t)131072u)
#define CM_HIR_DECL_METADATA_MAX_NAMESPACE_ENTRIES ((size_t)131072u)
#define CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES ((size_t)131072u)
#define CM_HIR_DECL_METADATA_MAX_VARIANTS \
    CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES

/* Kept for capture callers until they migrate to the family-specific names. */
#define CM_HIR_DECL_METADATA_MAX_RECORDS \
    CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES

typedef enum CmHirDeclarationMetadataStatus {
    CM_HIR_DECL_METADATA_OK = 0,
    CM_HIR_DECL_METADATA_INVALID_ARGUMENT,
    CM_HIR_DECL_METADATA_LIMIT_EXCEEDED,
    CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR,
    CM_HIR_DECL_METADATA_INVALID_FORMAT
} CmHirDeclarationMetadataStatus;

typedef struct CmHirDeclarationString {
    unsigned char *data;
    size_t length;
} CmHirDeclarationString;

typedef enum CmHirDeclarationEdition {
    CM_HIR_DECL_EDITION_2015 = 1,
    CM_HIR_DECL_EDITION_2018 = 2,
    CM_HIR_DECL_EDITION_2021 = 3,
    CM_HIR_DECL_EDITION_2024 = 4
} CmHirDeclarationEdition;

typedef enum CmHirDeclarationPanicStrategy {
    CM_HIR_DECL_PANIC_ABORT = 1,
    CM_HIR_DECL_PANIC_UNWIND = 2
} CmHirDeclarationPanicStrategy;

typedef enum CmHirDeclarationGenericOwner {
    CM_HIR_DECL_GENERIC_NOMINAL = 1,
    CM_HIR_DECL_GENERIC_ITEM = 3,
    CM_HIR_DECL_GENERIC_VALUE = 4
} CmHirDeclarationGenericOwner;

typedef enum CmHirDeclarationGenericKind {
    CM_HIR_DECL_GENERIC_LIFETIME = 1,
    CM_HIR_DECL_GENERIC_TYPE = 2,
    CM_HIR_DECL_GENERIC_CONST = 3
} CmHirDeclarationGenericKind;

typedef enum CmHirDeclarationPrimitive {
    CM_HIR_DECL_PRIMITIVE_UNIT = 1,
    CM_HIR_DECL_PRIMITIVE_BOOL = 2,
    CM_HIR_DECL_PRIMITIVE_CHAR = 3,
    CM_HIR_DECL_PRIMITIVE_STR = 4,
    CM_HIR_DECL_PRIMITIVE_I8 = 5,
    CM_HIR_DECL_PRIMITIVE_I16 = 6,
    CM_HIR_DECL_PRIMITIVE_I32 = 7,
    CM_HIR_DECL_PRIMITIVE_I64 = 8,
    CM_HIR_DECL_PRIMITIVE_I128 = 9,
    CM_HIR_DECL_PRIMITIVE_ISIZE = 10,
    CM_HIR_DECL_PRIMITIVE_U8 = 11,
    CM_HIR_DECL_PRIMITIVE_U16 = 12,
    CM_HIR_DECL_PRIMITIVE_U32 = 13,
    CM_HIR_DECL_PRIMITIVE_U64 = 14,
    CM_HIR_DECL_PRIMITIVE_U128 = 15,
    CM_HIR_DECL_PRIMITIVE_USIZE = 16,
    CM_HIR_DECL_PRIMITIVE_F32 = 17,
    CM_HIR_DECL_PRIMITIVE_F64 = 18
} CmHirDeclarationPrimitive;

typedef enum CmHirDeclarationTypeKind {
    CM_HIR_DECL_TYPE_PRIMITIVE = 1,
    CM_HIR_DECL_TYPE_GENERIC = 2,
    /* Exact zero-argument reference to a STRUCT or ENUM ITEM. */
    CM_HIR_DECL_TYPE_NAMED_ADT = 3,
    /* Reserved until an associated declaration provides an honest root. */
    CM_HIR_DECL_TYPE_SELF = 4,
    CM_HIR_DECL_TYPE_SLICE = 5,
    CM_HIR_DECL_TYPE_RAW_POINTER = 6,
    CM_HIR_DECL_TYPE_REFERENCE = 7,
    /* Nonempty ordered type arguments; zero arguments use NAMED_ADT. */
    CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION = 8
} CmHirDeclarationTypeKind;

typedef enum CmHirDeclarationMutability {
    CM_HIR_DECL_IMMUTABLE = 1,
    CM_HIR_DECL_MUTABLE = 2
} CmHirDeclarationMutability;

typedef enum CmHirDeclarationRegionKind {
    CM_HIR_DECL_REGION_STATIC = 1,
    CM_HIR_DECL_REGION_EARLY_BOUND = 2,
    CM_HIR_DECL_REGION_LATE_BOUND = 3
} CmHirDeclarationRegionKind;

typedef struct CmHirDeclarationRegion {
    uint8_t kind;
    uint32_t generic_local;
    uint32_t binder_index;
} CmHirDeclarationRegion;

typedef struct CmHirDeclarationModule {
    uint32_t parent_module;
    CmHirDeclarationString name;
} CmHirDeclarationModule;

typedef struct CmHirDeclarationTrait {
    uint32_t owner_module;
    CmHirDeclarationString name;
    uint32_t source_ordinal;
    uint32_t generic_start;
    uint32_t generic_count;
} CmHirDeclarationTrait;

typedef enum CmHirDeclarationVisibilityKind {
    CM_HIR_DECL_VISIBILITY_PRIVATE = 1,
    CM_HIR_DECL_VISIBILITY_PUBLIC = 2,
    CM_HIR_DECL_VISIBILITY_CRATE = 3,
    CM_HIR_DECL_VISIBILITY_RESTRICTED = 4
} CmHirDeclarationVisibilityKind;

typedef struct CmHirDeclarationVisibility {
    uint8_t kind;
    /* Nonzero only for RESTRICTED; this first ITEM slice accepts PUBLIC. */
    uint32_t restriction_module;
} CmHirDeclarationVisibility;

typedef enum CmHirDeclarationItemKind {
    CM_HIR_DECL_ITEM_STRUCT = 2,
    CM_HIR_DECL_ITEM_ENUM = 4,
    CM_HIR_DECL_ITEM_TYPE_ALIAS = 5
} CmHirDeclarationItemKind;

typedef enum CmHirDeclarationVariantKind {
    CM_HIR_DECL_VARIANT_UNIT = 1
} CmHirDeclarationVariantKind;

typedef struct CmHirDeclarationVariant {
    uint8_t kind;
    CmHirDeclarationString name;
    uint32_t source_ordinal;
    uint8_t discriminant_primitive;
    uint64_t discriminant_low;
    uint64_t discriminant_high;
} CmHirDeclarationVariant;

/*
 * The bounded ordinary ITEM slice includes public top-level unit structs,
 * repr(u8) unit-variant enums, and free type aliases to zero-argument STRUCT
 * ITEMs. Unit structs may own type parameters; enums and aliases require zero
 * generic/predicate ranges.
 * A STRUCT's public constructor availability is represented by the complete
 * VALUE namespace. ENUM variants are item-owned and never module VALUE mates.
 */
typedef struct CmHirDeclarationItem {
    uint8_t kind;
    uint32_t owner_module;
    CmHirDeclarationString name;
    CmHirDeclarationVisibility visibility;
    uint32_t source_ordinal;
    /* Required only for TYPE_ALIAS and names a NAMED_ADT STRUCT type. */
    uint32_t alias_target_type;
    /* A contiguous ITEM-owned GPAR range; aliases require zero. */
    uint32_t generic_start;
    uint32_t generic_count;
    /* ENUM requires U8 and a nonempty owned source-ordered variant array. */
    uint8_t enum_repr_primitive;
    uint32_t variant_count;
    CmHirDeclarationVariant *variants;
} CmHirDeclarationItem;

typedef struct CmHirDeclarationGeneric {
    uint8_t owner_kind;
    uint32_t owner_local;
    uint32_t index;
    uint8_t kind;
    uint8_t is_relaxed_sized;
    CmHirDeclarationString name;
} CmHirDeclarationGeneric;

typedef struct CmHirDeclarationType {
    uint8_t kind;
    uint8_t primitive;
    uint32_t generic_local;
    /* Required for NAMED_ADT and NAMED_ADT_APPLICATION. */
    uint32_t item_local;
    /* Required for SLICE, RAW_POINTER, and REFERENCE. */
    uint32_t child_type;
    /* Reserved for SELF; SELF is not accepted by this bounded slice. */
    uint32_t self_trait_local;
    /* Required for RAW_POINTER and REFERENCE. */
    uint8_t mutability;
    /* Required for REFERENCE; this slice accepts STATIC only. */
    CmHirDeclarationRegion region;
    /* Required and nonempty for NAMED_ADT_APPLICATION. */
    uint32_t argument_count;
    uint32_t *argument_types;
} CmHirDeclarationType;

typedef enum CmHirDeclarationValueKind {
    CM_HIR_DECL_VALUE_FUNCTION = 1,
    CM_HIR_DECL_VALUE_CONST = 2
} CmHirDeclarationValueKind;

typedef struct CmHirDeclarationValue {
    uint8_t kind;
    uint32_t owner_module;
    CmHirDeclarationString name;
    uint32_t source_ordinal;
    uint32_t generic_start;
    uint32_t generic_count;
    uint32_t predicate_start;
    uint32_t predicate_count;
    uint32_t parameter_count;
    uint32_t *parameter_types;
    /* UNIT is a real nonzero TYPE local; local zero is never a return type. */
    uint32_t return_type;
    /* Required only for CONST; functions require this to remain zero. */
    uint32_t declared_type;
    /* Required only for CONST, which must be immutable in this slice. */
    uint8_t mutability;
    uint8_t has_body;
} CmHirDeclarationValue;

typedef struct CmHirDeclarationPredicate {
    uint32_t owner_value;
    uint32_t ordinal;
    uint32_t subject_type;
    uint32_t trait_local;
    uint32_t argument_count;
    uint32_t *argument_types;
} CmHirDeclarationPredicate;

typedef enum CmHirDeclarationNamespace {
    CM_HIR_DECL_NAMESPACE_TYPE = 1,
    CM_HIR_DECL_NAMESPACE_VALUE = 2
} CmHirDeclarationNamespace;

typedef enum CmHirDeclarationNamespaceTarget {
    CM_HIR_DECL_TARGET_MODULE = 1,
    CM_HIR_DECL_TARGET_ITEM = 2,
    CM_HIR_DECL_TARGET_VALUE = 3,
    CM_HIR_DECL_TARGET_NOMINAL = 4
} CmHirDeclarationNamespaceTarget;

typedef struct CmHirDeclarationNamespaceEntry {
    uint32_t owner_module;
    uint8_t namespace_kind;
    CmHirDeclarationString name;
    uint8_t target_kind;
    uint32_t target_local;
    uint32_t export_ordinal;
} CmHirDeclarationNamespaceEntry;

/*
 * A complete descriptor for the first bounded v3.0 LOWER_SAFE slice.
 * All local references are one-based. Arrays must already use canonical
 * source-independent order. The decoder owns all returned storage.
 */
typedef struct CmHirDeclarationMetadata {
    CmHirDeclarationString crate_name;
    CmHirDeclarationString crate_disambiguator;
    uint8_t edition;
    CmHirDeclarationString target_triple;
    CmHirDeclarationString data_layout;
    uint8_t panic_strategy;
    CmHirDeclarationString *cfgs;
    size_t cfg_count;
    uint32_t root_module;
    CmHirDeclarationModule *modules;
    size_t module_count;
    CmHirDeclarationTrait *traits;
    size_t trait_count;
    CmHirDeclarationGeneric *generics;
    size_t generic_count;
    CmHirDeclarationType *types;
    size_t type_count;
    CmHirDeclarationItem *items;
    size_t item_count;
    CmHirDeclarationValue *values;
    size_t value_count;
    CmHirDeclarationPredicate *predicates;
    size_t predicate_count;
    CmHirDeclarationNamespaceEntry *namespace_entries;
    size_t namespace_count;
    int owns_storage;
} CmHirDeclarationMetadata;

void cm_hir_declaration_metadata_init(CmHirDeclarationMetadata *metadata);
void cm_hir_declaration_metadata_destroy(CmHirDeclarationMetadata *metadata);

CmHirDeclarationMetadataStatus cm_hir_declaration_metadata_validate(
    const CmHirDeclarationMetadata *metadata);
CmHirDeclarationMetadataStatus cm_hir_declaration_metadata_encode(
    const CmHirDeclarationMetadata *metadata, CmByteBuf *output);
CmHirDeclarationMetadataStatus cm_hir_declaration_metadata_decode(
    const void *encoded, size_t encoded_length,
    CmHirDeclarationMetadata *output);

const char *cm_hir_declaration_metadata_status_name(
    CmHirDeclarationMetadataStatus status);

#endif
