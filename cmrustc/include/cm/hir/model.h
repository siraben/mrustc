#ifndef CMRUSTC_CM_HIR_MODEL_H
#define CMRUSTC_CM_HIR_MODEL_H

#include <stdio.h>

#include "cm/arena.h"
#include "cm/hir/ids.h"
#include "cm/interner.h"
#include "cm/source.h"
#include "cm/vec.h"

typedef enum CmHirStatus {
    CM_HIR_OK = 0,
    CM_HIR_INVALID_ARGUMENT,
    CM_HIR_INVALID_ID,
    CM_HIR_ID_EXHAUSTED,
    CM_HIR_INVARIANT_VIOLATION
} CmHirStatus;

typedef enum CmHirEdition {
    CM_HIR_EDITION_2015 = 0,
    CM_HIR_EDITION_2018,
    CM_HIR_EDITION_2021,
    CM_HIR_EDITION_2024
} CmHirEdition;

typedef enum CmHirMutability {
    CM_HIR_IMMUTABLE = 0,
    CM_HIR_MUTABLE
} CmHirMutability;

typedef enum CmHirSafety {
    CM_HIR_SAFE = 0,
    CM_HIR_UNSAFE
} CmHirSafety;

typedef enum CmHirGenericParamKind {
    CM_HIR_GENERIC_LIFETIME = 0,
    CM_HIR_GENERIC_TYPE,
    CM_HIR_GENERIC_CONST
} CmHirGenericParamKind;

typedef enum CmHirRegionKind {
    CM_HIR_REGION_STATIC = 0,
    CM_HIR_REGION_EARLY_BOUND,
    CM_HIR_REGION_LATE_BOUND,
    CM_HIR_REGION_INFER,
    CM_HIR_REGION_ERASED,
    CM_HIR_REGION_ERROR
} CmHirRegionKind;

typedef struct CmHirRegion {
    CmHirRegionKind kind;
    union {
        CmHirGenericParamId parameter;
        uint32_t binder_index;
        uint32_t inference_variable;
        CmInternId error_reason;
    } data;
} CmHirRegion;

typedef enum CmHirConstArgKind {
    CM_HIR_CONST_VALUE = 0,
    CM_HIR_CONST_PARAMETER,
    CM_HIR_CONST_UNEVALUATED,
    CM_HIR_CONST_INFER,
    CM_HIR_CONST_ERROR
} CmHirConstArgKind;

/*
 * A scalar constant stores exact target-independent bits together with its
 * Rust type.  Interpretation (signedness and width) comes from that type.
 */
typedef struct CmHirConstArg {
    CmHirConstArgKind kind;
    CmHirTypeId type;
    union {
        struct {
            uint64_t low_bits;
            uint64_t high_bits;
        } value;
        CmHirGenericParamId parameter;
        CmHirDefId definition;
        uint32_t inference_variable;
        CmInternId error_reason;
    } data;
} CmHirConstArg;

typedef enum CmHirGenericArgKind {
    CM_HIR_GENERIC_ARG_LIFETIME = 0,
    CM_HIR_GENERIC_ARG_TYPE,
    CM_HIR_GENERIC_ARG_CONST
} CmHirGenericArgKind;

typedef struct CmHirGenericArg {
    CmHirGenericArgKind kind;
    union {
        CmHirRegion lifetime;
        CmHirTypeId type;
        CmHirConstArg constant;
    } data;
} CmHirGenericArg;

typedef enum CmHirIntType {
    CM_HIR_INT_I8 = 0,
    CM_HIR_INT_I16,
    CM_HIR_INT_I32,
    CM_HIR_INT_I64,
    CM_HIR_INT_I128,
    CM_HIR_INT_ISIZE,
    CM_HIR_INT_U8,
    CM_HIR_INT_U16,
    CM_HIR_INT_U32,
    CM_HIR_INT_U64,
    CM_HIR_INT_U128,
    CM_HIR_INT_USIZE
} CmHirIntType;

typedef enum CmHirFloatType {
    CM_HIR_FLOAT_F16 = 0,
    CM_HIR_FLOAT_F32,
    CM_HIR_FLOAT_F64,
    CM_HIR_FLOAT_F128
} CmHirFloatType;

typedef enum CmHirInferenceKind {
    CM_HIR_INFER_GENERAL = 0,
    CM_HIR_INFER_INTEGER,
    CM_HIR_INFER_FLOAT
} CmHirInferenceKind;

typedef enum CmHirTypeKind {
    CM_HIR_TYPE_ERROR_KIND = 0,
    CM_HIR_TYPE_INFER_KIND,
    CM_HIR_TYPE_NEVER_KIND,
    CM_HIR_TYPE_UNIT_KIND,
    CM_HIR_TYPE_BOOL_KIND,
    CM_HIR_TYPE_CHAR_KIND,
    CM_HIR_TYPE_STR_KIND,
    CM_HIR_TYPE_INTEGER_KIND,
    CM_HIR_TYPE_FLOAT_KIND,
    CM_HIR_TYPE_REFERENCE_KIND,
    CM_HIR_TYPE_RAW_POINTER_KIND,
    CM_HIR_TYPE_TUPLE_KIND,
    CM_HIR_TYPE_ARRAY_KIND,
    CM_HIR_TYPE_SLICE_KIND,
    CM_HIR_TYPE_FN_POINTER_KIND,
    CM_HIR_TYPE_FN_DEFINITION_KIND,
    CM_HIR_TYPE_ADT_KIND,
    /* Transient application retained until type-alias normalization. */
    CM_HIR_TYPE_ALIAS_APPLICATION_KIND,
    /* `Self` bound by one enclosing trait or impl item. */
    CM_HIR_TYPE_SELF_KIND,
    CM_HIR_TYPE_PARAMETER_KIND,
    CM_HIR_TYPE_PROJECTION_KIND,
    CM_HIR_TYPE_DYN_TRAIT_KIND,
    CM_HIR_TYPE_OPAQUE_KIND,
    CM_HIR_TYPE_CLOSURE_KIND,
    CM_HIR_TYPE_FOREIGN_KIND
} CmHirTypeKind;

