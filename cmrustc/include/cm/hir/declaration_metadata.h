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
#define CM_HIR_DECL_METADATA_MAX_FIELDS \
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
    CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION = 8,
    CM_HIR_DECL_TYPE_TUPLE = 9,
    CM_HIR_DECL_TYPE_ARRAY = 10,
    /* Exact associated-type projection with an authenticated trait path. */
    CM_HIR_DECL_TYPE_PROJECTION = 11
} CmHirDeclarationTypeKind;

typedef enum CmHirDeclarationMutability {
    CM_HIR_DECL_IMMUTABLE = 1,
    CM_HIR_DECL_MUTABLE = 2
} CmHirDeclarationMutability;

typedef enum CmHirDeclarationRegionKind {
    CM_HIR_DECL_REGION_STATIC = 1,
    CM_HIR_DECL_REGION_EARLY_BOUND = 2,
    CM_HIR_DECL_REGION_LATE_BOUND = 3,
    CM_HIR_DECL_REGION_ERASED = 4
} CmHirDeclarationRegionKind;

typedef enum CmHirDeclarationSafety {
    /* These tags intentionally match the existing HIR safety lattice. */
    CM_HIR_DECL_SAFETY_SAFE = 0,
    CM_HIR_DECL_SAFETY_UNSAFE = 1
} CmHirDeclarationSafety;

typedef struct CmHirDeclarationRegion {
    uint8_t kind;
    uint32_t generic_local;
    uint32_t binder_index;
} CmHirDeclarationRegion;

typedef struct CmHirDeclarationModule {
    uint32_t parent_module;
    CmHirDeclarationString name;
} CmHirDeclarationModule;

typedef enum CmHirDeclarationVisibilityKind {
    CM_HIR_DECL_VISIBILITY_PRIVATE = 1,
    CM_HIR_DECL_VISIBILITY_PUBLIC = 2,
    CM_HIR_DECL_VISIBILITY_CRATE = 3,
    CM_HIR_DECL_VISIBILITY_RESTRICTED = 4
} CmHirDeclarationVisibilityKind;

typedef struct CmHirDeclarationVisibility {
    uint8_t kind;
    /* Required only for RESTRICTED and names an exact ancestor module. */
    uint32_t restriction_module;
} CmHirDeclarationVisibility;

typedef struct CmHirDeclarationSupertrait CmHirDeclarationSupertrait;

typedef struct CmHirDeclarationTrait {
    uint32_t owner_module;
    CmHirDeclarationString name;
    CmHirDeclarationVisibility visibility;
    uint32_t source_ordinal;
    uint32_t generic_start;
    uint32_t generic_count;
    /* Exact nominal-owned predicate, outlives, and associated partitions. */
    uint32_t predicate_scope_start;
    uint32_t predicate_scope_count;
    uint32_t predicate_start;
    uint32_t predicate_count;
    uint32_t outlives_start;
    uint32_t outlives_count;
    /* Contiguous nominal-owned AITM range; zero count requires zero start. */
    uint32_t associated_start;
    uint32_t associated_count;
    uint8_t safety;
    uint8_t flags;
    /* Retained compiler-semantic attributes in the latent NOMD u16 slot. */
    uint16_t compiler_flags;
    /* Present exactly when CM_HIR_DECL_TRAIT_HAS_DIAGNOSTIC_ITEM is set. */
    CmHirDeclarationString diagnostic_item;
    /* Ordered direct supertraits; the first bounded slice is binder-free. */
    uint32_t supertrait_count;
    CmHirDeclarationSupertrait *supertraits;
    /* Present exactly when CM_HIR_DECL_TRAIT_HAS_LANG_ITEM is set. */
    CmHirDeclarationString lang_item;
} CmHirDeclarationTrait;

