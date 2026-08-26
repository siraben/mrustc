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
    /* Exact zero-argument reference to a STRUCT ITEM in this artifact. */
    CM_HIR_DECL_TYPE_NAMED_ADT = 3
} CmHirDeclarationTypeKind;

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
    CM_HIR_DECL_ITEM_TYPE_ALIAS = 5
} CmHirDeclarationItemKind;

/*
 * The first ordinary ITEM slice is a public, top-level, nongeneric unit
 * struct or free type alias. Generic, predicate, field, attribute, and repr
 * payloads are canonical zero. A STRUCT's public constructor availability is
 * represented exactly by the complete VALUE namespace, not by this record.
 */
typedef struct CmHirDeclarationItem {
    uint8_t kind;
    uint32_t owner_module;
    CmHirDeclarationString name;
    CmHirDeclarationVisibility visibility;
    uint32_t source_ordinal;
    /* Required only for TYPE_ALIAS; zero for STRUCT. */
    uint32_t alias_target_type;
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
    /* Required only for zero-argument NAMED_ADT; zero otherwise. */
    uint32_t item_local;
} CmHirDeclarationType;

typedef struct CmHirDeclarationValue {
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