typedef struct CmHirNamedType {
    CmHirDefId definition;
    CmHirGenericArg *arguments;
    uint32_t argument_count;
} CmHirNamedType;

typedef enum CmHirSupertraitModifier {
    CM_HIR_SUPERTRAIT_REQUIRED = 0,
    CM_HIR_SUPERTRAIT_CONST_IF_CONST
} CmHirSupertraitModifier;

typedef struct CmHirAssociatedTypeEquality {
    /* Targetless associated-type declaration owned by the bound trait. */
    CmHirDefId associated_type;
    CmHirTypeId value;
    CmSpan span;
} CmHirAssociatedTypeEquality;

typedef struct CmHirSupertrait {
    CmHirNamedType trait_type;
    CmHirAssociatedTypeEquality *equalities;
    uint32_t equality_count;
    CmSpan span;
    CmHirSupertraitModifier modifier;
} CmHirSupertrait;

typedef enum CmHirTraitAliasBoundKind {
    CM_HIR_TRAIT_ALIAS_BOUND_TRAIT = 0,
    CM_HIR_TRAIT_ALIAS_BOUND_LIFETIME
} CmHirTraitAliasBoundKind;

typedef struct CmHirTraitAliasBound {
    CmHirTraitAliasBoundKind kind;
    CmSpan span;
    union {
        CmHirSupertrait trait_bound;
        CmHirRegion lifetime;
    } data;
} CmHirTraitAliasBound;

typedef enum CmHirAssociatedTypeBoundModifier {
    CM_HIR_ASSOC_BOUND_REQUIRED = 0,
    CM_HIR_ASSOC_BOUND_RELAXED
} CmHirAssociatedTypeBoundModifier;

typedef struct CmHirAssociatedTypeBound {
    CmHirNamedType trait_type;
    CmHirAssociatedTypeEquality *equalities;
    uint32_t equality_count;
    CmSpan span;
    CmHirAssociatedTypeBoundModifier modifier;
} CmHirAssociatedTypeBound;

typedef enum CmHirTraitPredicateModifier {
    CM_HIR_PREDICATE_REQUIRED = 0,
    CM_HIR_PREDICATE_CONST_IF_CONST,
    CM_HIR_PREDICATE_CONST
} CmHirTraitPredicateModifier;

typedef struct CmHirLifetimeBinder {
    CmInternId *lifetimes;
    uint32_t lifetime_count;
    CmSpan span;
} CmHirLifetimeBinder;

typedef uint32_t CmHirPredicateScopeId;

#define CM_HIR_PREDICATE_SCOPE_NONE 0u

/* One atomic `Subject: Trait<...>` item predicate. */
typedef struct CmHirTraitPredicate {
    CmHirTypeId subject;
    CmHirNamedType trait_type;
    CmHirAssociatedTypeEquality *equalities;
    uint32_t equality_count;
    /* Item-owned predicate-prefix binder, or none for an atomic bound. */
    CmHirPredicateScopeId scope;
    /* Bound-position binder; empty whenever scope is not none. */
    CmHirLifetimeBinder binder;
    CmSpan span;
    CmHirTraitPredicateModifier modifier;
} CmHirTraitPredicate;

typedef enum CmHirOutlivesSubjectKind {
    CM_HIR_OUTLIVES_TYPE = 0,
    CM_HIR_OUTLIVES_LIFETIME
} CmHirOutlivesSubjectKind;

/* One atomic `Type: 'a` or `'a: 'b` item predicate. */
typedef struct CmHirOutlivesPredicate {
    CmHirOutlivesSubjectKind subject_kind;
    union {
        CmHirTypeId type;
        CmHirRegion lifetime;
    } subject;
    CmHirRegion bound;
    CmHirPredicateScopeId scope;
    CmSpan span;
} CmHirOutlivesPredicate;

/*
 * One source `for<...> Subject: Bound + ...` scope shared by every atomic
 * trait and outlives constraint expanded from that where predicate.
 */
typedef struct CmHirPredicateScope {
    CmHirOutlivesSubjectKind subject_kind;
    union {
        CmHirTypeId type;
        CmHirRegion lifetime;
    } subject;
    CmHirLifetimeBinder binder;
    uint32_t trait_predicate_count;
    uint32_t outlives_predicate_count;
    CmSpan span;
} CmHirPredicateScope;

typedef struct CmHirType {
    CmHirTypeKind kind;
    CmSpan span;
    union {
        struct {
            CmInternId reason;
        } error_type;
        struct {
            CmHirInferenceKind kind;
            uint32_t variable;
        } infer_type;
        struct {
            CmHirIntType kind;
        } integer_type;
        struct {
            CmHirFloatType kind;
        } float_type;
        struct {
            CmHirRegion region;
            CmHirTypeId pointee;
            CmHirMutability mutability;
        } reference_type;
        struct {
            CmHirTypeId pointee;
            CmHirMutability mutability;
        } raw_pointer_type;
        struct {
            CmHirTypeId *elements;
            uint32_t element_count;
        } tuple_type;
        struct {
            CmHirTypeId element;
            CmHirConstArg length;
        } array_type;
        struct {
            CmHirTypeId element;
        } slice_type;
        struct {
            CmHirTypeId *parameters;
            uint32_t parameter_count;
            CmHirTypeId return_type;
            CmInternId abi;
            CmHirSafety safety;
            int is_variadic;
        } fn_pointer_type;
        struct {
            /* Stable source-closure identity, never a generated item DefId. */
            CmHirClosureId closure;
        } closure_type;
        CmHirNamedType named_type;
        struct {
            CmHirDefId owner;
        } self_type;
        struct {
            CmHirGenericParamId parameter;
        } parameter_type;
        struct {
            CmHirTypeId self_type;
            /* The known trait selected by the qualified projection. */
            CmHirNamedType trait_type;
            /* A type-alias item owned by trait_type.definition. */
            CmHirNamedType associated_type;
        } projection_type;
        struct {
            CmHirNamedType principal_trait;
            int has_principal;
            /*
             * Canonical associated DefId order.  Each equality names a
             * nongeneric associated type declared directly by the principal
             * trait.  Auto-only objects cannot carry equalities.
             */
            CmHirAssociatedTypeEquality *equalities;
            uint32_t equality_count;
            /* Canonical DefId order; every entry is an authenticated auto trait. */
            CmHirNamedType *auto_traits;
            uint32_t auto_trait_count;
            CmHirRegion region;
        } dyn_trait_type;
    } data;
} CmHirType;