#define CM_HIR_DECL_TRAIT_HAS_DIAGNOSTIC_ITEM UINT8_C(1)
#define CM_HIR_DECL_TRAIT_HAS_LANG_ITEM UINT8_C(2)
#define CM_HIR_DECL_TRAIT_IS_CONST UINT8_C(4)
#define CM_HIR_DECL_TRAIT_RUSTC_PAREN_SUGAR UINT8_C(8)
#define CM_HIR_DECL_TRAIT_FUNDAMENTAL UINT8_C(16)
#define CM_HIR_DECL_TRAIT_DENY_EXPLICIT_IMPL UINT8_C(32)
#define CM_HIR_DECL_TRAIT_DO_NOT_IMPLEMENT_VIA_OBJECT UINT8_C(64)

#define CM_HIR_DECL_TRAIT_COMPILER_SPECIALIZATION UINT16_C(1)
#define CM_HIR_DECL_TRAIT_COMPILER_COINDUCTIVE UINT16_C(2)
#define CM_HIR_DECL_TRAIT_COMPILER_TRIVIAL_FIELD_READS UINT16_C(4)

typedef enum CmHirDeclarationSupertraitModifier {
    CM_HIR_DECL_SUPERTRAIT_REQUIRED = 1,
    CM_HIR_DECL_SUPERTRAIT_CONST_IF_CONST = 2
} CmHirDeclarationSupertraitModifier;

struct CmHirDeclarationSupertrait {
    uint8_t modifier;
    uint32_t trait_local;
    uint32_t argument_count;
    uint32_t *argument_types;
};

typedef enum CmHirDeclarationAssociatedKind {
    CM_HIR_DECL_ASSOCIATED_TYPE = 1,
    CM_HIR_DECL_ASSOCIATED_METHOD = 3
} CmHirDeclarationAssociatedKind;

typedef enum CmHirDeclarationAssociatedParentKind {
    CM_HIR_DECL_ASSOCIATED_PARENT_NOMINAL = 1
} CmHirDeclarationAssociatedParentKind;

typedef enum CmHirDeclarationReceiverKind {
    CM_HIR_DECL_RECEIVER_NONE = 0,
    CM_HIR_DECL_RECEIVER_VALUE = 1,
    CM_HIR_DECL_RECEIVER_REF_SHARED = 2,
    CM_HIR_DECL_RECEIVER_REF_MUTABLE = 3,
    CM_HIR_DECL_RECEIVER_CUSTOM = 4
} CmHirDeclarationReceiverKind;

/*
 * First bounded nominal-owned METHOD record. Parameter types include the
 * receiver in source slot zero. The current profiles accept authenticated
 * value/shared/mutable receivers and exact Rust or rust-call ABI, with zero
 * method generics and non-const/non-async/non-variadic declarations; explicit
 * fields keep those facts authenticated.
 */
typedef struct CmHirDeclarationAssociatedItem {
    uint8_t kind;
    uint8_t parent_kind;
    uint32_t parent_local;
    uint32_t implemented_associated_local;
    CmHirDeclarationString name;
    CmHirDeclarationVisibility visibility;
    uint32_t source_ordinal;
    uint8_t is_specializable;
    uint32_t generic_start;
    uint32_t generic_count;
    uint32_t predicate_start;
    uint32_t predicate_count;
    uint8_t receiver;
    uint32_t parameter_count;
    uint32_t *parameter_types;
    uint32_t return_type;
    CmHirDeclarationString abi;
    uint8_t safety;
    uint8_t is_const;
    uint8_t is_async;
    uint8_t is_variadic;
    uint8_t has_default_body;
    /* Common retained semantic identity; zero for existing METHOD records. */
    uint8_t flags;
    CmHirDeclarationString lang_item;
} CmHirDeclarationAssociatedItem;

#define CM_HIR_DECL_ASSOCIATED_HAS_LANG_ITEM UINT8_C(1)

typedef enum CmHirDeclarationItemKind {
    CM_HIR_DECL_ITEM_STRUCT = 2,
    CM_HIR_DECL_ITEM_UNION = 3,
    CM_HIR_DECL_ITEM_ENUM = 4,
    CM_HIR_DECL_ITEM_TYPE_ALIAS = 5
} CmHirDeclarationItemKind;

