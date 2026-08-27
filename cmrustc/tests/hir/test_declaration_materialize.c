#include "cm/hir/declaration_materialize.h"
#include "cm/hir/body.h"
#include "cm/hir/lower.h"

#include <assert.h>
#include <string.h>

#define S(text) { (unsigned char *)(text), sizeof(text) - 1u }

typedef struct TestFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationString cfgs[2];
    CmHirDeclarationModule modules[1];
    CmHirDeclarationTrait traits[1];
    CmHirDeclarationGeneric generics[2];
    CmHirDeclarationType types[3];
    CmHirDeclarationItem items[1];
    CmHirDeclarationValue values[1];
    uint32_t parameters[1];
    CmHirDeclarationPredicate predicates[1];
    uint32_t predicate_arguments[1];
    CmHirDeclarationNamespaceEntry namespace_entries[8];
} TestFixture;

typedef struct AliasFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationType types[1];
    CmHirDeclarationItem items[3];
    CmHirDeclarationNamespaceEntry namespace_entries[6];
} AliasFixture;

typedef struct CompositeFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationTrait traits[1];
    CmHirDeclarationItem items[1];
    CmHirDeclarationGeneric generics[3];
    CmHirDeclarationType types[7];
    uint32_t application_arguments[1];
    CmHirDeclarationValue values[1];
    uint32_t parameters[4];
    CmHirDeclarationPredicate predicates[1];
    uint32_t predicate_arguments[1];
    CmHirDeclarationNamespaceEntry namespace_entries[4];
} CompositeFixture;

typedef struct EnumFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationItem items[1];
    CmHirDeclarationVariant variants[2];
    CmHirDeclarationNamespaceEntry namespace_entries[2];
} EnumFixture;

typedef struct DefaultEnumFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationItem items[2];
    CmHirDeclarationVariant variants[4];
    CmHirDeclarationNamespaceEntry namespace_entries[6];
} DefaultEnumFixture;

typedef struct OptionFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationItem items[1];
    CmHirDeclarationGeneric generics[1];
    CmHirDeclarationType types[3];
    uint32_t application_arguments[1];
    CmHirDeclarationVariant variants[2];
    CmHirDeclarationVariantField some_fields[1];
    CmHirDeclarationValue values[1];
    uint32_t parameters[1];
    CmHirDeclarationNamespaceEntry namespace_entries[7];
} OptionFixture;

typedef struct ConstFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationType types[1];
    CmHirDeclarationValue values[1];
    CmHirDeclarationNamespaceEntry namespace_entries[2];
} ConstFixture;

typedef struct StaticFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationType types[5];
    uint32_t tuple_elements[3];
    CmHirDeclarationValue values[1];
    CmHirDeclarationNamespaceEntry namespace_entries[2];
} StaticFixture;

typedef struct AggregateFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationItem items[3];
    CmHirDeclarationField fields[7];
    CmHirDeclarationGeneric generics[2];
    CmHirDeclarationType types[5];
    uint32_t application_arguments[1];
    CmHirDeclarationNamespaceEntry namespace_entries[6];
} AggregateFixture;

typedef struct LayoutFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationItem items[3];
    CmHirDeclarationField fields[3];
    CmHirDeclarationVariant variants[4];
    CmHirDeclarationType types[3];
    CmHirDeclarationNamespaceEntry namespace_entries[4];
} LayoutFixture;

typedef struct TypeIdFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationItem items[1];
    CmHirDeclarationField fields[1];
    CmHirDeclarationType types[4];
    CmHirDeclarationNamespaceEntry namespace_entries[2];
} TypeIdFixture;

typedef struct TypeNameFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationGeneric generics[1];
    CmHirDeclarationType types[4];
    uint32_t parameters[1];
    CmHirDeclarationValue values[1];
    CmHirDeclarationNamespaceEntry namespace_entries[2];
} TypeNameFixture;

typedef struct UnitFunctionFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationType types[1];
    CmHirDeclarationValue values[1];
    CmHirDeclarationNamespaceEntry namespace_entries[2];
} UnitFunctionFixture;

#define PRIMITIVE_BINDING_COUNT 34u

typedef struct PrimitiveFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationNamespaceEntry
        namespace_entries[PRIMITIVE_BINDING_COUNT];
} PrimitiveFixture;

typedef struct PrimitiveBindingSpec {
    const char *name;
    uint8_t declaration_kind;
    CmHirPrimitiveKind hir_kind;
} PrimitiveBindingSpec;

typedef struct AssociatedMethodFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationTrait traits[2];
    CmHirDeclarationAssociatedItem associated_items[2];
    uint32_t allocate_parameters[2];
    uint32_t fallback_parameters[1];
    CmHirDeclarationType types[4];
    CmHirDeclarationPredicate predicates[1];
    CmHirDeclarationNamespaceEntry namespace_entries[3];
} AssociatedMethodFixture;

typedef struct AnyMethodFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationTrait traits[1];
    CmHirDeclarationAssociatedItem associated_items[1];
    uint32_t method_parameters[1];
    CmHirDeclarationType types[3];
    CmHirDeclarationOutlivesPredicate outlives[1];
    CmHirDeclarationNamespaceEntry namespace_entries[2];
} AnyMethodFixture;

typedef struct IntoIterFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[3];
    CmHirDeclarationTrait traits[1];
    CmHirDeclarationAssociatedItem associated_items[1];
    uint32_t method_parameters[2];
    CmHirDeclarationItem items[5];
    CmHirDeclarationField fields[8];
    CmHirDeclarationGeneric generics[5];
    CmHirDeclarationType types[13];
    uint32_t application_arguments[3];
    CmHirDeclarationPredicate predicates[1];
    CmHirDeclarationNamespaceEntry namespace_entries[5];
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

typedef struct TryFromFnFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationTrait traits[6];
    CmHirDeclarationSupertrait supertraits[2];
    uint32_t fn_mut_super_arguments[1];
    uint32_t try_super_arguments[1];
    CmHirDeclarationAssociatedItem associated[9];
    uint32_t call_mut_parameters[2];
    uint32_t call_once_parameters[2];
    uint32_t from_residual_parameters[1];
    uint32_t from_output_parameters[1];
    uint32_t branch_parameters[1];
    CmHirDeclarationItem items[1];
    CmHirDeclarationVariant variants[2];
    CmHirDeclarationVariantField variant_fields[2];
    CmHirDeclarationGeneric generics[9];
    CmHirDeclarationType types[28];
    uint32_t tuple_elements[1];
    uint32_t fn_mut_projection_arguments[1];
    uint32_t fn_once_projection_arguments[1];
    uint32_t try_type_projection_arguments[1];
    uint32_t control_flow_arguments[2];
    uint32_t return_projection_arguments[1];
    CmHirDeclarationValue values[1];
    uint32_t value_parameters[1];
    CmHirDeclarationPredicate predicates[6];
    uint32_t callable_arguments[1];
    uint32_t residual_arguments[1];
    CmHirDeclarationPredicateEquality callable_equalities[1];
    CmHirDeclarationPredicateEquality try_type_equalities[2];
    CmHirDeclarationNamespaceEntry namespace_entries[9];
} TryFromFnFixture;

typedef struct RepeatFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationTrait traits[5];
    CmHirDeclarationSupertrait clone_supertraits[1];
    CmHirDeclarationSupertrait meta_supertraits[1];
    CmHirDeclarationSupertrait sized_supertraits[1];
    CmHirDeclarationAssociatedItem associated[2];
    uint32_t clone_parameters[1];
    uint32_t clone_from_parameters[2];
    CmHirDeclarationGeneric generics[2];
    CmHirDeclarationType types[7];
    CmHirDeclarationPredicate predicates[2];
    CmHirDeclarationValue values[1];
    uint32_t value_parameters[1];
    CmHirDeclarationNamespaceEntry namespace_entries[7];
} RepeatFixture;

typedef struct FromMutFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationGeneric generics[1];
    CmHirDeclarationType types[5];
    CmHirDeclarationValue values[1];
    uint32_t value_parameters[1];
    CmHirDeclarationNamespaceEntry namespace_entries[2];
} FromMutFixture;

/* TYPE namespace canonical order: aliases first, then primitive spellings. */
static const PrimitiveBindingSpec primitive_binding_specs[
        PRIMITIVE_BINDING_COUNT] = {
    { "BoolAlias", CM_HIR_DECL_PRIMITIVE_BOOL, CM_HIR_PRIMITIVE_BOOL },
    { "CharAlias", CM_HIR_DECL_PRIMITIVE_CHAR, CM_HIR_PRIMITIVE_CHAR },
    { "F32Alias", CM_HIR_DECL_PRIMITIVE_F32, CM_HIR_PRIMITIVE_F32 },
    { "F64Alias", CM_HIR_DECL_PRIMITIVE_F64, CM_HIR_PRIMITIVE_F64 },
    { "I128Alias", CM_HIR_DECL_PRIMITIVE_I128, CM_HIR_PRIMITIVE_I128 },
    { "I16Alias", CM_HIR_DECL_PRIMITIVE_I16, CM_HIR_PRIMITIVE_I16 },
    { "I32Alias", CM_HIR_DECL_PRIMITIVE_I32, CM_HIR_PRIMITIVE_I32 },
    { "I64Alias", CM_HIR_DECL_PRIMITIVE_I64, CM_HIR_PRIMITIVE_I64 },
    { "I8Alias", CM_HIR_DECL_PRIMITIVE_I8, CM_HIR_PRIMITIVE_I8 },
    { "IsizeAlias", CM_HIR_DECL_PRIMITIVE_ISIZE, CM_HIR_PRIMITIVE_ISIZE },
    { "StrAlias", CM_HIR_DECL_PRIMITIVE_STR, CM_HIR_PRIMITIVE_STR },
    { "U128Alias", CM_HIR_DECL_PRIMITIVE_U128, CM_HIR_PRIMITIVE_U128 },
    { "U16Alias", CM_HIR_DECL_PRIMITIVE_U16, CM_HIR_PRIMITIVE_U16 },
    { "U32Alias", CM_HIR_DECL_PRIMITIVE_U32, CM_HIR_PRIMITIVE_U32 },
    { "U64Alias", CM_HIR_DECL_PRIMITIVE_U64, CM_HIR_PRIMITIVE_U64 },
    { "U8Alias", CM_HIR_DECL_PRIMITIVE_U8, CM_HIR_PRIMITIVE_U8 },
    { "UsizeAlias", CM_HIR_DECL_PRIMITIVE_USIZE, CM_HIR_PRIMITIVE_USIZE },
    { "bool", CM_HIR_DECL_PRIMITIVE_BOOL, CM_HIR_PRIMITIVE_BOOL },
    { "char", CM_HIR_DECL_PRIMITIVE_CHAR, CM_HIR_PRIMITIVE_CHAR },
    { "f32", CM_HIR_DECL_PRIMITIVE_F32, CM_HIR_PRIMITIVE_F32 },
    { "f64", CM_HIR_DECL_PRIMITIVE_F64, CM_HIR_PRIMITIVE_F64 },
    { "i128", CM_HIR_DECL_PRIMITIVE_I128, CM_HIR_PRIMITIVE_I128 },
    { "i16", CM_HIR_DECL_PRIMITIVE_I16, CM_HIR_PRIMITIVE_I16 },
    { "i32", CM_HIR_DECL_PRIMITIVE_I32, CM_HIR_PRIMITIVE_I32 },
    { "i64", CM_HIR_DECL_PRIMITIVE_I64, CM_HIR_PRIMITIVE_I64 },
    { "i8", CM_HIR_DECL_PRIMITIVE_I8, CM_HIR_PRIMITIVE_I8 },
    { "isize", CM_HIR_DECL_PRIMITIVE_ISIZE, CM_HIR_PRIMITIVE_ISIZE },
    { "str", CM_HIR_DECL_PRIMITIVE_STR, CM_HIR_PRIMITIVE_STR },
    { "u128", CM_HIR_DECL_PRIMITIVE_U128, CM_HIR_PRIMITIVE_U128 },
    { "u16", CM_HIR_DECL_PRIMITIVE_U16, CM_HIR_PRIMITIVE_U16 },
    { "u32", CM_HIR_DECL_PRIMITIVE_U32, CM_HIR_PRIMITIVE_U32 },
    { "u64", CM_HIR_DECL_PRIMITIVE_U64, CM_HIR_PRIMITIVE_U64 },
    { "u8", CM_HIR_DECL_PRIMITIVE_U8, CM_HIR_PRIMITIVE_U8 },
    { "usize", CM_HIR_DECL_PRIMITIVE_USIZE, CM_HIR_PRIMITIVE_USIZE }
};

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
    fixture->traits[0].name =
        (CmHirDeclarationString)S("AllocatorLike");
    fixture->traits[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->traits[0].source_ordinal = 1u;
    fixture->traits[0].associated_start = 1u;
    fixture->traits[0].associated_count = 2u;
    fixture->traits[0].safety = CM_HIR_DECL_SAFETY_UNSAFE;
    fixture->traits[1].owner_module = 1u;
    fixture->traits[1].name = (CmHirDeclarationString)S("SizedLike");
    fixture->traits[1].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->traits[1].source_ordinal = 5u;
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
    fixture->namespace_entries[0].name =
        (CmHirDeclarationString)S("AllocatorAlias");
    fixture->namespace_entries[0].target_kind =
        CM_HIR_DECL_TARGET_NOMINAL;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 4u;
    fixture->namespace_entries[1] = fixture->namespace_entries[0];
    fixture->namespace_entries[1].name = fixture->traits[0].name;
    fixture->namespace_entries[1].export_ordinal = 1u;
    fixture->namespace_entries[2] = fixture->namespace_entries[0];
    fixture->namespace_entries[2].name = fixture->traits[1].name;
    fixture->namespace_entries[2].target_local = 2u;
    fixture->namespace_entries[2].export_ordinal = 5u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 3u;
}

static void any_method_fixture_init(AnyMethodFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;
    CmHirDeclarationAssociatedItem *method;

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
    fixture->traits[0].name = (CmHirDeclarationString)S("AnyLike");
    fixture->traits[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->traits[0].source_ordinal = 1u;
    fixture->traits[0].outlives_start = 1u;
    fixture->traits[0].outlives_count = 1u;
    fixture->traits[0].associated_start = 1u;
    fixture->traits[0].associated_count = 1u;
    fixture->traits[0].safety = CM_HIR_DECL_SAFETY_SAFE;
    fixture->traits[0].flags =
        CM_HIR_DECL_TRAIT_HAS_DIAGNOSTIC_ITEM;
    fixture->traits[0].diagnostic_item =
        (CmHirDeclarationString)S("AnyLike");
    metadata->traits = fixture->traits;
    metadata->trait_count = 1u;

    fixture->types[0].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[0].primitive = CM_HIR_DECL_PRIMITIVE_UNIT;
    fixture->types[1].kind = CM_HIR_DECL_TYPE_SELF;
    fixture->types[1].self_trait_local = 1u;
    fixture->types[2].kind = CM_HIR_DECL_TYPE_REFERENCE;
    fixture->types[2].child_type = 2u;
    fixture->types[2].mutability = CM_HIR_DECL_IMMUTABLE;
    fixture->types[2].region.kind = CM_HIR_DECL_REGION_ERASED;
    metadata->types = fixture->types;
    metadata->type_count = 3u;

    fixture->outlives[0].owner_kind =
        CM_HIR_DECL_PREDICATE_OWNER_NOMINAL;
    fixture->outlives[0].owner_local = 1u;
    fixture->outlives[0].subject_type = 2u;
    fixture->outlives[0].bound.kind = CM_HIR_DECL_REGION_STATIC;
    metadata->outlives_predicates = fixture->outlives;
    metadata->outlives_predicate_count = 1u;

    fixture->method_parameters[0] = 3u;
    method = &fixture->associated_items[0];
    method->kind = CM_HIR_DECL_ASSOCIATED_METHOD;
    method->parent_kind = CM_HIR_DECL_ASSOCIATED_PARENT_NOMINAL;
    method->parent_local = 1u;
    method->name = (CmHirDeclarationString)S("type_id");
    method->visibility.kind = CM_HIR_DECL_VISIBILITY_PRIVATE;
    method->source_ordinal = 2u;
    method->receiver = CM_HIR_DECL_RECEIVER_REF_SHARED;
    method->parameter_count = 1u;
    method->parameter_types = fixture->method_parameters;
    method->return_type = 1u;
    method->abi = (CmHirDeclarationString)S("Rust");
    method->safety = CM_HIR_DECL_SAFETY_SAFE;
    metadata->associated_items = fixture->associated_items;
    metadata->associated_count = 1u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[0].name =
        (CmHirDeclarationString)S("AnyAlias");
    fixture->namespace_entries[0].target_kind =
        CM_HIR_DECL_TARGET_NOMINAL;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 3u;
    fixture->namespace_entries[1] = fixture->namespace_entries[0];
    fixture->namespace_entries[1].name = fixture->traits[0].name;
    fixture->namespace_entries[1].export_ordinal = 1u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 2u;
}

static void into_iter_fixture_init(IntoIterFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;
    CmHirDeclarationAssociatedItem *method;
    CmHirDeclarationItem *item;

    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("intoiter_like");
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

    fixture->method_parameters[0] = 9u;
    fixture->method_parameters[1] = 7u;
    method = &fixture->associated_items[0];
    method->kind = CM_HIR_DECL_ASSOCIATED_METHOD;
    method->parent_kind = CM_HIR_DECL_ASSOCIATED_PARENT_NOMINAL;
    method->parent_local = 1u;
    method->name = (CmHirDeclarationString)S("partial_drop");
    method->visibility.kind = CM_HIR_DECL_VISIBILITY_PRIVATE;
    method->source_ordinal = 2u;
    method->receiver = CM_HIR_DECL_RECEIVER_REF_MUTABLE;
    method->parameter_count = 2u;
    method->parameter_types = fixture->method_parameters;
    method->return_type = 1u;
    method->abi = (CmHirDeclarationString)S("Rust");
    method->safety = CM_HIR_DECL_SAFETY_UNSAFE;
    metadata->associated_items = fixture->associated_items;
    metadata->associated_count = 1u;

    item = &fixture->items[0];
    item->kind = CM_HIR_DECL_ITEM_STRUCT;
    item->owner_module = 1u;
    item->name = (CmHirDeclarationString)S("ManuallyDrop");
    item->visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    item->source_ordinal = 3u;
    item->generic_start = 1u;
    item->generic_count = 1u;
    item->aggregate_form = CM_HIR_DECL_AGGREGATE_NAMED;
    item->aggregate_repr = CM_HIR_DECL_AGGREGATE_REPR_TRANSPARENT;
    item->aggregate_flags = CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM
        | CM_HIR_DECL_AGGREGATE_RUSTC_PUB_TRANSPARENT;
    item->lang_item = (CmHirDeclarationString)S("manually_drop");
    item->field_count = 1u;
    item->fields = &fixture->fields[0];
    fixture->fields[0].name = (CmHirDeclarationString)S("value");
    fixture->fields[0].visibility.kind =
        CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->fields[0].type_local = 3u;

    item = &fixture->items[1];
    item->kind = CM_HIR_DECL_ITEM_UNION;
    item->owner_module = 1u;
    item->name = (CmHirDeclarationString)S("MaybeUninit");
    item->visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    item->source_ordinal = 4u;
    item->generic_start = 2u;
    item->generic_count = 1u;
    item->aggregate_form = CM_HIR_DECL_AGGREGATE_NAMED;
    item->aggregate_repr = CM_HIR_DECL_AGGREGATE_REPR_TRANSPARENT;
    item->aggregate_flags = CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM
        | CM_HIR_DECL_AGGREGATE_RUSTC_PUB_TRANSPARENT;
    item->lang_item = (CmHirDeclarationString)S("maybe_uninit");
    item->field_count = 2u;
    item->fields = &fixture->fields[1];
    fixture->fields[1].name = (CmHirDeclarationString)S("uninit");
    fixture->fields[1].visibility.kind =
        CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->fields[1].type_local = 1u;
    fixture->fields[2].name = (CmHirDeclarationString)S("value");
    fixture->fields[2].visibility.kind =
        CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->fields[2].source_ordinal = 1u;
    fixture->fields[2].type_local = 10u;

    item = &fixture->items[2];
    item->kind = CM_HIR_DECL_ITEM_STRUCT;
    item->owner_module = 2u;
    item->name = (CmHirDeclarationString)S("IndexRange");
    item->visibility.kind = CM_HIR_DECL_VISIBILITY_CRATE;
    item->source_ordinal = 5u;
    item->aggregate_form = CM_HIR_DECL_AGGREGATE_NAMED;
    item->aggregate_repr = CM_HIR_DECL_AGGREGATE_REPR_RUST;
    item->field_count = 2u;
    item->fields = &fixture->fields[3];
    fixture->fields[3].name = (CmHirDeclarationString)S("start");
    fixture->fields[3].visibility.kind =
        CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->fields[3].type_local = 2u;
    fixture->fields[4].name = (CmHirDeclarationString)S("end");
    fixture->fields[4].visibility.kind =
        CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->fields[4].source_ordinal = 1u;
    fixture->fields[4].type_local = 2u;

    item = &fixture->items[3];
    item->kind = CM_HIR_DECL_ITEM_STRUCT;
    item->owner_module = 2u;
    item->name = (CmHirDeclarationString)S("IntoIter");
    item->visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    item->source_ordinal = 6u;
    item->generic_start = 3u;
    item->generic_count = 2u;
    item->aggregate_form = CM_HIR_DECL_AGGREGATE_NAMED;
    item->aggregate_repr = CM_HIR_DECL_AGGREGATE_REPR_RUST;
    item->aggregate_flags = CM_HIR_DECL_AGGREGATE_HAS_DIAGNOSTIC_ITEM
        | CM_HIR_DECL_AGGREGATE_RUSTC_INSIGNIFICANT_DTOR;
    item->diagnostic_item =
        (CmHirDeclarationString)S("ArrayIntoIter");
    item->field_count = 1u;
    item->fields = &fixture->fields[5];
    fixture->fields[5].name = (CmHirDeclarationString)S("inner");
    fixture->fields[5].visibility.kind =
        CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->fields[5].type_local = 13u;

    item = &fixture->items[4];
    item->kind = CM_HIR_DECL_ITEM_STRUCT;
    item->owner_module = 3u;
    item->name = (CmHirDeclarationString)S("PolymorphicIter");
    item->visibility.kind = CM_HIR_DECL_VISIBILITY_RESTRICTED;
    item->visibility.restriction_module = 2u;
    item->source_ordinal = 7u;
    item->generic_start = 5u;
    item->generic_count = 1u;
    item->predicate_start = 1u;
    item->predicate_count = 1u;
    item->aggregate_form = CM_HIR_DECL_AGGREGATE_NAMED;
    item->aggregate_repr = CM_HIR_DECL_AGGREGATE_REPR_RUST;
    item->field_count = 2u;
    item->fields = &fixture->fields[6];
    fixture->fields[6].name = (CmHirDeclarationString)S("alive");
    fixture->fields[6].visibility.kind =
        CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->fields[6].type_local = 7u;
    fixture->fields[7].name = (CmHirDeclarationString)S("data");
    fixture->fields[7].visibility.kind =
        CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->fields[7].source_ordinal = 1u;
    fixture->fields[7].type_local = 6u;
    metadata->items = fixture->items;
    metadata->item_count = 5u;

    fixture->generics[0].owner_kind = CM_HIR_DECL_GENERIC_ITEM;
    fixture->generics[0].owner_local = 1u;
    fixture->generics[0].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[0].is_relaxed_sized = 1u;
    fixture->generics[0].name = (CmHirDeclarationString)S("T");
    fixture->generics[1].owner_kind = CM_HIR_DECL_GENERIC_ITEM;
    fixture->generics[1].owner_local = 2u;
    fixture->generics[1].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[1].name = (CmHirDeclarationString)S("T");
    fixture->generics[2].owner_kind = CM_HIR_DECL_GENERIC_ITEM;
    fixture->generics[2].owner_local = 4u;
    fixture->generics[2].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[2].name = (CmHirDeclarationString)S("T");
    fixture->generics[3].owner_kind = CM_HIR_DECL_GENERIC_ITEM;
    fixture->generics[3].owner_local = 4u;
    fixture->generics[3].index = 1u;
    fixture->generics[3].kind = CM_HIR_DECL_GENERIC_CONST;
    fixture->generics[3].name = (CmHirDeclarationString)S("N");
    fixture->generics[3].declared_type = 2u;
    fixture->generics[4].owner_kind = CM_HIR_DECL_GENERIC_ITEM;
    fixture->generics[4].owner_local = 5u;
    fixture->generics[4].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[4].is_relaxed_sized = 1u;
    fixture->generics[4].name = (CmHirDeclarationString)S("DATA");
    metadata->generics = fixture->generics;
    metadata->generic_count = 5u;

    fixture->types[0].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[0].primitive = CM_HIR_DECL_PRIMITIVE_UNIT;
    fixture->types[1].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[1].primitive = CM_HIR_DECL_PRIMITIVE_USIZE;
    fixture->types[2].kind = CM_HIR_DECL_TYPE_GENERIC;
    fixture->types[2].generic_local = 1u;
    fixture->types[3].kind = CM_HIR_DECL_TYPE_GENERIC;
    fixture->types[3].generic_local = 2u;
    fixture->types[4].kind = CM_HIR_DECL_TYPE_GENERIC;
    fixture->types[4].generic_local = 3u;
    fixture->types[5].kind = CM_HIR_DECL_TYPE_GENERIC;
    fixture->types[5].generic_local = 5u;
    fixture->types[6].kind = CM_HIR_DECL_TYPE_NAMED_ADT;
    fixture->types[6].item_local = 3u;
    fixture->types[7].kind = CM_HIR_DECL_TYPE_SELF;
    fixture->types[7].self_trait_local = 1u;
    fixture->types[8].kind = CM_HIR_DECL_TYPE_REFERENCE;
    fixture->types[8].child_type = 8u;
    fixture->types[8].mutability = CM_HIR_DECL_MUTABLE;
    fixture->types[8].region.kind = CM_HIR_DECL_REGION_ERASED;
    fixture->application_arguments[0] = 4u;
    fixture->types[9].kind = CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION;
    fixture->types[9].item_local = 1u;
    fixture->types[9].argument_count = 1u;
    fixture->types[9].argument_types = &fixture->application_arguments[0];
    fixture->application_arguments[1] = 5u;
    fixture->types[10].kind = CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION;
    fixture->types[10].item_local = 2u;
    fixture->types[10].argument_count = 1u;
    fixture->types[10].argument_types = &fixture->application_arguments[1];
    fixture->types[11].kind = CM_HIR_DECL_TYPE_ARRAY;
    fixture->types[11].child_type = 11u;
    fixture->types[11].array_length_kind =
        CM_HIR_DECL_ARRAY_LENGTH_CONST_PARAMETER;
    fixture->types[11].array_length_generic_local = 4u;
    fixture->application_arguments[2] = 12u;
    fixture->types[12].kind = CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION;
    fixture->types[12].item_local = 5u;
    fixture->types[12].argument_count = 1u;
    fixture->types[12].argument_types = &fixture->application_arguments[2];
    metadata->types = fixture->types;
    metadata->type_count = 13u;

    fixture->predicates[0].owner_kind =
        CM_HIR_DECL_PREDICATE_OWNER_ITEM;
    fixture->predicates[0].owner_item = 5u;
    fixture->predicates[0].subject_type = 6u;
    fixture->predicates[0].trait_local = 1u;
    metadata->predicates = fixture->predicates;
    metadata->predicate_count = 1u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[0].name =
        (CmHirDeclarationString)S("IntoIter");
    fixture->namespace_entries[0].target_kind =
        CM_HIR_DECL_TARGET_ITEM;
    fixture->namespace_entries[0].target_local = 4u;
    fixture->namespace_entries[0].export_ordinal = 8u;
    fixture->namespace_entries[1] = fixture->namespace_entries[0];
    fixture->namespace_entries[1].name =
        (CmHirDeclarationString)S("IntoIterAlias");
    fixture->namespace_entries[1].export_ordinal = 9u;
    fixture->namespace_entries[2] = fixture->namespace_entries[0];
    fixture->namespace_entries[2].owner_module = 1u;
    fixture->namespace_entries[2].name = fixture->items[0].name;
    fixture->namespace_entries[2].target_local = 1u;
    fixture->namespace_entries[2].export_ordinal = 3u;
    fixture->namespace_entries[3] = fixture->namespace_entries[2];
    fixture->namespace_entries[3].name = fixture->items[1].name;
    fixture->namespace_entries[3].target_local = 2u;
    fixture->namespace_entries[3].export_ordinal = 4u;
    fixture->namespace_entries[4] = fixture->namespace_entries[0];
    fixture->namespace_entries[4].owner_module = 2u;
    fixture->namespace_entries[4].name = fixture->items[3].name;
    fixture->namespace_entries[4].export_ordinal = 6u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 5u;
}

static void from_fn_fixture_init(FromFnFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;
    CmHirDeclarationAssociatedItem *associated;

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
    fixture->traits[0].flags = CM_HIR_DECL_TRAIT_HAS_LANG_ITEM
        | CM_HIR_DECL_TRAIT_IS_CONST
        | CM_HIR_DECL_TRAIT_RUSTC_PAREN_SUGAR
        | CM_HIR_DECL_TRAIT_FUNDAMENTAL;
    fixture->traits[0].lang_item = (CmHirDeclarationString)S("fn_mut");
    fixture->traits[0].supertrait_count = 1u;
    fixture->traits[0].supertraits = fixture->supertraits;
    fixture->supertrait_arguments[0] = 2u;
    fixture->supertraits[0].modifier = CM_HIR_DECL_SUPERTRAIT_REQUIRED;
    fixture->supertraits[0].trait_local = 2u;
    fixture->supertraits[0].argument_count = 1u;
    fixture->supertraits[0].argument_types =
        fixture->supertrait_arguments;

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
    fixture->traits[1].flags = CM_HIR_DECL_TRAIT_HAS_LANG_ITEM
        | CM_HIR_DECL_TRAIT_IS_CONST
        | CM_HIR_DECL_TRAIT_RUSTC_PAREN_SUGAR
        | CM_HIR_DECL_TRAIT_FUNDAMENTAL;
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
    /* Output is declared by FnOnce and remains selected through FnMut's
     * authenticated required-supertrait edge. */
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
    associated->safety = CM_HIR_DECL_SAFETY_SAFE;

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
    associated->safety = CM_HIR_DECL_SAFETY_SAFE;
    metadata->associated_items = fixture->associated_items;
    metadata->associated_count = 3u;

    fixture->value_predicate_arguments[0] = 9u;
    fixture->value_equalities[0].associated_local = 2u;
    fixture->value_equalities[0].value_type = 4u;
    fixture->predicates[0].owner_kind =
        CM_HIR_DECL_PREDICATE_OWNER_VALUE;
    fixture->predicates[0].owner_value = 1u;
    fixture->predicates[0].subject_type = 5u;
    fixture->predicates[0].trait_local = 1u;
    fixture->predicates[0].argument_count = 1u;
    fixture->predicates[0].argument_types =
        fixture->value_predicate_arguments;
    fixture->predicates[0].equality_count = 1u;
    fixture->predicates[0].equalities = fixture->value_equalities;
    fixture->predicates[1].owner_kind =
        CM_HIR_DECL_PREDICATE_OWNER_NOMINAL;
    fixture->predicates[1].owner_nominal = 1u;
    fixture->predicates[1].subject_type = 2u;
    fixture->predicates[1].trait_local = 3u;
    fixture->predicates[2].owner_kind =
        CM_HIR_DECL_PREDICATE_OWNER_NOMINAL;
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
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[0].name = fixture->traits[0].name;
    fixture->namespace_entries[0].target_kind =
        CM_HIR_DECL_TARGET_NOMINAL;
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
    fixture->namespace_entries[3].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
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

static void try_from_fn_fixture_init(TryFromFnFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;
    CmHirDeclarationAssociatedItem *associated;
    size_t index;

    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("try_from_fn_like");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("decl-try-from-fn-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;
    fixture->modules[0].name = metadata->crate_name;
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 1u;

    for (index = 0u; index < 6u; ++index) {
        fixture->traits[index].owner_module = 1u;
        fixture->traits[index].visibility.kind =
            CM_HIR_DECL_VISIBILITY_PUBLIC;
        fixture->traits[index].safety = CM_HIR_DECL_SAFETY_SAFE;
    }
    fixture->traits[0].name = (CmHirDeclarationString)S("FnMut");
    fixture->traits[0].source_ordinal = 3u;
    fixture->traits[0].generic_start = 1u;
    fixture->traits[0].generic_count = 1u;
    fixture->traits[0].predicate_start = 5u;
    fixture->traits[0].predicate_count = 1u;
    fixture->traits[0].associated_start = 1u;
    fixture->traits[0].associated_count = 1u;
    fixture->traits[0].flags = CM_HIR_DECL_TRAIT_HAS_LANG_ITEM
        | CM_HIR_DECL_TRAIT_IS_CONST
        | CM_HIR_DECL_TRAIT_RUSTC_PAREN_SUGAR
        | CM_HIR_DECL_TRAIT_FUNDAMENTAL;
    fixture->traits[0].lang_item = (CmHirDeclarationString)S("fn_mut");
    fixture->fn_mut_super_arguments[0] = 3u;
    fixture->supertraits[0].modifier = CM_HIR_DECL_SUPERTRAIT_REQUIRED;
    fixture->supertraits[0].trait_local = 2u;
    fixture->supertraits[0].argument_count = 1u;
    fixture->supertraits[0].argument_types =
        fixture->fn_mut_super_arguments;
    fixture->traits[0].supertrait_count = 1u;
    fixture->traits[0].supertraits = &fixture->supertraits[0];

    fixture->traits[1].name = (CmHirDeclarationString)S("FnOnce");
    fixture->traits[1].source_ordinal = 2u;
    fixture->traits[1].generic_start = 2u;
    fixture->traits[1].generic_count = 1u;
    fixture->traits[1].predicate_start = 6u;
    fixture->traits[1].predicate_count = 1u;
    fixture->traits[1].associated_start = 2u;
    fixture->traits[1].associated_count = 2u;
    fixture->traits[1].flags = CM_HIR_DECL_TRAIT_HAS_LANG_ITEM
        | CM_HIR_DECL_TRAIT_IS_CONST
        | CM_HIR_DECL_TRAIT_RUSTC_PAREN_SUGAR
        | CM_HIR_DECL_TRAIT_FUNDAMENTAL;
    fixture->traits[1].lang_item = (CmHirDeclarationString)S("fn_once");

    fixture->traits[2].name =
        (CmHirDeclarationString)S("FromResidual");
    fixture->traits[2].source_ordinal = 5u;
    fixture->traits[2].generic_start = 3u;
    fixture->traits[2].generic_count = 1u;
    fixture->traits[2].associated_start = 4u;
    fixture->traits[2].associated_count = 1u;
    fixture->traits[2].flags = CM_HIR_DECL_TRAIT_HAS_DIAGNOSTIC_ITEM
        | CM_HIR_DECL_TRAIT_IS_CONST;
    fixture->traits[2].diagnostic_item =
        (CmHirDeclarationString)S("FromResidual");

    fixture->traits[3].name = (CmHirDeclarationString)S("Residual");
    fixture->traits[3].source_ordinal = 7u;
    fixture->traits[3].generic_start = 4u;
    fixture->traits[3].generic_count = 1u;
    fixture->traits[3].associated_start = 5u;
    fixture->traits[3].associated_count = 1u;
    fixture->traits[3].flags = CM_HIR_DECL_TRAIT_IS_CONST;

    fixture->traits[4].name = (CmHirDeclarationString)S("Try");
    fixture->traits[4].source_ordinal = 6u;
    fixture->traits[4].associated_start = 6u;
    fixture->traits[4].associated_count = 4u;
    fixture->traits[4].flags = CM_HIR_DECL_TRAIT_HAS_LANG_ITEM
        | CM_HIR_DECL_TRAIT_IS_CONST;
    fixture->traits[4].lang_item = (CmHirDeclarationString)S("Try");
    fixture->try_super_arguments[0] = 25u;
    fixture->supertraits[1].modifier =
        CM_HIR_DECL_SUPERTRAIT_CONST_IF_CONST;
    fixture->supertraits[1].trait_local = 3u;
    fixture->supertraits[1].argument_count = 1u;
    fixture->supertraits[1].argument_types = fixture->try_super_arguments;
    fixture->traits[4].supertrait_count = 1u;
    fixture->traits[4].supertraits = &fixture->supertraits[1];

    fixture->traits[5].name = (CmHirDeclarationString)S("Tuple");
    fixture->traits[5].source_ordinal = 1u;
    fixture->traits[5].flags = CM_HIR_DECL_TRAIT_HAS_LANG_ITEM
        | CM_HIR_DECL_TRAIT_DENY_EXPLICIT_IMPL
        | CM_HIR_DECL_TRAIT_DO_NOT_IMPLEMENT_VIA_OBJECT;
    fixture->traits[5].lang_item =
        (CmHirDeclarationString)S("tuple_trait");
    metadata->traits = fixture->traits;
    metadata->trait_count = 6u;

    for (index = 0u; index < 9u; ++index) {
        fixture->generics[index].kind = CM_HIR_DECL_GENERIC_TYPE;
        fixture->generics[index].name =
            (CmHirDeclarationString)S("T");
    }
    fixture->generics[0].owner_kind = CM_HIR_DECL_GENERIC_NOMINAL;
    fixture->generics[0].owner_local = 1u;
    fixture->generics[0].name = (CmHirDeclarationString)S("Args");
    fixture->generics[1] = fixture->generics[0];
    fixture->generics[1].owner_local = 2u;
    fixture->generics[2].owner_kind = CM_HIR_DECL_GENERIC_NOMINAL;
    fixture->generics[2].owner_local = 3u;
    fixture->generics[2].name = (CmHirDeclarationString)S("R");
    fixture->generics[2].has_default = 1u;
    fixture->generics[2].default_type = 22u;
    fixture->generics[3].owner_kind = CM_HIR_DECL_GENERIC_NOMINAL;
    fixture->generics[3].owner_local = 4u;
    fixture->generics[3].name = (CmHirDeclarationString)S("O");
    fixture->generics[4].owner_kind = CM_HIR_DECL_GENERIC_ITEM;
    fixture->generics[4].owner_local = 1u;
    fixture->generics[4].name = (CmHirDeclarationString)S("B");
    fixture->generics[5].owner_kind = CM_HIR_DECL_GENERIC_ITEM;
    fixture->generics[5].owner_local = 1u;
    fixture->generics[5].index = 1u;
    fixture->generics[5].name = (CmHirDeclarationString)S("C");
    fixture->generics[5].has_default = 1u;
    fixture->generics[5].default_type = 1u;
    fixture->generics[6].owner_kind = CM_HIR_DECL_GENERIC_VALUE;
    fixture->generics[6].owner_local = 1u;
    fixture->generics[6].name = (CmHirDeclarationString)S("R");
    fixture->generics[7].owner_kind = CM_HIR_DECL_GENERIC_VALUE;
    fixture->generics[7].owner_local = 1u;
    fixture->generics[7].index = 1u;
    fixture->generics[7].kind = CM_HIR_DECL_GENERIC_CONST;
    fixture->generics[7].name = (CmHirDeclarationString)S("N");
    fixture->generics[7].declared_type = 2u;
    fixture->generics[8].owner_kind = CM_HIR_DECL_GENERIC_VALUE;
    fixture->generics[8].owner_local = 1u;
    fixture->generics[8].index = 2u;
    fixture->generics[8].name = (CmHirDeclarationString)S("F");
    metadata->generics = fixture->generics;
    metadata->generic_count = 9u;

    fixture->types[0].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[0].primitive = CM_HIR_DECL_PRIMITIVE_UNIT;
    fixture->types[1].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[1].primitive = CM_HIR_DECL_PRIMITIVE_USIZE;
    for (index = 0u; index < 8u; ++index)
        fixture->types[index + 2u].kind = CM_HIR_DECL_TYPE_GENERIC;
    fixture->types[2].generic_local = 1u;
    fixture->types[3].generic_local = 2u;
    fixture->types[4].generic_local = 3u;
    fixture->types[5].generic_local = 4u;
    fixture->types[6].generic_local = 5u;
    fixture->types[7].generic_local = 6u;
    fixture->types[8].generic_local = 7u;
    fixture->types[9].generic_local = 9u;
    for (index = 0u; index < 5u; ++index) {
        fixture->types[index + 10u].kind = CM_HIR_DECL_TYPE_SELF;
        fixture->types[index + 10u].self_trait_local =
            (uint32_t)(index + 1u);
    }
    fixture->types[15].kind = CM_HIR_DECL_TYPE_REFERENCE;
    fixture->types[15].child_type = 11u;
    fixture->types[15].mutability = CM_HIR_DECL_MUTABLE;
    fixture->types[15].region.kind = CM_HIR_DECL_REGION_ERASED;
    fixture->tuple_elements[0] = 2u;
    fixture->types[16].kind = CM_HIR_DECL_TYPE_TUPLE;
    fixture->types[16].element_count = 1u;
    fixture->types[16].element_types = fixture->tuple_elements;

#define SET_PROJECTION(type_index, self_local, trait_value, assoc_value) do { \
        fixture->types[(type_index) - 1u].kind = \
            CM_HIR_DECL_TYPE_PROJECTION; \
        fixture->types[(type_index) - 1u].projection_self_type = \
            (self_local); \
        fixture->types[(type_index) - 1u].projection_trait_local = \
            (trait_value); \
        fixture->types[(type_index) - 1u].projection_associated_local = \
            (assoc_value); \
    } while (0)
    SET_PROJECTION(18u, 9u, 5u, 6u);
    SET_PROJECTION(19u, 9u, 5u, 7u);
    SET_PROJECTION(20u, 11u, 2u, 2u);
    fixture->fn_mut_projection_arguments[0] = 3u;
    fixture->types[19].projection_argument_count = 1u;
    fixture->types[19].projection_argument_types =
        fixture->fn_mut_projection_arguments;
    SET_PROJECTION(21u, 12u, 2u, 2u);
    fixture->fn_once_projection_arguments[0] = 4u;
    fixture->types[20].projection_argument_count = 1u;
    fixture->types[20].projection_argument_types =
        fixture->fn_once_projection_arguments;
    SET_PROJECTION(22u, 13u, 5u, 7u);
    SET_PROJECTION(23u, 14u, 4u, 5u);
    fixture->try_type_projection_arguments[0] = 6u;
    fixture->types[22].projection_argument_count = 1u;
    fixture->types[22].projection_argument_types =
        fixture->try_type_projection_arguments;
    SET_PROJECTION(24u, 15u, 5u, 6u);
    SET_PROJECTION(25u, 15u, 5u, 7u);
#undef SET_PROJECTION
    fixture->control_flow_arguments[0] = 25u;
    fixture->control_flow_arguments[1] = 24u;
    fixture->types[25].kind = CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION;
    fixture->types[25].item_local = 1u;
    fixture->types[25].argument_count = 2u;
    fixture->types[25].argument_types = fixture->control_flow_arguments;
    fixture->types[26].kind = CM_HIR_DECL_TYPE_ARRAY;
    fixture->types[26].child_type = 18u;
    fixture->types[26].array_length_kind =
        CM_HIR_DECL_ARRAY_LENGTH_CONST_PARAMETER;
    fixture->types[26].array_length_generic_local = 8u;
    fixture->types[27].kind = CM_HIR_DECL_TYPE_PROJECTION;
    fixture->types[27].projection_self_type = 19u;
    fixture->types[27].projection_trait_local = 4u;
    fixture->types[27].projection_associated_local = 5u;
    fixture->return_projection_arguments[0] = 27u;
    fixture->types[27].projection_argument_count = 1u;
    fixture->types[27].projection_argument_types =
        fixture->return_projection_arguments;
    metadata->types = fixture->types;
    metadata->type_count = 28u;

    for (index = 0u; index < 9u; ++index) {
        fixture->associated[index].parent_kind =
            CM_HIR_DECL_ASSOCIATED_PARENT_NOMINAL;
        fixture->associated[index].visibility.kind =
            CM_HIR_DECL_VISIBILITY_PRIVATE;
        fixture->associated[index].safety = CM_HIR_DECL_SAFETY_SAFE;
    }
    fixture->call_mut_parameters[0] = 16u;
    fixture->call_mut_parameters[1] = 3u;
    associated = &fixture->associated[0];
    associated->kind = CM_HIR_DECL_ASSOCIATED_METHOD;
    associated->parent_local = 1u;
    associated->name = (CmHirDeclarationString)S("call_mut");
    associated->source_ordinal = 1u;
    associated->receiver = CM_HIR_DECL_RECEIVER_REF_MUTABLE;
    associated->parameter_count = 2u;
    associated->parameter_types = fixture->call_mut_parameters;
    associated->return_type = 20u;
    associated->abi = (CmHirDeclarationString)S("rust-call");
    associated = &fixture->associated[1];
    associated->kind = CM_HIR_DECL_ASSOCIATED_TYPE;
    associated->parent_local = 2u;
    associated->name = (CmHirDeclarationString)S("Output");
    associated->source_ordinal = 1u;
    associated->flags = CM_HIR_DECL_ASSOCIATED_HAS_LANG_ITEM;
    associated->lang_item =
        (CmHirDeclarationString)S("fn_once_output");
    fixture->call_once_parameters[0] = 12u;
    fixture->call_once_parameters[1] = 4u;
    associated = &fixture->associated[2];
    associated->kind = CM_HIR_DECL_ASSOCIATED_METHOD;
    associated->parent_local = 2u;
    associated->name = (CmHirDeclarationString)S("call_once");
    associated->source_ordinal = 2u;
    associated->receiver = CM_HIR_DECL_RECEIVER_VALUE;
    associated->parameter_count = 2u;
    associated->parameter_types = fixture->call_once_parameters;
    associated->return_type = 21u;
    associated->abi = (CmHirDeclarationString)S("rust-call");
    fixture->from_residual_parameters[0] = 5u;
    associated = &fixture->associated[3];
    associated->kind = CM_HIR_DECL_ASSOCIATED_METHOD;
    associated->parent_local = 3u;
    associated->name = (CmHirDeclarationString)S("from_residual");
    associated->source_ordinal = 1u;
    associated->receiver = CM_HIR_DECL_RECEIVER_NONE;
    associated->parameter_count = 1u;
    associated->parameter_types = fixture->from_residual_parameters;
    associated->return_type = 13u;
    associated->abi = (CmHirDeclarationString)S("Rust");
    associated->flags = CM_HIR_DECL_ASSOCIATED_HAS_LANG_ITEM;
    associated->lang_item =
        (CmHirDeclarationString)S("from_residual");
    associated = &fixture->associated[4];
    associated->kind = CM_HIR_DECL_ASSOCIATED_TYPE;
    associated->parent_local = 4u;
    associated->name = (CmHirDeclarationString)S("TryType");
    associated->source_ordinal = 1u;
    associated->predicate_start = 4u;
    associated->predicate_count = 1u;
    associated = &fixture->associated[5];
    associated->kind = CM_HIR_DECL_ASSOCIATED_TYPE;
    associated->parent_local = 5u;
    associated->name = (CmHirDeclarationString)S("Output");
    associated->source_ordinal = 1u;
    associated = &fixture->associated[6];
    associated->kind = CM_HIR_DECL_ASSOCIATED_TYPE;
    associated->parent_local = 5u;
    associated->name = (CmHirDeclarationString)S("Residual");
    associated->source_ordinal = 2u;
    fixture->from_output_parameters[0] = 24u;
    associated = &fixture->associated[7];
    associated->kind = CM_HIR_DECL_ASSOCIATED_METHOD;
    associated->parent_local = 5u;
    associated->name = (CmHirDeclarationString)S("from_output");
    associated->source_ordinal = 3u;
    associated->receiver = CM_HIR_DECL_RECEIVER_NONE;
    associated->parameter_count = 1u;
    associated->parameter_types = fixture->from_output_parameters;
    associated->return_type = 15u;
    associated->abi = (CmHirDeclarationString)S("Rust");
    associated->flags = CM_HIR_DECL_ASSOCIATED_HAS_LANG_ITEM;
    associated->lang_item = (CmHirDeclarationString)S("from_output");
    fixture->branch_parameters[0] = 15u;
    associated = &fixture->associated[8];
    associated->kind = CM_HIR_DECL_ASSOCIATED_METHOD;
    associated->parent_local = 5u;
    associated->name = (CmHirDeclarationString)S("branch");
    associated->source_ordinal = 4u;
    associated->receiver = CM_HIR_DECL_RECEIVER_VALUE;
    associated->parameter_count = 1u;
    associated->parameter_types = fixture->branch_parameters;
    associated->return_type = 26u;
    associated->abi = (CmHirDeclarationString)S("Rust");
    associated->flags = CM_HIR_DECL_ASSOCIATED_HAS_LANG_ITEM;
    associated->lang_item = (CmHirDeclarationString)S("branch");
    metadata->associated_items = fixture->associated;
    metadata->associated_count = 9u;

    fixture->variant_fields[0].source_ordinal = 0u;
    fixture->variant_fields[0].type_local = 8u;
    fixture->variant_fields[1].source_ordinal = 0u;
    fixture->variant_fields[1].type_local = 7u;
    fixture->variants[0].kind = CM_HIR_DECL_VARIANT_TUPLE;
    fixture->variants[0].name = (CmHirDeclarationString)S("Continue");
    fixture->variants[0].source_ordinal = 0u;
    fixture->variants[0].discriminant_primitive =
        CM_HIR_DECL_VARIANT_DISCRIMINANT_IMPLICIT;
    fixture->variants[0].flags = CM_HIR_DECL_VARIANT_HAS_LANG_ITEM;
    fixture->variants[0].field_count = 1u;
    fixture->variants[0].fields = &fixture->variant_fields[0];
    fixture->variants[0].lang_item =
        (CmHirDeclarationString)S("Continue");
    fixture->variants[1].kind = CM_HIR_DECL_VARIANT_TUPLE;
    fixture->variants[1].name = (CmHirDeclarationString)S("Break");
    fixture->variants[1].source_ordinal = 1u;
    fixture->variants[1].discriminant_primitive =
        CM_HIR_DECL_VARIANT_DISCRIMINANT_IMPLICIT;
    fixture->variants[1].flags = CM_HIR_DECL_VARIANT_HAS_LANG_ITEM;
    fixture->variants[1].field_count = 1u;
    fixture->variants[1].fields = &fixture->variant_fields[1];
    fixture->variants[1].lang_item =
        (CmHirDeclarationString)S("Break");
    fixture->items[0].kind = CM_HIR_DECL_ITEM_ENUM;
    fixture->items[0].owner_module = 1u;
    fixture->items[0].name = (CmHirDeclarationString)S("ControlFlow");
    fixture->items[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[0].source_ordinal = 4u;
    fixture->items[0].generic_start = 5u;
    fixture->items[0].generic_count = 2u;
    fixture->items[0].aggregate_flags = CM_HIR_DECL_AGGREGATE_MUST_USE;
    fixture->items[0].enum_repr_primitive = CM_HIR_DECL_ENUM_REPR_RUST;
    fixture->items[0].variant_count = 2u;
    fixture->items[0].variants = fixture->variants;
    fixture->items[0].diagnostic_item =
        (CmHirDeclarationString)S("ControlFlow");
    metadata->items = fixture->items;
    metadata->item_count = 1u;

    fixture->callable_arguments[0] = 17u;
    fixture->callable_equalities[0].associated_local = 2u;
    fixture->callable_equalities[0].value_type = 9u;
    fixture->predicates[0].owner_kind =
        CM_HIR_DECL_PREDICATE_OWNER_VALUE;
    fixture->predicates[0].owner_value = 1u;
    fixture->predicates[0].subject_type = 10u;
    fixture->predicates[0].trait_local = 1u;
    fixture->predicates[0].argument_count = 1u;
    fixture->predicates[0].argument_types = fixture->callable_arguments;
    fixture->predicates[0].equality_count = 1u;
    fixture->predicates[0].equalities = fixture->callable_equalities;
    fixture->predicates[1].owner_kind =
        CM_HIR_DECL_PREDICATE_OWNER_VALUE;
    fixture->predicates[1].owner_value = 1u;
    fixture->predicates[1].ordinal = 1u;
    fixture->predicates[1].subject_type = 9u;
    fixture->predicates[1].trait_local = 5u;
    fixture->residual_arguments[0] = 27u;
    fixture->predicates[2].owner_kind =
        CM_HIR_DECL_PREDICATE_OWNER_VALUE;
    fixture->predicates[2].owner_value = 1u;
    fixture->predicates[2].ordinal = 2u;
    fixture->predicates[2].subject_type = 19u;
    fixture->predicates[2].trait_local = 4u;
    fixture->predicates[2].argument_count = 1u;
    fixture->predicates[2].argument_types = fixture->residual_arguments;
    fixture->try_type_equalities[0].associated_local = 6u;
    fixture->try_type_equalities[0].value_type = 6u;
    fixture->try_type_equalities[1].associated_local = 7u;
    fixture->try_type_equalities[1].value_type = 14u;
    fixture->predicates[3].owner_kind =
        CM_HIR_DECL_PREDICATE_OWNER_ASSOCIATED;
    fixture->predicates[3].owner_associated = 5u;
    fixture->predicates[3].subject_type = 23u;
    fixture->predicates[3].trait_local = 5u;
    fixture->predicates[3].equality_count = 2u;
    fixture->predicates[3].equalities = fixture->try_type_equalities;
    fixture->predicates[4].owner_kind =
        CM_HIR_DECL_PREDICATE_OWNER_NOMINAL;
    fixture->predicates[4].owner_nominal = 1u;
    fixture->predicates[4].subject_type = 3u;
    fixture->predicates[4].trait_local = 6u;
    fixture->predicates[5].owner_kind =
        CM_HIR_DECL_PREDICATE_OWNER_NOMINAL;
    fixture->predicates[5].owner_nominal = 2u;
    fixture->predicates[5].subject_type = 4u;
    fixture->predicates[5].trait_local = 6u;
    metadata->predicates = fixture->predicates;
    metadata->predicate_count = 6u;

    fixture->value_parameters[0] = 10u;
    fixture->values[0].kind = CM_HIR_DECL_VALUE_FUNCTION;
    fixture->values[0].owner_module = 1u;
    fixture->values[0].name = (CmHirDeclarationString)S("try_from_fn");
    fixture->values[0].source_ordinal = 8u;
    fixture->values[0].generic_start = 7u;
    fixture->values[0].generic_count = 3u;
    fixture->values[0].predicate_start = 1u;
    fixture->values[0].predicate_count = 3u;
    fixture->values[0].parameter_count = 1u;
    fixture->values[0].parameter_types = fixture->value_parameters;
    fixture->values[0].return_type = 28u;
    fixture->values[0].has_body = 1u;
    metadata->values = fixture->values;
    metadata->value_count = 1u;

    for (index = 0u; index < 7u; ++index) {
        CmHirDeclarationNamespaceEntry *entry =
            &fixture->namespace_entries[index];
        entry->owner_module = 1u;
        entry->namespace_kind = CM_HIR_DECL_NAMESPACE_TYPE;
    }
    fixture->namespace_entries[0].name = fixture->items[0].name;
    fixture->namespace_entries[0].target_kind = CM_HIR_DECL_TARGET_ITEM;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 4u;
    for (index = 0u; index < 6u; ++index) {
        fixture->namespace_entries[index + 1u].name =
            fixture->traits[index].name;
        fixture->namespace_entries[index + 1u].target_kind =
            CM_HIR_DECL_TARGET_NOMINAL;
        fixture->namespace_entries[index + 1u].target_local =
            (uint32_t)(index + 1u);
        fixture->namespace_entries[index + 1u].export_ordinal =
            fixture->traits[index].source_ordinal;
    }
    fixture->namespace_entries[7].owner_module = 1u;
    fixture->namespace_entries[7].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[7].name = fixture->values[0].name;
    fixture->namespace_entries[7].target_kind = CM_HIR_DECL_TARGET_VALUE;
    fixture->namespace_entries[7].target_local = 1u;
    fixture->namespace_entries[7].export_ordinal = 8u;
    fixture->namespace_entries[8] = fixture->namespace_entries[7];
    fixture->namespace_entries[8].name =
        (CmHirDeclarationString)S("try_from_fn_alias");
    fixture->namespace_entries[8].export_ordinal = 9u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 9u;
}

typedef struct NonDropFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationTrait traits[1];
    CmHirDeclarationNamespaceEntry namespace_entries[1];
} NonDropFixture;

static void non_drop_fixture_init(NonDropFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;

    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("non_drop_like");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("decl-non-drop-v1");
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
    fixture->traits[0].source_ordinal = 1u;
    fixture->traits[0].safety = CM_HIR_DECL_SAFETY_SAFE;
    fixture->traits[0].name = (CmHirDeclarationString)S("NonDrop");
    fixture->traits[0].compiler_flags =
        CM_HIR_DECL_TRAIT_COMPILER_UNSAFE_SPECIALIZATION_MARKER;
    metadata->traits = fixture->traits;
    metadata->trait_count = 1u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[0].name = fixture->traits[0].name;
    fixture->namespace_entries[0].target_kind = CM_HIR_DECL_TARGET_NOMINAL;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 1u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 1u;
}

static void repeat_fixture_init(RepeatFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;
    const uint8_t sized_flags = CM_HIR_DECL_TRAIT_HAS_LANG_ITEM
        | CM_HIR_DECL_TRAIT_FUNDAMENTAL
        | CM_HIR_DECL_TRAIT_DENY_EXPLICIT_IMPL
        | CM_HIR_DECL_TRAIT_DO_NOT_IMPLEMENT_VIA_OBJECT;
    const uint16_t sized_compiler_flags =
        CM_HIR_DECL_TRAIT_COMPILER_SPECIALIZATION
        | CM_HIR_DECL_TRAIT_COMPILER_COINDUCTIVE;
    size_t index;

    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("repeat_like");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("decl-repeat-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;
    fixture->modules[0].name = metadata->crate_name;
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 1u;

    for (index = 0u; index < 5u; ++index) {
        fixture->traits[index].owner_module = 1u;
        fixture->traits[index].visibility.kind =
            CM_HIR_DECL_VISIBILITY_PUBLIC;
        fixture->traits[index].source_ordinal = (uint32_t)(index + 1u);
        fixture->traits[index].safety = CM_HIR_DECL_SAFETY_SAFE;
    }
    fixture->traits[0].name = (CmHirDeclarationString)S("Clone");
    fixture->traits[0].associated_start = 1u;
    fixture->traits[0].associated_count = 2u;
    fixture->traits[0].flags = CM_HIR_DECL_TRAIT_HAS_LANG_ITEM
        | CM_HIR_DECL_TRAIT_HAS_DIAGNOSTIC_ITEM
        | CM_HIR_DECL_TRAIT_IS_CONST;
    fixture->traits[0].compiler_flags =
        CM_HIR_DECL_TRAIT_COMPILER_TRIVIAL_FIELD_READS;
    fixture->traits[0].lang_item = (CmHirDeclarationString)S("clone");
    fixture->traits[0].diagnostic_item =
        (CmHirDeclarationString)S("Clone");
    fixture->clone_supertraits[0].modifier =
        CM_HIR_DECL_SUPERTRAIT_REQUIRED;
    fixture->clone_supertraits[0].trait_local = 5u;
    fixture->traits[0].supertrait_count = 1u;
    fixture->traits[0].supertraits = fixture->clone_supertraits;

    fixture->traits[1].name = (CmHirDeclarationString)S("Destruct");
    fixture->traits[1].flags = CM_HIR_DECL_TRAIT_HAS_LANG_ITEM
        | CM_HIR_DECL_TRAIT_IS_CONST
        | CM_HIR_DECL_TRAIT_DENY_EXPLICIT_IMPL
        | CM_HIR_DECL_TRAIT_DO_NOT_IMPLEMENT_VIA_OBJECT;
    fixture->traits[1].lang_item =
        (CmHirDeclarationString)S("destruct");

    fixture->traits[2].name = (CmHirDeclarationString)S("MetaSized");
    fixture->traits[2].flags = sized_flags;
    fixture->traits[2].compiler_flags = sized_compiler_flags;
    fixture->traits[2].lang_item =
        (CmHirDeclarationString)S("meta_sized");
    fixture->meta_supertraits[0].modifier =
        CM_HIR_DECL_SUPERTRAIT_REQUIRED;
    fixture->meta_supertraits[0].trait_local = 4u;
    fixture->traits[2].supertrait_count = 1u;
    fixture->traits[2].supertraits = fixture->meta_supertraits;

    fixture->traits[3].name =
        (CmHirDeclarationString)S("PointeeSized");
    fixture->traits[3].flags = sized_flags;
    fixture->traits[3].compiler_flags = sized_compiler_flags;
    fixture->traits[3].lang_item =
        (CmHirDeclarationString)S("pointee_sized");

    fixture->traits[4].name = (CmHirDeclarationString)S("Sized");
    fixture->traits[4].flags = sized_flags;
    fixture->traits[4].compiler_flags = sized_compiler_flags;
    fixture->traits[4].lang_item = (CmHirDeclarationString)S("sized");
    fixture->sized_supertraits[0].modifier =
        CM_HIR_DECL_SUPERTRAIT_REQUIRED;
    fixture->sized_supertraits[0].trait_local = 3u;
    fixture->traits[4].supertrait_count = 1u;
    fixture->traits[4].supertraits = fixture->sized_supertraits;
    metadata->traits = fixture->traits;
    metadata->trait_count = 5u;

    fixture->associated[0].kind = CM_HIR_DECL_ASSOCIATED_METHOD;
    fixture->associated[0].parent_kind =
        CM_HIR_DECL_ASSOCIATED_PARENT_NOMINAL;
    fixture->associated[0].parent_local = 1u;
    fixture->associated[0].name = (CmHirDeclarationString)S("clone");
    fixture->associated[0].visibility.kind =
        CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->associated[0].source_ordinal = 1u;
    fixture->associated[0].receiver = CM_HIR_DECL_RECEIVER_REF_SHARED;
    fixture->clone_parameters[0] = 5u;
    fixture->associated[0].parameter_count = 1u;
    fixture->associated[0].parameter_types = fixture->clone_parameters;
    fixture->associated[0].return_type = 4u;
    fixture->associated[0].abi = (CmHirDeclarationString)S("Rust");
    fixture->associated[0].safety = CM_HIR_DECL_SAFETY_SAFE;
    fixture->associated[0].flags =
        CM_HIR_DECL_ASSOCIATED_HAS_LANG_ITEM;
    fixture->associated[0].lang_item =
        (CmHirDeclarationString)S("clone_fn");

    fixture->associated[1].kind = CM_HIR_DECL_ASSOCIATED_METHOD;
    fixture->associated[1].parent_kind =
        CM_HIR_DECL_ASSOCIATED_PARENT_NOMINAL;
    fixture->associated[1].parent_local = 1u;
    fixture->associated[1].name =
        (CmHirDeclarationString)S("clone_from");
    fixture->associated[1].visibility.kind =
        CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->associated[1].source_ordinal = 2u;
    fixture->associated[1].predicate_start = 2u;
    fixture->associated[1].predicate_count = 1u;
    fixture->associated[1].receiver = CM_HIR_DECL_RECEIVER_REF_MUTABLE;
    fixture->clone_from_parameters[0] = 6u;
    fixture->clone_from_parameters[1] = 5u;
    fixture->associated[1].parameter_count = 2u;
    fixture->associated[1].parameter_types =
        fixture->clone_from_parameters;
    fixture->associated[1].return_type = 1u;
    fixture->associated[1].abi = (CmHirDeclarationString)S("Rust");
    fixture->associated[1].safety = CM_HIR_DECL_SAFETY_SAFE;
    fixture->associated[1].has_default_body = 1u;
    metadata->associated_items = fixture->associated;
    metadata->associated_count = 2u;

    fixture->generics[0].owner_kind = CM_HIR_DECL_GENERIC_VALUE;
    fixture->generics[0].owner_local = 1u;
    fixture->generics[0].index = 0u;
    fixture->generics[0].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[0].name = (CmHirDeclarationString)S("T");
    fixture->generics[1].owner_kind = CM_HIR_DECL_GENERIC_VALUE;
    fixture->generics[1].owner_local = 1u;
    fixture->generics[1].index = 1u;
    fixture->generics[1].kind = CM_HIR_DECL_GENERIC_CONST;
    fixture->generics[1].name = (CmHirDeclarationString)S("N");
    fixture->generics[1].declared_type = 2u;
    metadata->generics = fixture->generics;
    metadata->generic_count = 2u;

    fixture->types[0].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[0].primitive = CM_HIR_DECL_PRIMITIVE_UNIT;
    fixture->types[1].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[1].primitive = CM_HIR_DECL_PRIMITIVE_USIZE;
    fixture->types[2].kind = CM_HIR_DECL_TYPE_GENERIC;
    fixture->types[2].generic_local = 1u;
    fixture->types[3].kind = CM_HIR_DECL_TYPE_SELF;
    fixture->types[3].self_trait_local = 1u;
    fixture->types[4].kind = CM_HIR_DECL_TYPE_REFERENCE;
    fixture->types[4].child_type = 4u;
    fixture->types[4].mutability = CM_HIR_DECL_IMMUTABLE;
    fixture->types[4].region.kind = CM_HIR_DECL_REGION_ERASED;
    fixture->types[5].kind = CM_HIR_DECL_TYPE_REFERENCE;
    fixture->types[5].child_type = 4u;
    fixture->types[5].mutability = CM_HIR_DECL_MUTABLE;
    fixture->types[5].region.kind = CM_HIR_DECL_REGION_ERASED;
    fixture->types[6].kind = CM_HIR_DECL_TYPE_ARRAY;
    fixture->types[6].child_type = 3u;
    fixture->types[6].array_length_kind =
        CM_HIR_DECL_ARRAY_LENGTH_CONST_PARAMETER;
    fixture->types[6].array_length_generic_local = 2u;
    metadata->types = fixture->types;
    metadata->type_count = 7u;

    fixture->predicates[0].owner_kind =
        CM_HIR_DECL_PREDICATE_OWNER_VALUE;
    fixture->predicates[0].owner_value = 1u;
    fixture->predicates[0].ordinal = 0u;
    fixture->predicates[0].subject_type = 3u;
    fixture->predicates[0].trait_local = 1u;
    fixture->predicates[0].modifier =
        CM_HIR_DECL_PREDICATE_REQUIRED;
    fixture->predicates[1].owner_kind =
        CM_HIR_DECL_PREDICATE_OWNER_ASSOCIATED;
    fixture->predicates[1].owner_associated = 2u;
    fixture->predicates[1].ordinal = 0u;
    fixture->predicates[1].subject_type = 4u;
    fixture->predicates[1].trait_local = 2u;
    fixture->predicates[1].modifier =
        CM_HIR_DECL_PREDICATE_CONST_IF_CONST;
    metadata->predicates = fixture->predicates;
    metadata->predicate_count = 2u;

    fixture->value_parameters[0] = 3u;
    fixture->values[0].kind = CM_HIR_DECL_VALUE_FUNCTION;
    fixture->values[0].owner_module = 1u;
    fixture->values[0].name = (CmHirDeclarationString)S("repeat");
    fixture->values[0].source_ordinal = 6u;
    fixture->values[0].generic_start = 1u;
    fixture->values[0].generic_count = 2u;
    fixture->values[0].predicate_start = 1u;
    fixture->values[0].predicate_count = 1u;
    fixture->values[0].parameter_count = 1u;
    fixture->values[0].parameter_types = fixture->value_parameters;
    fixture->values[0].return_type = 7u;
    fixture->values[0].has_body = 1u;
    metadata->values = fixture->values;
    metadata->value_count = 1u;

    for (index = 0u; index < 5u; ++index) {
        fixture->namespace_entries[index].owner_module = 1u;
        fixture->namespace_entries[index].namespace_kind =
            CM_HIR_DECL_NAMESPACE_TYPE;
        fixture->namespace_entries[index].name =
            fixture->traits[index].name;
        fixture->namespace_entries[index].target_kind =
            CM_HIR_DECL_TARGET_NOMINAL;
        fixture->namespace_entries[index].target_local =
            (uint32_t)(index + 1u);
        fixture->namespace_entries[index].export_ordinal =
            fixture->traits[index].source_ordinal;
    }
    fixture->namespace_entries[5].owner_module = 1u;
    fixture->namespace_entries[5].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[5].name = fixture->values[0].name;
    fixture->namespace_entries[5].target_kind = CM_HIR_DECL_TARGET_VALUE;
    fixture->namespace_entries[5].target_local = 1u;
    fixture->namespace_entries[5].export_ordinal = 6u;
    fixture->namespace_entries[6] = fixture->namespace_entries[5];
    fixture->namespace_entries[6].name =
        (CmHirDeclarationString)S("repeat_alias");
    fixture->namespace_entries[6].export_ordinal = 7u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 7u;
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
    fixture->types[3].array_length_kind = CM_HIR_DECL_ARRAY_LENGTH_SCALAR;
    fixture->types[3].array_length_type = 1u;
    fixture->types[3].array_length_low_bits = UINT64_C(1);
    fixture->types[4].kind = CM_HIR_DECL_TYPE_REFERENCE;
    fixture->types[4].child_type = 4u;
    fixture->types[4].mutability = CM_HIR_DECL_MUTABLE;
    fixture->types[4].region.kind = CM_HIR_DECL_REGION_ERASED;
    metadata->types = fixture->types;
    metadata->type_count = 5u;

    fixture->value_parameters[0] = 3u;
    fixture->values[0].kind = CM_HIR_DECL_VALUE_FUNCTION;
    fixture->values[0].owner_module = 1u;
    fixture->values[0].name = (CmHirDeclarationString)S("from_mut");
    fixture->values[0].source_ordinal = 1u;
    fixture->values[0].generic_start = 1u;
    fixture->values[0].generic_count = 1u;
    fixture->values[0].parameter_count = 1u;
    fixture->values[0].parameter_types = fixture->value_parameters;
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

static void from_ref_fixture_init(FromMutFixture *fixture)
{
    from_mut_fixture_init(fixture);
    fixture->metadata.crate_name =
        (CmHirDeclarationString)S("from_ref_like");
    fixture->metadata.crate_disambiguator =
        (CmHirDeclarationString)S("decl-from-ref-v1");
    fixture->modules[0].name = fixture->metadata.crate_name;
    fixture->types[2].mutability = CM_HIR_DECL_IMMUTABLE;
    fixture->types[4].mutability = CM_HIR_DECL_IMMUTABLE;
    fixture->values[0].name = (CmHirDeclarationString)S("from_ref");
    fixture->namespace_entries[0].name = fixture->values[0].name;
    fixture->namespace_entries[1].name =
        (CmHirDeclarationString)S("from_ref_alias");
}

typedef struct ContextLengths {
    size_t crates;
    size_t modules;
    size_t items;
    size_t bodies;
    size_t expressions;
    size_t types;
    size_t generics;
    size_t definitions;
    size_t strings;
} ContextLengths;

static void fixture_init(TestFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;
    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("depcrate");
    metadata->crate_disambiguator = (CmHirDeclarationString)S("gate-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;
    fixture->cfgs[0] = (CmHirDeclarationString)S("target_arch=x86_64");
    fixture->cfgs[1] =
        (CmHirDeclarationString)S("target_pointer_width=64");
    metadata->cfgs = fixture->cfgs;
    metadata->cfg_count = 2u;

    fixture->modules[0].name = metadata->crate_name;
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 1u;

    fixture->traits[0].owner_module = 1u;
    fixture->traits[0].name = (CmHirDeclarationString)S("Gate");
    fixture->traits[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->traits[0].source_ordinal = 1u;
    fixture->traits[0].generic_start = 1u;
    fixture->traits[0].generic_count = 1u;
    metadata->traits = fixture->traits;
    metadata->trait_count = 1u;

    fixture->generics[0].owner_kind = CM_HIR_DECL_GENERIC_NOMINAL;
    fixture->generics[0].owner_local = 1u;
    fixture->generics[0].index = 0u;
    fixture->generics[0].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[0].is_relaxed_sized = 1u;
    fixture->generics[0].name = (CmHirDeclarationString)S("T");
    fixture->generics[1].owner_kind = CM_HIR_DECL_GENERIC_VALUE;
    fixture->generics[1].owner_local = 1u;
    fixture->generics[1].index = 0u;
    fixture->generics[1].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[1].name = (CmHirDeclarationString)S("X");
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
    fixture->values[0].owner_module = 1u;
    fixture->values[0].name = (CmHirDeclarationString)S("needs");
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
    fixture->predicates[0].ordinal = 0u;
    fixture->predicates[0].subject_type = 3u;
    fixture->predicates[0].trait_local = 1u;
    fixture->predicates[0].argument_count = 1u;
    fixture->predicates[0].argument_types = fixture->predicate_arguments;
    metadata->predicates = fixture->predicates;
    metadata->predicate_count = 1u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind = CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[0].name = fixture->traits[0].name;
    fixture->namespace_entries[0].target_kind = CM_HIR_DECL_TARGET_NOMINAL;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 1u;
    fixture->namespace_entries[1].owner_module = 1u;
    fixture->namespace_entries[1].namespace_kind = CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[1].name =
        (CmHirDeclarationString)S("GateReexport");
    fixture->namespace_entries[1].target_kind = CM_HIR_DECL_TARGET_NOMINAL;
    fixture->namespace_entries[1].target_local = 1u;
    fixture->namespace_entries[1].export_ordinal = 2u;
    fixture->namespace_entries[2].owner_module = 1u;
    fixture->namespace_entries[2].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[2].name = fixture->values[0].name;
    fixture->namespace_entries[2].target_kind = CM_HIR_DECL_TARGET_VALUE;
    fixture->namespace_entries[2].target_local = 1u;
    fixture->namespace_entries[2].export_ordinal = 2u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 3u;
}

static void item_fixture_init(TestFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;
    fixture_init(fixture);
    metadata = &fixture->metadata;

    fixture->items[0].kind = CM_HIR_DECL_ITEM_STRUCT;
    fixture->items[0].owner_module = 1u;
    fixture->items[0].name = (CmHirDeclarationString)S("Packet");
    fixture->items[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[0].source_ordinal = 3u;
    fixture->items[0].aggregate_form = CM_HIR_DECL_AGGREGATE_UNIT;
    metadata->items = fixture->items;
    metadata->item_count = 1u;

    fixture->namespace_entries[2].owner_module = 1u;
    fixture->namespace_entries[2].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[2].name = fixture->items[0].name;
    fixture->namespace_entries[2].target_kind = CM_HIR_DECL_TARGET_ITEM;
    fixture->namespace_entries[2].target_local = 1u;
    fixture->namespace_entries[2].export_ordinal = 3u;
    fixture->namespace_entries[3] = fixture->namespace_entries[2];
    fixture->namespace_entries[3].name =
        (CmHirDeclarationString)S("PacketReexport");
    fixture->namespace_entries[3].export_ordinal = 4u;
    fixture->namespace_entries[4] = fixture->namespace_entries[2];
    fixture->namespace_entries[4].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[5] = fixture->namespace_entries[3];
    fixture->namespace_entries[5].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[6].owner_module = 1u;
    fixture->namespace_entries[6].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[6].name = fixture->values[0].name;
    fixture->namespace_entries[6].target_kind = CM_HIR_DECL_TARGET_VALUE;
    fixture->namespace_entries[6].target_local = 1u;
    fixture->namespace_entries[6].export_ordinal = 2u;
    metadata->namespace_count = 7u;
}

static void alias_fixture_init(AliasFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;
    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("depcrate");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("layout-error-v1");
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
    fixture->items[0].name = (CmHirDeclarationString)S("AllocError");
    fixture->items[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[0].source_ordinal = 1u;
    fixture->items[0].aggregate_form = CM_HIR_DECL_AGGREGATE_UNIT;
    fixture->items[1].kind = CM_HIR_DECL_ITEM_TYPE_ALIAS;
    fixture->items[1].owner_module = 1u;
    fixture->items[1].name = (CmHirDeclarationString)S("LayoutErr");
    fixture->items[1].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[1].source_ordinal = 2u;
    fixture->items[1].alias_target_type = 1u;
    fixture->items[2].kind = CM_HIR_DECL_ITEM_STRUCT;
    fixture->items[2].owner_module = 1u;
    fixture->items[2].name = (CmHirDeclarationString)S("LayoutError");
    fixture->items[2].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[2].source_ordinal = 3u;
    fixture->items[2].aggregate_form = CM_HIR_DECL_AGGREGATE_UNIT;
    metadata->items = fixture->items;
    metadata->item_count = 3u;

    fixture->types[0].kind = CM_HIR_DECL_TYPE_NAMED_ADT;
    fixture->types[0].item_local = 3u;
    metadata->types = fixture->types;
    metadata->type_count = 1u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[0].name = fixture->items[0].name;
    fixture->namespace_entries[0].target_kind = CM_HIR_DECL_TARGET_ITEM;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 1u;
    fixture->namespace_entries[1] = fixture->namespace_entries[0];
    fixture->namespace_entries[1].name = fixture->items[1].name;
    fixture->namespace_entries[1].target_local = 2u;
    fixture->namespace_entries[1].export_ordinal = 2u;
    fixture->namespace_entries[2] = fixture->namespace_entries[1];
    fixture->namespace_entries[2].name =
        (CmHirDeclarationString)S("LayoutErrReexport");
    fixture->namespace_entries[2].export_ordinal = 4u;
    fixture->namespace_entries[3] = fixture->namespace_entries[0];
    fixture->namespace_entries[3].name = fixture->items[2].name;
    fixture->namespace_entries[3].target_local = 3u;
    fixture->namespace_entries[3].export_ordinal = 3u;
    fixture->namespace_entries[4] = fixture->namespace_entries[3];
    fixture->namespace_entries[4].name =
        (CmHirDeclarationString)S("LayoutErrorReexport");
    fixture->namespace_entries[4].export_ordinal = 5u;
    fixture->namespace_entries[5] = fixture->namespace_entries[0];
    fixture->namespace_entries[5].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 6u;
}

static void composite_fixture_init(CompositeFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;
    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("depcrate");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("composite-v1");
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
    fixture->traits[0].name = (CmHirDeclarationString)S("Gate");
    fixture->traits[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->traits[0].source_ordinal = 1u;
    fixture->traits[0].generic_start = 1u;
    fixture->traits[0].generic_count = 1u;
    metadata->traits = fixture->traits;
    metadata->trait_count = 1u;

    fixture->items[0].kind = CM_HIR_DECL_ITEM_STRUCT;
    fixture->items[0].owner_module = 1u;
    fixture->items[0].name = (CmHirDeclarationString)S("Wrap");
    fixture->items[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[0].source_ordinal = 2u;
    fixture->items[0].aggregate_form = CM_HIR_DECL_AGGREGATE_UNIT;
    fixture->items[0].generic_start = 2u;
    fixture->items[0].generic_count = 1u;
    metadata->items = fixture->items;
    metadata->item_count = 1u;

    fixture->generics[0].owner_kind = CM_HIR_DECL_GENERIC_NOMINAL;
    fixture->generics[0].owner_local = 1u;
    fixture->generics[0].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[0].name = (CmHirDeclarationString)S("G");
    fixture->generics[1].owner_kind = CM_HIR_DECL_GENERIC_ITEM;
    fixture->generics[1].owner_local = 1u;
    fixture->generics[1].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[1].name = (CmHirDeclarationString)S("T");
    fixture->generics[2].owner_kind = CM_HIR_DECL_GENERIC_VALUE;
    fixture->generics[2].owner_local = 1u;
    fixture->generics[2].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[2].name = (CmHirDeclarationString)S("X");
    metadata->generics = fixture->generics;
    metadata->generic_count = 3u;

    fixture->types[0].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[0].primitive = CM_HIR_DECL_PRIMITIVE_UNIT;
    fixture->types[1].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[1].primitive = CM_HIR_DECL_PRIMITIVE_U8;
    fixture->types[2].kind = CM_HIR_DECL_TYPE_GENERIC;
    fixture->types[2].generic_local = 3u;
    fixture->types[3].kind = CM_HIR_DECL_TYPE_SLICE;
    fixture->types[3].child_type = 2u;
    fixture->types[4].kind = CM_HIR_DECL_TYPE_RAW_POINTER;
    fixture->types[4].child_type = 2u;
    fixture->types[4].mutability = CM_HIR_DECL_MUTABLE;
    fixture->types[5].kind = CM_HIR_DECL_TYPE_REFERENCE;
    fixture->types[5].child_type = 2u;
    fixture->types[5].mutability = CM_HIR_DECL_IMMUTABLE;
    fixture->types[5].region.kind = CM_HIR_DECL_REGION_STATIC;
    fixture->application_arguments[0] = 2u;
    fixture->types[6].kind = CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION;
    fixture->types[6].item_local = 1u;
    fixture->types[6].argument_count = 1u;
    fixture->types[6].argument_types = fixture->application_arguments;
    metadata->types = fixture->types;
    metadata->type_count = 7u;

    fixture->parameters[0] = 4u;
    fixture->parameters[1] = 5u;
    fixture->parameters[2] = 6u;
    fixture->parameters[3] = 7u;
    fixture->values[0].kind = CM_HIR_DECL_VALUE_FUNCTION;
    fixture->values[0].owner_module = 1u;
    fixture->values[0].name = (CmHirDeclarationString)S("inspect");
    fixture->values[0].source_ordinal = 3u;
    fixture->values[0].generic_start = 3u;
    fixture->values[0].generic_count = 1u;
    fixture->values[0].predicate_start = 1u;
    fixture->values[0].predicate_count = 1u;
    fixture->values[0].parameter_count = 4u;
    fixture->values[0].parameter_types = fixture->parameters;
    fixture->values[0].return_type = 1u;
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
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[0].name = fixture->traits[0].name;
    fixture->namespace_entries[0].target_kind = CM_HIR_DECL_TARGET_NOMINAL;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 1u;
    fixture->namespace_entries[1] = fixture->namespace_entries[0];
    fixture->namespace_entries[1].name = fixture->items[0].name;
    fixture->namespace_entries[1].target_kind = CM_HIR_DECL_TARGET_ITEM;
    fixture->namespace_entries[1].export_ordinal = 2u;
    fixture->namespace_entries[2] = fixture->namespace_entries[1];
    fixture->namespace_entries[2].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[3].owner_module = 1u;
    fixture->namespace_entries[3].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[3].name = fixture->values[0].name;
    fixture->namespace_entries[3].target_kind = CM_HIR_DECL_TARGET_VALUE;
    fixture->namespace_entries[3].target_local = 1u;
    fixture->namespace_entries[3].export_ordinal = 3u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 4u;
}

static void enum_fixture_init(EnumFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;

    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("depcrate");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("char-enum-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;
    fixture->modules[0].name = metadata->crate_name;
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 1u;

    fixture->variants[0].kind = CM_HIR_DECL_VARIANT_UNIT;
    fixture->variants[0].name = (CmHirDeclarationString)S("Null");
    fixture->variants[0].source_ordinal = 2u;
    fixture->variants[0].discriminant_primitive =
        CM_HIR_DECL_PRIMITIVE_ISIZE;
    fixture->variants[0].discriminant_low = 0u;
    fixture->variants[1].kind = CM_HIR_DECL_VARIANT_UNIT;
    fixture->variants[1].name = (CmHirDeclarationString)S("Scalar");
    fixture->variants[1].source_ordinal = 3u;
    fixture->variants[1].discriminant_primitive =
        CM_HIR_DECL_PRIMITIVE_ISIZE;
    fixture->variants[1].discriminant_low = 255u;

    fixture->items[0].kind = CM_HIR_DECL_ITEM_ENUM;
    fixture->items[0].owner_module = 1u;
    fixture->items[0].name = (CmHirDeclarationString)S("Char");
    fixture->items[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    /* Canonical item and defining export ordinals may begin at zero. */
    fixture->items[0].source_ordinal = 0u;
    fixture->items[0].enum_repr_primitive = CM_HIR_DECL_PRIMITIVE_U8;
    fixture->items[0].variant_count = 2u;
    fixture->items[0].variants = fixture->variants;
    metadata->items = fixture->items;
    metadata->item_count = 1u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[0].name = fixture->items[0].name;
    fixture->namespace_entries[0].target_kind = CM_HIR_DECL_TARGET_ITEM;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 0u;
    fixture->namespace_entries[1] = fixture->namespace_entries[0];
    fixture->namespace_entries[1].name =
        (CmHirDeclarationString)S("CharReexport");
    fixture->namespace_entries[1].export_ordinal = 1u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 2u;
}

static void default_enum_fixture_init(DefaultEnumFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;

    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("depcrate");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("mir-reason-enum-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;
    fixture->modules[0].name = metadata->crate_name;
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 1u;

    fixture->variants[0].kind = CM_HIR_DECL_VARIANT_UNIT;
    fixture->variants[0].name = (CmHirDeclarationString)S("Normal");
    fixture->variants[0].source_ordinal = 1u;
    fixture->variants[0].discriminant_primitive =
        CM_HIR_DECL_VARIANT_DISCRIMINANT_IMPLICIT;
    fixture->variants[1].kind = CM_HIR_DECL_VARIANT_UNIT;
    fixture->variants[1].name = (CmHirDeclarationString)S("Cleanup");
    fixture->variants[1].source_ordinal = 2u;
    fixture->variants[1].discriminant_primitive =
        CM_HIR_DECL_VARIANT_DISCRIMINANT_IMPLICIT;

    fixture->items[0].kind = CM_HIR_DECL_ITEM_ENUM;
    fixture->items[0].owner_module = 1u;
    fixture->items[0].name = (CmHirDeclarationString)S("BasicBlock");
    fixture->items[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[0].source_ordinal = 0u;
    fixture->items[0].enum_repr_primitive = CM_HIR_DECL_ENUM_REPR_RUST;
    fixture->items[0].variant_count = 2u;
    fixture->items[0].variants = fixture->variants;
    fixture->items[0].diagnostic_item =
        (CmHirDeclarationString)S("mir_basic_block");
    fixture->items[1] = fixture->items[0];
    fixture->items[1].name =
        (CmHirDeclarationString)S("UnwindTerminateReason");
    fixture->items[1].source_ordinal = 3u;
    fixture->items[1].variants = &fixture->variants[2];
    fixture->items[1].diagnostic_item =
        (CmHirDeclarationString)S("mir_unwind_terminate_reason");
    fixture->variants[2] = fixture->variants[0];
    fixture->variants[2].name = (CmHirDeclarationString)S("Abi");
    fixture->variants[2].source_ordinal = 4u;
    fixture->variants[3] = fixture->variants[1];
    fixture->variants[3].name = (CmHirDeclarationString)S("InCleanup");
    fixture->variants[3].source_ordinal = 5u;
    metadata->items = fixture->items;
    metadata->item_count = 2u;

    /* Canonical NSPC order is TYPE names followed by VALUE names. */
    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[0].name = fixture->items[0].name;
    fixture->namespace_entries[0].target_kind =
        CM_HIR_DECL_TARGET_ITEM;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 0u;
    fixture->namespace_entries[1] = fixture->namespace_entries[0];
    fixture->namespace_entries[1].name =
        (CmHirDeclarationString)S("ReasonAbi");
    fixture->namespace_entries[1].target_kind =
        CM_HIR_DECL_TARGET_ENUM_VARIANT;
    fixture->namespace_entries[1].target_local = 3u;
    fixture->namespace_entries[1].export_ordinal = 6u;
    fixture->namespace_entries[2] = fixture->namespace_entries[0];
    fixture->namespace_entries[2].name =
        (CmHirDeclarationString)S("ReasonInCleanup");
    fixture->namespace_entries[2].target_kind =
        CM_HIR_DECL_TARGET_ENUM_VARIANT;
    fixture->namespace_entries[2].target_local = 4u;
    fixture->namespace_entries[2].export_ordinal = 6u;
    fixture->namespace_entries[3] = fixture->namespace_entries[0];
    fixture->namespace_entries[3].name = fixture->items[1].name;
    fixture->namespace_entries[3].target_local = 2u;
    fixture->namespace_entries[3].export_ordinal = 3u;
    fixture->namespace_entries[4] = fixture->namespace_entries[1];
    fixture->namespace_entries[4].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[5] = fixture->namespace_entries[2];
    fixture->namespace_entries[5].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 6u;
}

static void option_fixture_init(OptionFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;

    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("depcrate");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("option-tuple-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;
    fixture->modules[0].name = metadata->crate_name;
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 1u;

    fixture->generics[0].owner_kind = CM_HIR_DECL_GENERIC_ITEM;
    fixture->generics[0].owner_local = 1u;
    fixture->generics[0].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[0].name = (CmHirDeclarationString)S("T");
    metadata->generics = fixture->generics;
    metadata->generic_count = 1u;

    fixture->types[0].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[0].primitive = CM_HIR_DECL_PRIMITIVE_U8;
    fixture->types[1].kind = CM_HIR_DECL_TYPE_GENERIC;
    fixture->types[1].generic_local = 1u;
    fixture->application_arguments[0] = 1u;
    fixture->types[2].kind = CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION;
    fixture->types[2].item_local = 1u;
    fixture->types[2].argument_count = 1u;
    fixture->types[2].argument_types = fixture->application_arguments;
    metadata->types = fixture->types;
    metadata->type_count = 3u;

    fixture->variants[0].kind = CM_HIR_DECL_VARIANT_UNIT;
    fixture->variants[0].name = (CmHirDeclarationString)S("None");
    fixture->variants[0].source_ordinal = 2u;
    fixture->variants[0].discriminant_primitive =
        CM_HIR_DECL_VARIANT_DISCRIMINANT_IMPLICIT;
    fixture->variants[0].flags = CM_HIR_DECL_VARIANT_HAS_LANG_ITEM;
    fixture->variants[0].lang_item = (CmHirDeclarationString)S("None");
    fixture->variants[1].kind = CM_HIR_DECL_VARIANT_TUPLE;
    fixture->variants[1].name = (CmHirDeclarationString)S("Some");
    fixture->variants[1].source_ordinal = 3u;
    fixture->variants[1].discriminant_primitive =
        CM_HIR_DECL_VARIANT_DISCRIMINANT_IMPLICIT;
    fixture->variants[1].flags = CM_HIR_DECL_VARIANT_HAS_LANG_ITEM;
    fixture->variants[1].lang_item = (CmHirDeclarationString)S("Some");
    fixture->variants[1].field_count = 1u;
    fixture->variants[1].fields = fixture->some_fields;
    fixture->some_fields[0].source_ordinal = 0u;
    fixture->some_fields[0].type_local = 2u;

    fixture->items[0].kind = CM_HIR_DECL_ITEM_ENUM;
    fixture->items[0].owner_module = 1u;
    fixture->items[0].name = (CmHirDeclarationString)S("Option");
    fixture->items[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[0].source_ordinal = 1u;
    fixture->items[0].generic_start = 1u;
    fixture->items[0].generic_count = 1u;
    fixture->items[0].enum_repr_primitive = CM_HIR_DECL_ENUM_REPR_RUST;
    fixture->items[0].variant_count = 2u;
    fixture->items[0].variants = fixture->variants;
    fixture->items[0].diagnostic_item =
        (CmHirDeclarationString)S("Option");
    fixture->items[0].enum_flags = CM_HIR_DECL_ENUM_HAS_LANG_ITEM;
    fixture->items[0].enum_lang_item =
        (CmHirDeclarationString)S("Option");
    metadata->items = fixture->items;
    metadata->item_count = 1u;

    fixture->values[0].kind = CM_HIR_DECL_VALUE_STATIC;
    fixture->values[0].owner_module = 1u;
    fixture->values[0].name = (CmHirDeclarationString)S("WITNESS");
    fixture->values[0].source_ordinal = 5u;
    fixture->values[0].declared_type = 3u;
    fixture->values[0].mutability = CM_HIR_DECL_IMMUTABLE;
    fixture->values[0].has_body = 1u;
    metadata->values = fixture->values;
    metadata->value_count = 1u;

    /* Canonical TYPE entries, then canonical VALUE entries. */
    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[0].name =
        (CmHirDeclarationString)S("None");
    fixture->namespace_entries[0].target_kind =
        CM_HIR_DECL_TARGET_ENUM_VARIANT;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 4u;
    fixture->namespace_entries[1] = fixture->namespace_entries[0];
    fixture->namespace_entries[1].name = fixture->items[0].name;
    fixture->namespace_entries[1].target_kind = CM_HIR_DECL_TARGET_ITEM;
    fixture->namespace_entries[1].target_local = 1u;
    fixture->namespace_entries[1].export_ordinal = 1u;
    fixture->namespace_entries[2] = fixture->namespace_entries[1];
    fixture->namespace_entries[2].name =
        (CmHirDeclarationString)S("OptionAlias");
    fixture->namespace_entries[2].export_ordinal = 6u;
    fixture->namespace_entries[3] = fixture->namespace_entries[0];
    fixture->namespace_entries[3].name =
        (CmHirDeclarationString)S("Some");
    fixture->namespace_entries[3].target_local = 2u;
    fixture->namespace_entries[4] = fixture->namespace_entries[0];
    fixture->namespace_entries[4].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[5] = fixture->namespace_entries[3];
    fixture->namespace_entries[5].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[6].owner_module = 1u;
    fixture->namespace_entries[6].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[6].name = fixture->values[0].name;
    fixture->namespace_entries[6].target_kind = CM_HIR_DECL_TARGET_VALUE;
    fixture->namespace_entries[6].target_local = 1u;
    fixture->namespace_entries[6].export_ordinal = 5u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 7u;
}

static void const_fixture_init(ConstFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;

    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("depcrate");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("char-const-v1");
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
    fixture->types[0].primitive = CM_HIR_DECL_PRIMITIVE_CHAR;
    metadata->types = fixture->types;
    metadata->type_count = 1u;

    fixture->values[0].kind = CM_HIR_DECL_VALUE_CONST;
    fixture->values[0].owner_module = 1u;
    fixture->values[0].name = (CmHirDeclarationString)S("MAX");
    fixture->values[0].source_ordinal = 1u;
    fixture->values[0].declared_type = 1u;
    fixture->values[0].mutability = CM_HIR_DECL_IMMUTABLE;
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
        (CmHirDeclarationString)S("MAX_REEXPORT");
    fixture->namespace_entries[1].export_ordinal = 2u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 2u;
}

static void static_fixture_init(StaticFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;

    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("depcrate");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("cached-pow10-static-v1");
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
    fixture->namespace_entries[0].target_kind = CM_HIR_DECL_TARGET_VALUE;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 1u;
    fixture->namespace_entries[1] = fixture->namespace_entries[0];
    fixture->namespace_entries[1].name =
        (CmHirDeclarationString)S("CACHED_POW10_ALIAS");
    fixture->namespace_entries[1].export_ordinal = 2u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 2u;
}

static void primitive_fixture_init(PrimitiveFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;
    size_t index;

    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("depcrate");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("primitive-bindings-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;
    fixture->modules[0].name = metadata->crate_name;
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 1u;

    for (index = 0u; index < PRIMITIVE_BINDING_COUNT; ++index) {
        CmHirDeclarationNamespaceEntry *entry =
            &fixture->namespace_entries[index];
        entry->owner_module = 1u;
        entry->namespace_kind = CM_HIR_DECL_NAMESPACE_TYPE;
        entry->name.data = (unsigned char *)primitive_binding_specs[index].name;
        entry->name.length = strlen(primitive_binding_specs[index].name);
        entry->target_kind = CM_HIR_DECL_TARGET_PRIMITIVE;
        entry->target_local = primitive_binding_specs[index].declaration_kind;
        entry->export_ordinal = (uint32_t)index + 1u;
    }
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = PRIMITIVE_BINDING_COUNT;
}

static void aggregate_fixture_init(AggregateFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;
    uint32_t index;

    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("depcrate");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("memory-aggregates-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;
    fixture->modules[0].name = metadata->crate_name;
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 1u;

    fixture->generics[0].owner_kind = CM_HIR_DECL_GENERIC_ITEM;
    fixture->generics[0].owner_local = 2u;
    fixture->generics[0].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[0].is_relaxed_sized = 1u;
    fixture->generics[0].name = (CmHirDeclarationString)S("T");
    fixture->generics[1].owner_kind = CM_HIR_DECL_GENERIC_ITEM;
    fixture->generics[1].owner_local = 3u;
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
    fixture->types[4].item_local = 2u;
    fixture->types[4].argument_count = 1u;
    fixture->types[4].argument_types = fixture->application_arguments;
    metadata->types = fixture->types;
    metadata->type_count = 5u;

    fixture->items[0].kind = CM_HIR_DECL_ITEM_STRUCT;
    fixture->items[0].owner_module = 1u;
    fixture->items[0].name = (CmHirDeclarationString)S("Assume");
    fixture->items[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[0].aggregate_form = CM_HIR_DECL_AGGREGATE_NAMED;
    fixture->items[0].aggregate_repr = CM_HIR_DECL_AGGREGATE_REPR_RUST;
    fixture->items[0].aggregate_flags =
        CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM;
    fixture->items[0].lang_item =
        (CmHirDeclarationString)S("transmute_opts");
    fixture->items[0].field_count = 4u;
    fixture->items[0].fields = fixture->fields;
    fixture->items[1].kind = CM_HIR_DECL_ITEM_STRUCT;
    fixture->items[1].owner_module = 1u;
    fixture->items[1].name = (CmHirDeclarationString)S("ManuallyDrop");
    fixture->items[1].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[1].source_ordinal = 5u;
    fixture->items[1].generic_start = 1u;
    fixture->items[1].generic_count = 1u;
    fixture->items[1].aggregate_form = CM_HIR_DECL_AGGREGATE_NAMED;
    fixture->items[1].aggregate_repr =
        CM_HIR_DECL_AGGREGATE_REPR_TRANSPARENT;
    fixture->items[1].aggregate_flags =
        CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM
        | CM_HIR_DECL_AGGREGATE_RUSTC_PUB_TRANSPARENT;
    fixture->items[1].lang_item =
        (CmHirDeclarationString)S("manually_drop");
    fixture->items[1].field_count = 1u;
    fixture->items[1].fields = &fixture->fields[4];
    fixture->items[2].kind = CM_HIR_DECL_ITEM_UNION;
    fixture->items[2].owner_module = 1u;
    fixture->items[2].name = (CmHirDeclarationString)S("MaybeUninit");
    fixture->items[2].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[2].source_ordinal = 7u;
    fixture->items[2].generic_start = 2u;
    fixture->items[2].generic_count = 1u;
    fixture->items[2].aggregate_form = CM_HIR_DECL_AGGREGATE_NAMED;
    fixture->items[2].aggregate_repr =
        CM_HIR_DECL_AGGREGATE_REPR_TRANSPARENT;
    fixture->items[2].aggregate_flags =
        CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM
        | CM_HIR_DECL_AGGREGATE_RUSTC_PUB_TRANSPARENT;
    fixture->items[2].lang_item =
        (CmHirDeclarationString)S("maybe_uninit");
    fixture->items[2].field_count = 2u;
    fixture->items[2].fields = &fixture->fields[5];
    metadata->items = fixture->items;
    metadata->item_count = 3u;

    fixture->fields[0].name = (CmHirDeclarationString)S("alignment");
    fixture->fields[1].name = (CmHirDeclarationString)S("lifetimes");
    fixture->fields[2].name = (CmHirDeclarationString)S("safety");
    fixture->fields[3].name = (CmHirDeclarationString)S("validity");
    for (index = 0u; index < 4u; ++index) {
        fixture->fields[index].visibility.kind =
            CM_HIR_DECL_VISIBILITY_PUBLIC;
        fixture->fields[index].source_ordinal = index;
        fixture->fields[index].type_local = 2u;
    }
    fixture->fields[4].name = (CmHirDeclarationString)S("value");
    fixture->fields[4].visibility.kind =
        CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->fields[4].source_ordinal = 6u;
    fixture->fields[4].type_local = 3u;
    fixture->fields[5].name = (CmHirDeclarationString)S("uninit");
    fixture->fields[5].visibility.kind =
        CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->fields[5].source_ordinal = 8u;
    fixture->fields[5].type_local = 1u;
    fixture->fields[6].name = (CmHirDeclarationString)S("value");
    fixture->fields[6].visibility.kind =
        CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->fields[6].source_ordinal = 9u;
    fixture->fields[6].type_local = 5u;

    for (index = 0u; index < 6u; ++index) {
        uint32_t target = index < 2u ? 1u : index < 4u ? 2u : 3u;
        fixture->namespace_entries[index].owner_module = 1u;
        fixture->namespace_entries[index].namespace_kind =
            CM_HIR_DECL_NAMESPACE_TYPE;
        fixture->namespace_entries[index].target_kind =
            CM_HIR_DECL_TARGET_ITEM;
        fixture->namespace_entries[index].target_local = target;
    }
    fixture->namespace_entries[0].name = fixture->items[0].name;
    fixture->namespace_entries[0].export_ordinal = 0u;
    fixture->namespace_entries[1].name =
        (CmHirDeclarationString)S("AssumeAlias");
    fixture->namespace_entries[1].export_ordinal = 10u;
    fixture->namespace_entries[2].name = fixture->items[1].name;
    fixture->namespace_entries[2].export_ordinal = 5u;
    fixture->namespace_entries[3].name =
        (CmHirDeclarationString)S("ManuallyDropAlias");
    fixture->namespace_entries[3].export_ordinal = 11u;
    fixture->namespace_entries[4].name = fixture->items[2].name;
    fixture->namespace_entries[4].export_ordinal = 7u;
    fixture->namespace_entries[5].name =
        (CmHirDeclarationString)S("MaybeUninitAlias");
    fixture->namespace_entries[5].export_ordinal = 12u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 6u;
}

static void layout_fixture_init(LayoutFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;
    uint32_t index;

    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("layout_dep");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("layout-closure-v1");
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
    fixture->types[0].primitive = CM_HIR_DECL_PRIMITIVE_USIZE;
    fixture->types[1].kind = CM_HIR_DECL_TYPE_NAMED_ADT;
    fixture->types[1].item_local = 1u;
    fixture->types[2].kind = CM_HIR_DECL_TYPE_NAMED_ADT;
    fixture->types[2].item_local = 2u;
    metadata->types = fixture->types;
    metadata->type_count = 3u;

    fixture->items[0].kind = CM_HIR_DECL_ITEM_STRUCT;
    fixture->items[0].owner_module = 1u;
    fixture->items[0].name = (CmHirDeclarationString)S("Alignment");
    fixture->items[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[0].source_ordinal = 1u;
    fixture->items[0].aggregate_form = CM_HIR_DECL_AGGREGATE_TUPLE;
    fixture->items[0].aggregate_repr =
        CM_HIR_DECL_AGGREGATE_REPR_TRANSPARENT;
    fixture->items[0].field_count = 1u;
    fixture->items[0].fields = fixture->fields;
    fixture->fields[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->fields[0].source_ordinal = 0u;
    fixture->fields[0].type_local = 3u;

    fixture->items[1].kind = CM_HIR_DECL_ITEM_ENUM;
    fixture->items[1].owner_module = 1u;
    fixture->items[1].name =
        (CmHirDeclarationString)S("AlignmentEnum");
    fixture->items[1].visibility.kind = CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->items[1].source_ordinal = 2u;
    fixture->items[1].enum_repr_primitive =
        CM_HIR_DECL_ENUM_REPR_U64;
    fixture->items[1].variant_count = 4u;
    fixture->items[1].variants = fixture->variants;
    for (index = 0u; index < 4u; ++index) {
        fixture->variants[index].kind = CM_HIR_DECL_VARIANT_UNIT;
        fixture->variants[index].source_ordinal = index;
        fixture->variants[index].discriminant_primitive =
            CM_HIR_DECL_PRIMITIVE_ISIZE;
        fixture->variants[index].discriminant_low =
            index == 0u ? UINT64_C(1)
            : index == 1u ? UINT64_C(2)
            : index == 2u ? UINT64_C(4) : UINT64_C(1) << 63;
    }
    fixture->variants[0].name = (CmHirDeclarationString)S("Align1");
    fixture->variants[1].name = (CmHirDeclarationString)S("Align2");
    fixture->variants[2].name = (CmHirDeclarationString)S("Align4");
    fixture->variants[3].name = (CmHirDeclarationString)S("AlignHigh");

    fixture->items[2].kind = CM_HIR_DECL_ITEM_STRUCT;
    fixture->items[2].owner_module = 1u;
    fixture->items[2].name = (CmHirDeclarationString)S("Layout");
    fixture->items[2].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[2].source_ordinal = 4u;
    fixture->items[2].aggregate_form = CM_HIR_DECL_AGGREGATE_NAMED;
    fixture->items[2].aggregate_repr = CM_HIR_DECL_AGGREGATE_REPR_RUST;
    fixture->items[2].aggregate_flags =
        CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM;
    fixture->items[2].field_count = 2u;
    fixture->items[2].fields = &fixture->fields[1];
    fixture->items[2].lang_item =
        (CmHirDeclarationString)S("alloc_layout");
    fixture->fields[1].name = (CmHirDeclarationString)S("size");
    fixture->fields[1].visibility.kind = CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->fields[1].source_ordinal = 0u;
    fixture->fields[1].type_local = 1u;
    fixture->fields[2].name = (CmHirDeclarationString)S("align");
    fixture->fields[2].visibility.kind = CM_HIR_DECL_VISIBILITY_PRIVATE;
    fixture->fields[2].source_ordinal = 1u;
    fixture->fields[2].type_local = 2u;
    metadata->items = fixture->items;
    metadata->item_count = 3u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[0].name = fixture->items[0].name;
    fixture->namespace_entries[0].target_kind = CM_HIR_DECL_TARGET_ITEM;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 1u;
    fixture->namespace_entries[1] = fixture->namespace_entries[0];
    fixture->namespace_entries[1].name =
        (CmHirDeclarationString)S("AlignmentReexport");
    fixture->namespace_entries[1].export_ordinal = 5u;
    fixture->namespace_entries[2] = fixture->namespace_entries[0];
    fixture->namespace_entries[2].name = fixture->items[2].name;
    fixture->namespace_entries[2].target_local = 3u;
    fixture->namespace_entries[2].export_ordinal = 4u;
    fixture->namespace_entries[3] = fixture->namespace_entries[2];
    fixture->namespace_entries[3].name =
        (CmHirDeclarationString)S("LayoutReexport");
    fixture->namespace_entries[3].export_ordinal = 6u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 4u;
}

static void type_id_fixture_init(TypeIdFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;

    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("type_id_dep");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("type-id-closure-v1");
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
    fixture->types[1].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[1].primitive = CM_HIR_DECL_PRIMITIVE_USIZE;
    fixture->types[2].kind = CM_HIR_DECL_TYPE_RAW_POINTER;
    fixture->types[2].child_type = 1u;
    fixture->types[2].mutability = CM_HIR_DECL_IMMUTABLE;
    fixture->types[3].kind = CM_HIR_DECL_TYPE_ARRAY;
    fixture->types[3].child_type = 3u;
    fixture->types[3].array_length_type = 2u;
    fixture->types[3].array_length_low_bits = 2u;
    metadata->types = fixture->types;
    metadata->type_count = 4u;

    fixture->items[0].kind = CM_HIR_DECL_ITEM_STRUCT;
    fixture->items[0].owner_module = 1u;
    fixture->items[0].name = (CmHirDeclarationString)S("TypeIdLike");
    fixture->items[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[0].source_ordinal = 1u;
    fixture->items[0].aggregate_form = CM_HIR_DECL_AGGREGATE_NAMED;
    fixture->items[0].aggregate_repr = CM_HIR_DECL_AGGREGATE_REPR_RUST;
    fixture->items[0].aggregate_flags =
        CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM;
    fixture->items[0].lang_item = (CmHirDeclarationString)S("type_id");
    fixture->items[0].field_count = 1u;
    fixture->items[0].fields = fixture->fields;
    fixture->fields[0].name = (CmHirDeclarationString)S("data");
    fixture->fields[0].visibility.kind =
        CM_HIR_DECL_VISIBILITY_CRATE;
    fixture->fields[0].source_ordinal = 0u;
    fixture->fields[0].type_local = 4u;
    metadata->items = fixture->items;
    metadata->item_count = 1u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[0].name =
        (CmHirDeclarationString)S("TypeIdAlias");
    fixture->namespace_entries[0].target_kind = CM_HIR_DECL_TARGET_ITEM;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 2u;
    fixture->namespace_entries[1] = fixture->namespace_entries[0];
    fixture->namespace_entries[1].name = fixture->items[0].name;
    fixture->namespace_entries[1].export_ordinal = 1u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 2u;
}

static void type_name_fixture_init(TypeNameFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;

    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("name_dep");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("type-name-declaration-v1");
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

    fixture->values[0].kind = CM_HIR_DECL_VALUE_FUNCTION;
    fixture->values[0].owner_module = 1u;
    fixture->values[0].name = (CmHirDeclarationString)S("name_of");
    fixture->values[0].source_ordinal = 1u;
    fixture->values[0].generic_start = 1u;
    fixture->values[0].generic_count = 1u;
    fixture->parameters[0] = 4u;
    fixture->values[0].parameter_types = fixture->parameters;
    fixture->values[0].parameter_count = 1u;
    fixture->values[0].return_type = 3u;
    fixture->values[0].has_body = 1u;
    fixture->values[0].is_const = 1u;
    metadata->values = fixture->values;
    metadata->value_count = 1u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[0].name =
        (CmHirDeclarationString)S("name_alias");
    fixture->namespace_entries[0].target_kind = CM_HIR_DECL_TARGET_VALUE;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 2u;
    fixture->namespace_entries[1] = fixture->namespace_entries[0];
    fixture->namespace_entries[1].name = fixture->values[0].name;
    fixture->namespace_entries[1].export_ordinal = 1u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 2u;
}

static void unit_function_fixture_init(UnitFunctionFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;

    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("arch_dep");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("unit-function-declaration-v1");
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
    fixture->values[0].name =
        (CmHirDeclarationString)S("breakpoint_like");
    fixture->values[0].source_ordinal = 1u;
    fixture->values[0].return_type = 1u;
    fixture->values[0].has_body = 1u;
    metadata->values = fixture->values;
    metadata->value_count = 1u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[0].name =
        (CmHirDeclarationString)S("breakpoint_alias");
    fixture->namespace_entries[0].target_kind =
        CM_HIR_DECL_TARGET_VALUE;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 2u;
    fixture->namespace_entries[1] = fixture->namespace_entries[0];
    fixture->namespace_entries[1].name = fixture->values[0].name;
    fixture->namespace_entries[1].export_ordinal = 1u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 2u;
}

static CmHirDeclarationMaterializeExpectation expectation_for(
    const CmHirDeclarationMetadata *metadata)
{
    CmHirDeclarationMaterializeExpectation expectation;
    memset(&expectation, 0, sizeof(expectation));
    expectation.crate_name = metadata->crate_name;
    expectation.crate_disambiguator = metadata->crate_disambiguator;
    expectation.edition = metadata->edition;
    expectation.target_triple = metadata->target_triple;
    expectation.data_layout = metadata->data_layout;
    expectation.panic_strategy = metadata->panic_strategy;
    expectation.cfgs = metadata->cfgs;
    expectation.cfg_count = metadata->cfg_count;
    return expectation;
}

static ContextLengths context_lengths(const CmHirContext *context)
{
    ContextLengths lengths;
    lengths.crates = context->crates.len;
    lengths.modules = context->modules.len;
    lengths.items = context->items.len;
    lengths.bodies = context->bodies.len;
    lengths.expressions = context->expressions.len;
    lengths.types = context->types.len;
    lengths.generics = context->generic_parameters.len;
    lengths.definitions = context->definitions.len;
    lengths.strings = cm_interner_length(&context->strings);
    return lengths;
}

static void assert_context_lengths(const CmHirContext *context,
    ContextLengths expected)
{
    ContextLengths actual = context_lengths(context);
    assert(actual.crates == expected.crates);
    assert(actual.modules == expected.modules);
    assert(actual.items == expected.items);
    assert(actual.bodies == expected.bodies);
    assert(actual.expressions == expected.expressions);
    assert(actual.types == expected.types);
    assert(actual.generics == expected.generics);
    assert(actual.definitions == expected.definitions);
    assert(actual.strings == expected.strings);
}

static CmHirLibraryBinding lookup_binding(const CmHirLibraryArtifact *artifact,
    const char *name)
{
    CmHirLibraryPathSegment path[2];
    CmHirLibraryBinding binding;
    path[0].bytes = (const unsigned char *)"dep";
    path[0].length = sizeof("dep") - 1u;
    path[1].bytes = (const unsigned char *)name;
    path[1].length = strlen(name);
    memset(&binding, 0, sizeof(binding));
    assert(cm_hir_library_artifact_lookup_binding(artifact, path, 2u,
        &binding) == CM_HIR_LIBRARY_OK);
    return binding;
}

static CmHirLibraryBinding lookup_value_binding(
    const CmHirLibraryArtifact *artifact, const char *name)
{
    CmHirLibraryPathSegment path[2];
    CmHirLibraryBinding binding;
    path[0].bytes = (const unsigned char *)"dep";
    path[0].length = sizeof("dep") - 1u;
    path[1].bytes = (const unsigned char *)name;
    path[1].length = strlen(name);
    memset(&binding, 0, sizeof(binding));
    assert(cm_hir_library_artifact_lookup_value_binding(artifact, path, 2u,
        &binding) == CM_HIR_LIBRARY_OK);
    return binding;
}

static CmHirLibraryStatus lookup_value_binding_status(
    const CmHirLibraryArtifact *artifact, const char *name)
{
    CmHirLibraryPathSegment path[2];
    CmHirLibraryBinding binding;
    path[0].bytes = (const unsigned char *)"dep";
    path[0].length = sizeof("dep") - 1u;
    path[1].bytes = (const unsigned char *)name;
    path[1].length = strlen(name);
    memset(&binding, 0, sizeof(binding));
    return cm_hir_library_artifact_lookup_value_binding(artifact, path, 2u,
        &binding);
}

static const CmHirItem *find_item(const CmHirContext *context,
    CmHirItemKind kind, const char *name)
{
    size_t index;
    size_t length = strlen(name);
    for (index = 0u; index < context->items.len; ++index) {
        const CmHirItem *item = (const CmHirItem *)cm_vec_at_const(
            &context->items, index);
        const CmInternedString *item_name = item == NULL ? NULL
            : cm_interner_get(&context->strings, item->name);
        if (item != NULL && item->kind == kind && item_name != NULL
            && item_name->len == length
            && memcmp(item_name->bytes, name, length) == 0) return item;
    }
    return NULL;
}

static const CmHirItem *find_associated_item(const CmHirContext *context,
    CmHirItemKind kind, CmHirDefId parent, const char *name)
{
    size_t index;
    size_t length = strlen(name);
    for (index = 0u; index < context->items.len; ++index) {
        const CmHirItem *item = (const CmHirItem *)cm_vec_at_const(
            &context->items, index);
        const CmInternedString *item_name = item == NULL ? NULL
            : cm_interner_get(&context->strings, item->name);
        if (item != NULL && item->kind == kind
            && cm_hir_def_id_equal(item->parent_definition, parent)
            && item_name != NULL && item_name->len == length
            && memcmp(item_name->bytes, name, length) == 0) return item;
    }
    return NULL;
}

static void assert_item_attribute(const CmHirContext *context,
    const CmHirItem *item, uint32_t index, const char *expected,
    CmSourceId source)
{
    const CmHirAttribute *attribute;
    const CmInternedString *metadata;
    size_t length = strlen(expected);
    assert(item != NULL && index < item->attribute_count
        && item->attributes != NULL);
    attribute = &item->attributes[index];
    metadata = cm_interner_get(&context->strings, attribute->metadata);
    assert(metadata != NULL && metadata->len == length
        && memcmp(metadata->bytes, expected, length) == 0
        && attribute->span.source == source
        && attribute->span.start == item->span.start
        && attribute->span.end == item->span.end
        && attribute->source_attribute == index + 1u
        && attribute->expansion_depth == 0u);
}

static void assert_gate_predicate(const CmHirContext *context,
    const CmHirItem *item, CmHirDefId gate)
{
    const CmHirTraitPredicate *predicate;
    const CmHirType *subject;
    const CmHirType *argument;
    const CmHirGenericParam *parameter;
    assert(item != NULL && item->kind == CM_HIR_ITEM_FUNCTION
        && item->generic_parameter_count == 1u
        && item->predicate_count == 1u);
    predicate = &item->predicates[0];
    assert(cm_hir_def_id_equal(predicate->trait_type.definition, gate)
        && predicate->trait_type.argument_count == 1u
        && predicate->trait_type.arguments[0].kind == CM_HIR_GENERIC_ARG_TYPE);
    subject = cm_hir_get_type(context, predicate->subject);
    argument = cm_hir_get_type(context,
        predicate->trait_type.arguments[0].data.type);
    parameter = cm_hir_get_generic_param(context,
        item->generic_parameter_start);
    assert(subject != NULL && subject->kind == CM_HIR_TYPE_PARAMETER_KIND
        && subject->data.parameter_type.parameter
            == item->generic_parameter_start
        && parameter != NULL && parameter->kind == CM_HIR_GENERIC_TYPE
        && cm_hir_def_id_equal(parameter->owner, item->definition)
        && argument != NULL && argument->kind == CM_HIR_TYPE_INTEGER_KIND
        && argument->data.integer_type.kind == CM_HIR_INT_U8);
}

static void test_fresh_consumer(CmHirContext *context,
    const CmHirLibraryArtifact *artifact, CmHirDefId gate)
{
    static const unsigned char source_text[] =
        "use dep::GateReexport;\n"
        "pub fn direct<X: dep::Gate<u8>>(_x: X) {}\n"
        "pub fn via_alias<X: GateReexport<u8>>(_x: X) {}\n";
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    const CmHirLibraryArtifact *libraries[1];
    const CmHirItem *direct;
    const CmHirItem *via_alias;
    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_cfg_set_init(&cfg);
    cm_import_resolver_init(&imports);
    cm_hir_module_map_init(&map);
    assert(cm_source_add_memory(&sources, "consumer.rs", source_text,
        sizeof(source_text) - 1u, &root_source) == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    assert(graph_result.error_count == 0u);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    assert(import_result.revision == graph_result.revision);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "consumer";
    libraries[0] = artifact;
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    lower_result = cm_hir_lower_module_graph(context, &graph,
        graph_result.revision, &imports, &map, &lower_options);
    assert(lower_result.error_count == 0u);
    direct = find_item(context, CM_HIR_ITEM_FUNCTION, "direct");
    via_alias = find_item(context, CM_HIR_ITEM_FUNCTION, "via_alias");
    assert_gate_predicate(context, direct, gate);
    assert_gate_predicate(context, via_alias, gate);
    cm_hir_module_map_destroy(&map);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_const_fresh_consumer(CmHirContext *context,
    const CmHirLibraryArtifact *artifact, CmHirDefId constant)
{
    static const unsigned char source_text[] =
        "use dep::MAX;\n"
        "use dep::MAX_REEXPORT;\n";
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    const CmHirLibraryArtifact *libraries[1];
    const CmHirModule *root;
    uint32_t import_index;
    uint32_t matched;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_cfg_set_init(&cfg);
    cm_import_resolver_init(&imports);
    cm_hir_module_map_init(&map);
    assert(cm_source_add_memory(&sources, "const_consumer.rs", source_text,
        sizeof(source_text) - 1u, &root_source) == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    assert(graph_result.error_count == 0u);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    assert(import_result.revision == graph_result.revision);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "const_consumer";
    libraries[0] = artifact;
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    lower_result = cm_hir_lower_module_graph(context, &graph,
        graph_result.revision, &imports, &map, &lower_options);
    assert(lower_result.error_count == 0u);
    root = cm_hir_get_module(context, lower_result.root_module);
    assert(root != NULL && root->import_count == 2u);
    matched = 0u;
    for (import_index = 0u; import_index < root->import_count; ++import_index) {
        const CmHirImport *import_value = &root->imports[import_index];
        const CmHirImportBinding *binding;
        const CmInternedString *name;
        assert(import_value->binding_count == 1u
            && import_value->bindings != NULL);
        binding = &import_value->bindings[0];
        name = cm_interner_get(&context->strings, binding->name);
        assert(binding->namespace_kind == CM_HIR_NAMESPACE_VALUE
            && cm_hir_def_id_equal(binding->target, constant)
            && name != NULL
            && ((name->len == sizeof("MAX") - 1u
                    && memcmp(name->bytes, "MAX", name->len) == 0)
                || (name->len == sizeof("MAX_REEXPORT") - 1u
                    && memcmp(name->bytes, "MAX_REEXPORT", name->len)
                        == 0)));
        matched += 1u;
    }
    assert(matched == 2u);
    cm_hir_module_map_destroy(&map);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_static_fresh_consumer(CmHirContext *context,
    const CmHirLibraryArtifact *artifact, CmHirDefId static_definition)
{
    static const unsigned char source_text[] =
        "use dep::CACHED_POW10;\n"
        "use dep::CACHED_POW10_ALIAS;\n";
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    const CmHirLibraryArtifact *libraries[1];
    const CmHirModule *root;
    uint32_t import_index;
    uint32_t matched;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_cfg_set_init(&cfg);
    cm_import_resolver_init(&imports);
    cm_hir_module_map_init(&map);
    assert(cm_source_add_memory(&sources, "static_consumer.rs", source_text,
        sizeof(source_text) - 1u, &root_source) == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    assert(graph_result.error_count == 0u);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    assert(import_result.revision == graph_result.revision);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "static_consumer";
    libraries[0] = artifact;
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    lower_result = cm_hir_lower_module_graph(context, &graph,
        graph_result.revision, &imports, &map, &lower_options);
    assert(lower_result.error_count == 0u);
    root = cm_hir_get_module(context, lower_result.root_module);
    assert(root != NULL && root->import_count == 2u);
    matched = 0u;
    for (import_index = 0u; import_index < root->import_count; ++import_index) {
        const CmHirImport *import_value = &root->imports[import_index];
        const CmHirImportBinding *binding;
        const CmInternedString *name;
        assert(import_value->binding_count == 1u
            && import_value->bindings != NULL);
        binding = &import_value->bindings[0];
        name = cm_interner_get(&context->strings, binding->name);
        assert(binding->namespace_kind == CM_HIR_NAMESPACE_VALUE
            && cm_hir_def_id_equal(binding->target, static_definition)
            && name != NULL
            && ((name->len == sizeof("CACHED_POW10") - 1u
                    && memcmp(name->bytes, "CACHED_POW10", name->len) == 0)
                || (name->len == sizeof("CACHED_POW10_ALIAS") - 1u
                    && memcmp(name->bytes, "CACHED_POW10_ALIAS", name->len)
                        == 0)));
        matched += 1u;
    }
    assert(matched == 2u);
    cm_hir_module_map_destroy(&map);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void assert_item_parameter(const CmHirContext *context,
    const CmHirItem *function, CmHirDefId item_definition)
{
    const CmHirType *parameter;
    assert(function != NULL && function->kind == CM_HIR_ITEM_FUNCTION
        && function->data.function_item.signature.parameter_count == 1u);
    parameter = cm_hir_get_type(context,
        function->data.function_item.signature.parameters[0].type);
    assert(parameter != NULL && parameter->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(parameter->data.named_type.definition,
            item_definition)
        && parameter->data.named_type.argument_count == 0u);
}

static void assert_primitive_type(const CmHirContext *context,
    CmHirTypeId type_id, CmHirPrimitiveKind primitive)
{
    const CmHirType *type = cm_hir_get_type(context, type_id);
    assert(type != NULL);
    switch (primitive) {
    case CM_HIR_PRIMITIVE_BOOL:
        assert(type->kind == CM_HIR_TYPE_BOOL_KIND); break;
    case CM_HIR_PRIMITIVE_CHAR:
        assert(type->kind == CM_HIR_TYPE_CHAR_KIND); break;
    case CM_HIR_PRIMITIVE_STR:
        assert(type->kind == CM_HIR_TYPE_STR_KIND); break;
    case CM_HIR_PRIMITIVE_I8:
        assert(type->kind == CM_HIR_TYPE_INTEGER_KIND
            && type->data.integer_type.kind == CM_HIR_INT_I8); break;
    case CM_HIR_PRIMITIVE_I16:
        assert(type->kind == CM_HIR_TYPE_INTEGER_KIND
            && type->data.integer_type.kind == CM_HIR_INT_I16); break;
    case CM_HIR_PRIMITIVE_I32:
        assert(type->kind == CM_HIR_TYPE_INTEGER_KIND
            && type->data.integer_type.kind == CM_HIR_INT_I32); break;
    case CM_HIR_PRIMITIVE_I64:
        assert(type->kind == CM_HIR_TYPE_INTEGER_KIND
            && type->data.integer_type.kind == CM_HIR_INT_I64); break;
    case CM_HIR_PRIMITIVE_I128:
        assert(type->kind == CM_HIR_TYPE_INTEGER_KIND
            && type->data.integer_type.kind == CM_HIR_INT_I128); break;
    case CM_HIR_PRIMITIVE_ISIZE:
        assert(type->kind == CM_HIR_TYPE_INTEGER_KIND
            && type->data.integer_type.kind == CM_HIR_INT_ISIZE); break;
    case CM_HIR_PRIMITIVE_U8:
        assert(type->kind == CM_HIR_TYPE_INTEGER_KIND
            && type->data.integer_type.kind == CM_HIR_INT_U8); break;
    case CM_HIR_PRIMITIVE_U16:
        assert(type->kind == CM_HIR_TYPE_INTEGER_KIND
            && type->data.integer_type.kind == CM_HIR_INT_U16); break;
    case CM_HIR_PRIMITIVE_U32:
        assert(type->kind == CM_HIR_TYPE_INTEGER_KIND
            && type->data.integer_type.kind == CM_HIR_INT_U32); break;
    case CM_HIR_PRIMITIVE_U64:
        assert(type->kind == CM_HIR_TYPE_INTEGER_KIND
            && type->data.integer_type.kind == CM_HIR_INT_U64); break;
    case CM_HIR_PRIMITIVE_U128:
        assert(type->kind == CM_HIR_TYPE_INTEGER_KIND
            && type->data.integer_type.kind == CM_HIR_INT_U128); break;
    case CM_HIR_PRIMITIVE_USIZE:
        assert(type->kind == CM_HIR_TYPE_INTEGER_KIND
            && type->data.integer_type.kind == CM_HIR_INT_USIZE); break;
    case CM_HIR_PRIMITIVE_F32:
        assert(type->kind == CM_HIR_TYPE_FLOAT_KIND
            && type->data.float_type.kind == CM_HIR_FLOAT_F32); break;
    case CM_HIR_PRIMITIVE_F64:
        assert(type->kind == CM_HIR_TYPE_FLOAT_KIND
            && type->data.float_type.kind == CM_HIR_FLOAT_F64); break;
    default:
        assert(0);
    }
}

static void test_primitive_fresh_consumer(CmHirContext *context,
    const CmHirLibraryArtifact *artifact)
{
    static const unsigned char source_text[] =
        "pub fn p00(_: dep::BoolAlias) {}\n"
        "pub fn p01(_: dep::CharAlias) {}\n"
        "pub fn p02(_: dep::F32Alias) {}\n"
        "pub fn p03(_: dep::F64Alias) {}\n"
        "pub fn p04(_: dep::I128Alias) {}\n"
        "pub fn p05(_: dep::I16Alias) {}\n"
        "pub fn p06(_: dep::I32Alias) {}\n"
        "pub fn p07(_: dep::I64Alias) {}\n"
        "pub fn p08(_: dep::I8Alias) {}\n"
        "pub fn p09(_: dep::IsizeAlias) {}\n"
        "pub fn p10(_: dep::StrAlias) {}\n"
        "pub fn p11(_: dep::U128Alias) {}\n"
        "pub fn p12(_: dep::U16Alias) {}\n"
        "pub fn p13(_: dep::U32Alias) {}\n"
        "pub fn p14(_: dep::U64Alias) {}\n"
        "pub fn p15(_: dep::U8Alias) {}\n"
        "pub fn p16(_: dep::UsizeAlias) {}\n"
        "pub fn p17(_: dep::bool) {}\n"
        "pub fn p18(_: dep::char) {}\n"
        "pub fn p19(_: dep::f32) {}\n"
        "pub fn p20(_: dep::f64) {}\n"
        "pub fn p21(_: dep::i128) {}\n"
        "pub fn p22(_: dep::i16) {}\n"
        "pub fn p23(_: dep::i32) {}\n"
        "pub fn p24(_: dep::i64) {}\n"
        "pub fn p25(_: dep::i8) {}\n"
        "pub fn p26(_: dep::isize) {}\n"
        "pub fn p27(_: dep::str) {}\n"
        "pub fn p28(_: dep::u128) {}\n"
        "pub fn p29(_: dep::u16) {}\n"
        "pub fn p30(_: dep::u32) {}\n"
        "pub fn p31(_: dep::u64) {}\n"
        "pub fn p32(_: dep::u8) {}\n"
        "pub fn p33(_: dep::usize) {}\n";
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    const CmHirLibraryArtifact *libraries[1];
    size_t index;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_cfg_set_init(&cfg);
    cm_import_resolver_init(&imports);
    cm_hir_module_map_init(&map);
    assert(cm_source_add_memory(&sources, "primitive_consumer.rs",
        source_text, sizeof(source_text) - 1u, &root_source)
        == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    assert(graph_result.error_count == 0u);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    assert(import_result.revision == graph_result.revision);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "primitive_consumer";
    libraries[0] = artifact;
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    lower_result = cm_hir_lower_module_graph(context, &graph,
        graph_result.revision, &imports, &map, &lower_options);
    assert(lower_result.error_count == 0u);
    for (index = 0u; index < PRIMITIVE_BINDING_COUNT; ++index) {
        char name[4];
        const CmHirItem *function;
        name[0] = 'p';
        name[1] = (char)('0' + (index / 10u));
        name[2] = (char)('0' + (index % 10u));
        name[3] = '\0';
        function = find_item(context, CM_HIR_ITEM_FUNCTION, name);
        assert(function != NULL
            && function->data.function_item.signature.parameter_count == 1u);
        assert_primitive_type(context,
            function->data.function_item.signature.parameters[0].type,
            primitive_binding_specs[index].hir_kind);
    }
    cm_hir_module_map_destroy(&map);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_item_fresh_consumer(CmHirContext *context,
    const CmHirLibraryArtifact *artifact, CmHirDefId item_definition)
{
    static const unsigned char source_text[] =
        "pub fn direct_item(_x: dep::Packet) {}\n"
        "pub fn reexported_item(_x: dep::PacketReexport) {}\n";
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    const CmHirLibraryArtifact *libraries[1];
    const CmHirItem *direct;
    const CmHirItem *reexported;
    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_cfg_set_init(&cfg);
    cm_import_resolver_init(&imports);
    cm_hir_module_map_init(&map);
    assert(cm_source_add_memory(&sources, "item_consumer.rs", source_text,
        sizeof(source_text) - 1u, &root_source) == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    assert(graph_result.error_count == 0u);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    assert(import_result.revision == graph_result.revision);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "item_consumer";
    libraries[0] = artifact;
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    lower_result = cm_hir_lower_module_graph(context, &graph,
        graph_result.revision, &imports, &map, &lower_options);
    assert(lower_result.error_count == 0u);
    direct = find_item(context, CM_HIR_ITEM_FUNCTION, "direct_item");
    reexported = find_item(context, CM_HIR_ITEM_FUNCTION,
        "reexported_item");
    assert_item_parameter(context, direct, item_definition);
    assert_item_parameter(context, reexported, item_definition);
    cm_hir_module_map_destroy(&map);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_alias_fresh_consumer(CmHirContext *context,
    const CmHirLibraryArtifact *artifact, CmHirDefId layout_error)
{
    static const unsigned char source_text[] =
        "pub fn direct(_x: dep::LayoutError) {}\n"
        "pub fn direct_reexport(_x: dep::LayoutErrorReexport) {}\n"
        "pub fn alias(_x: dep::LayoutErr) {}\n"
        "pub fn alias_reexport(_x: dep::LayoutErrReexport) {}\n";
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    const CmHirLibraryArtifact *libraries[1];
    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_cfg_set_init(&cfg);
    cm_import_resolver_init(&imports);
    cm_hir_module_map_init(&map);
    assert(cm_source_add_memory(&sources, "alias_consumer.rs", source_text,
        sizeof(source_text) - 1u, &root_source) == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    assert(graph_result.error_count == 0u);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    assert(import_result.revision == graph_result.revision);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "alias_consumer";
    libraries[0] = artifact;
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    lower_result = cm_hir_lower_module_graph(context, &graph,
        graph_result.revision, &imports, &map, &lower_options);
    assert(lower_result.error_count == 0u);
    assert_item_parameter(context,
        find_item(context, CM_HIR_ITEM_FUNCTION, "direct"), layout_error);
    assert_item_parameter(context,
        find_item(context, CM_HIR_ITEM_FUNCTION, "direct_reexport"),
        layout_error);
    assert_item_parameter(context,
        find_item(context, CM_HIR_ITEM_FUNCTION, "alias"), layout_error);
    assert_item_parameter(context,
        find_item(context, CM_HIR_ITEM_FUNCTION, "alias_reexport"),
        layout_error);
    cm_hir_module_map_destroy(&map);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void assert_u8_type(const CmHirContext *context, CmHirTypeId id)
{
    const CmHirType *type = cm_hir_get_type(context, id);
    assert(type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_U8);
}

static void assert_composite_signature(const CmHirContext *context,
    const CmHirItem *function, CmHirDefId wrap)
{
    const CmHirFunctionSignature *signature;
    const CmHirType *type;
    assert(function != NULL && function->kind == CM_HIR_ITEM_FUNCTION);
    signature = &function->data.function_item.signature;
    assert(signature->parameter_count == 4u);
    type = cm_hir_get_type(context, signature->parameters[0].type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_SLICE_KIND);
    assert_u8_type(context, type->data.slice_type.element);
    type = cm_hir_get_type(context, signature->parameters[1].type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_RAW_POINTER_KIND
        && type->data.raw_pointer_type.mutability == CM_HIR_MUTABLE);
    assert_u8_type(context, type->data.raw_pointer_type.pointee);
    type = cm_hir_get_type(context, signature->parameters[2].type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_REFERENCE_KIND
        && type->data.reference_type.mutability == CM_HIR_IMMUTABLE
        && type->data.reference_type.region.kind == CM_HIR_REGION_STATIC);
    assert_u8_type(context, type->data.reference_type.pointee);
    type = cm_hir_get_type(context, signature->parameters[3].type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(type->data.named_type.definition, wrap)
        && type->data.named_type.argument_count == 1u
        && type->data.named_type.arguments != NULL
        && type->data.named_type.arguments[0].kind
            == CM_HIR_GENERIC_ARG_TYPE);
    assert_u8_type(context, type->data.named_type.arguments[0].data.type);
}

static void assert_imported_composite_signature(const CmHirContext *context,
    const CmHirItem *function, CmHirDefId wrap)
{
    const CmHirType *reference;
    const CmHirType *slice;
    assert(function != NULL && function->kind == CM_HIR_ITEM_FUNCTION
        && function->data.function_item.signature.parameter_count == 4u);
    reference = cm_hir_get_type(context,
        function->data.function_item.signature.parameters[0].type);
    assert(reference != NULL
        && reference->kind == CM_HIR_TYPE_REFERENCE_KIND
        && reference->data.reference_type.mutability == CM_HIR_IMMUTABLE
        && reference->data.reference_type.region.kind == CM_HIR_REGION_STATIC);
    slice = cm_hir_get_type(context, reference->data.reference_type.pointee);
    assert(slice != NULL && slice->kind == CM_HIR_TYPE_SLICE_KIND);
    assert_u8_type(context, slice->data.slice_type.element);
    {
        const CmHirFunctionSignature *signature =
            &function->data.function_item.signature;
        const CmHirType *type = cm_hir_get_type(context,
            signature->parameters[1].type);
        assert(type != NULL && type->kind == CM_HIR_TYPE_RAW_POINTER_KIND
            && type->data.raw_pointer_type.mutability == CM_HIR_MUTABLE);
        assert_u8_type(context, type->data.raw_pointer_type.pointee);
        type = cm_hir_get_type(context, signature->parameters[2].type);
        assert(type != NULL && type->kind == CM_HIR_TYPE_REFERENCE_KIND
            && type->data.reference_type.mutability == CM_HIR_IMMUTABLE
            && type->data.reference_type.region.kind == CM_HIR_REGION_STATIC);
        assert_u8_type(context, type->data.reference_type.pointee);
        type = cm_hir_get_type(context, signature->parameters[3].type);
        assert(type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
            && cm_hir_def_id_equal(type->data.named_type.definition, wrap)
            && type->data.named_type.argument_count == 1u
            && type->data.named_type.arguments[0].kind
                == CM_HIR_GENERIC_ARG_TYPE);
        assert_u8_type(context,
            type->data.named_type.arguments[0].data.type);
    }
}

static void test_composite_fresh_consumer(CmHirContext *context,
    const CmHirLibraryArtifact *artifact, CmHirDefId wrap)
{
    static const unsigned char source_text[] =
        "pub fn imported(_a: &'static [u8], _b: *mut u8, "
        "_c: &'static u8, _d: dep::Wrap<u8>) {}\n";
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    const CmHirLibraryArtifact *libraries[1];
    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_cfg_set_init(&cfg);
    cm_import_resolver_init(&imports);
    cm_hir_module_map_init(&map);
    assert(cm_source_add_memory(&sources, "composite_consumer.rs",
        source_text, sizeof(source_text) - 1u, &root_source)
        == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    assert(graph_result.error_count == 0u);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    assert(import_result.revision == graph_result.revision);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "composite_consumer";
    libraries[0] = artifact;
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    lower_result = cm_hir_lower_module_graph(context, &graph,
        graph_result.revision, &imports, &map, &lower_options);
    assert(lower_result.error_count == 0u);
    assert_imported_composite_signature(context,
        find_item(context, CM_HIR_ITEM_FUNCTION, "imported"), wrap);
    cm_hir_module_map_destroy(&map);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_aggregate_fresh_consumer(CmHirContext *context,
    const CmHirLibraryArtifact *artifact, CmHirDefId assume,
    CmHirDefId manually_drop, CmHirDefId maybe_uninit)
{
    static const unsigned char source_text[] =
        "pub fn imported(_a: dep::Assume, _aa: dep::AssumeAlias, "
        "_m: dep::ManuallyDrop<u8>, _ma: dep::ManuallyDropAlias<u8>, "
        "_u: dep::MaybeUninit<u8>, _ua: dep::MaybeUninitAlias<u8>) {}\n";
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    const CmHirLibraryArtifact *libraries[1];
    const CmHirItem *function;
    const CmHirFunctionSignature *signature;
    CmHirDefId expected[6];
    uint32_t index;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_cfg_set_init(&cfg);
    cm_import_resolver_init(&imports);
    cm_hir_module_map_init(&map);
    assert(cm_source_add_memory(&sources, "aggregate_consumer.rs",
        source_text, sizeof(source_text) - 1u, &root_source)
        == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    assert(graph_result.error_count == 0u);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    assert(import_result.revision == graph_result.revision);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "aggregate_consumer";
    libraries[0] = artifact;
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    lower_result = cm_hir_lower_module_graph(context, &graph,
        graph_result.revision, &imports, &map, &lower_options);
    assert(lower_result.error_count == 0u);
    function = find_item(context, CM_HIR_ITEM_FUNCTION, "imported");
    assert(function != NULL && function->data.function_item.signature
        .parameter_count == 6u);
    signature = &function->data.function_item.signature;
    expected[0] = assume;
    expected[1] = assume;
    expected[2] = manually_drop;
    expected[3] = manually_drop;
    expected[4] = maybe_uninit;
    expected[5] = maybe_uninit;
    for (index = 0u; index < 6u; ++index) {
        const CmHirType *type = cm_hir_get_type(context,
            signature->parameters[index].type);
        assert(type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
            && cm_hir_def_id_equal(type->data.named_type.definition,
                expected[index])
            && type->data.named_type.argument_count
                == (index < 2u ? 0u : 1u));
        if (index >= 2u) {
            assert(type->data.named_type.arguments != NULL
                && type->data.named_type.arguments[0].kind
                    == CM_HIR_GENERIC_ARG_TYPE);
            assert_u8_type(context,
                type->data.named_type.arguments[0].data.type);
        }
    }
    cm_hir_module_map_destroy(&map);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_layout_fresh_consumer(CmHirContext *context,
    const CmHirLibraryArtifact *artifact, CmHirDefId alignment,
    CmHirDefId layout)
{
    static const unsigned char source_text[] =
        "pub fn imported(_l: dep::Layout, _lr: dep::LayoutReexport, "
        "_a: dep::Alignment, _ar: dep::AlignmentReexport) {}\n";
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    const CmHirLibraryArtifact *libraries[1];
    const CmHirItem *function;
    CmHirDefId expected[4];
    uint32_t index;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_cfg_set_init(&cfg);
    cm_import_resolver_init(&imports);
    cm_hir_module_map_init(&map);
    assert(cm_source_add_memory(&sources, "layout_consumer.rs",
        source_text, sizeof(source_text) - 1u, &root_source)
        == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    assert(graph_result.error_count == 0u);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    assert(import_result.revision == graph_result.revision);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "layout_consumer";
    libraries[0] = artifact;
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    lower_result = cm_hir_lower_module_graph(context, &graph,
        graph_result.revision, &imports, &map, &lower_options);
    assert(lower_result.error_count == 0u);
    function = find_item(context, CM_HIR_ITEM_FUNCTION, "imported");
    assert(function != NULL
        && function->data.function_item.signature.parameter_count == 4u);
    expected[0] = layout;
    expected[1] = layout;
    expected[2] = alignment;
    expected[3] = alignment;
    for (index = 0u; index < 4u; ++index) {
        const CmHirType *type = cm_hir_get_type(context,
            function->data.function_item.signature.parameters[index].type);
        assert(type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
            && cm_hir_def_id_equal(type->data.named_type.definition,
                expected[index])
            && type->data.named_type.argument_count == 0u
            && type->data.named_type.arguments == NULL);
    }
    cm_hir_module_map_destroy(&map);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_type_id_fresh_consumer(CmHirContext *context,
    const CmHirLibraryArtifact *artifact, CmHirDefId type_id)
{
    static const unsigned char source_text[] =
        "pub fn imported(_direct: dep::TypeIdLike, "
        "_alias: dep::TypeIdAlias) {}\n";
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    const CmHirLibraryArtifact *libraries[1];
    const CmHirItem *function;
    uint32_t index;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_cfg_set_init(&cfg);
    cm_import_resolver_init(&imports);
    cm_hir_module_map_init(&map);
    assert(cm_source_add_memory(&sources, "type-id-consumer.rs",
        source_text, sizeof(source_text) - 1u, &root_source)
        == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    assert(graph_result.error_count == 0u);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    assert(import_result.revision == graph_result.revision);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "type_id_consumer";
    libraries[0] = artifact;
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    lower_result = cm_hir_lower_module_graph(context, &graph,
        graph_result.revision, &imports, &map, &lower_options);
    assert(lower_result.error_count == 0u);
    function = find_item(context, CM_HIR_ITEM_FUNCTION, "imported");
    assert(function != NULL
        && function->data.function_item.signature.parameter_count == 2u);
    for (index = 0u; index < 2u; ++index) {
        const CmHirType *type = cm_hir_get_type(context,
            function->data.function_item.signature.parameters[index].type);
        assert(type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
            && cm_hir_def_id_equal(type->data.named_type.definition,
                type_id)
            && type->data.named_type.argument_count == 0u);
    }
    cm_hir_module_map_destroy(&map);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_type_name_fresh_consumer(CmHirContext *context,
    const CmHirLibraryArtifact *artifact)
{
    static const unsigned char source_text[] =
        "use dep::name_of as direct_name;\n"
        "use dep::name_alias as alias_name;\n"
        "pub const fn direct<T: ?Sized>(value: &T) -> &'static str { "
            "direct_name::<T>(value) }\n"
        "pub const fn via_alias<T: ?Sized>(value: &T) -> &'static str { "
            "alias_name::<T>(value) }\n";
    static const char *const names[] = { "direct", "via_alias" };
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    const CmHirLibraryArtifact *libraries[1];
    size_t index;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_cfg_set_init(&cfg);
    cm_import_resolver_init(&imports);
    cm_hir_module_map_init(&map);
    assert(cm_source_add_memory(&sources, "type-name-consumer.rs",
        source_text, sizeof(source_text) - 1u, &root_source)
        == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    assert(graph_result.error_count == 0u);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    assert(import_result.revision == graph_result.revision);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "type_name_consumer";
    libraries[0] = artifact;
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    lower_result = cm_hir_lower_module_graph(context, &graph,
        graph_result.revision, &imports, &map, &lower_options);
    assert(lower_result.error_count == 0u);
    for (index = 0u; index < sizeof(names) / sizeof(names[0]); ++index) {
        const CmHirItem *function = find_item(context,
            CM_HIR_ITEM_FUNCTION, names[index]);
        CmHirBodyLowerResult body_result;
        const CmHirBody *body;
        const CmHirType *parameter;
        size_t expression_count = context->expressions.len;
        assert(function != NULL
            && function->generic_parameter_count == 1u
            && function->data.function_item.signature.is_const
            && function->data.function_item.signature.parameter_count == 1u
            && function->data.function_item.body != CM_HIR_BODY_NONE);
        parameter = cm_hir_get_type(context,
            function->data.function_item.signature.parameters[0].type);
        assert(parameter != NULL
            && parameter->kind == CM_HIR_TYPE_REFERENCE_KIND
            && parameter->data.reference_type.region.kind
                == CM_HIR_REGION_ERASED);
        body_result = cm_hir_lower_body(context,
            function->data.function_item.body, &graph,
            graph_result.revision, &imports, &map);
        assert(body_result.status == CM_HIR_BODY_LOWER_UNSUPPORTED_BODY
            && context->expressions.len == expression_count);
        body = cm_hir_get_body(context, function->data.function_item.body);
        assert(body != NULL && body->state == CM_HIR_BODY_UNLOWERED
            && body->root_expression == CM_HIR_EXPR_NONE);
    }
    cm_hir_module_map_destroy(&map);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_unit_function_fresh_consumer(CmHirContext *context,
    const CmHirLibraryArtifact *artifact)
{
    static const unsigned char source_text[] =
        "use dep::breakpoint_like as direct_breakpoint;\n"
        "use dep::breakpoint_alias as alias_breakpoint;\n"
        "pub fn direct() { direct_breakpoint() }\n"
        "pub fn via_alias() { alias_breakpoint() }\n";
    static const char *const names[] = { "direct", "via_alias" };
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    const CmHirLibraryArtifact *libraries[1];
    size_t index;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_cfg_set_init(&cfg);
    cm_import_resolver_init(&imports);
    cm_hir_module_map_init(&map);
    assert(cm_source_add_memory(&sources, "unit-function-consumer.rs",
        source_text, sizeof(source_text) - 1u, &root_source)
        == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    assert(graph_result.error_count == 0u);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    assert(import_result.revision == graph_result.revision);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "unit_function_consumer";
    libraries[0] = artifact;
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    lower_result = cm_hir_lower_module_graph(context, &graph,
        graph_result.revision, &imports, &map, &lower_options);
    assert(lower_result.error_count == 0u);
    for (index = 0u; index < sizeof(names) / sizeof(names[0]); ++index) {
        const CmHirItem *function = find_item(context,
            CM_HIR_ITEM_FUNCTION, names[index]);
        CmHirBodyLowerResult body_result;
        const CmHirBody *body;
        const CmHirType *return_type;
        size_t expression_count = context->expressions.len;

        assert(function != NULL
            && function->generic_parameter_start
                == CM_HIR_GENERIC_PARAM_NONE
            && function->generic_parameter_count == 0u
            && function->predicate_count == 0u
            && function->data.function_item.signature.parameter_count == 0u
            && function->data.function_item.signature.parameters == NULL
            && function->data.function_item.body != CM_HIR_BODY_NONE);
        return_type = cm_hir_get_type(context,
            function->data.function_item.signature.return_type);
        assert(return_type != NULL && return_type->kind
            == CM_HIR_TYPE_UNIT_KIND);
        body_result = cm_hir_lower_body(context,
            function->data.function_item.body, &graph,
            graph_result.revision, &imports, &map);
        assert(body_result.status == CM_HIR_BODY_LOWER_UNSUPPORTED_BODY
            && context->expressions.len == expression_count);
        body = cm_hir_get_body(context,
            function->data.function_item.body);
        assert(body != NULL && body->state == CM_HIR_BODY_UNLOWERED
            && body->root_expression == CM_HIR_EXPR_NONE);
    }
    assert(cm_hir_lower_body(context, CM_HIR_BODY_NONE, &graph,
        graph_result.revision, &imports, &map).status
        == CM_HIR_BODY_LOWER_INVALID_ARGUMENT);
    cm_hir_module_map_destroy(&map);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_from_fn_fresh_consumer(CmHirContext *context,
    const CmHirLibraryArtifact *artifact, CmHirDefId imported_definition)
{
    static const unsigned char source_text[] =
        "use dep::from_fn as direct_from_fn;\n"
        "use dep::from_fn_alias as alias_from_fn;\n"
        "pub fn direct<T, const N: usize, F>(f: F) -> [T; N] "
            "{ direct_from_fn(f) }\n"
        "pub fn via_alias<T, const N: usize, F>(f: F) -> [T; N] "
            "{ alias_from_fn(f) }\n";
    static const char *const names[] = { "direct", "via_alias" };
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    const CmHirLibraryArtifact *libraries[1];
    size_t index;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_cfg_set_init(&cfg);
    cm_import_resolver_init(&imports);
    cm_hir_module_map_init(&map);
    assert(cm_source_add_memory(&sources, "from-fn-consumer.rs",
        source_text, sizeof(source_text) - 1u, &root_source)
        == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    assert(graph_result.error_count == 0u);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    assert(import_result.revision == graph_result.revision);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "from_fn_consumer";
    libraries[0] = artifact;
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    lower_result = cm_hir_lower_module_graph(context, &graph,
        graph_result.revision, &imports, &map, &lower_options);
    if (lower_result.error_count != 0u) {
        fprintf(stderr, "from-fn consumer: %s: %s\n",
            cm_hir_lower_error_kind_name(lower_result.first_error.kind),
            lower_result.first_error.message);
    }
    assert(lower_result.error_count == 0u);
    for (index = 0u; index < sizeof(names) / sizeof(names[0]); ++index) {
        const CmHirItem *function = find_item(context,
            CM_HIR_ITEM_FUNCTION, names[index]);
        const CmHirGenericParam *constant;
        const CmHirType *return_type;
        assert(function != NULL && function->generic_parameter_count == 3u
            && function->predicate_count == 0u
            && function->data.function_item.signature.parameter_count == 1u
            && function->data.function_item.body != CM_HIR_BODY_NONE);
        constant = cm_hir_get_generic_param(context,
            function->generic_parameter_start + 1u);
        return_type = cm_hir_get_type(context,
            function->data.function_item.signature.return_type);
        assert(constant != NULL && constant->kind == CM_HIR_GENERIC_CONST
            && return_type != NULL
            && return_type->kind == CM_HIR_TYPE_ARRAY_KIND
            && return_type->data.array_type.length.kind
                == CM_HIR_CONST_PARAMETER
            && return_type->data.array_type.length.data.parameter
                == function->generic_parameter_start + 1u);
    }
    {
        const CmHirItem *imported = find_item(context,
            CM_HIR_ITEM_FUNCTION, "from_fn");
        assert(imported != NULL && cm_hir_def_id_equal(imported->definition,
            imported_definition)
            && imported->data.function_item.body == CM_HIR_BODY_NONE
            && cm_hir_get_body(context,
                imported->data.function_item.body) == NULL);
    }
    assert(cm_hir_lower_body(context, CM_HIR_BODY_NONE, &graph,
        graph_result.revision, &imports, &map).status
        == CM_HIR_BODY_LOWER_INVALID_ARGUMENT);
    cm_hir_module_map_destroy(&map);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_try_from_fn_fresh_consumer(CmHirContext *context,
    const CmHirLibraryArtifact *artifact, CmHirDefId imported_definition)
{
    static const unsigned char source_text[] =
        "use dep::FnMut;\n"
        "use dep::Residual;\n"
        "use dep::Try;\n"
        "use dep::try_from_fn as direct_try;\n"
        "use dep::try_from_fn_alias as alias_try;\n"
        "pub fn direct<R, const N: usize, F>(f: F) "
            "-> <<R as Try>::Residual as "
                "Residual<[<R as Try>::Output; N]>>::TryType "
            "where F: FnMut(usize) -> R, R: Try, "
                "R::Residual: Residual<[R::Output; N]> "
            "{ direct_try(f) }\n"
        "pub fn via_alias<R, const N: usize, F>(f: F) "
            "-> <<R as Try>::Residual as "
                "Residual<[<R as Try>::Output; N]>>::TryType "
            "where F: FnMut(usize) -> R, R: Try, "
                "R::Residual: Residual<[R::Output; N]> "
            "{ alias_try(f) }\n";
    static const char *const names[] = { "direct", "via_alias" };
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    const CmHirLibraryArtifact *libraries[1];
    size_t index;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_cfg_set_init(&cfg);
    cm_import_resolver_init(&imports);
    cm_hir_module_map_init(&map);
    assert(cm_source_add_memory(&sources, "try-from-fn-consumer.rs",
        source_text, sizeof(source_text) - 1u, &root_source)
        == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    assert(graph_result.error_count == 0u);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    assert(import_result.revision == graph_result.revision);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "try_from_fn_consumer";
    libraries[0] = artifact;
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    lower_result = cm_hir_lower_module_graph(context, &graph,
        graph_result.revision, &imports, &map, &lower_options);
    if (lower_result.error_count != 0u) {
        fprintf(stderr, "try-from-fn consumer: %s: %s\n",
            cm_hir_lower_error_kind_name(lower_result.first_error.kind),
            lower_result.first_error.message);
    }
    assert(lower_result.error_count == 0u);
    for (index = 0u; index < sizeof(names) / sizeof(names[0]); ++index) {
        const CmHirItem *function = find_item(context,
            CM_HIR_ITEM_FUNCTION, names[index]);
        const CmHirType *result = function == NULL ? NULL
            : cm_hir_get_type(context,
                function->data.function_item.signature.return_type);
        assert(function != NULL && function->generic_parameter_count == 3u
            && function->predicate_count == 3u
            && function->data.function_item.signature.parameter_count == 1u
            && function->data.function_item.body != CM_HIR_BODY_NONE
            && result != NULL
            && result->kind == CM_HIR_TYPE_PROJECTION_KIND);
    }
    {
        const CmHirItem *imported = find_item(context,
            CM_HIR_ITEM_FUNCTION, "try_from_fn");
        assert(imported != NULL && cm_hir_def_id_equal(imported->definition,
            imported_definition)
            && imported->data.function_item.body == CM_HIR_BODY_NONE
            && cm_hir_get_body(context,
                imported->data.function_item.body) == NULL);
    }
    assert(cm_hir_lower_body(context, CM_HIR_BODY_NONE, &graph,
        graph_result.revision, &imports, &map).status
        == CM_HIR_BODY_LOWER_INVALID_ARGUMENT);
    cm_hir_module_map_destroy(&map);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_repeat_fresh_consumer(CmHirContext *context,
    const CmHirLibraryArtifact *artifact, CmHirDefId repeat_definition,
    CmHirDefId clone_definition)
{
    static const unsigned char source_text[] =
        "use dep::Clone;\n"
        "use dep::repeat as imported_repeat;\n"
        "use dep::repeat_alias as imported_repeat_alias;\n"
        "pub fn direct<T: Clone, const N: usize>(val: T) -> [T; N] "
            "{ imported_repeat(val) }\n"
        "pub fn via_alias<T: Clone, const N: usize>(val: T) -> [T; N] "
            "{ imported_repeat_alias(val) }\n";
    static const char *const names[] = { "direct", "via_alias" };
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    const CmHirLibraryArtifact *libraries[1];
    size_t index;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_cfg_set_init(&cfg);
    cm_import_resolver_init(&imports);
    cm_hir_module_map_init(&map);
    assert(cm_source_add_memory(&sources, "repeat-consumer.rs",
        source_text, sizeof(source_text) - 1u, &root_source)
        == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    assert(graph_result.error_count == 0u);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    assert(import_result.revision == graph_result.revision);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "repeat_consumer";
    libraries[0] = artifact;
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    lower_result = cm_hir_lower_module_graph(context, &graph,
        graph_result.revision, &imports, &map, &lower_options);
    if (lower_result.error_count != 0u) {
        fprintf(stderr, "repeat consumer: %s: %s\n",
            cm_hir_lower_error_kind_name(lower_result.first_error.kind),
            lower_result.first_error.message);
    }
    assert(lower_result.error_count == 0u);
    for (index = 0u; index < sizeof(names) / sizeof(names[0]); ++index) {
        const CmHirItem *function = find_item(context,
            CM_HIR_ITEM_FUNCTION, names[index]);
        const CmHirGenericParam *constant = function == NULL
                || function->generic_parameter_count != 2u
            ? NULL : cm_hir_get_generic_param(context,
                function->generic_parameter_start + 1u);
        const CmHirType *result = function == NULL ? NULL
            : cm_hir_get_type(context,
                function->data.function_item.signature.return_type);
        CmHirBodyLowerResult body_result;
        size_t expression_count = context->expressions.len;

        assert(function != NULL && function->generic_parameter_count == 2u
            && function->predicate_count == 1u
            && function->predicates[0].modifier == CM_HIR_PREDICATE_REQUIRED
            && cm_hir_def_id_equal(function->predicates[0]
                .trait_type.definition, clone_definition)
            && function->data.function_item.signature.parameter_count == 1u
            && function->data.function_item.body != CM_HIR_BODY_NONE
            && constant != NULL && constant->kind == CM_HIR_GENERIC_CONST
            && result != NULL && result->kind == CM_HIR_TYPE_ARRAY_KIND
            && result->data.array_type.length.kind == CM_HIR_CONST_PARAMETER
            && result->data.array_type.length.data.parameter
                == function->generic_parameter_start + 1u);
        body_result = cm_hir_lower_body(context,
            function->data.function_item.body, &graph,
            graph_result.revision, &imports, &map);
        assert(body_result.status == CM_HIR_BODY_LOWER_UNSUPPORTED_BODY
            && context->expressions.len == expression_count);
    }
    {
        const CmHirItem *imported = find_item(context,
            CM_HIR_ITEM_FUNCTION, "repeat");
        assert(imported != NULL && cm_hir_def_id_equal(imported->definition,
            repeat_definition)
            && imported->data.function_item.body == CM_HIR_BODY_NONE
            && cm_hir_get_body(context,
                imported->data.function_item.body) == NULL);
    }
    cm_hir_module_map_destroy(&map);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_from_mut_fresh_consumer(CmHirContext *context,
    const CmHirLibraryArtifact *artifact, CmHirDefId imported_definition)
{
    static const unsigned char source_text[] =
        "use dep::from_mut as direct_from_mut;\n"
        "use dep::from_mut_alias as alias_from_mut;\n"
        "pub const fn direct<T>(s: &mut T) -> &mut [T; 1] "
            "{ direct_from_mut(s) }\n"
        "pub const fn via_alias<T>(s: &mut T) -> &mut [T; 1] "
            "{ alias_from_mut(s) }\n";
    static const char *const names[] = { "direct", "via_alias" };
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    const CmHirLibraryArtifact *libraries[1];
    size_t index;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_cfg_set_init(&cfg);
    cm_import_resolver_init(&imports);
    cm_hir_module_map_init(&map);
    assert(cm_source_add_memory(&sources, "from-mut-consumer.rs",
        source_text, sizeof(source_text) - 1u, &root_source)
        == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    assert(graph_result.error_count == 0u);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    assert(import_result.revision == graph_result.revision);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "from_mut_consumer";
    libraries[0] = artifact;
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    lower_result = cm_hir_lower_module_graph(context, &graph,
        graph_result.revision, &imports, &map, &lower_options);
    if (lower_result.error_count != 0u) {
        fprintf(stderr, "from-mut consumer: %s: %s\n",
            cm_hir_lower_error_kind_name(lower_result.first_error.kind),
            lower_result.first_error.message);
    }
    assert(lower_result.error_count == 0u);
    for (index = 0u; index < sizeof(names) / sizeof(names[0]); ++index) {
        const CmHirItem *function = find_item(context,
            CM_HIR_ITEM_FUNCTION, names[index]);
        const CmHirType *input = function == NULL
            ? NULL : cm_hir_get_type(context,
                function->data.function_item.signature.parameters[0].type);
        const CmHirType *output = function == NULL
            ? NULL : cm_hir_get_type(context,
                function->data.function_item.signature.return_type);
        const CmHirType *array = output == NULL
                || output->kind != CM_HIR_TYPE_REFERENCE_KIND
            ? NULL : cm_hir_get_type(context,
                output->data.reference_type.pointee);
        CmHirBodyLowerResult body_result;
        size_t expression_count = context->expressions.len;

        assert(function != NULL && function->generic_parameter_count == 1u
            && function->predicate_count == 0u
            && function->data.function_item.signature.is_const == 1
            && function->data.function_item.signature.parameter_count == 1u
            && function->data.function_item.body != CM_HIR_BODY_NONE
            && input != NULL && input->kind == CM_HIR_TYPE_REFERENCE_KIND
            && input->data.reference_type.mutability == CM_HIR_MUTABLE
            && input->data.reference_type.region.kind == CM_HIR_REGION_ERASED
            && output != NULL && output->kind == CM_HIR_TYPE_REFERENCE_KIND
            && output->data.reference_type.mutability == CM_HIR_MUTABLE
            && output->data.reference_type.region.kind
                == CM_HIR_REGION_ERASED
            && array != NULL && array->kind == CM_HIR_TYPE_ARRAY_KIND
            && array->data.array_type.length.kind == CM_HIR_CONST_VALUE
            && array->data.array_type.length.data.value.low_bits
                == UINT64_C(1));
        body_result = cm_hir_lower_body(context,
            function->data.function_item.body, &graph,
            graph_result.revision, &imports, &map);
        assert(body_result.status == CM_HIR_BODY_LOWER_UNSUPPORTED_BODY
            && context->expressions.len == expression_count);
    }
    {
        const CmHirItem *imported = find_item(context,
            CM_HIR_ITEM_FUNCTION, "from_mut");
        assert(imported != NULL && cm_hir_def_id_equal(imported->definition,
            imported_definition)
            && imported->data.function_item.body == CM_HIR_BODY_NONE
            && cm_hir_get_body(context,
                imported->data.function_item.body) == NULL);
    }
    assert(cm_hir_lower_body(context, CM_HIR_BODY_NONE, &graph,
        graph_result.revision, &imports, &map).status
        == CM_HIR_BODY_LOWER_INVALID_ARGUMENT);
    cm_hir_module_map_destroy(&map);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_from_ref_fresh_consumer(CmHirContext *context,
    const CmHirLibraryArtifact *artifact, CmHirDefId imported_definition)
{
    static const unsigned char source_text[] =
        "use dep::from_ref as direct_from_ref;\n"
        "use dep::from_ref_alias as alias_from_ref;\n"
        "pub const fn direct<T>(s: &T) -> &[T; 1] "
            "{ direct_from_ref(s) }\n"
        "pub const fn via_alias<T>(s: &T) -> &[T; 1] "
            "{ alias_from_ref(s) }\n";
    static const char *const names[] = { "direct", "via_alias" };
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    const CmHirLibraryArtifact *libraries[1];
    size_t index;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_cfg_set_init(&cfg);
    cm_import_resolver_init(&imports);
    cm_hir_module_map_init(&map);
    assert(cm_source_add_memory(&sources, "from-ref-consumer.rs",
        source_text, sizeof(source_text) - 1u, &root_source)
        == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    assert(graph_result.error_count == 0u);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    assert(import_result.revision == graph_result.revision);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "from_ref_consumer";
    libraries[0] = artifact;
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    lower_result = cm_hir_lower_module_graph(context, &graph,
        graph_result.revision, &imports, &map, &lower_options);
    if (lower_result.error_count != 0u) {
        fprintf(stderr, "from-ref consumer: %s: %s\n",
            cm_hir_lower_error_kind_name(lower_result.first_error.kind),
            lower_result.first_error.message);
    }
    assert(lower_result.error_count == 0u);
    for (index = 0u; index < sizeof(names) / sizeof(names[0]); ++index) {
        const CmHirItem *function = find_item(context,
            CM_HIR_ITEM_FUNCTION, names[index]);
        const CmHirType *input = function == NULL
            ? NULL : cm_hir_get_type(context,
                function->data.function_item.signature.parameters[0].type);
        const CmHirType *output = function == NULL
            ? NULL : cm_hir_get_type(context,
                function->data.function_item.signature.return_type);
        const CmHirType *array = output == NULL
                || output->kind != CM_HIR_TYPE_REFERENCE_KIND
            ? NULL : cm_hir_get_type(context,
                output->data.reference_type.pointee);
        CmHirBodyLowerResult body_result;
        size_t expression_count = context->expressions.len;

        assert(function != NULL && function->generic_parameter_count == 1u
            && function->predicate_count == 0u
            && function->data.function_item.signature.is_const == 1
            && function->data.function_item.signature.parameter_count == 1u
            && function->data.function_item.body != CM_HIR_BODY_NONE
            && input != NULL && input->kind == CM_HIR_TYPE_REFERENCE_KIND
            && input->data.reference_type.mutability == CM_HIR_IMMUTABLE
            && input->data.reference_type.region.kind == CM_HIR_REGION_ERASED
            && output != NULL && output->kind == CM_HIR_TYPE_REFERENCE_KIND
            && output->data.reference_type.mutability == CM_HIR_IMMUTABLE
            && output->data.reference_type.region.kind
                == CM_HIR_REGION_ERASED
            && array != NULL && array->kind == CM_HIR_TYPE_ARRAY_KIND
            && array->data.array_type.length.kind == CM_HIR_CONST_VALUE
            && array->data.array_type.length.data.value.low_bits
                == UINT64_C(1));
        body_result = cm_hir_lower_body(context,
            function->data.function_item.body, &graph,
            graph_result.revision, &imports, &map);
        assert(body_result.status == CM_HIR_BODY_LOWER_UNSUPPORTED_BODY
            && context->expressions.len == expression_count);
    }
    {
        const CmHirItem *imported = find_item(context,
            CM_HIR_ITEM_FUNCTION, "from_ref");
        assert(imported != NULL && cm_hir_def_id_equal(imported->definition,
            imported_definition)
            && imported->data.function_item.body == CM_HIR_BODY_NONE
            && cm_hir_get_body(context,
                imported->data.function_item.body) == NULL);
    }
    cm_hir_module_map_destroy(&map);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void assert_artifact_identity_same(const CmHirLibraryArtifact *artifact,
    const CmHirLibraryArtifactIdentity *expected)
{
    CmHirLibraryArtifactIdentity actual;
    assert(cm_hir_library_artifact_identity(artifact, &actual));
    assert(actual.context == expected->context
        && actual.crate_id == expected->crate_id
        && cm_hir_def_id_equal(actual.root_definition,
            expected->root_definition)
        && strcmp(actual.extern_name, expected->extern_name) == 0);
}

static void assert_expectation_rejected(CmHirContext *context,
    CmHirLibraryArtifact *artifact, const CmHirDeclarationMetadata *metadata,
    const CmHirDeclarationMaterializeExpectation *expectation,
    ContextLengths lengths, const CmHirLibraryArtifactIdentity *identity)
{
    CmHirDeclarationMaterializeResult result;
    result = cm_hir_declaration_metadata_materialize(context, artifact,
        metadata, expectation, "replacement", 91u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_INVALID_METADATA
        && result.metadata_status
            == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    assert_context_lengths(context, lengths);
    assert_artifact_identity_same(artifact, identity);
}

static void assert_item_metadata_rejected(CmHirContext *context,
    CmHirLibraryArtifact *artifact, const CmHirDeclarationMetadata *metadata,
    const CmHirDeclarationMaterializeExpectation *expectation,
    ContextLengths lengths, const CmHirLibraryArtifactIdentity *identity,
    CmSourceId source)
{
    CmHirDeclarationMaterializeResult result;
    result = cm_hir_declaration_metadata_materialize(context, artifact,
        metadata, expectation, "replacement", source);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_INVALID_METADATA
        && result.metadata_status
            == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    assert_context_lengths(context, lengths);
    assert_artifact_identity_same(artifact, identity);
}

static void test_materialize_decode_and_consume(void)
{
    TestFixture fixture;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirDeclarationMaterializeResult result;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirLibraryBinding canonical;
    CmHirLibraryBinding reexport;
    CmHirLibraryValue needs;
    CmHirLibraryPathSegment value_path[2];
    const CmHirItem *gate_item;
    const CmHirItem *needs_item;
    const CmHirGenericParam *trait_parameter;
    ContextLengths lengths;
    CmHirLibraryArtifactIdentity identity;
    CmHirDeclarationMaterializeExpectation wrong;
    CmHirDeclarationString saved_string;
    uint8_t saved_byte;
    size_t saved_count;
    uint32_t saved_owner;

    fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
        == CM_HIR_DECL_METADATA_OK);
    assert(encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

    expectation = expectation_for(&decoded);
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "dep", 77u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
        && result.module_count == 1u
        && result.item_count == 0u
        && result.public_type_entry_count == 2u
        && result.public_value_entry_count == 1u);
    canonical = lookup_binding(&artifact, "Gate");
    reexport = lookup_binding(&artifact, "GateReexport");
    assert(canonical.kind == CM_HIR_LIBRARY_BINDING_TRAIT
        && reexport.kind == CM_HIR_LIBRARY_BINDING_TRAIT
        && cm_hir_def_id_equal(canonical.definition, reexport.definition));
    gate_item = find_item(&context, CM_HIR_ITEM_TRAIT, "Gate");
    needs_item = find_item(&context, CM_HIR_ITEM_FUNCTION, "needs");
    assert(gate_item != NULL && gate_item->generic_parameter_count == 1u
        && cm_hir_def_id_equal(gate_item->definition, canonical.definition));
    trait_parameter = cm_hir_get_generic_param(&context,
        gate_item->generic_parameter_start);
    assert(trait_parameter != NULL && trait_parameter->is_relaxed_sized == 1
        && cm_hir_def_id_equal(trait_parameter->owner,
            gate_item->definition));
    assert_gate_predicate(&context, needs_item, canonical.definition);
    value_path[0].bytes = (const unsigned char *)"dep";
    value_path[0].length = 3u;
    value_path[1].bytes = (const unsigned char *)"needs";
    value_path[1].length = 5u;
    memset(&needs, 0, sizeof(needs));
    assert(cm_hir_library_artifact_lookup_value(&artifact, value_path, 2u,
        &needs) == CM_HIR_LIBRARY_OK
        && needs.data.function.predicate_count == 1u
        && needs.data.function.nominal_reference_count == 1u
        && cm_hir_def_id_equal(needs.data.function.nominal_references[0]
                .definition,
            canonical.definition));

    test_fresh_consumer(&context, &artifact, canonical.definition);
    lengths = context_lengths(&context);
    assert(cm_hir_library_artifact_identity(&artifact, &identity));

    wrong = expectation;
    wrong.crate_name = (CmHirDeclarationString)S("wrongcrate");
    assert_expectation_rejected(&context, &artifact, &decoded, &wrong,
        lengths, &identity);
    wrong = expectation;
    wrong.crate_disambiguator = (CmHirDeclarationString)S("wrong-disamb");
    assert_expectation_rejected(&context, &artifact, &decoded, &wrong,
        lengths, &identity);
    wrong = expectation;
    wrong.edition = CM_HIR_DECL_EDITION_2024;
    assert_expectation_rejected(&context, &artifact, &decoded, &wrong,
        lengths, &identity);
    wrong = expectation;
    wrong.target_triple = (CmHirDeclarationString)S("aarch64-unknown-linux-gnu");
    assert_expectation_rejected(&context, &artifact, &decoded, &wrong,
        lengths, &identity);
    wrong = expectation;
    wrong.data_layout = (CmHirDeclarationString)S("e-p:32:32");
    assert_expectation_rejected(&context, &artifact, &decoded, &wrong,
        lengths, &identity);
    wrong = expectation;
    wrong.panic_strategy = CM_HIR_DECL_PANIC_UNWIND;
    assert_expectation_rejected(&context, &artifact, &decoded, &wrong,
        lengths, &identity);
    wrong = expectation;
    wrong.cfg_count = 1u;
    assert_expectation_rejected(&context, &artifact, &decoded, &wrong,
        lengths, &identity);
    wrong = expectation;
    {
        CmHirDeclarationString wrong_cfgs[2];
        wrong_cfgs[0] = (CmHirDeclarationString)S("target_arch=aarch64");
        wrong_cfgs[1] = decoded.cfgs[1];
        wrong.cfgs = wrong_cfgs;
        assert_expectation_rejected(&context, &artifact, &decoded, &wrong,
            lengths, &identity);
    }

    saved_owner = decoded.predicates[0].owner_value;
    decoded.predicates[0].owner_value = 2u;
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "replacement", 92u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_INVALID_METADATA);
    decoded.predicates[0].owner_value = saved_owner;
    assert_context_lengths(&context, lengths);
    assert_artifact_identity_same(&artifact, &identity);

    saved_string = decoded.modules[0].name;
    decoded.modules[0].name = (CmHirDeclarationString)S("wrongroot");
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "replacement", 93u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_INVALID_METADATA);
    decoded.modules[0].name = saved_string;
    assert_context_lengths(&context, lengths);
    assert_artifact_identity_same(&artifact, &identity);

    saved_byte = decoded.namespace_entries[1].target_kind;
    decoded.namespace_entries[1].target_kind = CM_HIR_DECL_TARGET_VALUE;
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "replacement", 94u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_INVALID_METADATA);
    decoded.namespace_entries[1].target_kind = saved_byte;
    assert_context_lengths(&context, lengths);
    assert_artifact_identity_same(&artifact, &identity);

    saved_count = decoded.predicates[0].argument_count;
    decoded.predicates[0].argument_count = 0u;
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "replacement", 95u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_INVALID_METADATA);
    decoded.predicates[0].argument_count = (uint32_t)saved_count;
    assert_context_lengths(&context, lengths);
    assert_artifact_identity_same(&artifact, &identity);

    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "bad-name", 96u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_ARTIFACT_FAILURE
        && result.library_status == CM_HIR_LIBRARY_INVALID_ARGUMENT);
    assert_context_lengths(&context, lengths);
    assert_artifact_identity_same(&artifact, &identity);

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

static void test_item_materialize_and_consume(void)
{
    TestFixture fixture;
    CmByteBuf encoded;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirDeclarationMaterializeExpectation wrong;
    CmHirDeclarationMaterializeResult result;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirLibraryArtifactIdentity identity;
    CmHirLibraryBinding type_binding;
    CmHirLibraryBinding type_reexport;
    CmHirLibraryBinding constructor;
    CmHirLibraryBinding constructor_reexport;
    CmHirLibraryPathSegment path[2];
    CmHirLibraryType type;
    CmHirLibraryValue value;
    const CmHirItem *item;
    const CmHirDefinition *definition;
    ContextLengths lengths;
    uint8_t saved_kind;
    uint32_t saved_local;

    item_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    expectation = expectation_for(&decoded);
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "dep", 101u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
        && result.module_count == 1u
        && result.item_count == 1u
        && result.public_type_entry_count == 4u
        && result.public_value_entry_count == 3u);

    type_binding = lookup_binding(&artifact, "Packet");
    type_reexport = lookup_binding(&artifact, "PacketReexport");
    constructor = lookup_value_binding(&artifact, "Packet");
    constructor_reexport = lookup_value_binding(&artifact,
        "PacketReexport");
    assert(type_binding.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && type_binding.type_kind == CM_HIR_TYPE_ADT_KIND
        && type_reexport.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && constructor.kind
            == CM_HIR_LIBRARY_BINDING_STRUCT_CONSTRUCTOR
        && constructor.value_kind == CM_HIR_LIBRARY_VALUE_NONE
        && constructor_reexport.kind
            == CM_HIR_LIBRARY_BINDING_STRUCT_CONSTRUCTOR
        && cm_hir_def_id_equal(type_binding.definition,
            type_reexport.definition)
        && cm_hir_def_id_equal(type_binding.definition,
            constructor.definition)
        && cm_hir_def_id_equal(type_binding.definition,
            constructor_reexport.definition));
    item = find_item(&context, CM_HIR_ITEM_STRUCT, "Packet");
    definition = item == NULL ? NULL
        : cm_hir_lookup_definition(&context, item->definition);
    assert(item != NULL
        && definition != NULL
        && definition->state == CM_HIR_DEFINITION_BOUND
        && definition->kind == CM_HIR_DEFINITION_ITEM
        && definition->has_reserved_item_kind
        && definition->reserved_item_kind == CM_HIR_ITEM_STRUCT
        && cm_hir_def_id_equal(item->definition, type_binding.definition)
        && item->owner_module == result.root_module
        && item->visibility.kind == CM_HIR_VIS_PUBLIC
        && item->generic_parameter_count == 0u
        && item->predicate_scope_count == 0u
        && item->predicate_count == 0u
        && item->outlives_predicate_count == 0u
        && item->attribute_count == 0u
        && item->data.aggregate_item.form == CM_HIR_AGGREGATE_UNIT
        && item->data.aggregate_item.field_count == 0u
        && item->data.aggregate_item.fields == NULL);

    path[0].bytes = (const unsigned char *)"dep";
    path[0].length = 3u;
    path[1].bytes = (const unsigned char *)"Packet";
    path[1].length = 6u;
    memset(&type, 0, sizeof(type));
    assert(cm_hir_library_artifact_lookup_type(&artifact, path, 2u, &type)
        == CM_HIR_LIBRARY_OK
        && type.kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(type.definition, item->definition));
    memset(&value, 0, sizeof(value));
    assert(cm_hir_library_artifact_lookup_value(&artifact, path, 2u, &value)
        == CM_HIR_LIBRARY_WRONG_NAMESPACE);

    test_item_fresh_consumer(&context, &artifact, item->definition);
    lengths = context_lengths(&context);
    assert(cm_hir_library_artifact_identity(&artifact, &identity));

    wrong = expectation;
    wrong.crate_disambiguator =
        (CmHirDeclarationString)S("wrong-item-disambiguator");
    assert_expectation_rejected(&context, &artifact, &decoded, &wrong,
        lengths, &identity);

    saved_kind = decoded.namespace_entries[2].target_kind;
    decoded.namespace_entries[2].target_kind = CM_HIR_DECL_TARGET_NOMINAL;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 102u);
    decoded.namespace_entries[2].target_kind = saved_kind;

    saved_kind = decoded.namespace_entries[4].target_kind;
    decoded.namespace_entries[4].target_kind = CM_HIR_DECL_TARGET_VALUE;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 103u);
    decoded.namespace_entries[4].target_kind = saved_kind;

    saved_local = decoded.namespace_entries[4].target_local;
    decoded.namespace_entries[4].target_local = 2u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 104u);
    decoded.namespace_entries[4].target_local = saved_local;

    saved_kind = decoded.items[0].kind;
    decoded.items[0].kind = UINT8_C(1);
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 105u);
    decoded.items[0].kind = saved_kind;

    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "bad-name", 106u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_ARTIFACT_FAILURE
        && result.library_status == CM_HIR_LIBRARY_INVALID_ARGUMENT);
    assert_context_lengths(&context, lengths);
    assert_artifact_identity_same(&artifact, &identity);

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&encoded);
}

static void test_alias_materialize_and_consume(void)
{
    AliasFixture fixture;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirDeclarationMaterializeResult result;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirLibraryArtifactIdentity identity;
    CmHirLibraryBinding alloc_error;
    CmHirLibraryBinding alias;
    CmHirLibraryBinding alias_reexport;
    CmHirLibraryBinding layout_error;
    CmHirLibraryBinding layout_error_reexport;
    const CmHirItem *alias_item;
    const CmHirItem *layout_error_item;
    const CmHirDefinition *alias_definition;
    const CmHirDefinition *layout_error_definition;
    const CmHirType *alias_target;
    ContextLengths lengths;
    uint8_t saved_kind;
    uint32_t saved_local;

    alias_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
        == CM_HIR_DECL_METADATA_OK);
    assert(encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

    expectation = expectation_for(&decoded);
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "dep", 111u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
        && result.module_count == 1u
        && result.item_count == 3u
        && result.public_type_entry_count == 5u
        && result.public_value_entry_count == 1u);

    alloc_error = lookup_value_binding(&artifact, "AllocError");
    alias = lookup_binding(&artifact, "LayoutErr");
    alias_reexport = lookup_binding(&artifact, "LayoutErrReexport");
    layout_error = lookup_binding(&artifact, "LayoutError");
    layout_error_reexport = lookup_binding(&artifact,
        "LayoutErrorReexport");
    assert(alloc_error.kind == CM_HIR_LIBRARY_BINDING_STRUCT_CONSTRUCTOR
        && alias.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && alias.type_kind == CM_HIR_TYPE_ALIAS_APPLICATION_KIND
        && alias_reexport.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && alias_reexport.type_kind == CM_HIR_TYPE_ALIAS_APPLICATION_KIND
        && layout_error.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && layout_error.type_kind == CM_HIR_TYPE_ADT_KIND
        && layout_error_reexport.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && layout_error_reexport.type_kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(alias.definition,
            alias_reexport.definition)
        && cm_hir_def_id_equal(layout_error.definition,
            layout_error_reexport.definition)
        && !cm_hir_def_id_equal(alias.definition, layout_error.definition));
    assert(lookup_value_binding_status(&artifact, "LayoutErr")
            == CM_HIR_LIBRARY_NOT_FOUND
        && lookup_value_binding_status(&artifact, "LayoutErrReexport")
            == CM_HIR_LIBRARY_NOT_FOUND
        && lookup_value_binding_status(&artifact, "LayoutError")
            == CM_HIR_LIBRARY_NOT_FOUND
        && lookup_value_binding_status(&artifact, "LayoutErrorReexport")
            == CM_HIR_LIBRARY_NOT_FOUND);

    alias_item = find_item(&context, CM_HIR_ITEM_TYPE_ALIAS, "LayoutErr");
    layout_error_item = find_item(&context, CM_HIR_ITEM_STRUCT,
        "LayoutError");
    alias_definition = alias_item == NULL ? NULL
        : cm_hir_lookup_definition(&context, alias_item->definition);
    layout_error_definition = layout_error_item == NULL ? NULL
        : cm_hir_lookup_definition(&context, layout_error_item->definition);
    alias_target = alias_item == NULL ? NULL : cm_hir_get_type(&context,
        alias_item->data.type_alias_item.target);
    assert(alias_item != NULL && layout_error_item != NULL
        && alias_definition != NULL
        && alias_definition->state == CM_HIR_DEFINITION_BOUND
        && alias_definition->kind == CM_HIR_DEFINITION_ITEM
        && alias_definition->has_reserved_item_kind
        && alias_definition->reserved_item_kind == CM_HIR_ITEM_TYPE_ALIAS
        && layout_error_definition != NULL
        && layout_error_definition->state == CM_HIR_DEFINITION_BOUND
        && layout_error_definition->kind == CM_HIR_DEFINITION_ITEM
        && layout_error_definition->has_reserved_item_kind
        && layout_error_definition->reserved_item_kind == CM_HIR_ITEM_STRUCT
        && cm_hir_def_id_equal(alias_item->definition, alias.definition)
        && cm_hir_def_id_equal(layout_error_item->definition,
            layout_error.definition)
        && cm_hir_def_id_is_none(alias_item->parent_definition)
        && cm_hir_def_id_is_none(
            alias_item->data.type_alias_item.trait_item_definition)
        && alias_item->data.type_alias_item.bound_count == 0u
        && alias_item->data.type_alias_item.bounds == NULL
        && alias_target != NULL && alias_target->kind == CM_HIR_TYPE_ADT_KIND
        && alias_target->data.named_type.argument_count == 0u
        && cm_hir_def_id_equal(alias_target->data.named_type.definition,
            layout_error.definition)
        && layout_error_item->data.aggregate_item.form
            == CM_HIR_AGGREGATE_UNIT);

    test_alias_fresh_consumer(&context, &artifact,
        layout_error.definition);
    lengths = context_lengths(&context);
    assert(cm_hir_library_artifact_identity(&artifact, &identity));

    saved_local = decoded.items[1].alias_target_type;
    decoded.items[1].alias_target_type = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 112u);
    decoded.items[1].alias_target_type = saved_local;

    saved_local = decoded.types[0].item_local;
    decoded.types[0].item_local = 2u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 113u);
    decoded.types[0].item_local = saved_local;

    saved_local = decoded.namespace_entries[5].target_local;
    decoded.namespace_entries[5].target_local = 2u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 114u);
    decoded.namespace_entries[5].target_local = saved_local;

    saved_kind = decoded.items[1].kind;
    decoded.items[1].kind = CM_HIR_DECL_ITEM_STRUCT;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 115u);
    decoded.items[1].kind = saved_kind;

    saved_local = decoded.namespace_entries[1].target_local;
    decoded.namespace_entries[1].target_local = 3u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 116u);
    decoded.namespace_entries[1].target_local = saved_local;

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

static void test_composite_materialize_and_consume(void)
{
    CompositeFixture fixture;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirDeclarationMaterializeResult result;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirLibraryArtifactIdentity identity;
    CmHirLibraryBinding wrap_binding;
    const CmHirItem *wrap;
    const CmHirItem *inspect;
    const CmHirGenericParam *parameter;
    ContextLengths lengths;
    uint8_t saved_kind;
    uint32_t saved_count;
    uint32_t saved_local;

    composite_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
        == CM_HIR_DECL_METADATA_OK);
    assert(encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

    expectation = expectation_for(&decoded);
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "dep", 121u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
        && result.item_count == 1u
        && result.public_type_entry_count == 2u
        && result.public_value_entry_count == 2u);
    wrap_binding = lookup_binding(&artifact, "Wrap");
    wrap = find_item(&context, CM_HIR_ITEM_STRUCT, "Wrap");
    inspect = find_item(&context, CM_HIR_ITEM_FUNCTION, "inspect");
    assert(wrap_binding.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && wrap_binding.type_kind == CM_HIR_TYPE_ADT_KIND
        && wrap != NULL
        && cm_hir_def_id_equal(wrap_binding.definition, wrap->definition)
        && wrap->generic_parameter_count == 1u
        && wrap->generic_parameter_start != CM_HIR_GENERIC_PARAM_NONE
        && wrap->data.aggregate_item.form == CM_HIR_AGGREGATE_UNIT);
    parameter = cm_hir_get_generic_param(&context,
        wrap->generic_parameter_start);
    assert(parameter != NULL && parameter->kind == CM_HIR_GENERIC_TYPE
        && parameter->index == 0u
        && cm_hir_def_id_equal(parameter->owner, wrap->definition));
    assert_composite_signature(&context, inspect, wrap->definition);
    test_composite_fresh_consumer(&context, &artifact, wrap->definition);

    lengths = context_lengths(&context);
    assert(cm_hir_library_artifact_identity(&artifact, &identity));

    saved_kind = decoded.types[5].region.kind;
    decoded.types[5].region.kind = CM_HIR_DECL_REGION_EARLY_BOUND;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 122u);
    decoded.types[5].region.kind = saved_kind;

    saved_count = decoded.types[6].argument_count;
    decoded.types[6].argument_count = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 123u);
    decoded.types[6].argument_count = saved_count;

    saved_local = decoded.generics[1].owner_local;
    decoded.generics[1].owner_local = 2u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 124u);
    decoded.generics[1].owner_local = saved_local;

    saved_kind = decoded.types[3].kind;
    decoded.types[3].kind = CM_HIR_DECL_TYPE_SELF;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 125u);
    decoded.types[3].kind = saved_kind;

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

static void test_const_materialize_and_consume(void)
{
    ConstFixture fixture;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirDeclarationMaterializeResult result;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirLibraryArtifactIdentity identity;
    CmHirLibraryBinding direct;
    CmHirLibraryBinding reexport;
    CmHirLibraryValue direct_value;
    CmHirLibraryValue reexport_value;
    CmHirLibraryPathSegment path[2];
    const CmHirItem *constant;
    const CmHirDefinition *definition;
    const CmHirType *declared_type;
    ContextLengths lengths;
    uint32_t saved_local;
    uint8_t saved_byte;

    const_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
        == CM_HIR_DECL_METADATA_OK);
    assert(encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

    expectation = expectation_for(&decoded);
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "dep", 141u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
        && result.module_count == 1u && result.item_count == 0u
        && result.public_type_entry_count == 0u
        && result.public_value_entry_count == 2u
        && context.bodies.len == 0u);
    direct = lookup_value_binding(&artifact, "MAX");
    reexport = lookup_value_binding(&artifact, "MAX_REEXPORT");
    assert(direct.kind == CM_HIR_LIBRARY_BINDING_VALUE
        && direct.value_kind == CM_HIR_LIBRARY_VALUE_CONST
        && reexport.kind == CM_HIR_LIBRARY_BINDING_VALUE
        && reexport.value_kind == CM_HIR_LIBRARY_VALUE_CONST
        && cm_hir_def_id_equal(direct.definition, reexport.definition));

    constant = find_item(&context, CM_HIR_ITEM_CONST, "MAX");
    definition = constant == NULL ? NULL
        : cm_hir_lookup_definition(&context, constant->definition);
    declared_type = constant == NULL ? NULL
        : cm_hir_get_type(&context, constant->data.value_item.type);
    assert(constant != NULL && definition != NULL
        && definition->kind == CM_HIR_DEFINITION_ITEM
        && definition->state == CM_HIR_DEFINITION_BOUND
        && definition->has_reserved_item_kind
        && definition->reserved_item_kind == CM_HIR_ITEM_CONST
        && cm_hir_def_id_equal(constant->definition, direct.definition)
        && cm_hir_def_id_is_none(constant->parent_definition)
        && constant->generic_parameter_count == 0u
        && constant->predicate_count == 0u
        && constant->data.value_item.body == CM_HIR_BODY_NONE
        && constant->data.value_item.definition_kind
            == CM_HIR_VALUE_DEFINITION_METADATA_DECLARATION
        && constant->data.value_item.has_default_body == 0
        && constant->data.value_item.mutability == CM_HIR_IMMUTABLE
        && cm_hir_def_id_is_none(
            constant->data.value_item.trait_item_definition)
        && declared_type != NULL
        && declared_type->kind == CM_HIR_TYPE_CHAR_KIND);

    path[0].bytes = (const unsigned char *)"dep";
    path[0].length = sizeof("dep") - 1u;
    path[1].bytes = (const unsigned char *)"MAX";
    path[1].length = sizeof("MAX") - 1u;
    memset(&direct_value, 0, sizeof(direct_value));
    assert(cm_hir_library_artifact_lookup_value(&artifact, path, 2u,
        &direct_value) == CM_HIR_LIBRARY_OK);
    path[1].bytes = (const unsigned char *)"MAX_REEXPORT";
    path[1].length = sizeof("MAX_REEXPORT") - 1u;
    memset(&reexport_value, 0, sizeof(reexport_value));
    assert(cm_hir_library_artifact_lookup_value(&artifact, path, 2u,
        &reexport_value) == CM_HIR_LIBRARY_OK
        && direct_value.kind == CM_HIR_LIBRARY_VALUE_CONST
        && reexport_value.kind == CM_HIR_LIBRARY_VALUE_CONST
        && cm_hir_def_id_equal(direct_value.definition,
            reexport_value.definition)
        && direct_value.data.value.type == constant->data.value_item.type
        && reexport_value.data.value.type == constant->data.value_item.type
        && direct_value.data.value.mutability == CM_HIR_IMMUTABLE
        && reexport_value.data.value.mutability == CM_HIR_IMMUTABLE);

    test_const_fresh_consumer(&context, &artifact, constant->definition);
    lengths = context_lengths(&context);
    assert(cm_hir_library_artifact_identity(&artifact, &identity));

    saved_byte = decoded.values[0].kind;
    decoded.values[0].kind = CM_HIR_DECL_VALUE_FUNCTION;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 142u);
    decoded.values[0].kind = saved_byte;

    saved_local = decoded.values[0].declared_type;
    decoded.values[0].declared_type = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 143u);
    decoded.values[0].declared_type = saved_local;

    saved_byte = decoded.values[0].mutability;
    decoded.values[0].mutability = CM_HIR_DECL_MUTABLE;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 144u);
    decoded.values[0].mutability = saved_byte;

    saved_byte = decoded.values[0].has_body;
    decoded.values[0].has_body = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 145u);
    decoded.values[0].has_body = saved_byte;

    saved_local = decoded.values[0].return_type;
    decoded.values[0].return_type = 1u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 146u);
    decoded.values[0].return_type = saved_local;

    saved_local = decoded.namespace_entries[1].target_local;
    decoded.namespace_entries[1].target_local = 2u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 147u);
    decoded.namespace_entries[1].target_local = saved_local;

    saved_byte = decoded.namespace_entries[0].namespace_kind;
    decoded.namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 148u);
    decoded.namespace_entries[0].namespace_kind = saved_byte;

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

static void test_static_materialize_and_consume(void)
{
    StaticFixture fixture;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirDeclarationMaterializeResult result;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirLibraryArtifactIdentity identity;
    CmHirLibraryBinding direct;
    CmHirLibraryBinding reexport;
    CmHirLibraryValue direct_value;
    CmHirLibraryValue reexport_value;
    CmHirLibraryPathSegment path[2];
    const CmHirItem *static_item;
    const CmHirDefinition *definition;
    const CmHirType *array_type;
    const CmHirType *tuple_type;
    const CmHirType *length_type;
    const CmHirType *element_type;
    ContextLengths lengths;
    uint32_t saved_local;
    uint64_t saved_bits;
    uint8_t saved_byte;

    static_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
        == CM_HIR_DECL_METADATA_OK);
    assert(encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

    expectation = expectation_for(&decoded);
    {
        CmHirContext mutable_context;
        CmHirLibraryArtifact mutable_artifact;
        CmHirDeclarationMaterializeResult mutable_result;
        CmHirLibraryValue mutable_value;
        const CmHirItem *mutable_item;

        decoded.values[0].mutability = CM_HIR_DECL_MUTABLE;
        assert(cm_hir_declaration_metadata_validate(&decoded)
            == CM_HIR_DECL_METADATA_OK);
        cm_hir_context_init(&mutable_context);
        cm_hir_library_artifact_init(&mutable_artifact);
        mutable_result = cm_hir_declaration_metadata_materialize(
            &mutable_context, &mutable_artifact, &decoded, &expectation,
            "dep", 157u);
        assert(mutable_result.status == CM_HIR_DECL_MATERIALIZE_OK);
        path[0].bytes = (const unsigned char *)"dep";
        path[0].length = sizeof("dep") - 1u;
        path[1].bytes = (const unsigned char *)"CACHED_POW10";
        path[1].length = sizeof("CACHED_POW10") - 1u;
        memset(&mutable_value, 0, sizeof(mutable_value));
        assert(cm_hir_library_artifact_lookup_value(&mutable_artifact,
            path, 2u, &mutable_value) == CM_HIR_LIBRARY_OK
            && mutable_value.kind == CM_HIR_LIBRARY_VALUE_STATIC
            && mutable_value.data.value.mutability == CM_HIR_MUTABLE);
        mutable_item = find_item(&mutable_context, CM_HIR_ITEM_STATIC,
            "CACHED_POW10");
        assert(mutable_item != NULL
            && mutable_item->data.value_item.mutability == CM_HIR_MUTABLE
            && mutable_item->data.value_item.definition_kind
                == CM_HIR_VALUE_DEFINITION_METADATA_DECLARATION
            && mutable_item->data.value_item.body == CM_HIR_BODY_NONE);
        cm_hir_library_artifact_destroy(&mutable_artifact);
        cm_hir_context_destroy(&mutable_context);
        decoded.values[0].mutability = CM_HIR_DECL_IMMUTABLE;
    }
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "dep", 158u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
        && result.module_count == 1u && result.item_count == 0u
        && result.public_type_entry_count == 0u
        && result.public_value_entry_count == 2u
        && context.bodies.len == 0u);
    direct = lookup_value_binding(&artifact, "CACHED_POW10");
    reexport = lookup_value_binding(&artifact, "CACHED_POW10_ALIAS");
    assert(direct.kind == CM_HIR_LIBRARY_BINDING_VALUE
        && direct.value_kind == CM_HIR_LIBRARY_VALUE_STATIC
        && reexport.kind == CM_HIR_LIBRARY_BINDING_VALUE
        && reexport.value_kind == CM_HIR_LIBRARY_VALUE_STATIC
        && cm_hir_def_id_equal(direct.definition, reexport.definition));

    static_item = find_item(&context, CM_HIR_ITEM_STATIC, "CACHED_POW10");
    definition = static_item == NULL ? NULL
        : cm_hir_lookup_definition(&context, static_item->definition);
    array_type = static_item == NULL ? NULL
        : cm_hir_get_type(&context, static_item->data.value_item.type);
    tuple_type = array_type == NULL
            || array_type->kind != CM_HIR_TYPE_ARRAY_KIND ? NULL
        : cm_hir_get_type(&context, array_type->data.array_type.element);
    length_type = array_type == NULL
            || array_type->kind != CM_HIR_TYPE_ARRAY_KIND ? NULL
        : cm_hir_get_type(&context, array_type->data.array_type.length.type);
    assert(static_item != NULL && definition != NULL
        && definition->kind == CM_HIR_DEFINITION_ITEM
        && definition->state == CM_HIR_DEFINITION_BOUND
        && definition->has_reserved_item_kind
        && definition->reserved_item_kind == CM_HIR_ITEM_STATIC
        && cm_hir_def_id_equal(static_item->definition, direct.definition)
        && cm_hir_def_id_is_none(static_item->parent_definition)
        && static_item->generic_parameter_count == 0u
        && static_item->predicate_count == 0u
        && static_item->data.value_item.body == CM_HIR_BODY_NONE
        && static_item->data.value_item.definition_kind
            == CM_HIR_VALUE_DEFINITION_METADATA_DECLARATION
        && static_item->data.value_item.has_default_body == 0
        && static_item->data.value_item.mutability == CM_HIR_IMMUTABLE
        && cm_hir_def_id_is_none(
            static_item->data.value_item.trait_item_definition)
        && cm_hir_get_body(&context,
            static_item->data.value_item.body) == NULL
        && cm_hir_body_value_owner_kind(&context, static_item)
            == CM_HIR_BODY_VALUE_OWNER_UNSUPPORTED
        && array_type != NULL && array_type->kind == CM_HIR_TYPE_ARRAY_KIND
        && array_type->data.array_type.length.kind == CM_HIR_CONST_VALUE
        && array_type->data.array_type.length.data.value.low_bits
            == UINT64_C(81)
        && array_type->data.array_type.length.data.value.high_bits == 0u
        && length_type != NULL
        && length_type->kind == CM_HIR_TYPE_INTEGER_KIND
        && length_type->data.integer_type.kind == CM_HIR_INT_USIZE
        && tuple_type != NULL && tuple_type->kind == CM_HIR_TYPE_TUPLE_KIND
        && tuple_type->data.tuple_type.element_count == 3u
        && tuple_type->data.tuple_type.elements != NULL);
    element_type = cm_hir_get_type(&context,
        tuple_type->data.tuple_type.elements[0]);
    assert(element_type != NULL
        && element_type->kind == CM_HIR_TYPE_INTEGER_KIND
        && element_type->data.integer_type.kind == CM_HIR_INT_U64);
    element_type = cm_hir_get_type(&context,
        tuple_type->data.tuple_type.elements[1]);
    assert(element_type != NULL
        && element_type->kind == CM_HIR_TYPE_INTEGER_KIND
        && element_type->data.integer_type.kind == CM_HIR_INT_I16
        && tuple_type->data.tuple_type.elements[1]
            == tuple_type->data.tuple_type.elements[2]);

    path[0].bytes = (const unsigned char *)"dep";
    path[0].length = sizeof("dep") - 1u;
    path[1].bytes = (const unsigned char *)"CACHED_POW10";
    path[1].length = sizeof("CACHED_POW10") - 1u;
    memset(&direct_value, 0, sizeof(direct_value));
    assert(cm_hir_library_artifact_lookup_value(&artifact, path, 2u,
        &direct_value) == CM_HIR_LIBRARY_OK);
    path[1].bytes = (const unsigned char *)"CACHED_POW10_ALIAS";
    path[1].length = sizeof("CACHED_POW10_ALIAS") - 1u;
    memset(&reexport_value, 0, sizeof(reexport_value));
    assert(cm_hir_library_artifact_lookup_value(&artifact, path, 2u,
        &reexport_value) == CM_HIR_LIBRARY_OK
        && direct_value.kind == CM_HIR_LIBRARY_VALUE_STATIC
        && reexport_value.kind == CM_HIR_LIBRARY_VALUE_STATIC
        && cm_hir_def_id_equal(direct_value.definition,
            reexport_value.definition)
        && direct_value.data.value.type == static_item->data.value_item.type
        && reexport_value.data.value.type == static_item->data.value_item.type
        && direct_value.data.value.mutability == CM_HIR_IMMUTABLE
        && reexport_value.data.value.mutability == CM_HIR_IMMUTABLE);

    test_static_fresh_consumer(&context, &artifact,
        static_item->definition);
    lengths = context_lengths(&context);
    assert(cm_hir_library_artifact_identity(&artifact, &identity));

    saved_byte = decoded.values[0].kind;
    decoded.values[0].kind = CM_HIR_DECL_VALUE_FUNCTION;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 159u);
    decoded.values[0].kind = saved_byte;

    saved_byte = decoded.values[0].mutability;
    decoded.values[0].mutability = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 160u);
    decoded.values[0].mutability = saved_byte;

    saved_local = decoded.values[0].declared_type;
    decoded.values[0].declared_type = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 161u);
    decoded.values[0].declared_type = saved_local;

    saved_byte = decoded.values[0].has_body;
    decoded.values[0].has_body = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 162u);
    decoded.values[0].has_body = saved_byte;

    saved_local = decoded.types[4].array_length_type;
    decoded.types[4].array_length_type = 2u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 163u);
    decoded.types[4].array_length_type = saved_local;

    saved_bits = decoded.types[4].array_length_high_bits;
    decoded.types[4].array_length_high_bits = UINT64_C(1);
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 164u);
    decoded.types[4].array_length_high_bits = saved_bits;

    saved_local = decoded.types[3].element_types[1];
    decoded.types[3].element_types[1] = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 165u);
    decoded.types[3].element_types[1] = saved_local;

    saved_byte = decoded.namespace_entries[0].namespace_kind;
    decoded.namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 166u);
    decoded.namespace_entries[0].namespace_kind = saved_byte;

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

static void test_primitive_materialize_and_consume(void)
{
    PrimitiveFixture fixture;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirDeclarationMaterializeResult result;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirLibraryArtifactIdentity identity;
    ContextLengths lengths;
    size_t index;
    uint8_t saved_byte;
    uint32_t saved_local;

    primitive_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
        == CM_HIR_DECL_METADATA_OK);
    assert(encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

    expectation = expectation_for(&decoded);
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "dep", 181u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
        && result.module_count == 1u && result.item_count == 0u
        && result.public_type_entry_count == PRIMITIVE_BINDING_COUNT
        && result.public_value_entry_count == 0u
        && context.items.len == 0u && context.types.len == 0u
        && context.generic_parameters.len == 0u
        && context.bodies.len == 0u);
    for (index = 0u; index < PRIMITIVE_BINDING_COUNT; ++index) {
        const PrimitiveBindingSpec *spec = &primitive_binding_specs[index];
        CmHirLibraryPathSegment path[2];
        CmHirLibraryBinding binding = lookup_binding(&artifact, spec->name);
        CmHirLibraryType type;
        CmHirLibraryValue value;
        assert(binding.kind == CM_HIR_LIBRARY_BINDING_PRIMITIVE
            && cm_hir_def_id_is_none(binding.definition)
            && binding.type_kind == CM_HIR_TYPE_ERROR_KIND
            && binding.primitive_kind == spec->hir_kind
            && binding.value_kind == CM_HIR_LIBRARY_VALUE_NONE
            && cm_hir_def_id_is_none(binding.enum_definition)
            && binding.enum_variant_index == 0u
            && binding.enum_variant_form == 0u
            && binding.enum_variant_namespace == 0u);
        path[0].bytes = (const unsigned char *)"dep";
        path[0].length = sizeof("dep") - 1u;
        path[1].bytes = (const unsigned char *)spec->name;
        path[1].length = strlen(spec->name);
        memset(&type, 0, sizeof(type));
        assert(cm_hir_library_artifact_lookup_type(&artifact, path, 2u,
            &type) == CM_HIR_LIBRARY_OK
            && type.binding_kind == CM_HIR_LIBRARY_BINDING_PRIMITIVE
            && cm_hir_def_id_is_none(type.definition)
            && type.kind == CM_HIR_TYPE_ERROR_KIND
            && type.primitive_kind == spec->hir_kind);
        assert(lookup_value_binding_status(&artifact, spec->name)
            == CM_HIR_LIBRARY_NOT_FOUND);
        memset(&value, 0, sizeof(value));
        assert(cm_hir_library_artifact_lookup_value(&artifact, path, 2u,
            &value) == CM_HIR_LIBRARY_NOT_FOUND);
    }

    test_primitive_fresh_consumer(&context, &artifact);
    lengths = context_lengths(&context);
    assert(cm_hir_library_artifact_identity(&artifact, &identity));

    saved_byte = decoded.namespace_entries[0].namespace_kind;
    decoded.namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 182u);
    decoded.namespace_entries[0].namespace_kind = saved_byte;

    saved_byte = decoded.namespace_entries[0].target_kind;
    decoded.namespace_entries[0].target_kind = UINT8_C(255);
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 183u);
    decoded.namespace_entries[0].target_kind = saved_byte;

    saved_local = decoded.namespace_entries[0].target_local;
    decoded.namespace_entries[0].target_local = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 184u);
    decoded.namespace_entries[0].target_local =
        CM_HIR_DECL_PRIMITIVE_UNIT;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 185u);
    decoded.namespace_entries[0].target_local =
        (uint32_t)CM_HIR_DECL_PRIMITIVE_F64 + 1u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 186u);
    decoded.namespace_entries[0].target_local = saved_local;

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

static void test_aggregate_materialize_and_consume(void)
{
    AggregateFixture fixture;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirDeclarationMaterializeResult result;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirLibraryArtifactIdentity identity;
    CmHirLibraryBinding direct_binding;
    CmHirLibraryBinding alias_binding;
    const CmHirItem *assume;
    const CmHirItem *manually_drop;
    const CmHirItem *maybe_uninit;
    const CmHirGenericParam *generic;
    const CmHirType *type;
    ContextLengths lengths;
    uint32_t index;
    uint32_t saved_local;
    uint32_t saved_ordinal;
    uint8_t saved_byte;
    uint16_t saved_flags;

    aggregate_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
        == CM_HIR_DECL_METADATA_OK && encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);
    expectation = expectation_for(&decoded);
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "dep", 161u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
        && result.item_count == 3u
        && result.public_type_entry_count == 6u
        && result.public_value_entry_count == 0u);

    assume = find_item(&context, CM_HIR_ITEM_STRUCT, "Assume");
    manually_drop = find_item(&context, CM_HIR_ITEM_STRUCT,
        "ManuallyDrop");
    maybe_uninit = find_item(&context, CM_HIR_ITEM_UNION, "MaybeUninit");
    assert(assume != NULL && manually_drop != NULL && maybe_uninit != NULL
        && assume->data.aggregate_item.form == CM_HIR_AGGREGATE_NAMED
        && assume->data.aggregate_item.field_count == 4u
        && assume->attribute_count == 1u
        && manually_drop->data.aggregate_item.form
            == CM_HIR_AGGREGATE_NAMED
        && manually_drop->data.aggregate_item.field_count == 1u
        && manually_drop->attribute_count == 3u
        && maybe_uninit->data.aggregate_item.form
            == CM_HIR_AGGREGATE_NAMED
        && maybe_uninit->data.aggregate_item.field_count == 2u
        && maybe_uninit->attribute_count == 3u);
    assert_item_attribute(&context, assume, 0u,
        "lang = \"transmute_opts\"", 161u);
    assert_item_attribute(&context, manually_drop, 0u,
        "lang = \"manually_drop\"", 161u);
    assert_item_attribute(&context, manually_drop, 1u,
        "repr(transparent)", 161u);
    assert_item_attribute(&context, manually_drop, 2u,
        "rustc_pub_transparent", 161u);
    assert_item_attribute(&context, maybe_uninit, 0u,
        "lang = \"maybe_uninit\"", 161u);
    assert_item_attribute(&context, maybe_uninit, 1u,
        "repr(transparent)", 161u);
    assert_item_attribute(&context, maybe_uninit, 2u,
        "rustc_pub_transparent", 161u);

    for (index = 0u; index < 4u; ++index) {
        type = cm_hir_get_type(&context,
            assume->data.aggregate_item.fields[index].type);
        assert(type != NULL && type->kind == CM_HIR_TYPE_BOOL_KIND
            && assume->data.aggregate_item.fields[index].visibility.kind
                == CM_HIR_VIS_PUBLIC
            && assume->data.aggregate_item.fields[index].span.start
                == index);
    }
    generic = cm_hir_get_generic_param(&context,
        manually_drop->generic_parameter_start);
    type = cm_hir_get_type(&context,
        manually_drop->data.aggregate_item.fields[0].type);
    assert(manually_drop->generic_parameter_count == 1u
        && generic != NULL && generic->is_relaxed_sized
        && cm_hir_def_id_equal(generic->owner, manually_drop->definition)
        && manually_drop->data.aggregate_item.fields[0].visibility.kind
            == CM_HIR_VIS_PRIVATE
        && type != NULL && type->kind == CM_HIR_TYPE_PARAMETER_KIND
        && type->data.parameter_type.parameter
            == manually_drop->generic_parameter_start);
    generic = cm_hir_get_generic_param(&context,
        maybe_uninit->generic_parameter_start);
    type = cm_hir_get_type(&context,
        maybe_uninit->data.aggregate_item.fields[0].type);
    assert(maybe_uninit->generic_parameter_count == 1u
        && generic != NULL && !generic->is_relaxed_sized
        && cm_hir_def_id_equal(generic->owner, maybe_uninit->definition)
        && maybe_uninit->data.aggregate_item.fields[0].visibility.kind
            == CM_HIR_VIS_PRIVATE
        && maybe_uninit->data.aggregate_item.fields[1].visibility.kind
            == CM_HIR_VIS_PRIVATE
        && type != NULL && type->kind == CM_HIR_TYPE_UNIT_KIND);
    type = cm_hir_get_type(&context,
        maybe_uninit->data.aggregate_item.fields[1].type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(type->data.named_type.definition,
            manually_drop->definition)
        && type->data.named_type.argument_count == 1u
        && type->data.named_type.arguments[0].kind
            == CM_HIR_GENERIC_ARG_TYPE);
    type = cm_hir_get_type(&context,
        type->data.named_type.arguments[0].data.type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_PARAMETER_KIND
        && type->data.parameter_type.parameter
            == maybe_uninit->generic_parameter_start);

    direct_binding = lookup_binding(&artifact, "Assume");
    alias_binding = lookup_binding(&artifact, "AssumeAlias");
    assert(direct_binding.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && alias_binding.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && cm_hir_def_id_equal(direct_binding.definition,
            alias_binding.definition)
        && cm_hir_def_id_equal(direct_binding.definition,
            assume->definition));
    direct_binding = lookup_binding(&artifact, "ManuallyDrop");
    alias_binding = lookup_binding(&artifact, "ManuallyDropAlias");
    assert(direct_binding.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && alias_binding.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && cm_hir_def_id_equal(direct_binding.definition,
            alias_binding.definition)
        && cm_hir_def_id_equal(direct_binding.definition,
            manually_drop->definition));
    direct_binding = lookup_binding(&artifact, "MaybeUninit");
    alias_binding = lookup_binding(&artifact, "MaybeUninitAlias");
    assert(direct_binding.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && alias_binding.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && cm_hir_def_id_equal(direct_binding.definition,
            alias_binding.definition)
        && cm_hir_def_id_equal(direct_binding.definition,
            maybe_uninit->definition)
        && lookup_value_binding_status(&artifact, "Assume")
            == CM_HIR_LIBRARY_NOT_FOUND
        && lookup_value_binding_status(&artifact, "ManuallyDrop")
            == CM_HIR_LIBRARY_NOT_FOUND
        && lookup_value_binding_status(&artifact, "MaybeUninit")
            == CM_HIR_LIBRARY_NOT_FOUND);
    test_aggregate_fresh_consumer(&context, &artifact,
        assume->definition, manually_drop->definition,
        maybe_uninit->definition);
    lengths = context_lengths(&context);
    assert(cm_hir_library_artifact_identity(&artifact, &identity));

    saved_byte = decoded.items[2].aggregate_form;
    decoded.items[2].aggregate_form = CM_HIR_DECL_AGGREGATE_UNIT;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 162u);
    decoded.items[2].aggregate_form = saved_byte;
    saved_byte = decoded.items[2].aggregate_repr;
    decoded.items[2].aggregate_repr = CM_HIR_DECL_AGGREGATE_REPR_RUST;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 163u);
    decoded.items[2].aggregate_repr = saved_byte;
    saved_flags = decoded.items[1].aggregate_flags;
    decoded.items[1].aggregate_flags |= UINT16_C(0x8000);
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 164u);
    decoded.items[1].aggregate_flags = saved_flags;
    saved_ordinal = decoded.items[2].fields[1].source_ordinal;
    decoded.items[2].fields[1].source_ordinal =
        decoded.items[2].fields[0].source_ordinal;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 165u);
    decoded.items[2].fields[1].source_ordinal = saved_ordinal;
    saved_local = decoded.items[2].fields[1].type_local;
    decoded.items[2].fields[1].type_local = 3u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 166u);
    decoded.items[2].fields[1].type_local = saved_local;
    saved_local = decoded.types[4].item_local;
    decoded.types[4].item_local = 3u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 167u);
    decoded.types[4].item_local = saved_local;
    saved_byte = decoded.namespace_entries[4].namespace_kind;
    decoded.namespace_entries[4].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 168u);
    decoded.namespace_entries[4].namespace_kind = saved_byte;
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "bad-name", 169u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_ARTIFACT_FAILURE
        && result.library_status == CM_HIR_LIBRARY_INVALID_ARGUMENT);
    assert_context_lengths(&context, lengths);
    assert_artifact_identity_same(&artifact, &identity);

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

static void test_layout_wide_enum_reprs(void)
{
    static const uint8_t representations[2] = {
        CM_HIR_DECL_ENUM_REPR_U16, CM_HIR_DECL_ENUM_REPR_U32
    };
    static const uint64_t high_values[2] = {
        UINT64_C(1) << 15, UINT64_C(1) << 31
    };
    static const char *const attributes[2] = {
        "repr(u16)", "repr(u32)"
    };
    uint32_t index;

    for (index = 0u; index < 2u; ++index) {
        LayoutFixture fixture;
        CmHirDeclarationMaterializeExpectation expectation;
        CmHirDeclarationMaterializeResult result;
        CmHirContext context;
        CmHirLibraryArtifact artifact;
        const CmHirItem *alignment_enum;
        CmSourceId source = 189u + index;

        layout_fixture_init(&fixture);
        fixture.items[1].enum_repr_primitive = representations[index];
        fixture.variants[3].discriminant_low = high_values[index];
        assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
            == CM_HIR_DECL_METADATA_OK);
        expectation = expectation_for(&fixture.metadata);
        cm_hir_context_init(&context);
        cm_hir_library_artifact_init(&artifact);
        result = cm_hir_declaration_metadata_materialize(&context,
            &artifact, &fixture.metadata, &expectation, "dep", source);
        assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
            && result.item_count == 3u
            && result.public_type_entry_count == 4u
            && result.public_value_entry_count == 0u);
        alignment_enum = find_item(&context, CM_HIR_ITEM_ENUM,
            "AlignmentEnum");
        assert(alignment_enum != NULL
            && alignment_enum->visibility.kind == CM_HIR_VIS_PRIVATE
            && alignment_enum->data.enum_item.variant_count == 4u
            && alignment_enum->data.enum_item.variants[3]
                .discriminant.data.value.low_bits == high_values[index]);
        assert_item_attribute(&context, alignment_enum, 0u,
            attributes[index], source);
        cm_hir_library_artifact_destroy(&artifact);
        cm_hir_context_destroy(&context);
    }
}

static void test_layout_materialize_and_consume(void)
{
    LayoutFixture fixture;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirDeclarationMaterializeResult result;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirLibraryArtifactIdentity identity;
    CmHirLibraryBinding alignment_binding;
    CmHirLibraryBinding alignment_reexport;
    CmHirLibraryBinding layout_binding;
    CmHirLibraryBinding layout_reexport;
    CmHirLibraryPathSegment private_path[2];
    const CmHirItem *alignment;
    const CmHirItem *alignment_enum;
    const CmHirItem *layout;
    const CmHirType *type;
    ContextLengths lengths;
    uint64_t saved_low;
    uint64_t saved_high;
    uint32_t saved_local;
    uint8_t saved_byte;
    uint32_t index;

    layout_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
        == CM_HIR_DECL_METADATA_OK
        && encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

    expectation = expectation_for(&decoded);
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "dep", 191u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
        && result.item_count == 3u
        && result.public_type_entry_count == 4u
        && result.public_value_entry_count == 0u);

    alignment = find_item(&context, CM_HIR_ITEM_STRUCT, "Alignment");
    alignment_enum = find_item(&context, CM_HIR_ITEM_ENUM,
        "AlignmentEnum");
    layout = find_item(&context, CM_HIR_ITEM_STRUCT, "Layout");
    assert(alignment != NULL && alignment_enum != NULL && layout != NULL
        && alignment->visibility.kind == CM_HIR_VIS_PUBLIC
        && alignment->data.aggregate_item.form == CM_HIR_AGGREGATE_TUPLE
        && alignment->data.aggregate_item.field_count == 1u
        && alignment->data.aggregate_item.fields != NULL
        && alignment->data.aggregate_item.fields[0].name
            == CM_INTERN_ID_NONE
        && alignment->data.aggregate_item.fields[0].visibility.kind
            == CM_HIR_VIS_PRIVATE
        && alignment_enum->visibility.kind == CM_HIR_VIS_PRIVATE
        && alignment_enum->data.enum_item.variant_count == 4u
        && layout->visibility.kind == CM_HIR_VIS_PUBLIC
        && layout->data.aggregate_item.form == CM_HIR_AGGREGATE_NAMED
        && layout->data.aggregate_item.field_count == 2u);
    assert_item_attribute(&context, alignment, 0u, "repr(transparent)",
        191u);
    assert_item_attribute(&context, alignment_enum, 0u, "repr(u64)",
        191u);
    assert_item_attribute(&context, layout, 0u,
        "lang = \"alloc_layout\"", 191u);

    type = cm_hir_get_type(&context,
        alignment->data.aggregate_item.fields[0].type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(type->data.named_type.definition,
            alignment_enum->definition)
        && type->data.named_type.argument_count == 0u);
    type = cm_hir_get_type(&context,
        layout->data.aggregate_item.fields[0].type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_USIZE
        && layout->data.aggregate_item.fields[0].visibility.kind
            == CM_HIR_VIS_PRIVATE);
    type = cm_hir_get_type(&context,
        layout->data.aggregate_item.fields[1].type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(type->data.named_type.definition,
            alignment->definition)
        && layout->data.aggregate_item.fields[1].visibility.kind
            == CM_HIR_VIS_PRIVATE);

    for (index = 0u; index < 4u; ++index) {
        const CmHirVariant *variant =
            &alignment_enum->data.enum_item.variants[index];
        const CmHirType *discriminant = cm_hir_get_type(&context,
            variant->discriminant.type);
        uint64_t expected = index == 0u ? UINT64_C(1)
            : index == 1u ? UINT64_C(2)
            : index == 2u ? UINT64_C(4) : UINT64_C(1) << 63;
        assert(variant->form == CM_HIR_AGGREGATE_UNIT
            && variant->field_count == 0u && variant->fields == NULL
            && variant->has_discriminant
            && variant->discriminant.kind == CM_HIR_CONST_VALUE
            && variant->discriminant.data.value.low_bits == expected
            && variant->discriminant.data.value.high_bits == 0u
            && discriminant != NULL
            && discriminant->kind == CM_HIR_TYPE_INTEGER_KIND
            && discriminant->data.integer_type.kind == CM_HIR_INT_ISIZE);
    }

    alignment_binding = lookup_binding(&artifact, "Alignment");
    alignment_reexport = lookup_binding(&artifact, "AlignmentReexport");
    layout_binding = lookup_binding(&artifact, "Layout");
    layout_reexport = lookup_binding(&artifact, "LayoutReexport");
    assert(alignment_binding.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && alignment_reexport.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && layout_binding.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && layout_reexport.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && cm_hir_def_id_equal(alignment_binding.definition,
            alignment_reexport.definition)
        && cm_hir_def_id_equal(alignment_binding.definition,
            alignment->definition)
        && cm_hir_def_id_equal(layout_binding.definition,
            layout_reexport.definition)
        && cm_hir_def_id_equal(layout_binding.definition,
            layout->definition)
        && lookup_value_binding_status(&artifact, "Alignment")
            == CM_HIR_LIBRARY_NOT_FOUND
        && lookup_value_binding_status(&artifact, "Layout")
            == CM_HIR_LIBRARY_NOT_FOUND);
    private_path[0].bytes = (const unsigned char *)"dep";
    private_path[0].length = sizeof("dep") - 1u;
    private_path[1].bytes = (const unsigned char *)"AlignmentEnum";
    private_path[1].length = sizeof("AlignmentEnum") - 1u;
    assert(cm_hir_library_artifact_lookup_binding(&artifact, private_path,
        2u, &alignment_binding) == CM_HIR_LIBRARY_NOT_FOUND);
    test_layout_fresh_consumer(&context, &artifact, alignment->definition,
        layout->definition);

    lengths = context_lengths(&context);
    assert(cm_hir_library_artifact_identity(&artifact, &identity));

    saved_byte = decoded.items[0].aggregate_form;
    decoded.items[0].aggregate_form = CM_HIR_DECL_AGGREGATE_NAMED;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 192u);
    decoded.items[0].aggregate_form = saved_byte;

    decoded.items[0].fields[0].name =
        (CmHirDeclarationString)S("private_field");
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 193u);
    decoded.items[0].fields[0].name.data = NULL;
    decoded.items[0].fields[0].name.length = 0u;

    saved_byte = decoded.items[1].visibility.kind;
    decoded.items[1].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 194u);
    decoded.items[1].visibility.kind = saved_byte;

    saved_local = decoded.items[0].fields[0].type_local;
    decoded.items[0].fields[0].type_local = 1u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 195u);
    decoded.items[0].fields[0].type_local = saved_local;

    saved_local = decoded.namespace_entries[0].target_local;
    decoded.namespace_entries[0].target_local = 2u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 196u);
    decoded.namespace_entries[0].target_local = saved_local;

    saved_byte = decoded.items[1].enum_repr_primitive;
    decoded.items[1].enum_repr_primitive = CM_HIR_DECL_ENUM_REPR_U32;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 197u);
    decoded.items[1].enum_repr_primitive = saved_byte;

    saved_byte = decoded.items[1].variants[0].discriminant_primitive;
    decoded.items[1].variants[0].discriminant_primitive =
        CM_HIR_DECL_PRIMITIVE_U64;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 198u);
    decoded.items[1].variants[0].discriminant_primitive = saved_byte;

    saved_high = decoded.items[1].variants[0].discriminant_high;
    decoded.items[1].variants[0].discriminant_high = 1u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 199u);
    decoded.items[1].variants[0].discriminant_high = saved_high;

    saved_low = decoded.items[1].variants[1].discriminant_low;
    decoded.items[1].variants[1].discriminant_low =
        decoded.items[1].variants[0].discriminant_low;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 200u);
    decoded.items[1].variants[1].discriminant_low = saved_low;

    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "bad-name", 201u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_ARTIFACT_FAILURE
        && result.library_status == CM_HIR_LIBRARY_INVALID_ARGUMENT);
    assert_context_lengths(&context, lengths);
    assert_artifact_identity_same(&artifact, &identity);

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

static void test_type_id_materialize_and_consume(void)
{
    TypeIdFixture fixture;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirDeclarationMaterializeExpectation wrong;
    CmHirDeclarationMaterializeResult result;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirLibraryArtifactIdentity identity;
    CmHirLibraryBinding direct;
    CmHirLibraryBinding alias;
    const CmHirItem *item;
    const CmHirField *field;
    const CmHirType *array;
    const CmHirType *pointer;
    const CmHirType *unit;
    const CmHirType *length_type;
    ContextLengths lengths;
    uint64_t saved_bits;
    uint32_t saved_local;
    uint8_t saved_byte;

    type_id_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
        == CM_HIR_DECL_METADATA_OK
        && encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

    expectation = expectation_for(&decoded);
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "dep", 202u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
        && result.item_count == 1u
        && result.public_type_entry_count == 2u
        && result.public_value_entry_count == 0u);
    item = find_item(&context, CM_HIR_ITEM_STRUCT, "TypeIdLike");
    assert(item != NULL && item->visibility.kind == CM_HIR_VIS_PUBLIC
        && item->data.aggregate_item.form == CM_HIR_AGGREGATE_NAMED
        && item->data.aggregate_item.field_count == 1u
        && item->data.aggregate_item.fields != NULL
        && item->attribute_count == 1u);
    assert_item_attribute(&context, item, 0u, "lang = \"type_id\"",
        202u);
    field = &item->data.aggregate_item.fields[0];
    assert(field->visibility.kind == CM_HIR_VIS_CRATE
        && cm_hir_def_id_is_none(field->visibility.restriction));
    array = cm_hir_get_type(&context, field->type);
    assert(array != NULL && array->kind == CM_HIR_TYPE_ARRAY_KIND
        && array->data.array_type.length.kind == CM_HIR_CONST_VALUE
        && array->data.array_type.length.data.value.low_bits == 2u
        && array->data.array_type.length.data.value.high_bits == 0u);
    length_type = cm_hir_get_type(&context,
        array->data.array_type.length.type);
    pointer = cm_hir_get_type(&context, array->data.array_type.element);
    unit = pointer == NULL ? NULL : cm_hir_get_type(&context,
        pointer->data.raw_pointer_type.pointee);
    assert(length_type != NULL
        && length_type->kind == CM_HIR_TYPE_INTEGER_KIND
        && length_type->data.integer_type.kind == CM_HIR_INT_USIZE
        && pointer != NULL && pointer->kind == CM_HIR_TYPE_RAW_POINTER_KIND
        && pointer->data.raw_pointer_type.mutability == CM_HIR_IMMUTABLE
        && unit != NULL && unit->kind == CM_HIR_TYPE_UNIT_KIND);

    direct = lookup_binding(&artifact, "TypeIdLike");
    alias = lookup_binding(&artifact, "TypeIdAlias");
    assert(direct.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && alias.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && cm_hir_def_id_equal(direct.definition, item->definition)
        && cm_hir_def_id_equal(alias.definition, item->definition)
        && lookup_value_binding_status(&artifact, "TypeIdLike")
            == CM_HIR_LIBRARY_NOT_FOUND
        && lookup_value_binding_status(&artifact, "TypeIdAlias")
            == CM_HIR_LIBRARY_NOT_FOUND);
    test_type_id_fresh_consumer(&context, &artifact, item->definition);

    lengths = context_lengths(&context);
    assert(cm_hir_library_artifact_identity(&artifact, &identity));
    wrong = expectation;
    wrong.target_triple =
        (CmHirDeclarationString)S("i686-unknown-linux-gnu");
    assert_expectation_rejected(&context, &artifact, &decoded, &wrong,
        lengths, &identity);

    saved_local = decoded.items[0].fields[0].visibility.restriction_module;
    decoded.items[0].fields[0].visibility.restriction_module = 1u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 203u);
    decoded.items[0].fields[0].visibility.restriction_module = saved_local;

    saved_byte = decoded.items[0].fields[0].visibility.kind;
    decoded.items[0].fields[0].visibility.kind =
        CM_HIR_DECL_VISIBILITY_RESTRICTED;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 204u);
    decoded.items[0].fields[0].visibility.kind = saved_byte;

    saved_byte = decoded.types[2].mutability;
    decoded.types[2].mutability = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 205u);
    decoded.types[2].mutability = saved_byte;

    saved_local = decoded.types[3].array_length_type;
    decoded.types[3].array_length_type = 1u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 206u);
    decoded.types[3].array_length_type = saved_local;

    saved_bits = decoded.types[3].array_length_high_bits;
    decoded.types[3].array_length_high_bits = UINT64_C(1);
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 207u);
    decoded.types[3].array_length_high_bits = saved_bits;

    saved_local = decoded.items[0].fields[0].type_local;
    decoded.items[0].fields[0].type_local = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 208u);
    decoded.items[0].fields[0].type_local = saved_local;

    saved_byte = decoded.namespace_entries[1].namespace_kind;
    decoded.namespace_entries[1].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 209u);
    decoded.namespace_entries[1].namespace_kind = saved_byte;

    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "bad-name", 210u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_ARTIFACT_FAILURE
        && result.library_status == CM_HIR_LIBRARY_INVALID_ARGUMENT);
    assert_context_lengths(&context, lengths);
    assert_artifact_identity_same(&artifact, &identity);

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

static void test_type_name_materialize_and_consume(void)
{
    TypeNameFixture fixture;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirDeclarationMaterializeResult result;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirLibraryArtifactIdentity identity;
    CmHirLibraryBinding direct;
    CmHirLibraryBinding alias;
    CmHirLibraryPathSegment direct_path[2];
    CmHirLibraryPathSegment alias_path[2];
    CmHirLibraryValue direct_value;
    CmHirLibraryValue alias_value;
    const CmHirItem *item;
    const CmHirGenericParam *generic;
    const CmHirType *parameter_type;
    const CmHirType *parameter_pointee;
    const CmHirType *return_type;
    const CmHirType *pointee;
    const CmInternedString *abi;
    ContextLengths lengths;
    uint32_t saved_local;
    uint8_t saved_byte;

    type_name_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
        == CM_HIR_DECL_METADATA_OK
        && encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

    expectation = expectation_for(&decoded);
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "dep", 211u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
        && result.item_count == 0u
        && result.public_type_entry_count == 0u
        && result.public_value_entry_count == 2u);
    item = find_item(&context, CM_HIR_ITEM_FUNCTION, "name_of");
    generic = item == NULL || item->generic_parameter_count != 1u
        ? NULL : cm_hir_get_generic_param(&context,
            item->generic_parameter_start);
    parameter_type = item == NULL
            || item->data.function_item.signature.parameter_count != 1u
        ? NULL : cm_hir_get_type(&context,
            item->data.function_item.signature.parameters[0].type);
    parameter_pointee = parameter_type == NULL
            || parameter_type->kind != CM_HIR_TYPE_REFERENCE_KIND
        ? NULL : cm_hir_get_type(&context,
            parameter_type->data.reference_type.pointee);
    return_type = item == NULL ? NULL : cm_hir_get_type(&context,
        item->data.function_item.signature.return_type);
    pointee = return_type == NULL
            || return_type->kind != CM_HIR_TYPE_REFERENCE_KIND
        ? NULL : cm_hir_get_type(&context,
            return_type->data.reference_type.pointee);
    abi = item == NULL ? NULL : cm_interner_get(&context.strings,
        item->data.function_item.signature.abi);
    assert(item != NULL && item->visibility.kind == CM_HIR_VIS_PUBLIC
        && item->generic_parameter_count == 1u
        && item->predicate_count == 0u && item->predicates == NULL
        && item->attribute_count == 0u && item->attributes == NULL
        && item->data.function_item.signature.parameter_count == 1u
        && item->data.function_item.signature.parameters != NULL
        && item->data.function_item.signature.receiver == CM_HIR_RECEIVER_NONE
        && item->data.function_item.signature.safety == CM_HIR_SAFE
        && item->data.function_item.signature.is_const == 1
        && item->data.function_item.signature.is_async == 0
        && item->data.function_item.signature.is_variadic == 0
        && item->data.function_item.body == CM_HIR_BODY_NONE
        && item->data.function_item.has_default_body == 0
        && generic != NULL && generic->kind == CM_HIR_GENERIC_TYPE
        && generic->index == 0u && generic->is_relaxed_sized == 1
        && cm_hir_def_id_equal(generic->owner, item->definition)
        && parameter_type != NULL
        && parameter_type->data.reference_type.mutability
            == CM_HIR_IMMUTABLE
        && parameter_type->data.reference_type.region.kind
            == CM_HIR_REGION_ERASED
        && parameter_pointee != NULL
        && parameter_pointee->kind == CM_HIR_TYPE_PARAMETER_KIND
        && parameter_pointee->data.parameter_type.parameter
            == item->generic_parameter_start
        && return_type != NULL
        && return_type->data.reference_type.mutability == CM_HIR_IMMUTABLE
        && return_type->data.reference_type.region.kind
            == CM_HIR_REGION_STATIC
        && pointee != NULL && pointee->kind == CM_HIR_TYPE_STR_KIND
        && abi != NULL && abi->len == sizeof("Rust") - 1u
        && memcmp(abi->bytes, "Rust", sizeof("Rust") - 1u) == 0);

    direct = lookup_value_binding(&artifact, "name_of");
    alias = lookup_value_binding(&artifact, "name_alias");
    assert(direct.kind == CM_HIR_LIBRARY_BINDING_VALUE
        && alias.kind == CM_HIR_LIBRARY_BINDING_VALUE
        && cm_hir_def_id_equal(direct.definition, item->definition)
        && cm_hir_def_id_equal(alias.definition, item->definition));
    direct_path[0].bytes = (const unsigned char *)"dep";
    direct_path[0].length = sizeof("dep") - 1u;
    direct_path[1].bytes = (const unsigned char *)"name_of";
    direct_path[1].length = sizeof("name_of") - 1u;
    alias_path[0] = direct_path[0];
    alias_path[1].bytes = (const unsigned char *)"name_alias";
    alias_path[1].length = sizeof("name_alias") - 1u;
    memset(&direct_value, 0, sizeof(direct_value));
    memset(&alias_value, 0, sizeof(alias_value));
    assert(cm_hir_library_artifact_lookup_value(&artifact, direct_path, 2u,
            &direct_value) == CM_HIR_LIBRARY_OK
        && cm_hir_library_artifact_lookup_value(&artifact, alias_path, 2u,
            &alias_value) == CM_HIR_LIBRARY_OK
        && direct_value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        && alias_value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        && direct_value.data.function.generic_parameter_count == 1u
        && alias_value.data.function.generic_parameter_count == 1u
        && direct_value.data.function.predicate_count == 0u
        && alias_value.data.function.predicate_count == 0u
        && direct_value.data.function.parameter_count == 1u
        && alias_value.data.function.parameter_count == 1u
        && direct_value.data.function.parameter_types != NULL
        && alias_value.data.function.parameter_types != NULL
        && direct_value.data.function.parameter_types[0]
            == item->data.function_item.signature.parameters[0].type
        && alias_value.data.function.parameter_types[0]
            == item->data.function_item.signature.parameters[0].type
        && direct_value.data.function.is_const == 1
        && alias_value.data.function.is_const == 1
        && direct_value.data.function.return_type
            == item->data.function_item.signature.return_type
        && alias_value.data.function.return_type
            == item->data.function_item.signature.return_type
        && cm_hir_def_id_equal(direct_value.definition, item->definition)
        && cm_hir_def_id_equal(alias_value.definition, item->definition));
    test_type_name_fresh_consumer(&context, &artifact);

    lengths = context_lengths(&context);
    assert(cm_hir_library_artifact_identity(&artifact, &identity));
    saved_byte = decoded.values[0].is_const;
    decoded.values[0].is_const = 2u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 212u);
    decoded.values[0].is_const = saved_byte;

    saved_byte = decoded.generics[0].is_relaxed_sized;
    decoded.generics[0].is_relaxed_sized = 2u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 213u);
    decoded.generics[0].is_relaxed_sized = saved_byte;

    saved_local = decoded.generics[0].owner_local;
    decoded.generics[0].owner_local = 2u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 214u);
    decoded.generics[0].owner_local = saved_local;

    saved_local = decoded.values[0].return_type;
    decoded.values[0].return_type = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 215u);
    decoded.values[0].return_type = saved_local;

    saved_byte = decoded.types[3].mutability;
    decoded.types[3].mutability = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 216u);
    decoded.types[3].mutability = saved_byte;

    saved_byte = decoded.values[0].has_body;
    decoded.values[0].has_body = 2u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 217u);
    decoded.values[0].has_body = saved_byte;

    saved_local = decoded.namespace_entries[0].target_local;
    decoded.namespace_entries[0].target_local = 2u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 218u);
    decoded.namespace_entries[0].target_local = saved_local;

    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "bad-name", 219u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_ARTIFACT_FAILURE
        && result.library_status == CM_HIR_LIBRARY_INVALID_ARGUMENT);
    assert_context_lengths(&context, lengths);
    assert_artifact_identity_same(&artifact, &identity);

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

static void assert_enum_variant_path(const CmHirLibraryArtifact *artifact,
    const char *enum_name, const char *variant_name,
    CmHirDefId enum_definition, CmHirDefId variant_definition,
    uint32_t variant_index)
{
    CmHirLibraryPathSegment path[3];
    CmHirLibraryType type;
    CmHirLibraryBinding binding;
    CmHirLibraryValue value;

    path[0].bytes = (const unsigned char *)"dep";
    path[0].length = sizeof("dep") - 1u;
    path[1].bytes = (const unsigned char *)enum_name;
    path[1].length = strlen(enum_name);
    path[2].bytes = (const unsigned char *)variant_name;
    path[2].length = strlen(variant_name);
    memset(&type, 0, sizeof(type));
    assert(cm_hir_library_artifact_lookup_type(artifact, path, 3u, &type)
        == CM_HIR_LIBRARY_OK);
    assert(type.binding_kind == CM_HIR_LIBRARY_BINDING_ENUM_VARIANT
        && type.kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(type.definition, variant_definition)
        && cm_hir_def_id_equal(type.enum_definition, enum_definition)
        && type.enum_variant_index == variant_index
        && type.enum_variant_form == CM_HIR_AGGREGATE_UNIT);
    memset(&binding, 0, sizeof(binding));
    assert(cm_hir_library_artifact_lookup_value_binding(artifact, path, 3u,
        &binding) == CM_HIR_LIBRARY_OK);
    assert(binding.kind == CM_HIR_LIBRARY_BINDING_ENUM_VARIANT
        && cm_hir_def_id_equal(binding.definition, variant_definition)
        && cm_hir_def_id_equal(binding.enum_definition, enum_definition)
        && binding.enum_variant_index == variant_index
        && binding.enum_variant_form == CM_HIR_AGGREGATE_UNIT);
    memset(&value, 0, sizeof(value));
    assert(cm_hir_library_artifact_lookup_value(artifact, path, 3u, &value)
        == CM_HIR_LIBRARY_WRONG_NAMESPACE);
}

static void assert_enum_variant_alias(const CmHirLibraryArtifact *artifact,
    const char *name, CmHirDefId enum_definition,
    CmHirDefId variant_definition, uint32_t variant_index)
{
    CmHirLibraryPathSegment path[2];
    CmHirLibraryType type;
    CmHirLibraryBinding binding;
    CmHirLibraryValue value;

    path[0].bytes = (const unsigned char *)"dep";
    path[0].length = sizeof("dep") - 1u;
    path[1].bytes = (const unsigned char *)name;
    path[1].length = strlen(name);
    memset(&type, 0, sizeof(type));
    assert(cm_hir_library_artifact_lookup_type(artifact, path, 2u, &type)
        == CM_HIR_LIBRARY_OK);
    assert(type.binding_kind == CM_HIR_LIBRARY_BINDING_ENUM_VARIANT
        && type.kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(type.definition, variant_definition)
        && cm_hir_def_id_equal(type.enum_definition, enum_definition)
        && type.enum_variant_index == variant_index
        && type.enum_variant_form == CM_HIR_AGGREGATE_UNIT);
    memset(&binding, 0, sizeof(binding));
    assert(cm_hir_library_artifact_lookup_value_binding(artifact, path, 2u,
        &binding) == CM_HIR_LIBRARY_OK);
    assert(binding.kind == CM_HIR_LIBRARY_BINDING_ENUM_VARIANT
        && cm_hir_def_id_equal(binding.definition, variant_definition)
        && cm_hir_def_id_equal(binding.enum_definition, enum_definition)
        && binding.enum_variant_index == variant_index
        && binding.enum_variant_form == CM_HIR_AGGREGATE_UNIT
        && binding.enum_variant_namespace
            == CM_HIR_LIBRARY_ENUM_VARIANT_VALUE);
    memset(&value, 0, sizeof(value));
    assert(cm_hir_library_artifact_lookup_value(artifact, path, 2u, &value)
        == CM_HIR_LIBRARY_WRONG_NAMESPACE);
}

static void test_default_enum_fresh_consumer(CmHirContext *context,
    const CmHirLibraryArtifact *artifact, CmHirDefId abi,
    CmHirDefId cleanup)
{
    static const unsigned char source_text[] =
        "use dep::UnwindTerminateReason::Abi;\n"
        "use dep::ReasonAbi;\n"
        "use dep::ReasonInCleanup;\n";
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    const CmHirLibraryArtifact *libraries[1];
    const CmHirModule *root;
    uint32_t import_index;
    uint32_t type_count;
    uint32_t value_count;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_cfg_set_init(&cfg);
    cm_import_resolver_init(&imports);
    cm_hir_module_map_init(&map);
    assert(cm_source_add_memory(&sources, "variant_consumer.rs", source_text,
        sizeof(source_text) - 1u, &root_source) == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    assert(graph_result.error_count == 0u);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    assert(import_result.revision == graph_result.revision);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "variant_consumer";
    libraries[0] = artifact;
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    lower_result = cm_hir_lower_module_graph(context, &graph,
        graph_result.revision, &imports, &map, &lower_options);
    if (lower_result.error_count != 0u) {
        fprintf(stderr, "default enum consumer: %s: %s\n",
            cm_hir_lower_error_kind_name(lower_result.first_error.kind),
            lower_result.first_error.message);
    }
    assert(lower_result.error_count == 0u);
    root = cm_hir_get_module(context, lower_result.root_module);
    assert(root != NULL && root->import_count == 3u);
    type_count = 0u;
    value_count = 0u;
    for (import_index = 0u; import_index < root->import_count;
            ++import_index) {
        const CmHirImport *import_value = &root->imports[import_index];
        uint32_t binding_index;

        assert(import_value->binding_count == 2u
            && import_value->bindings != NULL);
        for (binding_index = 0u;
                binding_index < import_value->binding_count;
                ++binding_index) {
            const CmHirImportBinding *binding =
                &import_value->bindings[binding_index];
            const CmInternedString *name = cm_interner_get(
                &context->strings, binding->name);
            int is_abi;

            assert(name != NULL);
            is_abi = (name->len == sizeof("Abi") - 1u
                    && memcmp(name->bytes, "Abi", name->len) == 0)
                || (name->len == sizeof("ReasonAbi") - 1u
                    && memcmp(name->bytes, "ReasonAbi", name->len) == 0);
            assert(is_abi
                ? cm_hir_def_id_equal(binding->target, abi)
                : (name->len == sizeof("ReasonInCleanup") - 1u
                    && memcmp(name->bytes, "ReasonInCleanup", name->len)
                        == 0
                    && cm_hir_def_id_equal(binding->target, cleanup)));
            if (binding->namespace_kind == CM_HIR_NAMESPACE_TYPE)
                type_count += 1u;
            else {
                assert(binding->namespace_kind == CM_HIR_NAMESPACE_VALUE);
                value_count += 1u;
            }
        }
    }
    assert(type_count == 3u && value_count == 3u);
    cm_hir_module_map_destroy(&map);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_enum_materialize_and_restore_scope(void)
{
    EnumFixture fixture;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirDeclarationMaterializeResult result;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirLibraryArtifactIdentity identity;
    CmHirLibraryBinding direct;
    CmHirLibraryBinding reexport;
    const CmHirItem *enumeration;
    const CmHirDefinition *enum_definition;
    const CmInternedString *attribute_text;
    ContextLengths lengths;
    uint64_t saved_low;
    uint64_t saved_high;
    uint32_t saved_count;
    uint32_t saved_ordinal;
    uint8_t saved_kind;
    uint8_t saved_primitive;
    uint8_t saved_namespace;
    uint32_t index;

    enum_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
        == CM_HIR_DECL_METADATA_OK);
    assert(encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

    expectation = expectation_for(&decoded);
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "dep", 131u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
        && result.module_count == 1u && result.item_count == 1u
        && result.public_type_entry_count == 2u
        && result.public_value_entry_count == 0u);
    direct = lookup_binding(&artifact, "Char");
    reexport = lookup_binding(&artifact, "CharReexport");
    assert(direct.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && direct.type_kind == CM_HIR_TYPE_ADT_KIND
        && reexport.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && reexport.type_kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(direct.definition, reexport.definition)
        && lookup_value_binding_status(&artifact, "Char")
            == CM_HIR_LIBRARY_NOT_FOUND
        && lookup_value_binding_status(&artifact, "CharReexport")
            == CM_HIR_LIBRARY_NOT_FOUND);

    enumeration = find_item(&context, CM_HIR_ITEM_ENUM, "Char");
    enum_definition = enumeration == NULL ? NULL
        : cm_hir_lookup_definition(&context, enumeration->definition);
    attribute_text = enumeration == NULL || enumeration->attribute_count != 1u
        ? NULL : cm_interner_get(&context.strings,
            enumeration->attributes[0].metadata);
    assert(enumeration != NULL && enum_definition != NULL
        && enum_definition->state == CM_HIR_DEFINITION_BOUND
        && enum_definition->kind == CM_HIR_DEFINITION_ITEM
        && enum_definition->has_reserved_item_kind
        && enum_definition->reserved_item_kind == CM_HIR_ITEM_ENUM
        && cm_hir_def_id_equal(enumeration->definition, direct.definition)
        && enumeration->generic_parameter_count == 0u
        && enumeration->predicate_count == 0u
        && enumeration->attribute_count == 1u
        && enumeration->attributes != NULL && attribute_text != NULL
        && attribute_text->len == sizeof("repr(u8)") - 1u
        && memcmp(attribute_text->bytes, "repr(u8)",
            sizeof("repr(u8)") - 1u) == 0
        && enumeration->attributes[0].span.source == 131u
        && enumeration->attributes[0].span.start == 0u
        && enumeration->attributes[0].span.end == 1u
        && enumeration->attributes[0].source_attribute == 1u
        && enumeration->attributes[0].expansion_depth == 0u
        && enumeration->data.enum_item.variant_count == 2u
        && enumeration->data.enum_item.variants != NULL);
    for (index = 0u; index < 2u; ++index) {
        const CmHirVariant *variant =
            &enumeration->data.enum_item.variants[index];
        const CmHirDefinition *variant_definition =
            cm_hir_lookup_definition(&context, variant->definition);
        const CmHirType *discriminant = cm_hir_get_type(&context,
            variant->discriminant.type);
        const CmInternedString *name = cm_interner_get(&context.strings,
            variant->name);
        assert(variant_definition != NULL
            && variant_definition->kind == CM_HIR_DEFINITION_ENUM_VARIANT
            && variant_definition->state == CM_HIR_DEFINITION_BOUND
            && variant_definition->entity.enum_variant.enum_item_id
                == enum_definition->entity.item_id
            && variant_definition->entity.enum_variant.variant_index == index
            && variant->form == CM_HIR_AGGREGATE_UNIT
            && variant->field_count == 0u && variant->fields == NULL
            && variant->has_discriminant
            && variant->discriminant.kind == CM_HIR_CONST_VALUE
            && variant->discriminant.data.value.low_bits
                == (index == 0u ? 0u : 255u)
            && variant->discriminant.data.value.high_bits == 0u
            && discriminant != NULL
            && discriminant->kind == CM_HIR_TYPE_INTEGER_KIND
            && discriminant->data.integer_type.kind == CM_HIR_INT_ISIZE
            && discriminant->span.source == 131u
            && discriminant->span.start == index + 2u
            && name != NULL);
    }
    assert_enum_variant_path(&artifact, "Char", "Null",
        enumeration->definition,
        enumeration->data.enum_item.variants[0].definition, 0u);
    assert_enum_variant_path(&artifact, "CharReexport", "Null",
        enumeration->definition,
        enumeration->data.enum_item.variants[0].definition, 0u);

    lengths = context_lengths(&context);
    assert(cm_hir_library_artifact_identity(&artifact, &identity));

    saved_primitive = decoded.items[0].enum_repr_primitive;
    decoded.items[0].enum_repr_primitive = CM_HIR_DECL_PRIMITIVE_ISIZE;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 132u);
    decoded.items[0].enum_repr_primitive = saved_primitive;

    saved_kind = decoded.items[0].variants[0].kind;
    decoded.items[0].variants[0].kind = UINT8_C(2);
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 133u);
    decoded.items[0].variants[0].kind = saved_kind;

    saved_primitive = decoded.items[0].variants[0].discriminant_primitive;
    decoded.items[0].variants[0].discriminant_primitive =
        CM_HIR_DECL_PRIMITIVE_U8;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 134u);
    decoded.items[0].variants[0].discriminant_primitive = saved_primitive;

    saved_high = decoded.items[0].variants[0].discriminant_high;
    decoded.items[0].variants[0].discriminant_high = 1u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 135u);
    decoded.items[0].variants[0].discriminant_high = saved_high;

    saved_low = decoded.items[0].variants[1].discriminant_low;
    decoded.items[0].variants[1].discriminant_low = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 136u);
    decoded.items[0].variants[1].discriminant_low = saved_low;

    saved_ordinal = decoded.items[0].variants[1].source_ordinal;
    decoded.items[0].variants[1].source_ordinal =
        decoded.items[0].variants[0].source_ordinal;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 137u);
    decoded.items[0].variants[1].source_ordinal = saved_ordinal;

    saved_count = decoded.items[0].variant_count;
    decoded.items[0].variant_count = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 138u);
    decoded.items[0].variant_count = saved_count;

    saved_namespace = decoded.namespace_entries[0].namespace_kind;
    decoded.namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 139u);
    decoded.namespace_entries[0].namespace_kind = saved_namespace;

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

static void test_default_enum_materialize_and_variant_reexports(void)
{
    DefaultEnumFixture fixture;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirDeclarationMaterializeResult result;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirLibraryArtifactIdentity identity;
    CmHirLibraryBinding enum_binding;
    const CmHirItem *basic_block;
    const CmHirItem *enumeration;
    const CmHirDefinition *enum_definition;
    const CmInternedString *attribute_text;
    ContextLengths lengths;
    uint32_t index;
    uint32_t saved_count;
    uint32_t saved_local;
    uint8_t saved_byte;
    CmHirDeclarationString saved_string;

    default_enum_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
        == CM_HIR_DECL_METADATA_OK
        && encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

    expectation = expectation_for(&decoded);
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "dep", 151u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
        && result.module_count == 1u && result.item_count == 2u
        && result.public_type_entry_count == 4u
        && result.public_value_entry_count == 2u);
    enum_binding = lookup_binding(&artifact, "UnwindTerminateReason");
    basic_block = find_item(&context, CM_HIR_ITEM_ENUM, "BasicBlock");
    enumeration = find_item(&context, CM_HIR_ITEM_ENUM,
        "UnwindTerminateReason");
    enum_definition = enumeration == NULL ? NULL
        : cm_hir_lookup_definition(&context, enumeration->definition);
    attribute_text = enumeration == NULL || enumeration->attribute_count != 1u
        ? NULL : cm_interner_get(&context.strings,
            enumeration->attributes[0].metadata);
    assert(basic_block != NULL && basic_block->attribute_count == 1u
        && basic_block->data.enum_item.variant_count == 2u
        && basic_block->attributes != NULL
        && basic_block->attributes[0].span.source == 151u
        && basic_block->attributes[0].span.start == 0u
        && basic_block->attributes[0].span.end == 1u
        && basic_block->attributes[0].source_attribute == 1u
        && basic_block->attributes[0].expansion_depth == 0u
        && enumeration != NULL && enum_definition != NULL
        && enum_binding.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && enum_binding.type_kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(enum_binding.definition,
            enumeration->definition)
        && enumeration->attribute_count == 1u
        && enumeration->attributes != NULL && attribute_text != NULL
        && attribute_text->len
            == sizeof("rustc_diagnostic_item = \"mir_unwind_terminate_reason\"")
                - 1u
        && memcmp(attribute_text->bytes,
            "rustc_diagnostic_item = \"mir_unwind_terminate_reason\"",
            attribute_text->len) == 0
        && enumeration->attributes[0].span.source == 151u
        && enumeration->attributes[0].span.start == 3u
        && enumeration->attributes[0].span.end == 4u
        && enumeration->attributes[0].source_attribute == 1u
        && enumeration->attributes[0].expansion_depth == 0u
        && enumeration->data.enum_item.variant_count == 2u
        && enumeration->data.enum_item.variants != NULL);
    for (index = 0u; index < 2u; ++index) {
        const CmHirVariant *variant =
            &enumeration->data.enum_item.variants[index];
        const CmHirDefinition *definition = cm_hir_lookup_definition(
            &context, variant->definition);
        assert(definition != NULL
            && definition->kind == CM_HIR_DEFINITION_ENUM_VARIANT
            && definition->state == CM_HIR_DEFINITION_BOUND
            && definition->entity.enum_variant.enum_item_id
                == enum_definition->entity.item_id
            && definition->entity.enum_variant.variant_index == index
            && variant->form == CM_HIR_AGGREGATE_UNIT
            && variant->fields == NULL && variant->field_count == 0u
            && !variant->has_discriminant
            && variant->discriminant.kind == CM_HIR_CONST_VALUE
            && variant->discriminant.type == CM_HIR_TYPE_NONE
            && variant->discriminant.data.value.low_bits == 0u
            && variant->discriminant.data.value.high_bits == 0u);
    }
    assert_enum_variant_path(&artifact, "BasicBlock", "Normal",
        basic_block->definition,
        basic_block->data.enum_item.variants[0].definition, 0u);
    assert_enum_variant_path(&artifact, "UnwindTerminateReason", "Abi",
        enumeration->definition,
        enumeration->data.enum_item.variants[0].definition, 0u);
    assert_enum_variant_path(&artifact, "UnwindTerminateReason",
        "InCleanup", enumeration->definition,
        enumeration->data.enum_item.variants[1].definition, 1u);
    assert_enum_variant_alias(&artifact, "ReasonAbi",
        enumeration->definition,
        enumeration->data.enum_item.variants[0].definition, 0u);
    assert_enum_variant_alias(&artifact, "ReasonInCleanup",
        enumeration->definition,
        enumeration->data.enum_item.variants[1].definition, 1u);
    test_default_enum_fresh_consumer(&context, &artifact,
        enumeration->data.enum_item.variants[0].definition,
        enumeration->data.enum_item.variants[1].definition);

    lengths = context_lengths(&context);
    assert(cm_hir_library_artifact_identity(&artifact, &identity));

    saved_string = decoded.items[1].diagnostic_item;
    decoded.items[1].diagnostic_item = (CmHirDeclarationString)S("");
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 152u);
    decoded.items[1].diagnostic_item = saved_string;

    saved_byte = decoded.items[1].enum_repr_primitive;
    decoded.items[1].enum_repr_primitive = CM_HIR_DECL_PRIMITIVE_U8;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 153u);
    decoded.items[1].enum_repr_primitive = saved_byte;

    saved_byte = decoded.items[1].variants[0].discriminant_primitive;
    decoded.items[1].variants[0].discriminant_primitive =
        CM_HIR_DECL_PRIMITIVE_ISIZE;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 154u);
    decoded.items[1].variants[0].discriminant_primitive = saved_byte;

    saved_local = decoded.namespace_entries[1].target_local;
    decoded.namespace_entries[1].target_local = 5u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 155u);
    decoded.namespace_entries[1].target_local = saved_local;

    saved_byte = decoded.namespace_entries[1].target_kind;
    decoded.namespace_entries[1].target_kind = CM_HIR_DECL_TARGET_ITEM;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 156u);
    decoded.namespace_entries[1].target_kind = saved_byte;

    saved_count = (uint32_t)decoded.namespace_count;
    decoded.namespace_count = 5u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 157u);
    decoded.namespace_count = saved_count;

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

static void test_option_fresh_consumer(CmHirContext *context,
    const CmHirLibraryArtifact *artifact, CmHirDefId option,
    CmHirDefId some)
{
    static const unsigned char source_text[] =
        "use dep::Some;\n"
        "use dep::Option::Some as DirectSome;\n"
        "fn consume(x: dep::Option<u8>) -> dep::OptionAlias<u8> { x }\n";
    CmSourceSet sources;
    CmSourceId source;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    const CmHirLibraryArtifact *libraries[1];
    const CmHirModule *root;
    const CmHirItem *function;
    const CmHirType *parameter;
    const CmHirType *result_type;
    uint32_t import_index;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_cfg_set_init(&cfg);
    cm_import_resolver_init(&imports);
    cm_hir_module_map_init(&map);
    assert(cm_source_add_memory(&sources, "option_consumer.rs", source_text,
        sizeof(source_text) - 1u, &source) == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, source,
        &graph_options);
    assert(graph_result.error_count == 0u);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    assert(import_result.revision == graph_result.revision);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "option_consumer";
    libraries[0] = artifact;
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    lower_result = cm_hir_lower_module_graph(context, &graph,
        graph_result.revision, &imports, &map, &lower_options);
    if (lower_result.error_count != 0u) {
        fprintf(stderr, "Option consumer: %s: %s\n",
            cm_hir_lower_error_kind_name(lower_result.first_error.kind),
            lower_result.first_error.message);
    }
    assert(lower_result.error_count == 0u);
    root = cm_hir_get_module(context, lower_result.root_module);
    assert(root != NULL && root->import_count == 2u);
    for (import_index = 0u; import_index < root->import_count;
            ++import_index) {
        const CmHirImport *import_value = &root->imports[import_index];
        uint32_t binding_index;
        assert(import_value->binding_count == 2u);
        for (binding_index = 0u;
             binding_index < import_value->binding_count; ++binding_index) {
            assert(cm_hir_def_id_equal(
                import_value->bindings[binding_index].target, some));
        }
    }
    function = find_item(context, CM_HIR_ITEM_FUNCTION, "consume");
    parameter = function == NULL ? NULL : cm_hir_get_type(context,
        function->data.function_item.signature.parameters[0].type);
    result_type = function == NULL ? NULL : cm_hir_get_type(context,
        function->data.function_item.signature.return_type);
    assert(function != NULL
        && function->data.function_item.signature.parameter_count == 1u
        && parameter != NULL && parameter->kind == CM_HIR_TYPE_ADT_KIND
        && result_type != NULL && result_type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(parameter->data.named_type.definition, option)
        && cm_hir_def_id_equal(result_type->data.named_type.definition,
            option)
        && parameter->data.named_type.argument_count == 1u
        && result_type->data.named_type.argument_count == 1u);
    cm_hir_module_map_destroy(&map);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_into_iter_fresh_consumer(CmHirContext *context,
    const CmHirLibraryArtifact *artifact, CmHirDefId into_iter)
{
    static const unsigned char source_text[] =
        "fn consume(x: dep::IntoIter<u8, 4>) "
        "-> dep::IntoIterAlias<u8, 4> { x }\n";
    CmSourceSet sources;
    CmSourceId source;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    const CmHirLibraryArtifact *libraries[1];
    const CmHirItem *function;
    const CmHirType *parameter;
    const CmHirType *result_type;
    const CmHirConstArg *length;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_cfg_set_init(&cfg);
    cm_import_resolver_init(&imports);
    cm_hir_module_map_init(&map);
    assert(cm_source_add_memory(&sources, "intoiter_consumer.rs",
        source_text, sizeof(source_text) - 1u, &source) == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, source,
        &graph_options);
    assert(graph_result.error_count == 0u);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    assert(import_result.revision == graph_result.revision);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "intoiter_consumer";
    libraries[0] = artifact;
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    lower_result = cm_hir_lower_module_graph(context, &graph,
        graph_result.revision, &imports, &map, &lower_options);
    if (lower_result.error_count != 0u) {
        fprintf(stderr, "IntoIter consumer: %s: %s\n",
            cm_hir_lower_error_kind_name(lower_result.first_error.kind),
            lower_result.first_error.message);
    }
    assert(lower_result.error_count == 0u);
    function = find_item(context, CM_HIR_ITEM_FUNCTION, "consume");
    parameter = function == NULL ? NULL : cm_hir_get_type(context,
        function->data.function_item.signature.parameters[0].type);
    result_type = function == NULL ? NULL : cm_hir_get_type(context,
        function->data.function_item.signature.return_type);
    assert(function != NULL
        && function->data.function_item.signature.parameter_count == 1u
        && parameter != NULL && parameter->kind == CM_HIR_TYPE_ADT_KIND
        && result_type != NULL && result_type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(parameter->data.named_type.definition,
            into_iter)
        && cm_hir_def_id_equal(result_type->data.named_type.definition,
            into_iter)
        && parameter->data.named_type.argument_count == 2u
        && result_type->data.named_type.argument_count == 2u
        && parameter->data.named_type.arguments[0].kind
            == CM_HIR_GENERIC_ARG_TYPE
        && parameter->data.named_type.arguments[1].kind
            == CM_HIR_GENERIC_ARG_CONST
        && result_type->data.named_type.arguments[0].kind
            == CM_HIR_GENERIC_ARG_TYPE
        && result_type->data.named_type.arguments[1].kind
            == CM_HIR_GENERIC_ARG_CONST);
    length = parameter == NULL ? NULL
        : &parameter->data.named_type.arguments[1].data.constant;
    assert(length != NULL && length->kind == CM_HIR_CONST_VALUE
        && length->data.value.low_bits == UINT64_C(4)
        && length->data.value.high_bits == UINT64_C(0));
    length = result_type == NULL ? NULL
        : &result_type->data.named_type.arguments[1].data.constant;
    assert(length != NULL && length->kind == CM_HIR_CONST_VALUE
        && length->data.value.low_bits == UINT64_C(4)
        && length->data.value.high_bits == UINT64_C(0));
    cm_hir_module_map_destroy(&map);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_option_tuple_materialize_and_consume(void)
{
    OptionFixture fixture;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirDeclarationMaterializeResult result;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirLibraryArtifactIdentity identity;
    const CmHirItem *option;
    const CmHirItem *witness;
    const CmHirGenericParam *generic;
    const CmHirType *field_type;
    const CmHirType *signature_type;
    const CmInternedString *text;
    CmHirLibraryPathSegment path[3];
    CmHirLibraryType type;
    CmHirLibraryBinding binding;
    ContextLengths lengths;
    uint32_t saved_local;
    uint32_t saved_count;
    uint16_t saved_flags;
    CmHirDeclarationString saved_string;

    option_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
        == CM_HIR_DECL_METADATA_OK && encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);
    expectation = expectation_for(&decoded);
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "dep", 171u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
        && result.item_count == 1u
        && result.public_type_entry_count == 4u
        && result.public_value_entry_count == 3u);
    option = find_item(&context, CM_HIR_ITEM_ENUM, "Option");
    witness = find_item(&context, CM_HIR_ITEM_STATIC, "WITNESS");
    generic = option == NULL ? NULL : cm_hir_get_generic_param(&context,
        option->generic_parameter_start);
    assert(option != NULL && witness != NULL && generic != NULL
        && option->generic_parameter_count == 1u
        && cm_hir_def_id_equal(generic->owner, option->definition)
        && option->attribute_count == 2u
        && option->data.enum_item.variant_count == 2u
        && option->data.enum_item.variants[0].form
            == CM_HIR_AGGREGATE_UNIT
        && option->data.enum_item.variants[1].form
            == CM_HIR_AGGREGATE_TUPLE
        && option->data.enum_item.variants[1].field_count == 1u
        && option->data.enum_item.variants[1].fields != NULL);
    text = cm_interner_get(&context.strings, option->attributes[0].metadata);
    assert(text != NULL
        && text->len == sizeof("rustc_diagnostic_item = \"Option\"") - 1u
        && memcmp(text->bytes, "rustc_diagnostic_item = \"Option\"",
            text->len) == 0);
    text = cm_interner_get(&context.strings, option->attributes[1].metadata);
    assert(text != NULL && text->len == sizeof("lang = \"Option\"") - 1u
        && memcmp(text->bytes, "lang = \"Option\"", text->len) == 0
        && option->attributes[0].source_attribute == 1u
        && option->attributes[1].source_attribute == 2u);
    text = cm_interner_get(&context.strings,
        option->data.enum_item.variants[0].lang_item);
    assert(text != NULL && text->len == sizeof("None") - 1u
        && memcmp(text->bytes, "None", text->len) == 0);
    text = cm_interner_get(&context.strings,
        option->data.enum_item.variants[1].lang_item);
    assert(text != NULL && text->len == sizeof("Some") - 1u
        && memcmp(text->bytes, "Some", text->len) == 0);
    field_type = cm_hir_get_type(&context,
        option->data.enum_item.variants[1].fields[0].type);
    assert(field_type != NULL
        && field_type->kind == CM_HIR_TYPE_PARAMETER_KIND
        && field_type->data.parameter_type.parameter
            == option->generic_parameter_start
        && option->data.enum_item.variants[1].fields[0].visibility.kind
            == CM_HIR_VIS_PRIVATE);
    signature_type = cm_hir_get_type(&context,
        witness->data.value_item.type);
    assert(signature_type != NULL
        && signature_type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(signature_type->data.named_type.definition,
            option->definition)
        && signature_type->data.named_type.argument_count == 1u);

    path[0].bytes = (const unsigned char *)"dep";
    path[0].length = sizeof("dep") - 1u;
    path[1].bytes = (const unsigned char *)"OptionAlias";
    path[1].length = sizeof("OptionAlias") - 1u;
    path[2].bytes = (const unsigned char *)"Some";
    path[2].length = sizeof("Some") - 1u;
    memset(&type, 0, sizeof(type));
    assert(cm_hir_library_artifact_lookup_type(&artifact, path, 3u, &type)
        == CM_HIR_LIBRARY_OK
        && type.binding_kind == CM_HIR_LIBRARY_BINDING_ENUM_VARIANT
        && type.enum_variant_form == CM_HIR_AGGREGATE_TUPLE
        && cm_hir_def_id_equal(type.definition,
            option->data.enum_item.variants[1].definition));
    memset(&binding, 0, sizeof(binding));
    assert(cm_hir_library_artifact_lookup_value_binding(&artifact, path, 3u,
        &binding) == CM_HIR_LIBRARY_OK
        && binding.kind == CM_HIR_LIBRARY_BINDING_ENUM_VARIANT
        && binding.enum_variant_form == CM_HIR_AGGREGATE_TUPLE
        && cm_hir_def_id_equal(binding.definition, type.definition));
    test_option_fresh_consumer(&context, &artifact, option->definition,
        option->data.enum_item.variants[1].definition);

    lengths = context_lengths(&context);
    assert(cm_hir_library_artifact_identity(&artifact, &identity));
    saved_local = decoded.items[0].variants[1].fields[0].type_local;
    decoded.items[0].variants[1].fields[0].type_local = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 172u);
    decoded.items[0].variants[1].fields[0].type_local = saved_local;
    saved_count = decoded.types[2].argument_count;
    decoded.types[2].argument_count = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 173u);
    decoded.types[2].argument_count = saved_count;
    saved_flags = decoded.items[0].variants[1].flags;
    decoded.items[0].variants[1].flags = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 174u);
    decoded.items[0].variants[1].flags = saved_flags;
    saved_string = decoded.items[0].variants[1].lang_item;
    decoded.items[0].variants[1].lang_item =
        decoded.items[0].variants[0].lang_item;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 175u);
    decoded.items[0].variants[1].lang_item = saved_string;
    saved_count = (uint32_t)decoded.namespace_count;
    decoded.namespace_count = 6u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 176u);
    decoded.namespace_count = saved_count;

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

static void test_associated_method_fresh_consumer(CmHirContext *context,
    const CmHirLibraryArtifact *artifact, CmHirDefId allocator_trait)
{
    static const unsigned char source_text[] =
        "pub fn direct<A: dep::AllocatorLike>(_value: &A) {}\n"
        "pub fn via_alias<A: dep::AllocatorAlias>(_value: &A) {}\n";
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    const CmHirLibraryArtifact *libraries[1];
    const CmHirItem *direct;
    const CmHirItem *via_alias;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_cfg_set_init(&cfg);
    cm_import_resolver_init(&imports);
    cm_hir_module_map_init(&map);
    assert(cm_source_add_memory(&sources, "associated-consumer.rs",
        source_text, sizeof(source_text) - 1u, &root_source)
        == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    assert(graph_result.error_count == 0u);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    assert(import_result.error_count == 0u);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "associated_consumer";
    libraries[0] = artifact;
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    lower_result = cm_hir_lower_module_graph(context, &graph,
        graph_result.revision, &imports, &map, &lower_options);
    assert(lower_result.error_count == 0u);
    direct = find_item(context, CM_HIR_ITEM_FUNCTION, "direct");
    via_alias = find_item(context, CM_HIR_ITEM_FUNCTION, "via_alias");
    assert(direct != NULL && via_alias != NULL
        && direct->predicate_count == 1u
        && via_alias->predicate_count == 1u
        && cm_hir_def_id_equal(
            direct->predicates[0].trait_type.definition, allocator_trait)
        && cm_hir_def_id_equal(
            via_alias->predicates[0].trait_type.definition,
            allocator_trait));
    cm_hir_module_map_destroy(&map);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_any_method_fresh_consumer(CmHirContext *context,
    const CmHirLibraryArtifact *artifact, CmHirDefId any_trait)
{
    static const unsigned char source_text[] =
        "pub fn direct<A: dep::AnyLike>(_value: &A) {}\n"
        "pub fn via_alias<A: dep::AnyAlias>(_value: &A) {}\n";
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    const CmHirLibraryArtifact *libraries[1];
    const CmHirItem *direct;
    const CmHirItem *via_alias;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_cfg_set_init(&cfg);
    cm_import_resolver_init(&imports);
    cm_hir_module_map_init(&map);
    assert(cm_source_add_memory(&sources, "any-consumer.rs", source_text,
        sizeof(source_text) - 1u, &root_source) == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    assert(graph_result.error_count == 0u);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    assert(import_result.error_count == 0u);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "any_consumer";
    libraries[0] = artifact;
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    lower_result = cm_hir_lower_module_graph(context, &graph,
        graph_result.revision, &imports, &map, &lower_options);
    assert(lower_result.error_count == 0u);
    direct = find_item(context, CM_HIR_ITEM_FUNCTION, "direct");
    via_alias = find_item(context, CM_HIR_ITEM_FUNCTION, "via_alias");
    assert(direct != NULL && via_alias != NULL
        && direct->predicate_count == 1u
        && via_alias->predicate_count == 1u
        && cm_hir_def_id_equal(
            direct->predicates[0].trait_type.definition, any_trait)
        && cm_hir_def_id_equal(
            via_alias->predicates[0].trait_type.definition, any_trait));
    cm_hir_module_map_destroy(&map);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_any_method_materialize_and_restore(void)
{
    AnyMethodFixture fixture;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirDeclarationMaterializeResult result;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    const CmHirItem *trait_item;
    const CmHirItem *method_item;
    const CmHirType *subject;
    CmHirLibraryBinding direct;
    CmHirLibraryBinding alias;
    CmHirLibraryPathSegment method_name;
    CmHirLibraryValue method;
    ContextLengths lengths;
    CmHirLibraryArtifactIdentity identity;
    uint32_t saved_local;
    uint32_t saved_count;
    uint8_t saved_byte;
    CmHirDeclarationString saved_string;

    any_method_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
        == CM_HIR_DECL_METADATA_OK
        && replay.len == encoded.len
        && memcmp(replay.data, encoded.data, encoded.len) == 0);

    expectation = expectation_for(&decoded);
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "dep", 191u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
        && result.item_count == 0u
        && result.public_type_entry_count == 2u
        && result.public_value_entry_count == 0u);
    trait_item = find_item(&context, CM_HIR_ITEM_TRAIT, "AnyLike");
    method_item = find_item(&context, CM_HIR_ITEM_FUNCTION, "type_id");
    assert(trait_item != NULL && method_item != NULL
        && trait_item->data.trait_item.safety == CM_HIR_SAFE
        && trait_item->data.trait_item.supertrait_count == 0u
        && trait_item->outlives_predicate_count == 1u
        && trait_item->outlives_predicates != NULL
        && trait_item->outlives_predicates[0].subject_kind
            == CM_HIR_OUTLIVES_TYPE
        && trait_item->outlives_predicates[0].bound.kind
            == CM_HIR_REGION_STATIC
        && trait_item->outlives_predicates[0].scope
            == CM_HIR_PREDICATE_SCOPE_NONE
        && method_item->data.function_item.body == CM_HIR_BODY_NONE
        && method_item->data.function_item.has_default_body == 0
        && method_item->data.function_item.signature.safety == CM_HIR_SAFE
        && method_item->data.function_item.signature.receiver
            == CM_HIR_RECEIVER_REF_SHARED
        && cm_hir_def_id_equal(method_item->parent_definition,
            trait_item->definition));
    assert_item_attribute(&context, trait_item, 0u,
        "rustc_diagnostic_item = \"AnyLike\"", 191u);
    subject = cm_hir_get_type(&context,
        trait_item->outlives_predicates[0].subject.type);
    assert(subject != NULL && subject->kind == CM_HIR_TYPE_SELF_KIND
        && cm_hir_def_id_equal(subject->data.self_type.owner,
            trait_item->definition));

    direct = lookup_binding(&artifact, "AnyLike");
    alias = lookup_binding(&artifact, "AnyAlias");
    assert(direct.kind == CM_HIR_LIBRARY_BINDING_TRAIT
        && alias.kind == CM_HIR_LIBRARY_BINDING_TRAIT
        && cm_hir_def_id_equal(direct.definition, alias.definition));
    method_name.bytes = (const unsigned char *)"type_id";
    method_name.length = sizeof("type_id") - 1u;
    memset(&method, 0, sizeof(method));
    assert(cm_hir_library_artifact_lookup_associated_method(&artifact,
        direct.definition, &method_name, &method) == CM_HIR_LIBRARY_OK
        && cm_hir_def_id_equal(method.definition, method_item->definition)
        && method.data.function.safety == CM_HIR_SAFE
        && method.data.function.has_default_body == 0);
    assert(lookup_value_binding_status(&artifact, "type_id")
        == CM_HIR_LIBRARY_NOT_FOUND);
    test_any_method_fresh_consumer(&context, &artifact,
        trait_item->definition);

    lengths = context_lengths(&context);
    assert(cm_hir_library_artifact_identity(&artifact, &identity));
    saved_byte = decoded.traits[0].flags;
    decoded.traits[0].flags = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 192u);
    decoded.traits[0].flags = saved_byte;
    saved_string = decoded.traits[0].diagnostic_item;
    decoded.traits[0].diagnostic_item = (CmHirDeclarationString)S("");
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 193u);
    decoded.traits[0].diagnostic_item = saved_string;
    saved_local = decoded.outlives_predicates[0].owner_local;
    decoded.outlives_predicates[0].owner_local = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 194u);
    decoded.outlives_predicates[0].owner_local = saved_local;
    saved_local = decoded.outlives_predicates[0].subject_type;
    decoded.outlives_predicates[0].subject_type = 1u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 195u);
    decoded.outlives_predicates[0].subject_type = saved_local;
    saved_byte = decoded.outlives_predicates[0].bound.kind;
    decoded.outlives_predicates[0].bound.kind = CM_HIR_DECL_REGION_ERASED;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 196u);
    decoded.outlives_predicates[0].bound.kind = saved_byte;
    saved_count = decoded.traits[0].outlives_count;
    decoded.traits[0].outlives_count = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 197u);
    decoded.traits[0].outlives_count = saved_count;
    saved_local = decoded.associated_items[0].parent_local;
    decoded.associated_items[0].parent_local = 2u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 198u);
    decoded.associated_items[0].parent_local = saved_local;
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "bad-name", 199u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_ARTIFACT_FAILURE
        && result.library_status == CM_HIR_LIBRARY_INVALID_ARGUMENT);
    assert_context_lengths(&context, lengths);
    assert_artifact_identity_same(&artifact, &identity);

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

static void test_associated_method_materialize_and_restore(void)
{
    AssociatedMethodFixture fixture;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirDeclarationMaterializeResult result;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    const CmHirItem *trait_item;
    const CmHirItem *sized_item;
    const CmHirItem *allocate_item;
    const CmHirItem *fallback_item;
    const CmHirType *receiver_type;
    CmHirLibraryBinding direct;
    CmHirLibraryBinding alias;
    CmHirLibraryPathSegment method_name;
    CmHirLibraryValue method;
    ContextLengths lengths;
    CmHirLibraryArtifactIdentity identity;
    uint32_t saved_local;
    uint8_t saved_byte;

    associated_method_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
        == CM_HIR_DECL_METADATA_OK
        && replay.len == encoded.len
        && memcmp(replay.data, encoded.data, encoded.len) == 0);

    expectation = expectation_for(&decoded);
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "dep", 181u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
        && result.item_count == 0u
        && result.public_type_entry_count == 3u
        && result.public_value_entry_count == 0u);
    trait_item = find_item(&context, CM_HIR_ITEM_TRAIT, "AllocatorLike");
    sized_item = find_item(&context, CM_HIR_ITEM_TRAIT, "SizedLike");
    allocate_item = find_item(&context, CM_HIR_ITEM_FUNCTION, "allocate");
    fallback_item = find_item(&context, CM_HIR_ITEM_FUNCTION, "fallback");
    assert(trait_item != NULL && sized_item != NULL && allocate_item != NULL
        && fallback_item != NULL
        && trait_item->data.trait_item.safety == CM_HIR_UNSAFE
        && cm_hir_def_id_equal(allocate_item->parent_definition,
            trait_item->definition)
        && allocate_item->data.function_item.body == CM_HIR_BODY_NONE
        && cm_hir_get_body(&context,
            allocate_item->data.function_item.body) == NULL
        && allocate_item->data.function_item.has_default_body == 0
        && allocate_item->data.function_item.signature.receiver
            == CM_HIR_RECEIVER_REF_SHARED
        && allocate_item->data.function_item.signature.safety
            == CM_HIR_UNSAFE
        && fallback_item->data.function_item.body == CM_HIR_BODY_NONE
        && cm_hir_get_body(&context,
            fallback_item->data.function_item.body) == NULL
        && fallback_item->data.function_item.has_default_body == 1
        && fallback_item->predicate_count == 1u
        && fallback_item->predicates[0].subject != CM_HIR_TYPE_NONE
        && cm_hir_def_id_equal(
            fallback_item->predicates[0].trait_type.definition,
            sized_item->definition)
        && fallback_item->predicates[0].trait_type.argument_count == 0u);
    receiver_type = cm_hir_get_type(&context,
        allocate_item->data.function_item.signature.parameters[0].type);
    assert(receiver_type != NULL
        && receiver_type->kind == CM_HIR_TYPE_REFERENCE_KIND
        && receiver_type->data.reference_type.region.kind
            == CM_HIR_REGION_ERASED);
    receiver_type = cm_hir_get_type(&context,
        receiver_type->data.reference_type.pointee);
    assert(receiver_type != NULL
        && receiver_type->kind == CM_HIR_TYPE_SELF_KIND
        && cm_hir_def_id_equal(receiver_type->data.self_type.owner,
            trait_item->definition));

    direct = lookup_binding(&artifact, "AllocatorLike");
    alias = lookup_binding(&artifact, "AllocatorAlias");
    assert(direct.kind == CM_HIR_LIBRARY_BINDING_TRAIT
        && alias.kind == CM_HIR_LIBRARY_BINDING_TRAIT
        && cm_hir_def_id_equal(direct.definition, alias.definition));
    method_name.bytes = (const unsigned char *)"allocate";
    method_name.length = sizeof("allocate") - 1u;
    memset(&method, 0, sizeof(method));
    assert(cm_hir_library_artifact_lookup_associated_method(&artifact,
        direct.definition, &method_name, &method) == CM_HIR_LIBRARY_OK
        && cm_hir_def_id_equal(method.definition,
            allocate_item->definition)
        && method.data.function.receiver == CM_HIR_RECEIVER_REF_SHARED
        && method.data.function.safety == CM_HIR_UNSAFE
        && method.data.function.has_default_body == 0);
    method_name.bytes = (const unsigned char *)"fallback";
    method_name.length = sizeof("fallback") - 1u;
    assert(cm_hir_library_artifact_lookup_associated_method(&artifact,
        alias.definition, &method_name, &method) == CM_HIR_LIBRARY_OK
        && cm_hir_def_id_equal(method.definition,
            fallback_item->definition)
        && method.data.function.has_default_body == 1
        && method.data.function.predicate_count == 1u
        && method.data.function.nominal_reference_count == 1u
        && cm_hir_def_id_equal(
            method.data.function.nominal_references[0].definition,
            sized_item->definition));
    assert(lookup_value_binding_status(&artifact, "allocate")
        == CM_HIR_LIBRARY_NOT_FOUND);
    test_associated_method_fresh_consumer(&context, &artifact,
        trait_item->definition);

    lengths = context_lengths(&context);
    assert(cm_hir_library_artifact_identity(&artifact, &identity));
    saved_local = decoded.types[2].self_trait_local;
    decoded.types[2].self_trait_local = 2u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 182u);
    decoded.types[2].self_trait_local = saved_local;
    saved_byte = decoded.types[3].region.kind;
    decoded.types[3].region.kind = CM_HIR_DECL_REGION_STATIC;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 183u);
    decoded.types[3].region.kind = saved_byte;
    saved_local = decoded.associated_items[0].parent_local;
    decoded.associated_items[0].parent_local = 2u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 184u);
    decoded.associated_items[0].parent_local = saved_local;
    saved_byte = decoded.associated_items[1].has_default_body;
    decoded.associated_items[1].has_default_body = 2u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 185u);
    decoded.associated_items[1].has_default_body = saved_byte;
    saved_byte = decoded.associated_items[0].kind;
    decoded.associated_items[0].kind = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 186u);
    decoded.associated_items[0].kind = saved_byte;
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "bad-name", 187u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_ARTIFACT_FAILURE
        && result.library_status == CM_HIR_LIBRARY_INVALID_ARGUMENT);
    assert_context_lengths(&context, lengths);
    assert_artifact_identity_same(&artifact, &identity);

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

static void test_into_iter_materialize_and_consume(void)
{
    IntoIterFixture fixture;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirDeclarationMaterializeResult result;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirLibraryArtifactIdentity identity;
    const CmHirItem *into_iter;
    const CmHirItem *polymorphic;
    const CmHirItem *index_range;
    const CmHirItem *partial_drop;
    const CmHirItem *method;
    const CmHirGenericParam *type_generic;
    const CmHirGenericParam *const_generic;
    const CmHirType *type;
    const CmHirType *array;
    const CmHirType *maybe;
    const CmHirModule *iter_module = NULL;
    CmHirLibraryBinding direct;
    CmHirLibraryBinding alias;
    CmHirLibraryPathSegment path[2];
    CmHirLibraryPathSegment method_name;
    CmHirLibraryValue library_method;
    ContextLengths lengths;
    size_t module_index;
    uint32_t saved_local;
    uint16_t saved_flags;
    uint8_t saved_byte;

    into_iter_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
            == CM_HIR_DECL_METADATA_OK
        && encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

    expectation = expectation_for(&decoded);
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "dep", 231u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
        && result.item_count == 5u
        && result.public_type_entry_count == 5u
        && result.public_value_entry_count == 0u);

    into_iter = find_item(&context, CM_HIR_ITEM_STRUCT, "IntoIter");
    polymorphic = find_item(&context, CM_HIR_ITEM_STRUCT,
        "PolymorphicIter");
    index_range = find_item(&context, CM_HIR_ITEM_STRUCT, "IndexRange");
    partial_drop = find_item(&context, CM_HIR_ITEM_TRAIT, "PartialDrop");
    method = find_item(&context, CM_HIR_ITEM_FUNCTION, "partial_drop");
    assert(into_iter != NULL && polymorphic != NULL && index_range != NULL
        && partial_drop != NULL && method != NULL
        && into_iter->visibility.kind == CM_HIR_VIS_PUBLIC
        && into_iter->generic_parameter_count == 2u
        && into_iter->data.aggregate_item.form == CM_HIR_AGGREGATE_NAMED
        && into_iter->data.aggregate_item.field_count == 1u
        && into_iter->attribute_count == 2u
        && polymorphic->visibility.kind == CM_HIR_VIS_RESTRICTED
        && polymorphic->generic_parameter_count == 1u
        && polymorphic->predicate_scope_count == 0u
        && polymorphic->predicate_count == 1u
        && index_range->visibility.kind == CM_HIR_VIS_CRATE
        && partial_drop->visibility.kind == CM_HIR_VIS_PRIVATE
        && partial_drop->data.trait_item.safety == CM_HIR_SAFE
        && method->visibility.kind == CM_HIR_VIS_PRIVATE
        && method->data.function_item.signature.receiver
            == CM_HIR_RECEIVER_REF_MUTABLE
        && method->data.function_item.signature.safety == CM_HIR_UNSAFE
        && method->data.function_item.body == CM_HIR_BODY_NONE
        && method->data.function_item.has_default_body == 0
        && method->data.function_item.signature.parameter_count == 2u
        && cm_hir_def_id_equal(method->parent_definition,
            partial_drop->definition));
    assert_item_attribute(&context, into_iter, 0u,
        "rustc_insignificant_dtor", 231u);
    assert_item_attribute(&context, into_iter, 1u,
        "rustc_diagnostic_item = \"ArrayIntoIter\"", 231u);

    for (module_index = 0u; module_index < context.modules.len;
            ++module_index) {
        const CmHirModule *candidate = (const CmHirModule *)cm_vec_at_const(
            &context.modules, module_index);
        const CmInternedString *name = candidate == NULL ? NULL
            : cm_interner_get(&context.strings, candidate->name);
        if (name != NULL && name->len == sizeof("iter") - 1u
            && memcmp(name->bytes, "iter", sizeof("iter") - 1u) == 0)
            iter_module = candidate;
    }
    assert(iter_module != NULL && cm_hir_def_id_equal(
        polymorphic->visibility.restriction, iter_module->definition));

    type_generic = cm_hir_get_generic_param(&context,
        into_iter->generic_parameter_start);
    const_generic = cm_hir_get_generic_param(&context,
        into_iter->generic_parameter_start + 1u);
    type = const_generic == NULL ? NULL : cm_hir_get_type(&context,
        const_generic->declared_type);
    assert(type_generic != NULL && const_generic != NULL
        && type_generic->kind == CM_HIR_GENERIC_TYPE
        && const_generic->kind == CM_HIR_GENERIC_CONST
        && !const_generic->has_default
        && type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_USIZE);

    type = cm_hir_get_type(&context,
        into_iter->data.aggregate_item.fields[0].type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(type->data.named_type.definition,
            polymorphic->definition)
        && type->data.named_type.argument_count == 1u
        && type->data.named_type.arguments[0].kind
            == CM_HIR_GENERIC_ARG_TYPE);
    array = type == NULL ? NULL : cm_hir_get_type(&context,
        type->data.named_type.arguments[0].data.type);
    assert(array != NULL && array->kind == CM_HIR_TYPE_ARRAY_KIND
        && array->data.array_type.length.kind == CM_HIR_CONST_PARAMETER
        && array->data.array_type.length.data.parameter
            == into_iter->generic_parameter_start + 1u
        && array->data.array_type.length.type ==
            const_generic->declared_type);
    maybe = array == NULL ? NULL : cm_hir_get_type(&context,
        array->data.array_type.element);
    assert(maybe != NULL && maybe->kind == CM_HIR_TYPE_ADT_KIND
        && maybe->data.named_type.argument_count == 1u
        && maybe->data.named_type.arguments[0].kind
            == CM_HIR_GENERIC_ARG_TYPE);
    type = maybe == NULL ? NULL : cm_hir_get_type(&context,
        maybe->data.named_type.arguments[0].data.type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_PARAMETER_KIND
        && type->data.parameter_type.parameter
            == into_iter->generic_parameter_start);
    type = cm_hir_get_type(&context, polymorphic->predicates[0].subject);
    assert(type != NULL && type->kind == CM_HIR_TYPE_PARAMETER_KIND
        && type->data.parameter_type.parameter
            == polymorphic->generic_parameter_start
        && polymorphic->predicates[0].scope == CM_HIR_PREDICATE_SCOPE_NONE
        && cm_hir_def_id_equal(
            polymorphic->predicates[0].trait_type.definition,
            partial_drop->definition)
        && polymorphic->predicates[0].trait_type.argument_count == 0u);

    direct = lookup_binding(&artifact, "IntoIter");
    alias = lookup_binding(&artifact, "IntoIterAlias");
    assert(direct.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && alias.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && cm_hir_def_id_equal(direct.definition, into_iter->definition)
        && cm_hir_def_id_equal(alias.definition, into_iter->definition)
        && lookup_value_binding_status(&artifact, "IntoIter")
            == CM_HIR_LIBRARY_NOT_FOUND);
    path[0].bytes = (const unsigned char *)"dep";
    path[0].length = sizeof("dep") - 1u;
    path[1].bytes = (const unsigned char *)"PartialDrop";
    path[1].length = sizeof("PartialDrop") - 1u;
    assert(cm_hir_library_artifact_lookup_binding(&artifact, path, 2u,
        &direct) == CM_HIR_LIBRARY_NOT_FOUND);
    method_name.bytes = (const unsigned char *)"partial_drop";
    method_name.length = sizeof("partial_drop") - 1u;
    memset(&library_method, 0, sizeof(library_method));
    assert(cm_hir_library_artifact_lookup_associated_method(&artifact,
        partial_drop->definition, &method_name, &library_method)
        == CM_HIR_LIBRARY_NOT_FOUND);
    test_into_iter_fresh_consumer(&context, &artifact,
        into_iter->definition);

    lengths = context_lengths(&context);
    assert(cm_hir_library_artifact_identity(&artifact, &identity));
    saved_byte = decoded.generics[3].kind;
    decoded.generics[3].kind = CM_HIR_DECL_GENERIC_TYPE;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 232u);
    decoded.generics[3].kind = saved_byte;
    saved_local = decoded.generics[3].declared_type;
    decoded.generics[3].declared_type = 1u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 233u);
    decoded.generics[3].declared_type = saved_local;
    saved_byte = decoded.generics[3].has_default;
    decoded.generics[3].has_default = 1u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 234u);
    decoded.generics[3].has_default = saved_byte;
    saved_local = decoded.types[11].array_length_generic_local;
    decoded.types[11].array_length_generic_local = 3u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 235u);
    decoded.types[11].array_length_generic_local = saved_local;
    saved_local = decoded.predicates[0].owner_item;
    decoded.predicates[0].owner_item = 4u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 236u);
    decoded.predicates[0].owner_item = saved_local;
    saved_byte = decoded.items[4].visibility.kind;
    decoded.items[4].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 237u);
    decoded.items[4].visibility.kind = saved_byte;
    saved_byte = decoded.associated_items[0].receiver;
    decoded.associated_items[0].receiver =
        CM_HIR_DECL_RECEIVER_REF_SHARED;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 238u);
    decoded.associated_items[0].receiver = saved_byte;
    saved_flags = decoded.items[3].aggregate_flags;
    decoded.items[3].aggregate_flags &=
        (uint16_t)~CM_HIR_DECL_AGGREGATE_HAS_DIAGNOSTIC_ITEM;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 239u);
    decoded.items[3].aggregate_flags = saved_flags;
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "bad-name", 240u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_ARTIFACT_FAILURE
        && result.library_status == CM_HIR_LIBRARY_INVALID_ARGUMENT);
    assert_context_lengths(&context, lengths);
    assert_artifact_identity_same(&artifact, &identity);

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

static void test_from_fn_materialize_and_consume(void)
{
    FromFnFixture fixture;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirDeclarationMaterializeResult result;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirLibraryArtifactIdentity identity;
    const CmHirItem *fn_mut;
    const CmHirItem *fn_once;
    const CmHirItem *tuple_trait;
    const CmHirItem *output;
    const CmHirItem *call_mut;
    const CmHirItem *call_once;
    const CmHirItem *from_fn;
    const CmHirGenericParam *type_parameter;
    const CmHirGenericParam *const_parameter;
    const CmHirGenericParam *function_parameter;
    const CmHirType *type;
    const CmHirType *tuple;
    const CmHirType *projection;
    const CmInternedString *abi;
    CmHirLibraryBinding direct;
    CmHirLibraryBinding alias;
    CmHirLibraryPathSegment path[2];
    CmHirLibraryPathSegment method_name;
    CmHirLibraryValue library_function;
    CmHirLibraryValue library_method;
    ContextLengths lengths;
    uint32_t saved_local;
    uint32_t saved_count;
    uint8_t saved_byte;
    CmHirDeclarationString saved_string;

    from_fn_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
            == CM_HIR_DECL_METADATA_OK
        && replay.len == encoded.len
        && memcmp(replay.data, encoded.data, encoded.len) == 0);

    expectation = expectation_for(&decoded);
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "dep", 241u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
        && result.item_count == 0u
        && result.public_type_entry_count == 3u
        && result.public_value_entry_count == 2u);

    fn_mut = find_item(&context, CM_HIR_ITEM_TRAIT, "FnMut");
    fn_once = find_item(&context, CM_HIR_ITEM_TRAIT, "FnOnce");
    tuple_trait = find_item(&context, CM_HIR_ITEM_TRAIT, "Tuple");
    output = find_item(&context, CM_HIR_ITEM_TYPE_ALIAS, "Output");
    call_mut = find_item(&context, CM_HIR_ITEM_FUNCTION, "call_mut");
    call_once = find_item(&context, CM_HIR_ITEM_FUNCTION, "call_once");
    from_fn = find_item(&context, CM_HIR_ITEM_FUNCTION, "from_fn");
    assert(fn_mut != NULL && fn_once != NULL && tuple_trait != NULL
        && output != NULL && call_mut != NULL && call_once != NULL
        && from_fn != NULL
        && fn_mut->data.trait_item.safety == CM_HIR_SAFE
        && fn_mut->data.trait_item.is_const == 1
        && fn_mut->data.trait_item.supertrait_count == 1u
        && fn_mut->predicate_count == 1u
        && fn_once->data.trait_item.safety == CM_HIR_SAFE
        && fn_once->data.trait_item.is_const == 1
        && fn_once->data.trait_item.supertrait_count == 0u
        && fn_once->predicate_count == 1u
        && tuple_trait->data.trait_item.safety == CM_HIR_SAFE
        && tuple_trait->data.trait_item.is_const == 0
        && tuple_trait->predicate_count == 0u);
    assert_item_attribute(&context, fn_mut, 0u, "lang = \"fn_mut\"",
        241u);
    assert_item_attribute(&context, fn_mut, 1u, "rustc_paren_sugar", 241u);
    assert_item_attribute(&context, fn_mut, 2u, "fundamental", 241u);
    assert_item_attribute(&context, fn_mut, 3u, "const_trait", 241u);
    assert_item_attribute(&context, fn_once, 0u, "lang = \"fn_once\"",
        241u);
    assert_item_attribute(&context, fn_once, 1u, "rustc_paren_sugar",
        241u);
    assert_item_attribute(&context, fn_once, 2u, "fundamental", 241u);
    assert_item_attribute(&context, fn_once, 3u, "const_trait", 241u);
    assert_item_attribute(&context, tuple_trait, 0u,
        "lang = \"tuple_trait\"", 241u);
    assert_item_attribute(&context, tuple_trait, 1u,
        "rustc_deny_explicit_impl", 241u);
    assert_item_attribute(&context, tuple_trait, 2u,
        "rustc_do_not_implement_via_object", 241u);
    assert(fn_mut->attribute_count == 4u && fn_once->attribute_count == 4u
        && tuple_trait->attribute_count == 3u
        && cm_hir_def_id_equal(fn_mut->data.trait_item.supertraits[0]
            .trait_type.definition, fn_once->definition)
        && fn_mut->data.trait_item.supertraits[0].modifier
            == CM_HIR_SUPERTRAIT_REQUIRED
        && fn_mut->data.trait_item.supertraits[0]
            .trait_type.argument_count == 1u
        && fn_mut->data.trait_item.supertraits[0].equality_count == 0u
        && cm_hir_def_id_equal(fn_mut->predicates[0]
            .trait_type.definition, tuple_trait->definition)
        && cm_hir_def_id_equal(fn_once->predicates[0]
            .trait_type.definition, tuple_trait->definition));
    type = cm_hir_get_type(&context,
        fn_mut->data.trait_item.supertraits[0].trait_type.arguments[0]
            .data.type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_PARAMETER_KIND
        && type->data.parameter_type.parameter
            == fn_mut->generic_parameter_start);

    assert(cm_hir_def_id_equal(output->parent_definition,
            fn_once->definition)
        && output->data.type_alias_item.target == CM_HIR_TYPE_NONE
        && output->data.type_alias_item.bound_count == 0u
        && output->attribute_count == 1u);
    assert_item_attribute(&context, output, 0u,
        "lang = \"fn_once_output\"", 241u);
    assert(call_mut->data.function_item.body == CM_HIR_BODY_NONE
        && call_once->data.function_item.body == CM_HIR_BODY_NONE
        && cm_hir_get_body(&context,
            call_mut->data.function_item.body) == NULL
        && cm_hir_get_body(&context,
            call_once->data.function_item.body) == NULL
        && call_mut->data.function_item.signature.receiver
            == CM_HIR_RECEIVER_REF_MUTABLE
        && call_once->data.function_item.signature.receiver
            == CM_HIR_RECEIVER_VALUE
        && call_mut->data.function_item.signature.parameter_count == 2u
        && call_once->data.function_item.signature.parameter_count == 2u
        && cm_hir_def_id_equal(call_mut->parent_definition,
            fn_mut->definition)
        && cm_hir_def_id_equal(call_once->parent_definition,
            fn_once->definition));
    abi = cm_interner_get(&context.strings,
        call_mut->data.function_item.signature.abi);
    assert(abi != NULL && abi->len == sizeof("rust-call") - 1u
        && memcmp(abi->bytes, "rust-call", sizeof("rust-call") - 1u)
            == 0);
    type = cm_hir_get_type(&context,
        call_mut->data.function_item.signature.parameters[0].type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_REFERENCE_KIND
        && type->data.reference_type.mutability == CM_HIR_MUTABLE
        && type->data.reference_type.region.kind == CM_HIR_REGION_ERASED);
    projection = cm_hir_get_type(&context,
        call_mut->data.function_item.signature.return_type);
    assert(projection != NULL
        && projection->kind == CM_HIR_TYPE_PROJECTION_KIND
        && cm_hir_def_id_equal(projection->data.projection_type
            .trait_type.definition, fn_once->definition)
        && cm_hir_def_id_equal(projection->data.projection_type
            .associated_type.definition, output->definition)
        && projection->data.projection_type.trait_type.argument_count == 1u);

    assert(from_fn->visibility.kind == CM_HIR_VIS_PUBLIC
        && from_fn->generic_parameter_count == 3u
        && from_fn->predicate_count == 1u
        && from_fn->data.function_item.signature.parameter_count == 1u
        && from_fn->data.function_item.body == CM_HIR_BODY_NONE
        && cm_hir_get_body(&context,
            from_fn->data.function_item.body) == NULL);
    type_parameter = cm_hir_get_generic_param(&context,
        from_fn->generic_parameter_start);
    const_parameter = cm_hir_get_generic_param(&context,
        from_fn->generic_parameter_start + 1u);
    function_parameter = cm_hir_get_generic_param(&context,
        from_fn->generic_parameter_start + 2u);
    type = const_parameter == NULL ? NULL : cm_hir_get_type(&context,
        const_parameter->declared_type);
    assert(type_parameter != NULL && const_parameter != NULL
        && function_parameter != NULL
        && type_parameter->kind == CM_HIR_GENERIC_TYPE
        && const_parameter->kind == CM_HIR_GENERIC_CONST
        && !const_parameter->has_default
        && function_parameter->kind == CM_HIR_GENERIC_TYPE
        && type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_USIZE);
    type = cm_hir_get_type(&context,
        from_fn->data.function_item.signature.return_type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_ARRAY_KIND
        && type->data.array_type.length.kind == CM_HIR_CONST_PARAMETER
        && type->data.array_type.length.data.parameter
            == from_fn->generic_parameter_start + 1u);
    assert(from_fn->predicates[0].scope == CM_HIR_PREDICATE_SCOPE_NONE
        && cm_hir_def_id_equal(from_fn->predicates[0]
            .trait_type.definition, fn_mut->definition)
        && from_fn->predicates[0].trait_type.argument_count == 1u
        && from_fn->predicates[0].equality_count == 1u
        && cm_hir_def_id_equal(from_fn->predicates[0].equalities[0]
            .associated_type, output->definition));
    tuple = cm_hir_get_type(&context,
        from_fn->predicates[0].trait_type.arguments[0].data.type);
    type = cm_hir_get_type(&context,
        from_fn->predicates[0].equalities[0].value);
    assert(tuple != NULL && tuple->kind == CM_HIR_TYPE_TUPLE_KIND
        && tuple->data.tuple_type.element_count == 1u
        && type != NULL && type->kind == CM_HIR_TYPE_PARAMETER_KIND
        && type->data.parameter_type.parameter
            == from_fn->generic_parameter_start);

    direct = lookup_value_binding(&artifact, "from_fn");
    alias = lookup_value_binding(&artifact, "from_fn_alias");
    assert(direct.kind == CM_HIR_LIBRARY_BINDING_VALUE
        && alias.kind == CM_HIR_LIBRARY_BINDING_VALUE
        && cm_hir_def_id_equal(direct.definition, from_fn->definition)
        && cm_hir_def_id_equal(alias.definition, from_fn->definition));
    path[0].bytes = (const unsigned char *)"dep";
    path[0].length = sizeof("dep") - 1u;
    path[1].bytes = (const unsigned char *)"from_fn";
    path[1].length = sizeof("from_fn") - 1u;
    memset(&library_function, 0, sizeof(library_function));
    assert(cm_hir_library_artifact_lookup_value(&artifact, path, 2u,
            &library_function) == CM_HIR_LIBRARY_OK
        && library_function.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        && cm_hir_def_id_equal(library_function.definition,
            from_fn->definition)
        && library_function.data.function.nominal_reference_count == 3u
        && library_function.data.function.nominal_references[0].kind
            == CM_HIR_LIBRARY_NOMINAL_TRAIT
        && cm_hir_def_id_equal(library_function.data.function
            .nominal_references[0].definition, fn_mut->definition)
        && library_function.data.function.nominal_references[1].kind
            == CM_HIR_LIBRARY_NOMINAL_TRAIT
        && cm_hir_def_id_equal(library_function.data.function
            .nominal_references[1].definition, fn_once->definition)
        && library_function.data.function.nominal_references[2].kind
            == CM_HIR_LIBRARY_NOMINAL_ASSOCIATED_TYPE
        && cm_hir_def_id_equal(library_function.data.function
            .nominal_references[2].definition, output->definition)
        && library_function.data.function.associated_availability_count
            == 1u
        && cm_hir_def_id_equal(library_function.data.function
            .associated_availability[0].direct_trait, fn_mut->definition)
        && cm_hir_def_id_equal(library_function.data.function
            .associated_availability[0].associated_type,
            output->definition));
    method_name.bytes = (const unsigned char *)"call_mut";
    method_name.length = sizeof("call_mut") - 1u;
    memset(&library_method, 0, sizeof(library_method));
    assert(cm_hir_library_artifact_lookup_associated_method(&artifact,
            fn_mut->definition, &method_name, &library_method)
            == CM_HIR_LIBRARY_OK
        && cm_hir_def_id_equal(library_method.definition,
            call_mut->definition)
        && library_method.data.function.receiver
            == CM_HIR_RECEIVER_REF_MUTABLE);
    method_name.bytes = (const unsigned char *)"call_once";
    method_name.length = sizeof("call_once") - 1u;
    assert(cm_hir_library_artifact_lookup_associated_method(&artifact,
            fn_once->definition, &method_name, &library_method)
            == CM_HIR_LIBRARY_OK
        && cm_hir_def_id_equal(library_method.definition,
            call_once->definition)
        && library_method.data.function.receiver == CM_HIR_RECEIVER_VALUE
        && lookup_value_binding_status(&artifact, "call_once")
            == CM_HIR_LIBRARY_NOT_FOUND
        && lookup_value_binding_status(&artifact, "Output")
            == CM_HIR_LIBRARY_NOT_FOUND);
    test_from_fn_fresh_consumer(&context, &artifact, from_fn->definition);

    lengths = context_lengths(&context);
    assert(cm_hir_library_artifact_identity(&artifact, &identity));
    saved_byte = decoded.generics[3].kind;
    decoded.generics[3].kind = CM_HIR_DECL_GENERIC_TYPE;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 242u);
    decoded.generics[3].kind = saved_byte;
    saved_local = decoded.generics[3].declared_type;
    decoded.generics[3].declared_type = 2u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 243u);
    decoded.generics[3].declared_type = saved_local;
    saved_local = decoded.types[9].array_length_generic_local;
    decoded.types[9].array_length_generic_local = 3u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 244u);
    decoded.types[9].array_length_generic_local = saved_local;
    saved_count = decoded.predicates[0].equality_count;
    decoded.predicates[0].equality_count = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 245u);
    decoded.predicates[0].equality_count = saved_count;
    saved_local = decoded.predicates[0].equalities[0].associated_local;
    decoded.predicates[0].equalities[0].associated_local = 1u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 246u);
    decoded.predicates[0].equalities[0].associated_local = saved_local;
    saved_count = decoded.traits[0].supertrait_count;
    decoded.traits[0].supertrait_count = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 247u);
    decoded.traits[0].supertrait_count = saved_count;
    saved_local = decoded.associated_items[1].parent_local;
    decoded.associated_items[1].parent_local = 1u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 248u);
    decoded.associated_items[1].parent_local = saved_local;
    saved_byte = decoded.associated_items[0].receiver;
    decoded.associated_items[0].receiver = CM_HIR_DECL_RECEIVER_REF_SHARED;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 249u);
    decoded.associated_items[0].receiver = saved_byte;
    saved_byte = decoded.associated_items[2].has_default_body;
    decoded.associated_items[2].has_default_body = 1u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 253u);
    decoded.associated_items[2].has_default_body = saved_byte;
    saved_local = decoded.types[11].projection_associated_local;
    decoded.types[11].projection_associated_local = 1u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 250u);
    decoded.types[11].projection_associated_local = saved_local;
    saved_string = decoded.traits[0].lang_item;
    decoded.traits[0].lang_item = decoded.traits[1].lang_item;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 251u);
    decoded.traits[0].lang_item = saved_string;
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "bad-name", 252u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_ARTIFACT_FAILURE
        && result.library_status == CM_HIR_LIBRARY_INVALID_ARGUMENT);
    assert_context_lengths(&context, lengths);
    assert_artifact_identity_same(&artifact, &identity);

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

static void test_try_from_fn_materialize_and_consume(void)
{
    TryFromFnFixture fixture;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirDeclarationMaterializeResult result;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirLibraryArtifactIdentity identity;
    const CmHirItem *fn_mut;
    const CmHirItem *fn_once;
    const CmHirItem *from_residual;
    const CmHirItem *residual;
    const CmHirItem *try_trait;
    const CmHirItem *control_flow;
    const CmHirItem *try_type;
    const CmHirItem *from_output;
    const CmHirItem *from_residual_method;
    const CmHirItem *try_function;
    const CmHirGenericParam *parameter;
    const CmHirType *type;
    const CmHirType *other_type;
    CmHirLibraryBinding direct;
    CmHirLibraryBinding alias;
    CmHirLibraryPathSegment path[2];
    CmHirLibraryValue library_function;
    CmHirLibraryPathSegment method_name;
    CmHirLibraryValue library_method;
    ContextLengths lengths;
    uint32_t saved_local;
    uint32_t saved_count;
    uint8_t saved_byte;

    try_from_fn_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
            == CM_HIR_DECL_METADATA_OK
        && replay.len == encoded.len
        && memcmp(replay.data, encoded.data, encoded.len) == 0);

    expectation = expectation_for(&decoded);
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "dep", 261u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
        && result.item_count == 1u
        && result.public_type_entry_count == 7u
        && result.public_value_entry_count == 2u);

    fn_mut = find_item(&context, CM_HIR_ITEM_TRAIT, "FnMut");
    fn_once = find_item(&context, CM_HIR_ITEM_TRAIT, "FnOnce");
    from_residual = find_item(&context, CM_HIR_ITEM_TRAIT,
        "FromResidual");
    residual = find_item(&context, CM_HIR_ITEM_TRAIT, "Residual");
    try_trait = find_item(&context, CM_HIR_ITEM_TRAIT, "Try");
    control_flow = find_item(&context, CM_HIR_ITEM_ENUM, "ControlFlow");
    try_function = find_item(&context, CM_HIR_ITEM_FUNCTION,
        "try_from_fn");
    assert(fn_mut != NULL && fn_once != NULL && from_residual != NULL
        && residual != NULL && try_trait != NULL && control_flow != NULL
        && try_function != NULL);
    try_type = find_associated_item(&context, CM_HIR_ITEM_TYPE_ALIAS,
        residual->definition, "TryType");
    from_output = find_associated_item(&context, CM_HIR_ITEM_FUNCTION,
        try_trait->definition, "from_output");
    from_residual_method = find_associated_item(&context,
        CM_HIR_ITEM_FUNCTION, from_residual->definition, "from_residual");
    assert(try_type != NULL && from_output != NULL
        && from_residual_method != NULL);

    assert(try_trait->data.trait_item.safety == CM_HIR_SAFE
        && try_trait->data.trait_item.is_const == 1
        && try_trait->data.trait_item.supertrait_count == 1u
        && try_trait->data.trait_item.supertraits[0].modifier
            == CM_HIR_SUPERTRAIT_CONST_IF_CONST
        && cm_hir_def_id_equal(try_trait->data.trait_item.supertraits[0]
            .trait_type.definition, from_residual->definition)
        && try_trait->data.trait_item.supertraits[0]
            .trait_type.argument_count == 1u);
    assert_item_attribute(&context, try_trait, 0u, "lang = \"Try\"",
        261u);
    assert_item_attribute(&context, try_trait, 1u, "const_trait", 261u);
    assert_item_attribute(&context, from_residual, 0u,
        "rustc_diagnostic_item = \"FromResidual\"", 261u);
    assert_item_attribute(&context, from_residual, 1u, "const_trait", 261u);
    assert_item_attribute(&context, residual, 0u, "const_trait", 261u);

    parameter = cm_hir_get_generic_param(&context,
        from_residual->generic_parameter_start);
    assert(parameter != NULL && parameter->has_default
        && parameter->default_argument.kind == CM_HIR_GENERIC_ARG_TYPE);
    type = cm_hir_get_type(&context,
        parameter->default_argument.data.type);
    other_type = cm_hir_get_type(&context,
        try_trait->data.trait_item.supertraits[0]
            .trait_type.arguments[0].data.type);
    assert(type != NULL && other_type != NULL
        && type->kind == CM_HIR_TYPE_PROJECTION_KIND
        && other_type->kind == CM_HIR_TYPE_PROJECTION_KIND
        && type->data.projection_type.self_type
            != other_type->data.projection_type.self_type);
    type = cm_hir_get_type(&context, type->data.projection_type.self_type);
    other_type = cm_hir_get_type(&context,
        other_type->data.projection_type.self_type);
    assert(type != NULL && other_type != NULL
        && type->kind == CM_HIR_TYPE_SELF_KIND
        && other_type->kind == CM_HIR_TYPE_SELF_KIND
        && cm_hir_def_id_equal(type->data.self_type.owner,
            from_residual->definition)
        && cm_hir_def_id_equal(other_type->data.self_type.owner,
            try_trait->definition));

    assert(try_type->predicate_count == 0u
        && try_type->data.type_alias_item.target == CM_HIR_TYPE_NONE
        && try_type->data.type_alias_item.bound_count == 1u
        && try_type->data.type_alias_item.bounds != NULL
        && try_type->data.type_alias_item.bounds[0].modifier
            == CM_HIR_ASSOC_BOUND_REQUIRED
        && cm_hir_def_id_equal(try_type->data.type_alias_item.bounds[0]
            .trait_type.definition, try_trait->definition)
        && try_type->data.type_alias_item.bounds[0].equality_count == 2u);
    assert(cm_hir_def_id_equal(try_type->data.type_alias_item.bounds[0]
            .equalities[0].associated_type,
        find_associated_item(&context, CM_HIR_ITEM_TYPE_ALIAS,
            try_trait->definition, "Output")->definition)
        && cm_hir_def_id_equal(try_type->data.type_alias_item.bounds[0]
            .equalities[1].associated_type,
        find_associated_item(&context, CM_HIR_ITEM_TYPE_ALIAS,
            try_trait->definition, "Residual")->definition));

    assert(from_output->data.function_item.signature.receiver
            == CM_HIR_RECEIVER_NONE
        && from_output->data.function_item.signature.parameter_count == 1u
        && from_output->data.function_item.signature.parameters[0].name
            == CM_INTERN_ID_NONE
        && from_output->data.function_item.signature.parameters[0]
            .binding_kind == CM_HIR_BINDING_DISCARD
        && from_residual_method->data.function_item.signature.receiver
            == CM_HIR_RECEIVER_NONE
        && from_residual_method->data.function_item.signature.parameters[0]
            .name == CM_INTERN_ID_NONE
        && from_residual_method->data.function_item.signature.parameters[0]
            .binding_kind == CM_HIR_BINDING_DISCARD);
    assert_item_attribute(&context, from_output, 0u,
        "lang = \"from_output\"", 261u);
    assert_item_attribute(&context, from_residual_method, 0u,
        "lang = \"from_residual\"", 261u);

    assert(control_flow->generic_parameter_count == 2u
        && control_flow->data.enum_item.variant_count == 2u
        && control_flow->attribute_count == 2u);
    assert_item_attribute(&context, control_flow, 0u,
        "rustc_diagnostic_item = \"ControlFlow\"", 261u);
    assert_item_attribute(&context, control_flow, 1u, "must_use", 261u);
    parameter = cm_hir_get_generic_param(&context,
        control_flow->generic_parameter_start + 1u);
    type = parameter == NULL || !parameter->has_default
        || parameter->default_argument.kind != CM_HIR_GENERIC_ARG_TYPE
        ? NULL : cm_hir_get_type(&context,
            parameter->default_argument.data.type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_UNIT_KIND
        && control_flow->data.enum_item.variants[0].form
            == CM_HIR_AGGREGATE_TUPLE
        && control_flow->data.enum_item.variants[1].form
            == CM_HIR_AGGREGATE_TUPLE);
    {
        const CmInternedString *continue_lang = cm_interner_get(
            &context.strings,
            control_flow->data.enum_item.variants[0].lang_item);
        const CmInternedString *break_lang = cm_interner_get(
            &context.strings,
            control_flow->data.enum_item.variants[1].lang_item);
        assert(continue_lang != NULL && break_lang != NULL
            && continue_lang->len == sizeof("Continue") - 1u
            && memcmp(continue_lang->bytes, "Continue",
                sizeof("Continue") - 1u) == 0
            && break_lang->len == sizeof("Break") - 1u
            && memcmp(break_lang->bytes, "Break",
                sizeof("Break") - 1u) == 0);
    }

    assert(try_function->generic_parameter_count == 3u
        && try_function->predicate_count == 3u
        && try_function->data.function_item.signature.parameter_count == 1u
        && try_function->data.function_item.body == CM_HIR_BODY_NONE
        && cm_hir_get_body(&context,
            try_function->data.function_item.body) == NULL);
    type = cm_hir_get_type(&context,
        try_function->data.function_item.signature.return_type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_PROJECTION_KIND
        && cm_hir_def_id_equal(type->data.projection_type
            .trait_type.definition, residual->definition)
        && cm_hir_def_id_equal(type->data.projection_type
            .associated_type.definition, try_type->definition));

    direct = lookup_value_binding(&artifact, "try_from_fn");
    alias = lookup_value_binding(&artifact, "try_from_fn_alias");
    assert(direct.kind == CM_HIR_LIBRARY_BINDING_VALUE
        && alias.kind == CM_HIR_LIBRARY_BINDING_VALUE
        && cm_hir_def_id_equal(direct.definition, try_function->definition)
        && cm_hir_def_id_equal(alias.definition,
            try_function->definition));
    path[0].bytes = (const unsigned char *)"dep";
    path[0].length = sizeof("dep") - 1u;
    path[1].bytes = (const unsigned char *)"try_from_fn";
    path[1].length = sizeof("try_from_fn") - 1u;
    memset(&library_function, 0, sizeof(library_function));
    assert(cm_hir_library_artifact_lookup_value(&artifact, path, 2u,
            &library_function) == CM_HIR_LIBRARY_OK
        && library_function.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        && library_function.data.function.nominal_reference_count == 6u
        && library_function.data.function.associated_availability_count
            == 1u);
    method_name.bytes = (const unsigned char *)"from_output";
    method_name.length = sizeof("from_output") - 1u;
    memset(&library_method, 0, sizeof(library_method));
    assert(cm_hir_library_artifact_lookup_associated_method(&artifact,
            try_trait->definition, &method_name, &library_method)
            == CM_HIR_LIBRARY_OK
        && cm_hir_def_id_equal(library_method.definition,
            from_output->definition)
        && library_method.data.function.receiver == CM_HIR_RECEIVER_NONE
        && lookup_value_binding_status(&artifact, "from_output")
            == CM_HIR_LIBRARY_NOT_FOUND);
    test_try_from_fn_fresh_consumer(&context, &artifact,
        try_function->definition);

    lengths = context_lengths(&context);
    assert(cm_hir_library_artifact_identity(&artifact, &identity));
    saved_local = decoded.generics[2].default_type;
    decoded.generics[2].default_type = 25u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 262u);
    decoded.generics[2].default_type = saved_local;
    saved_local = decoded.traits[4].supertraits[0].argument_types[0];
    decoded.traits[4].supertraits[0].argument_types[0] = 22u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 263u);
    decoded.traits[4].supertraits[0].argument_types[0] = saved_local;
    saved_byte = decoded.traits[4].supertraits[0].modifier;
    decoded.traits[4].supertraits[0].modifier =
        CM_HIR_DECL_SUPERTRAIT_REQUIRED;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 264u);
    decoded.traits[4].supertraits[0].modifier = saved_byte;
    saved_count = decoded.associated_items[4].predicate_count;
    decoded.associated_items[4].predicate_count = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 265u);
    decoded.associated_items[4].predicate_count = saved_count;
    saved_local = decoded.predicates[3].equalities[1].associated_local;
    decoded.predicates[3].equalities[1].associated_local = 6u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 266u);
    decoded.predicates[3].equalities[1].associated_local = saved_local;
    saved_byte = decoded.associated_items[7].receiver;
    decoded.associated_items[7].receiver = CM_HIR_DECL_RECEIVER_VALUE;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 267u);
    decoded.associated_items[7].receiver = saved_byte;
    saved_local = decoded.values[0].return_type;
    decoded.values[0].return_type = 27u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 268u);
    decoded.values[0].return_type = saved_local;
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "bad-name", 269u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_ARTIFACT_FAILURE
        && result.library_status == CM_HIR_LIBRARY_INVALID_ARGUMENT);
    assert_context_lengths(&context, lengths);
    assert_artifact_identity_same(&artifact, &identity);

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

static void test_non_drop_marker_materialize(void)
{
    NonDropFixture fixture;
    CmByteBuf encoded;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirDeclarationMaterializeResult result;
    const CmHirItem *non_drop;

    non_drop_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    expectation = expectation_for(&decoded);
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "dep", 272u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
        && result.public_type_entry_count == 1u);
    non_drop = find_item(&context, CM_HIR_ITEM_TRAIT, "NonDrop");
    assert(non_drop != NULL
        && non_drop->visibility.kind == CM_HIR_VIS_PUBLIC
        && non_drop->data.trait_item.safety == CM_HIR_SAFE
        && non_drop->data.trait_item.is_const == 0
        && non_drop->data.trait_item.is_auto == 0
        && non_drop->data.trait_item.supertrait_count == 0u
        && non_drop->generic_parameter_count == 0u
        && non_drop->predicate_count == 0u
        && non_drop->attribute_count == 1u);
    assert_item_attribute(&context, non_drop, 0u,
        "rustc_unsafe_specialization_marker", 272u);
    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&encoded);
}

static void test_repeat_materialize_and_consume(void)
{
    RepeatFixture fixture;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirDeclarationMaterializeResult result;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirLibraryArtifactIdentity identity;
    const CmHirItem *clone_trait;
    const CmHirItem *destruct_trait;
    const CmHirItem *meta_sized;
    const CmHirItem *pointee_sized;
    const CmHirItem *sized_trait;
    const CmHirItem *clone_method;
    const CmHirItem *clone_from_method;
    const CmHirItem *repeat;
    const CmHirGenericParam *type_generic;
    const CmHirGenericParam *const_generic;
    const CmHirType *const_declared_type;
    const CmHirType *parameter;
    const CmHirType *return_type;
    const CmHirType *element;
    const CmHirType *clone_from_receiver;
    const CmHirType *clone_from_source;
    CmHirLibraryBinding direct;
    CmHirLibraryBinding alias;
    CmHirLibraryPathSegment path[2];
    CmHirLibraryPathSegment method_name;
    CmHirLibraryValue library_value;
    ContextLengths lengths;
    size_t reference_index;
    unsigned int reference_mask = 0u;
    uint32_t saved_local;
    uint16_t saved_flags;
    uint8_t saved_byte;

    repeat_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
            == CM_HIR_DECL_METADATA_OK
        && replay.len == encoded.len
        && memcmp(replay.data, encoded.data, encoded.len) == 0);

    expectation = expectation_for(&decoded);
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "dep", 272u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
        && result.item_count == 0u
        && result.public_type_entry_count == 5u
        && result.public_value_entry_count == 2u);
    clone_trait = find_item(&context, CM_HIR_ITEM_TRAIT, "Clone");
    destruct_trait = find_item(&context, CM_HIR_ITEM_TRAIT, "Destruct");
    meta_sized = find_item(&context, CM_HIR_ITEM_TRAIT, "MetaSized");
    pointee_sized = find_item(&context, CM_HIR_ITEM_TRAIT, "PointeeSized");
    sized_trait = find_item(&context, CM_HIR_ITEM_TRAIT, "Sized");
    clone_method = find_item(&context, CM_HIR_ITEM_FUNCTION, "clone");
    clone_from_method = find_item(&context, CM_HIR_ITEM_FUNCTION,
        "clone_from");
    repeat = find_item(&context, CM_HIR_ITEM_FUNCTION, "repeat");
    assert(clone_trait != NULL && destruct_trait != NULL
        && meta_sized != NULL && pointee_sized != NULL
        && sized_trait != NULL && clone_method != NULL
        && clone_from_method != NULL && repeat != NULL
        && clone_trait->visibility.kind == CM_HIR_VIS_PUBLIC
        && clone_trait->data.trait_item.safety == CM_HIR_SAFE
        && clone_trait->data.trait_item.is_const == 1
        && clone_trait->data.trait_item.supertrait_count == 1u
        && cm_hir_def_id_equal(clone_trait->data.trait_item.supertraits[0]
            .trait_type.definition, sized_trait->definition)
        && meta_sized->data.trait_item.supertrait_count == 1u
        && cm_hir_def_id_equal(meta_sized->data.trait_item.supertraits[0]
            .trait_type.definition, pointee_sized->definition)
        && sized_trait->data.trait_item.supertrait_count == 1u
        && cm_hir_def_id_equal(sized_trait->data.trait_item.supertraits[0]
            .trait_type.definition, meta_sized->definition)
        && pointee_sized->data.trait_item.supertrait_count == 0u
        && destruct_trait->data.trait_item.safety == CM_HIR_SAFE
        && destruct_trait->data.trait_item.is_const == 1);
    assert(clone_trait->attribute_count == 4u);
    assert_item_attribute(&context, clone_trait, 0u, "lang = \"clone\"",
        272u);
    assert_item_attribute(&context, clone_trait, 1u,
        "rustc_diagnostic_item = \"Clone\"", 272u);
    assert_item_attribute(&context, clone_trait, 2u,
        "rustc_trivial_field_reads", 272u);
    assert_item_attribute(&context, clone_trait, 3u, "const_trait", 272u);
    assert(sized_trait->attribute_count == 6u);
    assert_item_attribute(&context, sized_trait, 0u, "lang = \"sized\"",
        272u);
    assert_item_attribute(&context, sized_trait, 1u, "fundamental", 272u);
    assert_item_attribute(&context, sized_trait, 2u,
        "rustc_specialization_trait", 272u);
    assert_item_attribute(&context, sized_trait, 3u,
        "rustc_deny_explicit_impl", 272u);
    assert_item_attribute(&context, sized_trait, 4u,
        "rustc_do_not_implement_via_object", 272u);
    assert_item_attribute(&context, sized_trait, 5u, "rustc_coinductive",
        272u);
    assert(clone_from_method->data.function_item.signature.parameter_count
        == 2u);
    clone_from_receiver = cm_hir_get_type(&context,
        clone_from_method->data.function_item.signature.parameters[0].type);
    clone_from_source = cm_hir_get_type(&context,
        clone_from_method->data.function_item.signature.parameters[1].type);
    assert(cm_hir_def_id_equal(clone_method->parent_definition,
            clone_trait->definition)
        && clone_method->data.function_item.signature.receiver
            == CM_HIR_RECEIVER_REF_SHARED
        && clone_method->data.function_item.has_default_body == 0
        && clone_method->data.function_item.body == CM_HIR_BODY_NONE
        && clone_method->attribute_count == 1u
        && clone_from_method->data.function_item.signature.receiver
            == CM_HIR_RECEIVER_REF_MUTABLE
        && clone_from_method->data.function_item.signature.safety
            == CM_HIR_SAFE
        && clone_from_receiver != NULL
        && clone_from_receiver->kind == CM_HIR_TYPE_REFERENCE_KIND
        && clone_from_receiver->data.reference_type.mutability
            == CM_HIR_MUTABLE
        && clone_from_receiver->data.reference_type.region.kind
            == CM_HIR_REGION_ERASED
        && clone_from_source != NULL
        && clone_from_source->kind == CM_HIR_TYPE_REFERENCE_KIND
        && clone_from_source->data.reference_type.mutability
            == CM_HIR_IMMUTABLE
        && clone_from_source->data.reference_type.region.kind
            == CM_HIR_REGION_ERASED
        && clone_from_method->data.function_item.has_default_body == 1
        && clone_from_method->data.function_item.body == CM_HIR_BODY_NONE
        && clone_from_method->predicate_count == 1u
        && clone_from_method->predicates[0].modifier
            == CM_HIR_PREDICATE_CONST_IF_CONST
        && cm_hir_def_id_equal(clone_from_method->predicates[0]
            .trait_type.definition, destruct_trait->definition));
    assert_item_attribute(&context, clone_method, 0u,
        "lang = \"clone_fn\"", 272u);

    type_generic = cm_hir_get_generic_param(&context,
        repeat->generic_parameter_start);
    const_generic = cm_hir_get_generic_param(&context,
        repeat->generic_parameter_start + 1u);
    const_declared_type = const_generic == NULL ? NULL
        : cm_hir_get_type(&context, const_generic->declared_type);
    parameter = cm_hir_get_type(&context,
        repeat->data.function_item.signature.parameters[0].type);
    return_type = cm_hir_get_type(&context,
        repeat->data.function_item.signature.return_type);
    element = return_type == NULL
            || return_type->kind != CM_HIR_TYPE_ARRAY_KIND
        ? NULL : cm_hir_get_type(&context,
            return_type->data.array_type.element);
    assert(repeat->visibility.kind == CM_HIR_VIS_PUBLIC
        && repeat->attribute_count == 0u
        && repeat->generic_parameter_count == 2u
        && repeat->predicate_count == 1u
        && repeat->predicates[0].modifier == CM_HIR_PREDICATE_REQUIRED
        && cm_hir_def_id_equal(repeat->predicates[0]
            .trait_type.definition, clone_trait->definition)
        && repeat->data.function_item.signature.parameter_count == 1u
        && repeat->data.function_item.signature.safety == CM_HIR_SAFE
        && repeat->data.function_item.signature.is_const == 0
        && repeat->data.function_item.body == CM_HIR_BODY_NONE
        && cm_hir_get_body(&context,
            repeat->data.function_item.body) == NULL
        && type_generic != NULL && type_generic->kind == CM_HIR_GENERIC_TYPE
        && !type_generic->is_relaxed_sized && !type_generic->has_default
        && const_generic != NULL
        && const_generic->kind == CM_HIR_GENERIC_CONST
        && !const_generic->has_default
        && const_declared_type != NULL
        && const_declared_type->kind == CM_HIR_TYPE_INTEGER_KIND
        && const_declared_type->data.integer_type.kind == CM_HIR_INT_USIZE
        && parameter != NULL
        && parameter->kind == CM_HIR_TYPE_PARAMETER_KIND
        && parameter->data.parameter_type.parameter
            == repeat->generic_parameter_start
        && return_type != NULL
        && return_type->kind == CM_HIR_TYPE_ARRAY_KIND
        && element != NULL && element->kind == CM_HIR_TYPE_PARAMETER_KIND
        && element->data.parameter_type.parameter
            == repeat->generic_parameter_start
        && return_type->data.array_type.length.kind
            == CM_HIR_CONST_PARAMETER
        && return_type->data.array_type.length.data.parameter
            == repeat->generic_parameter_start + 1u);

    direct = lookup_value_binding(&artifact, "repeat");
    alias = lookup_value_binding(&artifact, "repeat_alias");
    assert(direct.kind == CM_HIR_LIBRARY_BINDING_VALUE
        && alias.kind == CM_HIR_LIBRARY_BINDING_VALUE
        && cm_hir_def_id_equal(direct.definition, repeat->definition)
        && cm_hir_def_id_equal(alias.definition, repeat->definition));
    path[0].bytes = (const unsigned char *)"dep";
    path[0].length = sizeof("dep") - 1u;
    path[1].bytes = (const unsigned char *)"repeat";
    path[1].length = sizeof("repeat") - 1u;
    memset(&library_value, 0, sizeof(library_value));
    assert(cm_hir_library_artifact_lookup_value(&artifact, path, 2u,
            &library_value) == CM_HIR_LIBRARY_OK
        && library_value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        && library_value.data.function.generic_parameter_count == 2u
        && library_value.data.function.predicate_count == 1u
        && library_value.data.function.parameter_count == 1u
        && library_value.data.function.return_type
            == repeat->data.function_item.signature.return_type
        && library_value.data.function.nominal_reference_count == 4u);
    for (reference_index = 0u; reference_index < 4u; ++reference_index) {
        CmHirDefId definition = library_value.data.function
            .nominal_references[reference_index].definition;
        unsigned int bit = 0u;
        assert(library_value.data.function
            .nominal_references[reference_index].kind
                == CM_HIR_LIBRARY_NOMINAL_TRAIT);
        if (cm_hir_def_id_equal(definition, clone_trait->definition))
            bit = 1u;
        else if (cm_hir_def_id_equal(definition,
                sized_trait->definition)) bit = 2u;
        else if (cm_hir_def_id_equal(definition,
                meta_sized->definition)) bit = 4u;
        else if (cm_hir_def_id_equal(definition,
                pointee_sized->definition)) bit = 8u;
        assert(bit != 0u && (reference_mask & bit) == 0u);
        reference_mask |= bit;
    }
    assert(reference_mask == 15u);
    method_name.bytes = (const unsigned char *)"clone_from";
    method_name.length = sizeof("clone_from") - 1u;
    assert(cm_hir_library_artifact_lookup_associated_method(&artifact,
            clone_trait->definition, &method_name, &library_value)
            == CM_HIR_LIBRARY_OK
        && cm_hir_def_id_equal(library_value.definition,
            clone_from_method->definition)
        && library_value.data.function.receiver
            == CM_HIR_RECEIVER_REF_MUTABLE
        && library_value.data.function.has_default_body == 1
        && library_value.data.function.predicate_count == 1u
        && library_value.data.function.predicates[0].modifier
            == CM_HIR_PREDICATE_CONST_IF_CONST
        && cm_hir_def_id_equal(library_value.data.function.predicates[0]
            .trait_type.definition, destruct_trait->definition)
        && library_value.data.function.nominal_reference_count == 1u
        && cm_hir_def_id_equal(library_value.data.function
            .nominal_references[0].definition, destruct_trait->definition));
    test_repeat_fresh_consumer(&context, &artifact, repeat->definition,
        clone_trait->definition);

    lengths = context_lengths(&context);
    assert(cm_hir_library_artifact_identity(&artifact, &identity));
    saved_byte = decoded.predicates[1].modifier;
    decoded.predicates[1].modifier = CM_HIR_DECL_PREDICATE_REQUIRED;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 273u);
    decoded.predicates[1].modifier = saved_byte;
    saved_flags = decoded.traits[0].compiler_flags;
    decoded.traits[0].compiler_flags = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 274u);
    decoded.traits[0].compiler_flags = saved_flags;
    saved_local = decoded.traits[0].supertraits[0].trait_local;
    decoded.traits[0].supertraits[0].trait_local = 3u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 275u);
    decoded.traits[0].supertraits[0].trait_local = saved_local;
    saved_local = decoded.types[6].array_length_generic_local;
    decoded.types[6].array_length_generic_local = 1u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 276u);
    decoded.types[6].array_length_generic_local = saved_local;
    saved_local = decoded.predicates[0].trait_local;
    decoded.predicates[0].trait_local = 5u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 277u);
    decoded.predicates[0].trait_local = saved_local;
    saved_byte = decoded.associated_items[1].has_default_body;
    decoded.associated_items[1].has_default_body = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 278u);
    decoded.associated_items[1].has_default_body = saved_byte;
    saved_byte = decoded.associated_items[0].flags;
    decoded.associated_items[0].flags = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 279u);
    decoded.associated_items[0].flags = saved_byte;
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "bad-name", 280u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_ARTIFACT_FAILURE
        && result.library_status == CM_HIR_LIBRARY_INVALID_ARGUMENT);
    assert_context_lengths(&context, lengths);
    assert_artifact_identity_same(&artifact, &identity);

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

static void test_from_mut_materialize_and_consume(void)
{
    FromMutFixture fixture;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirDeclarationMaterializeResult result;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirLibraryArtifactIdentity identity;
    const CmHirItem *item;
    const CmHirGenericParam *generic;
    const CmHirType *input;
    const CmHirType *input_pointee;
    const CmHirType *output;
    const CmHirType *array;
    const CmHirType *array_element;
    const CmHirType *length_type;
    CmHirLibraryBinding direct;
    CmHirLibraryBinding alias;
    CmHirLibraryPathSegment direct_path[2];
    CmHirLibraryPathSegment alias_path[2];
    CmHirLibraryValue direct_value;
    CmHirLibraryValue alias_value;
    ContextLengths lengths;
    uint32_t saved_local;
    uint32_t saved_count;
    uint64_t saved_bits;
    uint8_t saved_byte;

    from_mut_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
            == CM_HIR_DECL_METADATA_OK
        && replay.len == encoded.len
        && memcmp(replay.data, encoded.data, encoded.len) == 0);

    expectation = expectation_for(&decoded);
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "dep", 254u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
        && result.item_count == 0u
        && result.public_type_entry_count == 0u
        && result.public_value_entry_count == 2u);
    item = find_item(&context, CM_HIR_ITEM_FUNCTION, "from_mut");
    generic = item == NULL || item->generic_parameter_count != 1u
        ? NULL : cm_hir_get_generic_param(&context,
            item->generic_parameter_start);
    input = item == NULL
            || item->data.function_item.signature.parameter_count != 1u
        ? NULL : cm_hir_get_type(&context,
            item->data.function_item.signature.parameters[0].type);
    input_pointee = input == NULL
            || input->kind != CM_HIR_TYPE_REFERENCE_KIND
        ? NULL : cm_hir_get_type(&context,
            input->data.reference_type.pointee);
    output = item == NULL ? NULL : cm_hir_get_type(&context,
        item->data.function_item.signature.return_type);
    array = output == NULL || output->kind != CM_HIR_TYPE_REFERENCE_KIND
        ? NULL : cm_hir_get_type(&context,
            output->data.reference_type.pointee);
    array_element = array == NULL || array->kind != CM_HIR_TYPE_ARRAY_KIND
        ? NULL : cm_hir_get_type(&context,
            array->data.array_type.element);
    length_type = array == NULL || array->kind != CM_HIR_TYPE_ARRAY_KIND
            || array->data.array_type.length.kind != CM_HIR_CONST_VALUE
        ? NULL : cm_hir_get_type(&context,
            array->data.array_type.length.type);
    assert(item != NULL && item->visibility.kind == CM_HIR_VIS_PUBLIC
        && item->generic_parameter_count == 1u
        && item->predicate_count == 0u && item->predicates == NULL
        && item->data.function_item.signature.parameter_count == 1u
        && item->data.function_item.signature.receiver == CM_HIR_RECEIVER_NONE
        && item->data.function_item.signature.safety == CM_HIR_SAFE
        && item->data.function_item.signature.is_const == 1
        && item->data.function_item.body == CM_HIR_BODY_NONE
        && cm_hir_get_body(&context,
            item->data.function_item.body) == NULL
        && generic != NULL && generic->kind == CM_HIR_GENERIC_TYPE
        && !generic->is_relaxed_sized && !generic->has_default
        && cm_hir_def_id_equal(generic->owner, item->definition)
        && input != NULL && input->kind == CM_HIR_TYPE_REFERENCE_KIND
        && input->data.reference_type.mutability == CM_HIR_MUTABLE
        && input->data.reference_type.region.kind == CM_HIR_REGION_ERASED
        && input_pointee != NULL
        && input_pointee->kind == CM_HIR_TYPE_PARAMETER_KIND
        && input_pointee->data.parameter_type.parameter
            == item->generic_parameter_start
        && output != NULL && output->kind == CM_HIR_TYPE_REFERENCE_KIND
        && output->data.reference_type.mutability == CM_HIR_MUTABLE
        && output->data.reference_type.region.kind == CM_HIR_REGION_ERASED
        && array != NULL && array->kind == CM_HIR_TYPE_ARRAY_KIND
        && array_element != NULL
        && array_element->kind == CM_HIR_TYPE_PARAMETER_KIND
        && array_element->data.parameter_type.parameter
            == item->generic_parameter_start
        && array->data.array_type.length.kind == CM_HIR_CONST_VALUE
        && array->data.array_type.length.data.value.low_bits == UINT64_C(1)
        && array->data.array_type.length.data.value.high_bits == UINT64_C(0)
        && length_type != NULL
        && length_type->kind == CM_HIR_TYPE_INTEGER_KIND
        && length_type->data.integer_type.kind == CM_HIR_INT_USIZE);

    direct = lookup_value_binding(&artifact, "from_mut");
    alias = lookup_value_binding(&artifact, "from_mut_alias");
    assert(direct.kind == CM_HIR_LIBRARY_BINDING_VALUE
        && alias.kind == CM_HIR_LIBRARY_BINDING_VALUE
        && cm_hir_def_id_equal(direct.definition, item->definition)
        && cm_hir_def_id_equal(alias.definition, item->definition));
    direct_path[0].bytes = (const unsigned char *)"dep";
    direct_path[0].length = sizeof("dep") - 1u;
    direct_path[1].bytes = (const unsigned char *)"from_mut";
    direct_path[1].length = sizeof("from_mut") - 1u;
    alias_path[0] = direct_path[0];
    alias_path[1].bytes = (const unsigned char *)"from_mut_alias";
    alias_path[1].length = sizeof("from_mut_alias") - 1u;
    memset(&direct_value, 0, sizeof(direct_value));
    memset(&alias_value, 0, sizeof(alias_value));
    assert(cm_hir_library_artifact_lookup_value(&artifact, direct_path, 2u,
            &direct_value) == CM_HIR_LIBRARY_OK
        && cm_hir_library_artifact_lookup_value(&artifact, alias_path, 2u,
            &alias_value) == CM_HIR_LIBRARY_OK
        && direct_value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        && alias_value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        && direct_value.data.function.generic_parameter_count == 1u
        && alias_value.data.function.generic_parameter_count == 1u
        && direct_value.data.function.predicate_count == 0u
        && alias_value.data.function.predicate_count == 0u
        && direct_value.data.function.parameter_count == 1u
        && alias_value.data.function.parameter_count == 1u
        && direct_value.data.function.parameter_types[0]
            == item->data.function_item.signature.parameters[0].type
        && alias_value.data.function.parameter_types[0]
            == item->data.function_item.signature.parameters[0].type
        && direct_value.data.function.return_type
            == item->data.function_item.signature.return_type
        && alias_value.data.function.return_type
            == item->data.function_item.signature.return_type
        && direct_value.data.function.is_const == 1
        && alias_value.data.function.is_const == 1);
    test_from_mut_fresh_consumer(&context, &artifact, item->definition);

    lengths = context_lengths(&context);
    assert(cm_hir_library_artifact_identity(&artifact, &identity));
    saved_byte = decoded.types[2].region.kind;
    decoded.types[2].region.kind = CM_HIR_DECL_REGION_STATIC;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 255u);
    decoded.types[2].region.kind = saved_byte;
    saved_byte = decoded.types[4].region.kind;
    decoded.types[4].region.kind = CM_HIR_DECL_REGION_STATIC;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 256u);
    decoded.types[4].region.kind = saved_byte;
    saved_byte = decoded.types[2].mutability;
    decoded.types[2].mutability = CM_HIR_DECL_IMMUTABLE;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 257u);
    decoded.types[2].mutability = saved_byte;
    saved_byte = decoded.types[4].mutability;
    decoded.types[4].mutability = CM_HIR_DECL_IMMUTABLE;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 258u);
    decoded.types[4].mutability = saved_byte;
    saved_bits = decoded.types[3].array_length_low_bits;
    decoded.types[3].array_length_low_bits = UINT64_C(2);
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 259u);
    decoded.types[3].array_length_low_bits = saved_bits;
    saved_bits = decoded.types[3].array_length_high_bits;
    decoded.types[3].array_length_high_bits = UINT64_C(1);
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 260u);
    decoded.types[3].array_length_high_bits = saved_bits;
    saved_local = decoded.types[3].array_length_type;
    decoded.types[3].array_length_type = 2u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 261u);
    decoded.types[3].array_length_type = saved_local;
    saved_byte = decoded.values[0].is_const;
    decoded.values[0].is_const = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 262u);
    decoded.values[0].is_const = saved_byte;
    saved_count = decoded.values[0].generic_count;
    decoded.values[0].generic_count = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 263u);
    decoded.values[0].generic_count = saved_count;
    saved_local = decoded.namespace_entries[1].target_local;
    decoded.namespace_entries[1].target_local = 2u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 264u);
    decoded.namespace_entries[1].target_local = saved_local;
    saved_byte = decoded.namespace_entries[1].namespace_kind;
    decoded.namespace_entries[1].namespace_kind = CM_HIR_DECL_NAMESPACE_TYPE;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 265u);
    decoded.namespace_entries[1].namespace_kind = saved_byte;
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "bad-name", 266u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_ARTIFACT_FAILURE
        && result.library_status == CM_HIR_LIBRARY_INVALID_ARGUMENT);
    assert_context_lengths(&context, lengths);
    assert_artifact_identity_same(&artifact, &identity);

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

static void test_from_ref_materialize_and_consume(void)
{
    FromMutFixture fixture;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirDeclarationMaterializeResult result;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirLibraryArtifactIdentity identity;
    const CmHirItem *item;
    const CmHirType *input;
    const CmHirType *output;
    const CmHirType *array;
    CmHirLibraryBinding direct;
    CmHirLibraryBinding alias;
    CmHirLibraryPathSegment direct_path[2];
    CmHirLibraryPathSegment alias_path[2];
    CmHirLibraryValue direct_value;
    CmHirLibraryValue alias_value;
    ContextLengths lengths;
    uint8_t saved_byte;

    from_ref_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
            == CM_HIR_DECL_METADATA_OK
        && replay.len == encoded.len
        && memcmp(replay.data, encoded.data, encoded.len) == 0);

    expectation = expectation_for(&decoded);
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "dep", 267u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
        && result.item_count == 0u
        && result.public_type_entry_count == 0u
        && result.public_value_entry_count == 2u);
    item = find_item(&context, CM_HIR_ITEM_FUNCTION, "from_ref");
    input = item == NULL ? NULL : cm_hir_get_type(&context,
        item->data.function_item.signature.parameters[0].type);
    output = item == NULL ? NULL : cm_hir_get_type(&context,
        item->data.function_item.signature.return_type);
    array = output == NULL || output->kind != CM_HIR_TYPE_REFERENCE_KIND
        ? NULL : cm_hir_get_type(&context,
            output->data.reference_type.pointee);
    assert(item != NULL && item->visibility.kind == CM_HIR_VIS_PUBLIC
        && item->generic_parameter_count == 1u
        && item->predicate_count == 0u
        && item->data.function_item.signature.is_const == 1
        && item->data.function_item.signature.parameter_count == 1u
        && item->data.function_item.body == CM_HIR_BODY_NONE
        && input != NULL && input->kind == CM_HIR_TYPE_REFERENCE_KIND
        && input->data.reference_type.mutability == CM_HIR_IMMUTABLE
        && input->data.reference_type.region.kind == CM_HIR_REGION_ERASED
        && output != NULL && output->kind == CM_HIR_TYPE_REFERENCE_KIND
        && output->data.reference_type.mutability == CM_HIR_IMMUTABLE
        && output->data.reference_type.region.kind == CM_HIR_REGION_ERASED
        && array != NULL && array->kind == CM_HIR_TYPE_ARRAY_KIND
        && array->data.array_type.length.kind == CM_HIR_CONST_VALUE
        && array->data.array_type.length.data.value.low_bits == UINT64_C(1)
        && array->data.array_type.length.data.value.high_bits == UINT64_C(0)
        && cm_hir_get_body(&context,
            item->data.function_item.body) == NULL);

    direct = lookup_value_binding(&artifact, "from_ref");
    alias = lookup_value_binding(&artifact, "from_ref_alias");
    assert(direct.kind == CM_HIR_LIBRARY_BINDING_VALUE
        && alias.kind == CM_HIR_LIBRARY_BINDING_VALUE
        && cm_hir_def_id_equal(direct.definition, item->definition)
        && cm_hir_def_id_equal(alias.definition, item->definition));
    direct_path[0].bytes = (const unsigned char *)"dep";
    direct_path[0].length = sizeof("dep") - 1u;
    direct_path[1].bytes = (const unsigned char *)"from_ref";
    direct_path[1].length = sizeof("from_ref") - 1u;
    alias_path[0] = direct_path[0];
    alias_path[1].bytes = (const unsigned char *)"from_ref_alias";
    alias_path[1].length = sizeof("from_ref_alias") - 1u;
    memset(&direct_value, 0, sizeof(direct_value));
    memset(&alias_value, 0, sizeof(alias_value));
    assert(cm_hir_library_artifact_lookup_value(&artifact, direct_path, 2u,
            &direct_value) == CM_HIR_LIBRARY_OK
        && cm_hir_library_artifact_lookup_value(&artifact, alias_path, 2u,
            &alias_value) == CM_HIR_LIBRARY_OK
        && direct_value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        && alias_value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        && direct_value.data.function.is_const == 1
        && alias_value.data.function.is_const == 1
        && direct_value.data.function.parameter_count == 1u
        && alias_value.data.function.parameter_count == 1u
        && direct_value.data.function.parameter_types[0]
            == item->data.function_item.signature.parameters[0].type
        && alias_value.data.function.parameter_types[0]
            == item->data.function_item.signature.parameters[0].type
        && direct_value.data.function.return_type
            == item->data.function_item.signature.return_type
        && alias_value.data.function.return_type
            == item->data.function_item.signature.return_type);
    test_from_ref_fresh_consumer(&context, &artifact, item->definition);

    lengths = context_lengths(&context);
    assert(cm_hir_library_artifact_identity(&artifact, &identity));
    saved_byte = decoded.types[2].mutability;
    decoded.types[2].mutability = CM_HIR_DECL_MUTABLE;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 268u);
    decoded.types[2].mutability = saved_byte;
    saved_byte = decoded.types[4].mutability;
    decoded.types[4].mutability = CM_HIR_DECL_MUTABLE;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 269u);
    decoded.types[4].mutability = saved_byte;
    saved_byte = decoded.types[2].region.kind;
    decoded.types[2].region.kind = CM_HIR_DECL_REGION_STATIC;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 270u);
    decoded.types[2].region.kind = saved_byte;
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "bad-name", 271u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_ARTIFACT_FAILURE
        && result.library_status == CM_HIR_LIBRARY_INVALID_ARGUMENT);
    assert_context_lengths(&context, lengths);
    assert_artifact_identity_same(&artifact, &identity);

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

static void test_unit_function_materialize_and_consume(void)
{
    UnitFunctionFixture fixture;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirDeclarationMaterializeResult result;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirLibraryArtifactIdentity identity;
    CmHirLibraryBinding direct;
    CmHirLibraryBinding alias;
    CmHirLibraryPathSegment direct_path[2];
    CmHirLibraryPathSegment alias_path[2];
    CmHirLibraryValue direct_value;
    CmHirLibraryValue alias_value;
    const CmHirItem *item;
    const CmHirType *return_type;
    const CmInternedString *abi;
    ContextLengths lengths;
    uint32_t saved_local;
    uint8_t saved_byte;

    unit_function_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
            == CM_HIR_DECL_METADATA_OK
        && encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

    expectation = expectation_for(&decoded);
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "dep", 221u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
        && result.item_count == 0u
        && result.public_type_entry_count == 0u
        && result.public_value_entry_count == 2u);
    item = find_item(&context, CM_HIR_ITEM_FUNCTION, "breakpoint_like");
    return_type = item == NULL ? NULL : cm_hir_get_type(&context,
        item->data.function_item.signature.return_type);
    abi = item == NULL ? NULL : cm_interner_get(&context.strings,
        item->data.function_item.signature.abi);
    assert(item != NULL && item->visibility.kind == CM_HIR_VIS_PUBLIC
        && item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE
        && item->generic_parameter_count == 0u
        && item->predicate_scope_count == 0u
        && item->predicate_scopes == NULL
        && item->predicate_count == 0u && item->predicates == NULL
        && item->outlives_predicate_count == 0u
        && item->outlives_predicates == NULL
        && item->attribute_count == 0u && item->attributes == NULL
        && item->data.function_item.signature.parameter_count == 0u
        && item->data.function_item.signature.parameters == NULL
        && item->data.function_item.signature.receiver == CM_HIR_RECEIVER_NONE
        && item->data.function_item.signature.safety == CM_HIR_SAFE
        && item->data.function_item.signature.is_const == 0
        && item->data.function_item.signature.is_async == 0
        && item->data.function_item.signature.is_variadic == 0
        && item->data.function_item.body == CM_HIR_BODY_NONE
        && item->data.function_item.has_default_body == 0
        && return_type != NULL
        && return_type->kind == CM_HIR_TYPE_UNIT_KIND
        && abi != NULL && abi->len == sizeof("Rust") - 1u
        && memcmp(abi->bytes, "Rust", sizeof("Rust") - 1u) == 0
        && cm_hir_get_body(&context,
            item->data.function_item.body) == NULL);

    direct = lookup_value_binding(&artifact, "breakpoint_like");
    alias = lookup_value_binding(&artifact, "breakpoint_alias");
    assert(direct.kind == CM_HIR_LIBRARY_BINDING_VALUE
        && alias.kind == CM_HIR_LIBRARY_BINDING_VALUE
        && cm_hir_def_id_equal(direct.definition, item->definition)
        && cm_hir_def_id_equal(alias.definition, item->definition));
    direct_path[0].bytes = (const unsigned char *)"dep";
    direct_path[0].length = sizeof("dep") - 1u;
    direct_path[1].bytes = (const unsigned char *)"breakpoint_like";
    direct_path[1].length = sizeof("breakpoint_like") - 1u;
    alias_path[0] = direct_path[0];
    alias_path[1].bytes = (const unsigned char *)"breakpoint_alias";
    alias_path[1].length = sizeof("breakpoint_alias") - 1u;
    memset(&direct_value, 0, sizeof(direct_value));
    memset(&alias_value, 0, sizeof(alias_value));
    assert(cm_hir_library_artifact_lookup_value(&artifact, direct_path, 2u,
            &direct_value) == CM_HIR_LIBRARY_OK
        && cm_hir_library_artifact_lookup_value(&artifact, alias_path, 2u,
            &alias_value) == CM_HIR_LIBRARY_OK
        && direct_value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        && alias_value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        && direct_value.data.function.generic_parameter_start
            == CM_HIR_GENERIC_PARAM_NONE
        && alias_value.data.function.generic_parameter_start
            == CM_HIR_GENERIC_PARAM_NONE
        && direct_value.data.function.generic_parameter_count == 0u
        && alias_value.data.function.generic_parameter_count == 0u
        && direct_value.data.function.predicate_count == 0u
        && alias_value.data.function.predicate_count == 0u
        && direct_value.data.function.parameter_count == 0u
        && alias_value.data.function.parameter_count == 0u
        && direct_value.data.function.parameter_types == NULL
        && alias_value.data.function.parameter_types == NULL
        && direct_value.data.function.return_type
            == item->data.function_item.signature.return_type
        && alias_value.data.function.return_type
            == item->data.function_item.signature.return_type
        && direct_value.data.function.is_const == 0
        && alias_value.data.function.is_const == 0
        && cm_hir_def_id_equal(direct_value.definition, item->definition)
        && cm_hir_def_id_equal(alias_value.definition, item->definition));
    test_unit_function_fresh_consumer(&context, &artifact);

    lengths = context_lengths(&context);
    assert(cm_hir_library_artifact_identity(&artifact, &identity));
    saved_local = decoded.values[0].generic_start;
    decoded.values[0].generic_start = 1u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 222u);
    decoded.values[0].generic_start = saved_local;

    saved_local = decoded.values[0].generic_count;
    decoded.values[0].generic_count = 1u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 223u);
    decoded.values[0].generic_count = saved_local;

    saved_local = decoded.values[0].parameter_count;
    decoded.values[0].parameter_count = 1u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 224u);
    decoded.values[0].parameter_count = saved_local;

    saved_byte = decoded.values[0].has_body;
    decoded.values[0].has_body = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 225u);
    decoded.values[0].has_body = saved_byte;

    saved_byte = decoded.values[0].is_const;
    decoded.values[0].is_const = 1u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 226u);
    decoded.values[0].is_const = saved_byte;

    saved_byte = decoded.types[0].primitive;
    decoded.types[0].primitive = CM_HIR_DECL_PRIMITIVE_BOOL;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 227u);
    decoded.types[0].primitive = saved_byte;

    saved_local = decoded.namespace_entries[1].target_local;
    decoded.namespace_entries[1].target_local = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 228u);
    decoded.namespace_entries[1].target_local = saved_local;

    saved_byte = decoded.namespace_entries[1].namespace_kind;
    decoded.namespace_entries[1].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 229u);
    decoded.namespace_entries[1].namespace_kind = saved_byte;

    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "bad-name", 230u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_ARTIFACT_FAILURE
        && result.library_status == CM_HIR_LIBRARY_INVALID_ARGUMENT);
    assert_context_lengths(&context, lengths);
    assert_artifact_identity_same(&artifact, &identity);

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

int main(void)
{
    test_materialize_decode_and_consume();
    test_item_materialize_and_consume();
    test_alias_materialize_and_consume();
    test_composite_materialize_and_consume();
    test_const_materialize_and_consume();
    test_static_materialize_and_consume();
    test_primitive_materialize_and_consume();
    test_aggregate_materialize_and_consume();
    test_layout_wide_enum_reprs();
    test_layout_materialize_and_consume();
    test_type_id_materialize_and_consume();
    test_type_name_materialize_and_consume();
    test_unit_function_materialize_and_consume();
    test_enum_materialize_and_restore_scope();
    test_default_enum_materialize_and_variant_reexports();
    test_option_tuple_materialize_and_consume();
    test_into_iter_materialize_and_consume();
    test_from_fn_materialize_and_consume();
    test_try_from_fn_materialize_and_consume();
    test_repeat_materialize_and_consume();
    test_non_drop_marker_materialize();
    test_from_mut_materialize_and_consume();
    test_from_ref_materialize_and_consume();
    test_any_method_materialize_and_restore();
    test_associated_method_materialize_and_restore();
    return 0;
}