typedef enum CmHirItemKind {
    CM_HIR_ITEM_FUNCTION = 0,
    CM_HIR_ITEM_STRUCT,
    CM_HIR_ITEM_UNION,
    CM_HIR_ITEM_ENUM,
    CM_HIR_ITEM_TYPE_ALIAS,
    CM_HIR_ITEM_CONST,
    CM_HIR_ITEM_STATIC,
    CM_HIR_ITEM_MODULE,
    CM_HIR_ITEM_TRAIT,
    CM_HIR_ITEM_IMPL,
    CM_HIR_ITEM_EXTERN_TYPE,
    CM_HIR_ITEM_TRAIT_ALIAS
} CmHirItemKind;

typedef enum CmHirMacroDefinitionForm {
    CM_HIR_MACRO_RULES_DEFINITION = 0,
    CM_HIR_MACRO_DECLARATIVE_DEFINITION
} CmHirMacroDefinitionForm;

typedef enum CmHirDefinitionKind {
    CM_HIR_DEFINITION_MODULE = 0,
    CM_HIR_DEFINITION_ITEM,
    CM_HIR_DEFINITION_ENUM_VARIANT,
    CM_HIR_DEFINITION_MACRO
} CmHirDefinitionKind;

typedef enum CmHirDefinitionState {
    CM_HIR_DEFINITION_RESERVED = 0,
    CM_HIR_DEFINITION_BOUND
} CmHirDefinitionState;

typedef struct CmHirDefinition {
    CmHirDefId id;
    CmHirDefinitionKind kind;
    CmHirDefinitionState state;
    CmSpan span;
    /* Optional fail-closed promise made while reserving an item DefId. */
    CmHirItemKind reserved_item_kind;
    int has_reserved_item_kind;
    union {
        CmHirModuleId module_id;
        CmHirItemId item_id;
        struct {
            CmHirItemId enum_item_id;
            uint32_t variant_index;
        } enum_variant;
        struct {
            CmHirModuleId owner_module;
            CmInternId name;
            CmHirMacroDefinitionForm form;
        } macro_definition;
    } entity;
} CmHirDefinition;

/* Effective metadata body for one retained structural attribute. */
typedef struct CmHirAttribute {
    CmInternId metadata;
    CmSpan span;
    uint32_t source_attribute;
    uint32_t expansion_depth;
} CmHirAttribute;

typedef struct CmHirCrate {
    CmInternId name;
    CmHirEdition edition;
    CmHirModuleId root_module;
    CmSpan span;
    uint32_t next_definition_index;
    CmHirAttribute *inner_attributes;
    uint32_t inner_attribute_count;
} CmHirCrate;

typedef enum CmHirVisibilityKind {
    CM_HIR_VIS_PRIVATE = 0,
    CM_HIR_VIS_PUBLIC,
    CM_HIR_VIS_CRATE,
    CM_HIR_VIS_RESTRICTED
} CmHirVisibilityKind;

typedef struct CmHirVisibility {
    CmHirVisibilityKind kind;
    CmHirDefId restriction;
} CmHirVisibility;

typedef enum CmHirNamespace {
    CM_HIR_NAMESPACE_TYPE = 0,
    CM_HIR_NAMESPACE_VALUE,
    CM_HIR_NAMESPACE_MACRO
} CmHirNamespace;

typedef enum CmHirPrimitiveKind {
    CM_HIR_PRIMITIVE_NONE = 0,
    CM_HIR_PRIMITIVE_BOOL,
    CM_HIR_PRIMITIVE_CHAR,
    CM_HIR_PRIMITIVE_STR,
    CM_HIR_PRIMITIVE_I8,
    CM_HIR_PRIMITIVE_I16,
    CM_HIR_PRIMITIVE_I32,
    CM_HIR_PRIMITIVE_I64,
    CM_HIR_PRIMITIVE_I128,
    CM_HIR_PRIMITIVE_ISIZE,
    CM_HIR_PRIMITIVE_U8,
    CM_HIR_PRIMITIVE_U16,
    CM_HIR_PRIMITIVE_U32,
    CM_HIR_PRIMITIVE_U64,
    CM_HIR_PRIMITIVE_U128,
    CM_HIR_PRIMITIVE_USIZE,
    CM_HIR_PRIMITIVE_F16,
    CM_HIR_PRIMITIVE_F32,
    CM_HIR_PRIMITIVE_F64,
    CM_HIR_PRIMITIVE_F128
} CmHirPrimitiveKind;

/*
 * One resolver result produced by a structural import declaration. Results
 * retain leaf order, so shadowed or repeated local names are not coalesced.
 */
typedef struct CmHirImportBinding {
    CmInternId name;
    CmHirNamespace namespace_kind;
    /* May name a loaded dependency crate in the same HIR context. */
    CmHirDefId target;
    /* Non-none only when target is empty and this binds a primitive type. */
    CmHirPrimitiveKind primitive_kind;
    int is_anonymous;
} CmHirImportBinding;

typedef enum CmHirImportKind {
    CM_HIR_IMPORT_USE = 0,
    CM_HIR_IMPORT_EXTERN_CRATE
} CmHirImportKind;