typedef enum CmHirDeclarationAggregateForm {
    CM_HIR_DECL_AGGREGATE_UNIT = 1,
    CM_HIR_DECL_AGGREGATE_TUPLE = 2,
    CM_HIR_DECL_AGGREGATE_NAMED = 3
} CmHirDeclarationAggregateForm;

#define CM_HIR_DECL_AGGREGATE_REPR_RUST UINT8_C(0)
#define CM_HIR_DECL_AGGREGATE_REPR_TRANSPARENT UINT8_C(1)

#define CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM UINT16_C(1)
#define CM_HIR_DECL_AGGREGATE_RUSTC_PUB_TRANSPARENT UINT16_C(2)
#define CM_HIR_DECL_AGGREGATE_HAS_DIAGNOSTIC_ITEM UINT16_C(4)
#define CM_HIR_DECL_AGGREGATE_RUSTC_INSIGNIFICANT_DTOR UINT16_C(8)
#define CM_HIR_DECL_AGGREGATE_MUST_USE UINT16_C(16)

typedef struct CmHirDeclarationField {
    CmHirDeclarationString name;
    CmHirDeclarationVisibility visibility;
    /* Source-order identity; fields are stored in strictly increasing order. */
    uint32_t source_ordinal;
    uint32_t type_local;
} CmHirDeclarationField;

typedef enum CmHirDeclarationVariantKind {
    CM_HIR_DECL_VARIANT_UNIT = 1,
    CM_HIR_DECL_VARIANT_TUPLE = 2
} CmHirDeclarationVariantKind;

/* Zero is reserved for the bounded Rust-default enum representation. */
#define CM_HIR_DECL_ENUM_REPR_RUST UINT8_C(0)
#define CM_HIR_DECL_ENUM_REPR_U8 CM_HIR_DECL_PRIMITIVE_U8
#define CM_HIR_DECL_ENUM_REPR_U16 CM_HIR_DECL_PRIMITIVE_U16
#define CM_HIR_DECL_ENUM_REPR_U32 CM_HIR_DECL_PRIMITIVE_U32
#define CM_HIR_DECL_ENUM_REPR_U64 CM_HIR_DECL_PRIMITIVE_U64
/* Zero denotes a source-level implicit discriminant in that representation. */
#define CM_HIR_DECL_VARIANT_DISCRIMINANT_IMPLICIT UINT8_C(0)

#define CM_HIR_DECL_ENUM_HAS_LANG_ITEM UINT8_C(1)
#define CM_HIR_DECL_VARIANT_HAS_LANG_ITEM UINT16_C(1)

typedef struct CmHirDeclarationVariantField {
    /* Source-order identity within the owning variant; zero is valid. */
    uint32_t source_ordinal;
    uint32_t type_local;
} CmHirDeclarationVariantField;

typedef struct CmHirDeclarationVariant {
    uint8_t kind;
    CmHirDeclarationString name;
    uint32_t source_ordinal;
    uint8_t discriminant_primitive;
    uint64_t discriminant_low;
    uint64_t discriminant_high;
    uint16_t flags;
    uint32_t field_count;
    CmHirDeclarationVariantField *fields;
    /* Required exactly when flags has HAS_LANG_ITEM. */
    CmHirDeclarationString lang_item;
} CmHirDeclarationVariant;