/*
 * Module-owned import metadata. Imports are syntax/name-resolution records,
 * not definitions or fake HIR items. One use tree can introduce many names;
 * one extern-crate declaration introduces exactly one type-namespace alias.
 */
typedef struct CmHirImport {
    CmHirImportKind kind;
    CmInternId tree;
    CmHirVisibility visibility;
    CmSpan span;
    uint32_t source_item;
    CmHirAttribute *attributes;
    uint32_t attribute_count;
    CmHirImportBinding *bindings;
    uint32_t binding_count;
} CmHirImport;

typedef struct CmHirModule {
    CmHirCrateId crate_id;
    CmHirModuleId parent;
    CmHirDefId definition;
    CmInternId name;
    CmSpan span;
    /* Effective outer attributes on this module's declaration. */
    CmHirAttribute *outer_attributes;
    uint32_t outer_attribute_count;
    /* Effective inner attributes owned by this module's contents. */
    CmHirAttribute *inner_attributes;
    uint32_t inner_attribute_count;
    CmHirImport *imports;
    uint32_t import_count;
} CmHirModule;

typedef enum CmHirBindingKind {
    CM_HIR_BINDING_NAMED = 0,
    /* Positional ABI slot with no name-resolution binding, e.g. `_`. */
    CM_HIR_BINDING_DISCARD,
    /* One ABI tuple slot destructured into one or two lexical move bindings. */
    CM_HIR_BINDING_TUPLE_PATTERN,
    /* One applied tuple-newtype ABI slot destructured into one move binding. */
    CM_HIR_BINDING_NEWTYPE_PATTERN
} CmHirBindingKind;

/* How a named function-parameter pattern binds its ABI input value. */
typedef enum CmHirParameterBindingMode {
    CM_HIR_PARAMETER_BINDING_MOVE = 0,
    CM_HIR_PARAMETER_BINDING_REF_SHARED,
    CM_HIR_PARAMETER_BINDING_REF_MUTABLE,
    /* Shared-reference pattern `&binding`: move/copy the pointee locally. */
    CM_HIR_PARAMETER_BINDING_DEREF_SHARED
} CmHirParameterBindingMode;

/* Inline capacity; active arity comes from the parameter's tuple type. */
#define CM_HIR_TUPLE_PARAMETER_BINDING_COUNT UINT32_C(2)

typedef struct CmHirTupleParameterBinding {
    CmInternId name;
    CmSpan span;
} CmHirTupleParameterBinding;

typedef struct CmHirNewtypeParameterBinding {
    CmInternId name;
    CmSpan span;
} CmHirNewtypeParameterBinding;

typedef struct CmHirFunctionParameter {
    /* Root binding name; none for discard and destructuring parameters. */
    CmInternId name;
    /* Exact incoming ABI type, including the tuple before destructuring. */
    CmHirTypeId type;
    CmSpan span;
    CmHirBindingKind binding_kind;
    CmHirParameterBindingMode binding_mode;
    /* Embedded, bounded lexical leaves for TUPLE_PATTERN only. */
    CmHirTupleParameterBinding
        tuple_bindings[CM_HIR_TUPLE_PARAMETER_BINDING_COUNT];
    /* The sole lexical leaf for NEWTYPE_PATTERN only. */
    CmHirNewtypeParameterBinding newtype_binding;
} CmHirFunctionParameter;

typedef enum CmHirReceiverKind {
    CM_HIR_RECEIVER_NONE = 0,
    CM_HIR_RECEIVER_VALUE,
    CM_HIR_RECEIVER_REF_SHARED,
    CM_HIR_RECEIVER_REF_MUTABLE,
    CM_HIR_RECEIVER_CUSTOM
} CmHirReceiverKind;

typedef struct CmHirFunctionSignature {
    CmHirFunctionParameter *parameters;
    uint32_t parameter_count;
    CmHirReceiverKind receiver;
    CmHirTypeId return_type;
    CmInternId abi;
    CmHirSafety safety;
    int is_const;
    int is_async;
    int is_variadic;
} CmHirFunctionSignature;

typedef struct CmHirField {
    CmInternId name;
    CmHirTypeId type;
    CmHirVisibility visibility;
    CmSpan span;
} CmHirField;

typedef enum CmHirAggregateForm {
    CM_HIR_AGGREGATE_UNIT = 0,
    CM_HIR_AGGREGATE_TUPLE,
    CM_HIR_AGGREGATE_NAMED
} CmHirAggregateForm;

typedef struct CmHirVariant {
    /* Stable constructor/type identity distinct from the parent enum. */
    CmHirDefId definition;
    CmInternId name;
    CmHirAggregateForm form;
    CmHirField *fields;
    uint32_t field_count;
    int has_discriminant;
    CmHirConstArg discriminant;
    CmSpan span;
} CmHirVariant;