/*
 * The bounded ordinary ITEM slice includes public top-level unit, tuple, and
 * named structs, named unions, explicit repr(u8/u16/u32/u64) unit-variant
 * enums, Rust-default diagnostic unit enums, Rust-default generic UNIT/TUPLE
 * enums, and free type aliases to zero-argument STRUCT ITEMs. Private support
 * ITEMs are retained only when transitively required by public declarations;
 * they have no namespace entries. Structs, unions, and the bounded generic
 * enum profile may own type parameters; aliases require zero generic/predicate
 * ranges and every enum requires zero predicates. A unit STRUCT's public
 * constructor availability is represented by the complete VALUE namespace.
 * Other structs, unions, and enum parents are TYPE-only in module namespace
 * metadata; UNIT/TUPLE enum constructors use ENUM_VARIANT namespace targets.
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
    /* Exact ITEM-owned predicate partitions; zero count requires zero start. */
    uint32_t predicate_scope_start;
    uint32_t predicate_scope_count;
    uint32_t predicate_start;
    uint32_t predicate_count;
    uint32_t outlives_start;
    uint32_t outlives_count;
    /* STRUCT/UNION aggregate shape; zero for ENUM/TYPE_ALIAS. */
    uint8_t aggregate_form;
    uint8_t aggregate_repr;
    uint16_t aggregate_flags;
    uint32_t field_count;
    CmHirDeclarationField *fields;
    /* Required exactly when aggregate_flags has HAS_LANG_ITEM. */
    CmHirDeclarationString lang_item;
    /* ENUM requires a supported repr and an owned source-ordered variant array. */
    uint8_t enum_repr_primitive;
    uint32_t variant_count;
    CmHirDeclarationVariant *variants;
    /* Required by diagnostic aggregates and supported diagnostic enums. */
    CmHirDeclarationString diagnostic_item;
    uint8_t enum_flags;
    /* Required exactly when enum_flags has HAS_LANG_ITEM. */
    CmHirDeclarationString enum_lang_item;
} CmHirDeclarationItem;

typedef struct CmHirDeclarationGeneric {
    uint8_t owner_kind;
    uint32_t owner_local;
    uint32_t index;
    uint8_t kind;
    uint8_t is_relaxed_sized;
    CmHirDeclarationString name;
    /* TYPE-only default; encoded in the legacy trailing GPAR zero u32. */
    uint8_t has_default;
    /* CONST-only declared type; TYPE parameters require zero. */
    uint32_t declared_type;
    /* Required exactly for a TYPE parameter with has_default set. */
    uint32_t default_type;
} CmHirDeclarationGeneric;

typedef enum CmHirDeclarationArrayLengthKind {
    /* Zero preserves the original scalar ARRAY wire representation. */
    CM_HIR_DECL_ARRAY_LENGTH_SCALAR = 0,
    CM_HIR_DECL_ARRAY_LENGTH_CONST_PARAMETER = 1
} CmHirDeclarationArrayLengthKind;

typedef struct CmHirDeclarationType {
    uint8_t kind;
    uint8_t primitive;
    uint32_t generic_local;
    /* Required for NAMED_ADT and NAMED_ADT_APPLICATION. */
    uint32_t item_local;
    /* Required for SLICE, RAW_POINTER, and REFERENCE. */
    uint32_t child_type;
    /* Required for SELF and names its exact enclosing trait. */
    uint32_t self_trait_local;
    /* Required for RAW_POINTER and REFERENCE. */
    uint8_t mutability;
    /* Required for REFERENCE; this slice accepts STATIC or ERASED. */
    CmHirDeclarationRegion region;
    /* Required and nonempty for NAMED_ADT_APPLICATION. */
    uint32_t argument_count;
    uint32_t *argument_types;
    /* Required and nonempty for TUPLE, in semantic element order. */
    uint32_t element_count;
    uint32_t *element_types;
    /* ARRAY uses child_type as its element. */
    uint8_t array_length_kind;
    /* SCALAR carries the original exact usize scalar representation. */
    uint32_t array_length_type;
    uint64_t array_length_low_bits;
    uint64_t array_length_high_bits;
    /* CONST_PARAMETER names an owner-local CONST generic declared as usize. */
    uint32_t array_length_generic_local;
    /* PROJECTION names its self type, direct trait, declaring TYPE, and args. */
    uint32_t projection_self_type;
    uint32_t projection_trait_local;
    uint32_t projection_associated_local;
    uint32_t projection_argument_count;
    uint32_t *projection_argument_types;
} CmHirDeclarationType;

typedef enum CmHirDeclarationValueKind {
    CM_HIR_DECL_VALUE_FUNCTION = 1,
    CM_HIR_DECL_VALUE_CONST = 2,
    CM_HIR_DECL_VALUE_STATIC = 3
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
    /* Required only for CONST/STATIC; functions require this to remain zero. */
    uint32_t declared_type;
    /* Required only for CONST/STATIC; CONST must be immutable. */
    uint8_t mutability;
    uint8_t has_body;
    /* FUNCTION-only boolean; free-function safety/ABI remain fixed. */
    uint8_t is_const;
} CmHirDeclarationValue;