typedef struct CmHirItem {
    CmHirItemKind kind;
    CmHirDefId definition;
    CmHirModuleId owner_module;
    /* Non-none only for a function, const, or type alias in a trait/impl. */
    CmHirDefId parent_definition;
    /*
     * An impl-associated `default fn/type/const` may be overridden by a
     * more-specific impl.  This is mrustc's per-ImplEnt marker and the
     * value-bearing subset of rustc_hir::Defaultness; containing `default
     * impl` semantics remain deliberately outside this structural slice.
     */
    int is_specializable;
    CmInternId name;
    CmHirVisibility visibility;
    CmSpan span;
    /* Effective structural `#[...]` attributes in source order. */
    CmHirAttribute *attributes;
    uint32_t attribute_count;
    /* Half-open, definition-owned range in the context's generic arena. */
    CmHirGenericParamId generic_parameter_start;
    uint32_t generic_parameter_count;
    /* Predicate-prefix binders in source where-predicate order. */
    CmHirPredicateScope *predicate_scopes;
    uint32_t predicate_scope_count;
    /* Atomic trait predicates in declaration order. */
    CmHirTraitPredicate *predicates;
    uint32_t predicate_count;
    /* Atomic type/lifetime outlives predicates in declaration order. */
    CmHirOutlivesPredicate *outlives_predicates;
    uint32_t outlives_predicate_count;
    union {
        struct {
            CmHirFunctionSignature signature;
            CmHirBodyId body;
            /* Trait declaration implemented by an impl method; none otherwise. */
            CmHirDefId trait_item_definition;
        } function_item;
        struct {
            CmHirAggregateForm form;
            CmHirField *fields;
            uint32_t field_count;
        } aggregate_item;
        struct {
            CmHirVariant *variants;
            uint32_t variant_count;
        } enum_item;
        struct {
            CmHirTypeId target;
            /*
             * For an impl-associated definition, the corresponding
             * targetless declaration owned by the implemented trait.
             * None for free aliases and trait declarations.
             */
            CmHirDefId trait_item_definition;
            /* Present only on targetless trait-associated declarations. */
            CmHirAssociatedTypeBound *bounds;
            uint32_t bound_count;
        } type_alias_item;
        struct {
            CmHirTypeId type;
            CmHirBodyId body;
            CmHirMutability mutability;
            /* Trait declaration implemented by an impl const; none otherwise. */
            CmHirDefId trait_item_definition;
        } value_item;
        struct {
            CmHirModuleId module_id;
        } module_item;
        struct {
            CmHirSafety safety;
            /* Compiler-authenticated `auto trait`; never inferred. */
            int is_auto;
            /* Effective compiler-authenticated `#[const_trait]`. */
            int is_const;
            CmHirSupertrait *supertraits;
            uint32_t supertrait_count;
        } trait_item;
        struct {
            /* Ordered `+`-separated trait and lifetime RHS bounds. */
            CmHirTraitAliasBound *bounds;
            uint32_t bound_count;
        } trait_alias_item;
        struct {
            CmHirTypeId self_type;
            int has_trait;
            CmHirNamedType trait_type;
            CmHirSafety safety;
            int is_negative;
            int is_const;
        } impl_item;
    } data;
} CmHirItem;

typedef struct CmHirGenericParam {
    CmHirGenericParamKind kind;
    CmHirDefId owner;
    uint32_t index;
    CmInternId name;
    CmSpan span;
    /* Required for const parameters; none for lifetime/type parameters. */
    CmHirTypeId declared_type;
    /* Type parameter explicitly opts out of the implicit Sized bound. */
    int is_relaxed_sized;
    int has_default;
    CmHirGenericArg default_argument;
} CmHirGenericParam;

typedef struct CmHirLocal {
    CmInternId name;
    CmHirTypeId type;
    CmHirMutability mutability;
    CmSpan span;
    /* ABI signature position, or none for a non-parameter body local. */
    uint32_t parameter_index;
    /* Lexical leaf within that ABI slot; zero for ordinary/user locals. */
    uint32_t parameter_binding_index;
} CmHirLocal;

#define CM_HIR_PARAMETER_INDEX_NONE ((uint32_t)UINT32_MAX)

typedef enum CmHirClosureState {
    /* Signature is complete; body_expression has not been bound yet. */
    CM_HIR_CLOSURE_SIGNATURE_RESERVED = 0,
    CM_HIR_CLOSURE_BODY_BOUND
} CmHirClosureState;

/*
 * The incoming value context proven by the pre-region usage pass.  UNKNOWN
 * is the only state accepted by expression construction; later states are
 * semantic evidence and are written atomically for a complete body manifest.
 */
typedef enum CmHirValueUsage {
    CM_HIR_USAGE_UNKNOWN = 0,
    CM_HIR_USAGE_BORROW,
    CM_HIR_USAGE_MUTATE,
    CM_HIR_USAGE_MOVE
} CmHirValueUsage;

typedef enum CmHirClosureCaptureState {
    CM_HIR_CLOSURE_CAPTURES_UNMARKED = 0,
    CM_HIR_CLOSURE_CAPTURES_MARKED
} CmHirClosureCaptureState;

/* Ordered to match the strongest use which selected the callable trait. */
typedef enum CmHirClosureClass {
    CM_HIR_CLOSURE_CLASS_UNKNOWN = 0,
    CM_HIR_CLOSURE_CLASS_NO_CAPTURE,
    CM_HIR_CLOSURE_CLASS_SHARED,
    CM_HIR_CLOSURE_CLASS_MUT,
    CM_HIR_CLOSURE_CLASS_ONCE
} CmHirClosureClass;

/*
 * One exact outer item-body local captured by a source closure.  Field and
 * dereference projections remain a later evidence format; MARKED rejects
 * those closure-body shapes rather than silently widening this identity.
 */
typedef struct CmHirClosureCapture {
    uint32_t local_index;
    CmHirTypeId type;
    CmHirValueUsage usage;
} CmHirClosureCapture;

/* Closure parameters are lexical bindings, not flat item-body locals. */
typedef struct CmHirClosureParam {
    CmInternId name;
    CmHirTypeId type;
    CmSpan span;
    CmHirBindingKind binding_kind;
} CmHirClosureParam;

/* Durable source-closure identity plus MARKED capture evidence. */
typedef struct CmHirClosure {
    CmHirClosureState state;
    CmHirBodyId owner_body;
    uint32_t source_expression_id;
    CmHirClosureParam *parameters;
    uint32_t parameter_count;
    CmHirTypeId return_type;
    CmHirExprId body_expression;
    /* Exact outer-local prefix visible at the closure's source position. */
    uint32_t visible_local_count;
    int is_move;
    CmHirClosureCaptureState capture_state;
    CmHirClosureCapture *captures;
    uint32_t capture_count;
    CmHirClosureClass callable_class;
    /* Meaningful only when capture_state is CAPTURES_MARKED. */
    int is_copy;
    CmSpan span;
} CmHirClosure;