typedef struct CmHirDeclarationPredicate {
    uint32_t owner_value;
    uint32_t ordinal;
    uint32_t subject_type;
    uint32_t trait_local;
    uint32_t argument_count;
    uint32_t *argument_types;
    uint32_t equality_count;
    struct CmHirDeclarationPredicateEquality *equalities;
    /* VALUE=0 preserves the original v3.0 predicate bytes. */
    uint8_t owner_kind;
    /* Required only for ASSOCIATED; owner_value is then zero. */
    uint32_t owner_associated;
    /* Required only for ITEM; other owner locals are then zero. */
    uint32_t owner_item;
    /* Required only for NOMINAL; every other owner local is then zero. */
    uint32_t owner_nominal;
    /* Wire tag is modifier + 1, preserving REQUIRED as the legacy byte 1. */
    uint8_t modifier;
} CmHirDeclarationPredicate;

typedef enum CmHirDeclarationPredicateModifier {
    CM_HIR_DECL_PREDICATE_REQUIRED = 0,
    CM_HIR_DECL_PREDICATE_CONST_IF_CONST = 1,
    CM_HIR_DECL_PREDICATE_CONST = 2
} CmHirDeclarationPredicateModifier;

typedef struct CmHirDeclarationPredicateEquality {
    uint32_t associated_local;
    uint32_t value_type;
} CmHirDeclarationPredicateEquality;

typedef enum CmHirDeclarationPredicateOwnerKind {
    CM_HIR_DECL_PREDICATE_OWNER_VALUE = 0,
    CM_HIR_DECL_PREDICATE_OWNER_ASSOCIATED = 1,
    CM_HIR_DECL_PREDICATE_OWNER_NOMINAL = 2,
    CM_HIR_DECL_PREDICATE_OWNER_ITEM = 3
} CmHirDeclarationPredicateOwnerKind;

typedef struct CmHirDeclarationOutlivesPredicate {
    /* The current slice accepts NOMINAL owners only. */
    uint8_t owner_kind;
    uint32_t owner_local;
    uint32_t ordinal;
    uint32_t subject_type;
    CmHirDeclarationRegion bound;
    /* Exact HIR predicate scope; this slice accepts the root scope only. */
    uint32_t scope;
} CmHirDeclarationOutlivesPredicate;

typedef enum CmHirDeclarationNamespace {
    CM_HIR_DECL_NAMESPACE_TYPE = 1,
    CM_HIR_DECL_NAMESPACE_VALUE = 2
} CmHirDeclarationNamespace;

typedef enum CmHirDeclarationNamespaceTarget {
    CM_HIR_DECL_TARGET_MODULE = 1,
    CM_HIR_DECL_TARGET_ITEM = 2,
    CM_HIR_DECL_TARGET_VALUE = 3,
    CM_HIR_DECL_TARGET_NOMINAL = 4,
    CM_HIR_DECL_TARGET_ENUM_VARIANT = 5,
    CM_HIR_DECL_TARGET_PRIMITIVE = 6
} CmHirDeclarationNamespaceTarget;

typedef struct CmHirDeclarationNamespaceEntry {
    uint32_t owner_module;
    uint8_t namespace_kind;
    CmHirDeclarationString name;
    uint8_t target_kind;
    /*
     * For ENUM_VARIANT this is a one-based flattened variant local: ENUM
     * ITEMs in canonical ITEM order, then variants in source order. For
     * PRIMITIVE this is the exact CmHirDeclarationPrimitive tag; UNIT is not
     * namespace-bindable. Other target kinds retain their family-specific
     * one-based local.
     */
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
    CmHirDeclarationAssociatedItem *associated_items;
    size_t associated_count;
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
    CmHirDeclarationOutlivesPredicate *outlives_predicates;
    size_t outlives_predicate_count;
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