typedef enum CmHirExprKind {
    CM_HIR_EXPR_INTEGER = 0,
    CM_HIR_EXPR_BLOCK,
    /* A typed read of one exact local in owner_body. */
    CM_HIR_EXPR_LOCAL,
    /* A resolved free-function call with explicit type substitution. */
    CM_HIR_EXPR_CALL,
    /* An unresolved `receiver.method(arguments)` callable site. */
    CM_HIR_EXPR_METHOD_CALL,
    /* An unresolved, explicitly qualified trait callable site. */
    CM_HIR_EXPR_QUALIFIED_CALL,
    /* One fully typed, body-owned binary operation. */
    CM_HIR_EXPR_BINARY,
    /* Complete construction of one known local nongeneric named struct. */
    CM_HIR_EXPR_AGGREGATE,
    /* Direct projection of one authenticated aggregate declaration field. */
    CM_HIR_EXPR_FIELD,
    /* Exact boolean selection between two value-producing branch blocks. */
    CM_HIR_EXPR_IF,
    /* Explicit shared borrow of one body-owned place expression. */
    CM_HIR_EXPR_BORROW_SHARED,
    /* Explicit built-in dereference of one immutable erased reference. */
    CM_HIR_EXPR_DEREFERENCE,
    /* A read of one parameter owned by an enclosing source closure. */
    CM_HIR_EXPR_CLOSURE_PARAMETER,
    /* Construction of one source closure; not executable before expansion. */
    CM_HIR_EXPR_CLOSURE
} CmHirExprKind;

typedef enum CmHirCallableSyntax {
    CM_HIR_CALLABLE_QUALIFIED_TRAIT_METHOD = 0,
    CM_HIR_CALLABLE_DOT_METHOD
} CmHirCallableSyntax;

#define CM_HIR_CALLABLE_RECEIVER_NONE ((uint32_t)UINT32_MAX)

/* Extend only when the model and source lowerer implement exact semantics. */
typedef enum CmHirBinaryOperator {
    CM_HIR_BINARY_ADD = 0,
    CM_HIR_BINARY_SUBTRACT,
    /* Exact u32 equality producing bool; no coercion or truthiness. */
    CM_HIR_BINARY_EQUAL,
    /* Exact target-width usize ordering producing bool. */
    CM_HIR_BINARY_LESS
} CmHirBinaryOperator;

typedef enum CmHirStatementKind {
    CM_HIR_STATEMENT_LET = 0
} CmHirStatementKind;

/* One source-ordered statement retained by a body-owned block expression. */
typedef struct CmHirStatement {
    CmHirStatementKind kind;
    CmSpan span;
    union {
        struct {
            uint32_t local_index;
            CmHirExprId initializer;
        } let_statement;
    } data;
} CmHirStatement;

/* One source-ordered explicit field value in a complete struct expression. */
typedef struct CmHirAggregateFieldValue {
    uint32_t field_index;
    CmHirExprId value;
    CmSpan span;
} CmHirAggregateFieldValue;

typedef enum CmHirStaticBorrowState {
    CM_HIR_STATIC_BORROW_UNKNOWN = 0,
    CM_HIR_STATIC_BORROW_NOT_PROMOTED,
    CM_HIR_STATIC_BORROW_PROMOTED
} CmHirStaticBorrowState;

typedef struct CmHirExpr {
    CmHirExprKind kind;
    /* None only while a legacy integer/block tree awaits publication. */
    CmHirBodyId owner_body;
    CmHirTypeId type;
    CmSpan span;
    CmHirValueUsage usage;
    /* Pre-region static-borrow promotion evidence for this exact node. */
    CmHirStaticBorrowState static_borrow_state;
    union {
        struct {
            uint64_t low_bits;
            uint64_t high_bits;
        } integer;
        struct {
            CmHirStatement *statements;
            uint32_t statement_count;
            CmHirExprId tail_expression;
        } block;
        struct {
            uint32_t local_index;
        } local;
        struct {
            CmHirClosureId closure;
            uint32_t parameter_index;
        } closure_parameter;
        struct {
            CmHirClosureId closure;
        } closure;
        struct {
            CmHirDefId callee;
            /* Substitutions and arguments occupy one contiguous slice. */
            CmHirTypeId *type_substitutions;
            uint32_t type_substitution_count;
            CmHirExprId *arguments;
            uint32_t argument_count;
            /* Non-null only when this node owns the transaction allocation. */
            uint32_t *owned_storage;
        } call;
        struct {
            CmHirCallableSyntax syntax;
            CmInternId method_name;
            CmHirExprId receiver;
            CmHirExprId *arguments;
            uint32_t argument_count;
            /* Exact, deduplicated trait identities visible at this site. */
            CmHirDefId *in_scope_traits;
            uint32_t in_scope_trait_count;
        } method_call;
        struct {
            CmHirCallableSyntax syntax;
            CmHirTypeId requested_self_type;
            CmHirDefId requested_trait;
            CmHirDefId declared_trait_callable;
            CmHirExprId *arguments;
            uint32_t argument_count;
            uint32_t receiver_argument;
            /* Non-null only when this node owns the argument allocation. */
            uint32_t *owned_storage;
        } qualified_call;
        struct {
            CmHirBinaryOperator operator_kind;
            CmHirExprId left;
            CmHirExprId right;
        } binary;
        struct {
            /* Bound struct declaration also authenticated by expression.type. */
            CmHirDefId definition;
            /* Source order, which need not match declaration field order. */
            CmHirAggregateFieldValue *fields;
            uint32_t field_count;
            /* Non-null only when this node owns the transaction allocation. */
            CmHirAggregateFieldValue *owned_storage;
        } aggregate;
        struct {
            CmHirExprId base;
            CmHirDefId definition;
            uint32_t field_index;
        } field;
        struct {
            CmHirExprId condition;
            CmHirExprId then_expression;
            CmHirExprId else_expression;
        } if_expr;
        struct {
            CmHirExprId operand;
        } borrow_shared;
        struct {
            CmHirExprId operand;
        } dereference;
    } data;
} CmHirExpr;

typedef enum CmHirBodyState {
    /* Parsed source exists but typed expression lowering has not happened. */
    CM_HIR_BODY_UNLOWERED = 0,
    /* root_expression is fully typed; source identity remains provenance. */
    CM_HIR_BODY_TYPED,
    /* Lowering failed; error_reason must identify the diagnostic class. */
    CM_HIR_BODY_ERROR
} CmHirBodyState;

typedef enum CmHirBodyOriginKind {
    /* A source expression attached directly to one function/const/static. */
    CM_HIR_BODY_ORIGIN_ITEM_SOURCE = 0
} CmHirBodyOriginKind;

typedef struct CmHirBodyOrigin {
    CmHirBodyOriginKind kind;
    /* Unique executable body identity; the item DefId in this checkpoint. */
    CmHirDefId definition;
    /* Lexical/generic environment; equal to owner during this migration. */
    CmHirDefId enclosing_definition;
    union {
        struct {
            CmHirDefId item_definition;
        } item_source;
    } data;
} CmHirBodyOrigin;

typedef struct CmHirBody {
    /* Transitional lexical/generic owner; do not infer origin from this. */
    CmHirDefId owner;
    CmHirBodyOrigin origin;
    CmHirBodyState state;
    CmHirTypeId expected_type;
    CmHirLocal *locals;
    uint32_t local_count;
    /* Source/signature arity; discarded parameters create no local. */
    uint32_t parameter_count;
    /* Source owning source_expression_id; retained for unlowered/typed bodies. */
    CmSourceId source;
    uint32_t source_expression_id;
    CmHirExprId root_expression;
    CmInternId error_reason;
    CmSpan span;
} CmHirBody;

typedef struct CmHirContext {
    /*
     * Storage is exposed for read-only traversal and implementation modules.
     * External direct writes bypass model invariants and are not supported;
     * every semantic write must use a public mutator or the compound-builder
     * mutation hook below.
     */
    CmArena storage;
    CmInterner strings;
    CmVec crates;
    CmVec modules;
    CmVec items;
    CmVec bodies;
    CmVec closures;
    CmVec expressions;
    CmVec types;
    CmVec generic_parameters;
    CmVec definitions;
    CmVec prebound_associated_types;
    /* Monotonic invalidation token for proof-relevant semantic mutation. */
    uint64_t semantic_generation;
    /* Monotonic invalidation token for observers holding HIR identities. */
    uint64_t rewind_generation;
} CmHirContext;

/*
 * One process-local append transaction over every HIR arena.  Marks are
 * single-use and belong to exactly one context.  Rewind restores all vector,
 * interner, and arena lengths captured by the mark; commit keeps appended
 * state.  Resolving an outer mark invalidates every still-active inner mark.
 * This is an in-memory construction primitive and is never metadata.
 */
typedef struct CmHirContextMark {
    CmArenaMark storage;
    CmInternerMark strings;
    size_t crates;
    size_t modules;
    size_t items;
    size_t bodies;
    size_t closures;
    size_t expressions;
    size_t types;
    size_t generic_parameters;
    size_t definitions;
    size_t prebound_associated_types;
    const CmHirContext *context;
    int active;
} CmHirContextMark;

/*
 * Context ownership is transitive: every variable-length input passed to an
 * add function is copied.  Getter results are borrowed and remain valid only
 * until a later insertion into the same entity arena (or context destruction);
 * stable IDs, rather than pointers, are the durable references.
 */
void cm_hir_context_init(CmHirContext *context);
void cm_hir_context_destroy(CmHirContext *context);
CmHirStatus cm_hir_context_mark(CmHirContext *context,
    CmHirContextMark *out_mark);
CmHirStatus cm_hir_context_rewind(CmHirContext *context,
    CmHirContextMark *mark);
CmHirStatus cm_hir_context_commit(CmHirContext *context,
    CmHirContextMark *mark);

/*
 * Record a successful proof-relevant mutation performed by a compound HIR
 * builder outside model.c.  Interning alone is deliberately not semantic.
 */
void cm_hir_context_record_semantic_mutation(CmHirContext *context);

CmInternId cm_hir_intern(CmHirContext *context, const char *text);

CmHirStatus cm_hir_create_crate(CmHirContext *context, CmInternId name,
    CmHirEdition edition, CmSpan span, CmHirCrateId *out_crate,
    CmHirModuleId *out_root_module);
/* Reserve before constructing a recursive ADT type, then bind via add_item. */
CmHirStatus cm_hir_reserve_item_definition(CmHirContext *context,
    CmHirCrateId crate_id, CmSpan span, CmHirDefId *out_definition);
/* Reserve with an item-kind promise, checked both provisionally and at bind. */
CmHirStatus cm_hir_reserve_item_definition_as(CmHirContext *context,
    CmHirCrateId crate_id, CmHirItemKind item_kind, CmSpan span,
    CmHirDefId *out_definition);
/* Reserve an enum-variant identity before binding it through add_item. */
CmHirStatus cm_hir_reserve_enum_variant_definition(CmHirContext *context,
    CmHirCrateId crate_id, CmSpan span, CmHirDefId *out_definition);
/* Add one expanded-away named macro definition for canonical import identity. */
CmHirStatus cm_hir_add_macro_definition(CmHirContext *context,
    CmHirModuleId owner_module, CmInternId name,
    CmHirMacroDefinitionForm form, CmSpan span,
    CmHirDefId *out_definition);
CmHirStatus cm_hir_add_module(CmHirContext *context, CmHirCrateId crate_id,
    CmHirModuleId parent, CmInternId name, CmSpan span,
    CmHirModuleId *out_id);
/*
 * Deep-copy one nonempty immutable list. A zero-count call is a validated
 * no-op and does not prevent a later nonempty assignment.
 */
CmHirStatus cm_hir_set_crate_inner_attributes(CmHirContext *context,
    CmHirCrateId crate_id, const CmHirAttribute *attributes,
    uint32_t attribute_count);
CmHirStatus cm_hir_set_module_outer_attributes(CmHirContext *context,
    CmHirModuleId module_id, const CmHirAttribute *attributes,
    uint32_t attribute_count);
CmHirStatus cm_hir_set_module_inner_attributes(CmHirContext *context,
    CmHirModuleId module_id, const CmHirAttribute *attributes,
    uint32_t attribute_count);
/*
 * Deep-copy one immutable, declaration-ordered structural import list.
 * Restricted visibility must name the owning module or one of its ancestors.
 */
CmHirStatus cm_hir_set_module_imports(CmHirContext *context,
    CmHirModuleId module_id, const CmHirImport *imports,
    uint32_t import_count);
CmHirStatus cm_hir_add_type(CmHirContext *context, const CmHirType *type,
    CmHirTypeId *out_id);
CmHirStatus cm_hir_add_item(CmHirContext *context, const CmHirItem *item,
    CmHirItemId *out_id);
/* Register one targetless, nongeneric projection identity for a reserved
 * trait without publishing or binding the item.  The supplied skeleton is
 * boundless; validated bounds and predicates may accompany later publication.
 * out_id remains NONE. */
CmHirStatus cm_hir_prebind_trait_associated_type_declaration(
    CmHirContext *context, const CmHirItem *item, CmHirItemId *out_id);
CmHirStatus cm_hir_add_generic_param(CmHirContext *context,
    const CmHirGenericParam *parameter, CmHirGenericParamId *out_id);
/* Assign exactly one validated default while the owner remains reserved. */
CmHirStatus cm_hir_set_generic_param_declared_type(
    CmHirContext *context, CmHirGenericParamId parameter_id,
    CmHirTypeId type);

CmHirStatus cm_hir_set_generic_param_default(CmHirContext *context,
    CmHirGenericParamId parameter_id, const CmHirGenericArg *argument);
/* Construct the only body origin admitted by this additive checkpoint. */
CmHirBodyOrigin cm_hir_body_origin_item_source(CmHirDefId definition);
CmHirStatus cm_hir_add_body(CmHirContext *context, const CmHirBody *body,
    CmHirBodyId *out_id);
/*
 * Reserve a stable identity and complete signature before constructing its
 * nominal type and parameter reads. The nested body is bound exactly once.
 */
CmHirStatus cm_hir_reserve_closure(CmHirContext *context,
    CmHirBodyId owner_body, uint32_t source_expression_id,
    const CmHirClosureParam *parameters, uint32_t parameter_count,
    CmHirTypeId return_type, uint32_t visible_local_count, int is_move,
    CmSpan span, CmHirClosureId *out_id);
CmHirStatus cm_hir_bind_closure_body(CmHirContext *context,
    CmHirClosureId closure, CmHirExprId body_expression);
CmHirStatus cm_hir_add_expr(CmHirContext *context, const CmHirExpr *expression,
    CmHirExprId *out_id);
/*
 * Transaction-builder hook for a validated call whose substitutions and
 * arguments form one contiguous slice. `owned_storage` is non-null only when
 * this call adopts the allocation shared by one or more call and/or aggregate
 * slices. Exactly one node owns a transaction allocation. This never allocates
 * and therefore requires one pre-reserved expression slot; rejection leaves
 * ownership with the caller. Ordinary clients should use the deep-copy body
 * API instead.
 */
CmHirStatus cm_hir_add_owned_call_expr(CmHirContext *context,
    const CmHirExpr *expression, CmHirExprId *out_id);
CmHirStatus cm_hir_add_owned_qualified_call_expr(CmHirContext *context,
    const CmHirExpr *expression, CmHirExprId *out_id);
/*
 * Transaction-builder hook for a validated aggregate field slice. For each
 * shared allocation, owned_storage is non-null only when this aggregate owns
 * the transaction allocation and equals its fields pointer; later aggregate
 * and call slices leave ownership null. This never allocates and therefore
 * requires one pre-reserved expression slot; rejection leaves ownership with
 * the caller. Ordinary clients should use the deep-copy body API instead.
 */
CmHirStatus cm_hir_add_owned_aggregate_expr(CmHirContext *context,
    const CmHirExpr *expression, CmHirExprId *out_id);
/* Idempotently release a call or aggregate transaction allocation, if any. */
void cm_hir_release_expr_owned_storage(CmHirExpr *expression);
/* Publish a validated typed root on an existing unlowered body. */
CmHirStatus cm_hir_set_body_root_expression(CmHirContext *context,
    CmHirBodyId body, CmHirExprId root_expression);

const CmHirCrate *cm_hir_get_crate(const CmHirContext *context,
    CmHirCrateId id);
const CmHirModule *cm_hir_get_module(const CmHirContext *context,
    CmHirModuleId id);
const CmHirItem *cm_hir_get_item(const CmHirContext *context,
    CmHirItemId id);
const CmHirBody *cm_hir_get_body(const CmHirContext *context,
    CmHirBodyId id);
const CmHirClosure *cm_hir_get_closure(const CmHirContext *context,
    CmHirClosureId id);
const CmHirExpr *cm_hir_get_expr(const CmHirContext *context,
    CmHirExprId id);
const CmHirType *cm_hir_get_type(const CmHirContext *context,
    CmHirTypeId id);
const CmHirGenericParam *cm_hir_get_generic_param(
    const CmHirContext *context, CmHirGenericParamId id);
const CmHirDefinition *cm_hir_lookup_definition(const CmHirContext *context,
    CmHirDefId id);

/*
 * Structural custom-receiver boundary used before Receiver-trait solving.
 * Accept Self behind references and single-type-argument nominal wrappers;
 * reject receiver types that are not rooted in the enclosing trait/impl Self.
 */
int cm_hir_custom_receiver_type_valid(const CmHirContext *context,
    CmHirTypeId type, CmHirDefId expected_owner);

const char *cm_hir_status_name(CmHirStatus status);

/* Deterministic insertion-order form intended for focused schema tests. */
int cm_hir_dump(FILE *stream, const CmHirContext *context);

#endif
