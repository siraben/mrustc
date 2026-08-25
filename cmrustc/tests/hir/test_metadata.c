#include "cm/hir/metadata.h"
#include "cm/hir/lower.h"
#include "cm/hir/trait_solver.h"
#include "cm/hir/type_alias.h"

#include "../../src/hir/library_internal.h"
#include "../../src/hir/metadata_codec.h"

#include "cm/alloc.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#define TEST_METADATA_CRC_OFFSET 32u

static const unsigned char generic_function_source[] =
    "pub fn select<'a, T, const N: usize>(value: &'a T, "
        "bytes: [u8; N]) -> &'a T { let _ = bytes; value }\n";

static const unsigned char predicate_function_source[] =
    "trait FnOnce<Args> { type Output; }\n"
    "trait FnMut<Args>: FnOnce<Args> {}\n"
    "trait Fn<Args>: FnMut<Args> {}\n"
    "trait Copy {}\n"
    "trait Pointee { type Metadata; }\n"
    "trait PointeeSized {}\n"
    "trait Thin = Pointee<Metadata = ()> + PointeeSized;\n"
    "pub fn constrained<Ret, C>(cond: C) -> C "
        "where C: Fn(&Ret) -> bool + Copy + 'static { cond }\n"
    "pub fn scoped<Ret, C>(cond: C) -> C "
        "where for<'a> C: Fn(&'a Ret) -> bool { cond }\n"
    "pub const fn null<T: PointeeSized + Thin>() {}\n";

static const unsigned char constrained_function_source[] =
    "trait FnOnce<Args> { type Output; }\n"
    "trait FnMut<Args>: FnOnce<Args> {}\n"
    "trait Fn<Args>: FnMut<Args> {}\n"
    "trait Copy {}\n"
    "pub const fn constrained<Ret, C>(cond: C) -> C "
        "where C: Fn(&Ret) -> bool + Copy + 'static { cond }\n";

static const unsigned char early_predicate_function_source[] =
    "trait Uses<T> {}\n"
    "pub fn rejected<'a, T, C>(cond: C) -> C "
        "where C: Uses<&'a T> { cond }\n";

static const unsigned char duplicate_const_callable_source[] =
    "trait FnOnce<Args> { type Output; }\n"
    "pub const fn const_eval_select<ARG, F, G, RET>("
        "_arg: ARG, _called_in_const: F, _called_at_rt: G) -> RET "
        "where G: FnOnce<ARG, Output = RET>, "
        "F: const FnOnce<ARG, Output = RET> { loop {} }\n";

static const unsigned char carrying_mul_add_source[] =
    "trait Sized {}\n"
    "trait Clone: Sized {}\n"
    "trait Copy: Clone {}\n"
    "trait CarryingMulAdd: Copy { type Unsigned; }\n"
    "pub const fn carrying_mul_add<T, U>(_lhs: T, _rhs: T, _carry: U) "
        "-> (U, T) where T: ~const CarryingMulAdd<Unsigned = U> "
        "{ loop {} }\n";

static const unsigned char thin_predicate_source[] =
    "trait PointeeSized {}\n"
    "trait Pointee: PointeeSized { type Metadata; }\n"
    "trait Thin = Pointee<Metadata = ()> + PointeeSized;\n"
    "pub const fn null<T: PointeeSized + Thin>() -> *const T { loop {} }\n";

static const unsigned char thin_predicate_swapped_source[] =
    "trait PointeeSized {}\n"
    "trait Pointee: PointeeSized { type Metadata; }\n"
    "trait Thin = Pointee<Metadata = ()> + PointeeSized;\n"
    "pub const fn null<T: Thin + PointeeSized>() -> *const T { loop {} }\n";

static const unsigned char multi_predicate_source_a[] =
    "trait Pair<A, B> { type First; type Second; }\n"
    "trait Left { type Item; }\n"
    "trait Right { type Item; }\n"
    "trait Marker { type Item; }\n"
    "pub fn multi<T, U, C>(cond: C) -> C "
        "where C: Pair<T, U, Second = U, First = T>, "
        "T: Marker<Item = T> + 'static, "
        "U: Marker<Item = U> + 'static { cond }\n"
    "pub fn both<C>(cond: C) -> C "
        "where C: Left<Item = ()> + Right<Item = ()> { cond }\n";

static const unsigned char multi_predicate_source_b[] =
    "trait Pair<A, B> { type First; type Second; }\n"
    "trait Left { type Item; }\n"
    "trait Right { type Item; }\n"
    "trait Marker { type Item; }\n"
    "pub fn multi<T, U, C>(cond: C) -> C "
        "where C: Pair<T, U, First = T, Second = U>, "
        "U: Marker<Item = U> + 'static, "
        "T: Marker<Item = T> + 'static { cond }\n"
    "pub fn both<C>(cond: C) -> C "
        "where C: Right<Item = ()> + Left<Item = ()> { cond }\n";

typedef struct ProducerFixture {
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    CmHirModuleId child_module;
    CmHirModuleId zeta_module;
    CmHirDefId root_definition;
    CmHirDefId child_definition;
    CmHirDefId zeta_definition;
    CmHirDefId root_api;
    CmHirDefId child_api;
    CmHirDefId wrapper;
    CmHirDefId choice;
    CmHirDefId alias;
    CmHirLibraryArtifact artifact;
} ProducerFixture;

typedef struct ParsedProducerFixture {
    CmHirContext context;
    CmHirLibraryArtifact artifact;
} ParsedProducerFixture;

typedef struct DeclarationProducerFixture {
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirTypeId u32_type;
    CmHirTypeId bool_type;
    CmHirDefId shared_type;
    CmHirDefId function_value;
    CmHirDefId const_value;
    CmHirDefId static_value;
} DeclarationProducerFixture;

typedef struct ContextLengths {
    size_t crates;
    size_t modules;
    size_t items;
    size_t bodies;
    size_t expressions;
    size_t types;
    size_t generic_parameters;
    size_t definitions;
    size_t prebound_associated_types;
    size_t strings;
} ContextLengths;

typedef enum SemanticMetadataCorruption {
    SEMANTIC_CORRUPT_CLOSED = 0,
    SEMANTIC_CORRUPT_TRAIT_HANDLE,
    SEMANTIC_CORRUPT_SELF_TYPE_HANDLE,
    SEMANTIC_CORRUPT_NEGATIVE_POLARITY,
    SEMANTIC_CORRUPT_NEGATIVE_SAFETY
} SemanticMetadataCorruption;

static void recompute_metadata_crc(CmByteBuf *encoded);
static void replace_v24_section(CmByteBuf *encoded,
    const unsigned char tag[4], const CmByteBuf *replacement_contents);
static void corrupt_trait_universe(CmByteBuf *encoded,
    SemanticMetadataCorruption corruption);
static void assert_sentinel_preserved(const CmHirContext *context,
    const CmHirLibraryArtifact *artifact, ContextLengths lengths,
    const CmHirLibraryArtifactIdentity *identity);

static CmSpan test_span(uint32_t start, uint32_t end)
{
    CmSpan span;

    span.source = 1u;
    span.start = start;
    span.end = end;
    return span;
}

static CmHirDefId add_extern_type(CmHirContext *context,
    CmHirCrateId crate_id, CmHirModuleId owner, const char *name,
    uint32_t start)
{
    CmHirDefId definition;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(context, crate_id,
        CM_HIR_ITEM_EXTERN_TYPE, test_span(start, start + 1u),
        &definition) == CM_HIR_OK);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_EXTERN_TYPE;
    item.definition = definition;
    item.owner_module = owner;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(context, name);
    item.visibility.kind = CM_HIR_VIS_PUBLIC;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(start, start + 1u);
    assert(cm_hir_add_item(context, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirTypeId add_integer_type(CmHirContext *context,
    CmHirIntType kind, uint32_t start)
{
    CmHirType type;
    CmHirTypeId id;

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INTEGER_KIND;
    type.span = test_span(start, start + 1u);
    type.data.integer_type.kind = kind;
    assert(cm_hir_add_type(context, &type, &id) == CM_HIR_OK);
    return id;
}

static CmHirTypeId add_bool_type(CmHirContext *context, uint32_t start)
{
    CmHirType type;
    CmHirTypeId id;

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_BOOL_KIND;
    type.span = test_span(start, start + 1u);
    assert(cm_hir_add_type(context, &type, &id) == CM_HIR_OK);
    return id;
}

static CmHirDefId add_metadata_trait(CmHirContext *context,
    CmHirCrateId crate_id, CmHirModuleId owner, const char *name,
    int is_auto, uint32_t start)
{
    CmHirDefId definition;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(context, crate_id,
        CM_HIR_ITEM_TRAIT, test_span(start, start + 1u), &definition)
        == CM_HIR_OK);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_TRAIT;
    item.definition = definition;
    item.owner_module = owner;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(context, name);
    item.visibility.kind = CM_HIR_VIS_PUBLIC;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(start, start + 1u);
    item.data.trait_item.safety = CM_HIR_SAFE;
    item.data.trait_item.is_auto = is_auto;
    assert(cm_hir_add_item(context, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_metadata_impl(CmHirContext *context,
    CmHirCrateId crate_id, CmHirModuleId owner,
    CmHirDefId trait_definition, CmHirTypeId self_type, int is_negative,
    uint32_t start)
{
    CmHirDefId definition;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(context, crate_id,
        CM_HIR_ITEM_IMPL, test_span(start, start + 1u), &definition)
        == CM_HIR_OK);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_IMPL;
    item.definition = definition;
    item.owner_module = owner;
    item.parent_definition = cm_hir_def_id_none();
    item.name = CM_INTERN_ID_NONE;
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(start, start + 1u);
    item.data.impl_item.self_type = self_type;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = trait_definition;
    item.data.impl_item.safety = CM_HIR_SAFE;
    item.data.impl_item.is_negative = is_negative;
    assert(cm_hir_add_item(context, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static void add_metadata_generic_impl(CmHirContext *context,
    CmHirCrateId crate_id, CmHirModuleId owner,
    CmHirDefId trait_definition, uint32_t start)
{
    CmHirDefId definition;
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_id;
    CmHirType parameter_type;
    CmHirTypeId self_type;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(context, crate_id,
        CM_HIR_ITEM_IMPL, test_span(start, start + 4u), &definition)
        == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = definition;
    parameter.name = cm_hir_intern(context, "T");
    parameter.span = test_span(start + 1u, start + 2u);
    assert(cm_hir_add_generic_param(context, &parameter, &parameter_id)
        == CM_HIR_OK);
    memset(&parameter_type, 0, sizeof(parameter_type));
    parameter_type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    parameter_type.span = test_span(start + 2u, start + 3u);
    parameter_type.data.parameter_type.parameter = parameter_id;
    assert(cm_hir_add_type(context, &parameter_type, &self_type)
        == CM_HIR_OK);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_IMPL;
    item.definition = definition;
    item.owner_module = owner;
    item.parent_definition = cm_hir_def_id_none();
    item.name = CM_INTERN_ID_NONE;
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(start, start + 4u);
    item.generic_parameter_start = parameter_id;
    item.generic_parameter_count = 1u;
    item.data.impl_item.self_type = self_type;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = trait_definition;
    item.data.impl_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(context, &item, &item_id) == CM_HIR_OK);
}

static void add_metadata_predicate_impl(CmHirContext *context,
    CmHirCrateId crate_id, CmHirModuleId owner,
    CmHirDefId implemented_trait, CmHirDefId predicate_trait,
    CmHirTypeId self_type, uint32_t start)
{
    CmHirDefId definition;
    CmHirTraitPredicate predicate;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(context, crate_id,
        CM_HIR_ITEM_IMPL, test_span(start, start + 3u), &definition)
        == CM_HIR_OK);
    memset(&predicate, 0, sizeof(predicate));
    predicate.subject = self_type;
    predicate.trait_type.definition = predicate_trait;
    predicate.span = test_span(start + 1u, start + 2u);
    predicate.modifier = CM_HIR_PREDICATE_REQUIRED;
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_IMPL;
    item.definition = definition;
    item.owner_module = owner;
    item.parent_definition = cm_hir_def_id_none();
    item.name = CM_INTERN_ID_NONE;
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(start, start + 3u);
    item.predicates = &predicate;
    item.predicate_count = 1u;
    item.data.impl_item.self_type = self_type;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = implemented_trait;
    item.data.impl_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(context, &item, &item_id) == CM_HIR_OK);
}

static CmHirDefId add_wrapper(CmHirContext *context, CmHirCrateId crate_id,
    CmHirModuleId owner, uint32_t start)
{
    CmHirDefId definition;
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_id;
    CmHirGenericArg default_argument;
    CmHirTypeId u32_type;
    CmHirType parameter_type_value;
    CmHirTypeId parameter_type;
    CmHirField field;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(context, crate_id,
        CM_HIR_ITEM_STRUCT, test_span(start, start + 9u), &definition)
        == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = definition;
    parameter.index = 0u;
    parameter.name = cm_hir_intern(context, "T");
    parameter.span = test_span(start + 1u, start + 2u);
    assert(cm_hir_add_generic_param(context, &parameter, &parameter_id)
        == CM_HIR_OK);
    u32_type = add_integer_type(context, CM_HIR_INT_U32, start + 2u);
    memset(&default_argument, 0, sizeof(default_argument));
    default_argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    default_argument.data.type = u32_type;
    assert(cm_hir_set_generic_param_default(context, parameter_id,
        &default_argument) == CM_HIR_OK);
    memset(&parameter_type_value, 0, sizeof(parameter_type_value));
    parameter_type_value.kind = CM_HIR_TYPE_PARAMETER_KIND;
    parameter_type_value.span = test_span(start + 3u, start + 4u);
    parameter_type_value.data.parameter_type.parameter = parameter_id;
    assert(cm_hir_add_type(context, &parameter_type_value, &parameter_type)
        == CM_HIR_OK);
    memset(&field, 0, sizeof(field));
    field.name = CM_INTERN_ID_NONE;
    field.type = parameter_type;
    field.visibility.kind = CM_HIR_VIS_PUBLIC;
    field.visibility.restriction = cm_hir_def_id_none();
    field.span = test_span(start + 4u, start + 5u);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_STRUCT;
    item.definition = definition;
    item.owner_module = owner;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(context, "Wrapper");
    item.visibility.kind = CM_HIR_VIS_PUBLIC;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(start, start + 9u);
    item.generic_parameter_start = parameter_id;
    item.generic_parameter_count = 1u;
    item.data.aggregate_item.form = CM_HIR_AGGREGATE_TUPLE;
    item.data.aggregate_item.fields = &field;
    item.data.aggregate_item.field_count = 1u;
    assert(cm_hir_add_item(context, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_choice(CmHirContext *context, CmHirCrateId crate_id,
    CmHirModuleId owner, uint32_t start)
{
    CmHirDefId definition;
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_id;
    CmHirType parameter_type_value;
    CmHirTypeId parameter_type;
    CmHirField some_field;
    CmHirVariant variants[2];
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(context, crate_id,
        CM_HIR_ITEM_ENUM, test_span(start, start + 9u), &definition)
        == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = definition;
    parameter.index = 0u;
    parameter.name = cm_hir_intern(context, "T");
    parameter.span = test_span(start + 1u, start + 2u);
    assert(cm_hir_add_generic_param(context, &parameter, &parameter_id)
        == CM_HIR_OK);
    memset(&parameter_type_value, 0, sizeof(parameter_type_value));
    parameter_type_value.kind = CM_HIR_TYPE_PARAMETER_KIND;
    parameter_type_value.span = test_span(start + 2u, start + 3u);
    parameter_type_value.data.parameter_type.parameter = parameter_id;
    assert(cm_hir_add_type(context, &parameter_type_value, &parameter_type)
        == CM_HIR_OK);
    memset(&some_field, 0, sizeof(some_field));
    some_field.name = CM_INTERN_ID_NONE;
    some_field.type = parameter_type;
    some_field.visibility.kind = CM_HIR_VIS_PRIVATE;
    some_field.visibility.restriction = cm_hir_def_id_none();
    some_field.span = test_span(start + 3u, start + 4u);
    memset(variants, 0, sizeof(variants));
    variants[0].definition = cm_hir_def_id_none();
    variants[0].name = cm_hir_intern(context, "None");
    variants[0].form = CM_HIR_AGGREGATE_UNIT;
    variants[0].span = test_span(start + 4u, start + 5u);
    variants[1].definition = cm_hir_def_id_none();
    variants[1].name = cm_hir_intern(context, "Some");
    variants[1].form = CM_HIR_AGGREGATE_TUPLE;
    variants[1].fields = &some_field;
    variants[1].field_count = 1u;
    variants[1].span = test_span(start + 5u, start + 6u);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_ENUM;
    item.definition = definition;
    item.owner_module = owner;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(context, "Choice");
    item.visibility.kind = CM_HIR_VIS_PUBLIC;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(start, start + 9u);
    item.generic_parameter_start = parameter_id;
    item.generic_parameter_count = 1u;
    item.data.enum_item.variants = variants;
    item.data.enum_item.variant_count = 2u;
    assert(cm_hir_add_item(context, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_alias(CmHirContext *context, CmHirCrateId crate_id,
    CmHirModuleId owner, CmHirDefId wrapper, uint32_t start)
{
    CmHirTypeId u32_type;
    CmHirGenericArg argument;
    CmHirType application;
    CmHirTypeId application_type;
    CmHirDefId definition;
    CmHirItem item;
    CmHirItemId item_id;

    u32_type = add_integer_type(context, CM_HIR_INT_U32, start + 1u);
    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    argument.data.type = u32_type;
    memset(&application, 0, sizeof(application));
    application.kind = CM_HIR_TYPE_ADT_KIND;
    application.span = test_span(start + 2u, start + 3u);
    application.data.named_type.definition = wrapper;
    application.data.named_type.arguments = &argument;
    application.data.named_type.argument_count = 1u;
    assert(cm_hir_add_type(context, &application, &application_type)
        == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(context, crate_id,
        CM_HIR_ITEM_TYPE_ALIAS, test_span(start, start + 9u), &definition)
        == CM_HIR_OK);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_TYPE_ALIAS;
    item.definition = definition;
    item.owner_module = owner;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(context, "Alias");
    item.visibility.kind = CM_HIR_VIS_PUBLIC;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(start, start + 9u);
    item.data.type_alias_item.target = application_type;
    item.data.type_alias_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(context, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirLibraryBinding module_binding(CmHirDefId definition)
{
    CmHirLibraryBinding binding;

    memset(&binding, 0, sizeof(binding));
    binding.kind = CM_HIR_LIBRARY_BINDING_MODULE;
    binding.definition = definition;
    binding.type_kind = CM_HIR_TYPE_ERROR_KIND;
    binding.primitive_kind = CM_HIR_PRIMITIVE_NONE;
    return binding;
}

static CmHirLibraryBinding extern_type_binding(CmHirDefId definition)
{
    CmHirLibraryBinding binding;

    memset(&binding, 0, sizeof(binding));
    binding.kind = CM_HIR_LIBRARY_BINDING_TYPE;
    binding.definition = definition;
    binding.type_kind = CM_HIR_TYPE_FOREIGN_KIND;
    binding.primitive_kind = CM_HIR_PRIMITIVE_NONE;
    return binding;
}

static CmHirLibraryBinding type_binding(CmHirDefId definition,
    CmHirTypeKind type_kind)
{
    CmHirLibraryBinding binding;

    memset(&binding, 0, sizeof(binding));
    binding.kind = CM_HIR_LIBRARY_BINDING_TYPE;
    binding.definition = definition;
    binding.type_kind = type_kind;
    binding.primitive_kind = CM_HIR_PRIMITIVE_NONE;
    return binding;
}

static CmHirLibraryBinding primitive_binding(CmHirPrimitiveKind primitive)
{
    CmHirLibraryBinding binding;

    memset(&binding, 0, sizeof(binding));
    binding.kind = CM_HIR_LIBRARY_BINDING_PRIMITIVE;
    binding.definition = cm_hir_def_id_none();
    binding.type_kind = CM_HIR_TYPE_ERROR_KIND;
    binding.primitive_kind = primitive;
    return binding;
}

static CmHirLibraryBinding value_binding(CmHirDefId definition,
    CmHirLibraryValueKind kind)
{
    CmHirLibraryBinding binding;

    memset(&binding, 0, sizeof(binding));
    binding.kind = CM_HIR_LIBRARY_BINDING_VALUE;
    binding.definition = definition;
    binding.type_kind = CM_HIR_TYPE_ERROR_KIND;
    binding.primitive_kind = CM_HIR_PRIMITIVE_NONE;
    binding.value_kind = kind;
    return binding;
}

static void add_entry(CmHirLibraryOwnedData *owned, size_t module_index,
    const char *name, CmHirLibraryBinding binding)
{
    assert(cm_hir_library_owned_data_add_entry(owned, module_index,
        (const unsigned char *)name, strlen(name), &binding)
        == CM_HIR_LIBRARY_OK);
}

static void declaration_producer_init(DeclarationProducerFixture *fixture)
{
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    const CmHirModule *module;
    CmHirLibraryOwnedData owned;
    CmHirLibraryArtifactResult restored;
    CmHirLibraryValue value;
    CmHirTypeId parameter_types[1];
    size_t root_index;

    memset(fixture, 0, sizeof(*fixture));
    cm_hir_context_init(&fixture->context);
    assert(cm_hir_create_crate(&fixture->context,
        cm_hir_intern(&fixture->context, "declaration_wire"),
        CM_HIR_EDITION_2024, test_span(1u, 100u), &crate_id,
        &root_module) == CM_HIR_OK);
    module = cm_hir_get_module(&fixture->context, root_module);
    assert(module != NULL);
    fixture->shared_type = add_extern_type(&fixture->context, crate_id,
        root_module, "Shared", 10u);
    fixture->u32_type = add_integer_type(&fixture->context,
        CM_HIR_INT_U32, 11u);
    fixture->bool_type = add_bool_type(&fixture->context, 12u);
    assert(cm_hir_reserve_item_definition_as(&fixture->context, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(20u, 21u),
        &fixture->function_value) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&fixture->context, crate_id,
        CM_HIR_ITEM_CONST, test_span(22u, 23u), &fixture->const_value)
        == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&fixture->context, crate_id,
        CM_HIR_ITEM_STATIC, test_span(24u, 25u), &fixture->static_value)
        == CM_HIR_OK);

    cm_hir_library_owned_data_init(&owned);
    assert(cm_hir_library_owned_data_add_module(&owned,
        module->definition, &root_index) == CM_HIR_LIBRARY_OK);
    memset(&value, 0, sizeof(value));
    value.definition = fixture->static_value;
    value.kind = CM_HIR_LIBRARY_VALUE_STATIC;
    value.data.value.type = fixture->u32_type;
    value.data.value.mutability = CM_HIR_MUTABLE;
    assert(cm_hir_library_owned_data_add_value(&owned, &value)
        == CM_HIR_LIBRARY_OK);
    parameter_types[0] = fixture->u32_type;
    memset(&value, 0, sizeof(value));
    value.definition = fixture->function_value;
    value.kind = CM_HIR_LIBRARY_VALUE_FUNCTION;
    value.data.function.parameter_types = parameter_types;
    value.data.function.parameter_count = 1u;
    value.data.function.return_type = fixture->bool_type;
    value.data.function.abi = cm_hir_intern(&fixture->context, "C");
    value.data.function.safety = CM_HIR_UNSAFE;
    value.data.function.is_const = 0;
    value.data.function.is_async = 0;
    value.data.function.is_variadic = 0;
    assert(cm_hir_library_owned_data_add_value(&owned, &value)
        == CM_HIR_LIBRARY_OK);
    memset(&value, 0, sizeof(value));
    value.definition = fixture->const_value;
    value.kind = CM_HIR_LIBRARY_VALUE_CONST;
    value.data.value.type = fixture->u32_type;
    value.data.value.mutability = CM_HIR_IMMUTABLE;
    assert(cm_hir_library_owned_data_add_value(&owned, &value)
        == CM_HIR_LIBRARY_OK);

    add_entry(&owned, root_index, "Shared", extern_type_binding(
        fixture->shared_type));
    add_entry(&owned, root_index, "Shared", value_binding(
        fixture->function_value, CM_HIR_LIBRARY_VALUE_FUNCTION));
    add_entry(&owned, root_index, "LIMIT", value_binding(
        fixture->const_value, CM_HIR_LIBRARY_VALUE_CONST));
    add_entry(&owned, root_index, "COUNTER", value_binding(
        fixture->static_value, CM_HIR_LIBRARY_VALUE_STATIC));
    cm_hir_library_artifact_init(&fixture->artifact);
    restored = cm_hir_library_artifact_restore_owned(&fixture->artifact,
        &fixture->context, crate_id, module->definition, "producer", &owned);
    assert(restored.status == CM_HIR_LIBRARY_OK);
    assert(restored.public_type_entry_count == 1u);
    assert(restored.public_value_entry_count == 3u);
    cm_hir_library_owned_data_destroy(&owned);
}

static void declaration_producer_destroy(
    DeclarationProducerFixture *fixture)
{
    cm_hir_library_artifact_destroy(&fixture->artifact);
    cm_hir_context_destroy(&fixture->context);
}

static void producer_init(ProducerFixture *fixture, int reverse_order)
{
    CmHirCrateId dummy_crate;
    CmHirModuleId dummy_root;
    const CmHirModule *module;
    CmHirLibraryOwnedData owned;
    CmHirLibraryArtifactResult restored;
    size_t root_index;
    size_t child_index;
    size_t zeta_index;

    memset(fixture, 0, sizeof(*fixture));
    cm_hir_context_init(&fixture->context);
    assert(cm_hir_create_crate(&fixture->context,
        cm_hir_intern(&fixture->context, "dummy"), CM_HIR_EDITION_2021,
        test_span(0u, 1u), &dummy_crate, &dummy_root) == CM_HIR_OK);
    assert(cm_hir_create_crate(&fixture->context,
        cm_hir_intern(&fixture->context, "wire_dep"), CM_HIR_EDITION_2024,
        test_span(2u, 100u), &fixture->crate_id, &fixture->root_module)
        == CM_HIR_OK);
    assert(fixture->crate_id != dummy_crate);
    if (reverse_order) {
        assert(cm_hir_add_module(&fixture->context, fixture->crate_id,
            fixture->root_module, cm_hir_intern(&fixture->context, "zeta"),
            test_span(21u, 29u), &fixture->zeta_module) == CM_HIR_OK);
        assert(cm_hir_add_module(&fixture->context, fixture->crate_id,
            fixture->root_module,
            cm_hir_intern(&fixture->context, "child"),
            test_span(10u, 20u), &fixture->child_module) == CM_HIR_OK);
    } else {
        assert(cm_hir_add_module(&fixture->context, fixture->crate_id,
            fixture->root_module,
            cm_hir_intern(&fixture->context, "child"),
            test_span(10u, 20u), &fixture->child_module) == CM_HIR_OK);
        assert(cm_hir_add_module(&fixture->context, fixture->crate_id,
            fixture->root_module, cm_hir_intern(&fixture->context, "zeta"),
            test_span(21u, 29u), &fixture->zeta_module) == CM_HIR_OK);
    }
    module = cm_hir_get_module(&fixture->context, fixture->root_module);
    assert(module != NULL);
    fixture->root_definition = module->definition;
    module = cm_hir_get_module(&fixture->context, fixture->child_module);
    assert(module != NULL);
    fixture->child_definition = module->definition;
    module = cm_hir_get_module(&fixture->context, fixture->zeta_module);
    assert(module != NULL);
    fixture->zeta_definition = module->definition;
    if (reverse_order) {
        fixture->child_api = add_extern_type(&fixture->context,
            fixture->crate_id, fixture->child_module, "ChildApi", 40u);
        fixture->root_api = add_extern_type(&fixture->context,
            fixture->crate_id, fixture->root_module, "RootApi", 30u);
        fixture->choice = add_choice(&fixture->context, fixture->crate_id,
            fixture->root_module, 70u);
        fixture->wrapper = add_wrapper(&fixture->context, fixture->crate_id,
            fixture->root_module, 50u);
    } else {
        fixture->root_api = add_extern_type(&fixture->context,
            fixture->crate_id, fixture->root_module, "RootApi", 30u);
        fixture->child_api = add_extern_type(&fixture->context,
            fixture->crate_id, fixture->child_module, "ChildApi", 40u);
        fixture->wrapper = add_wrapper(&fixture->context, fixture->crate_id,
            fixture->root_module, 50u);
        fixture->choice = add_choice(&fixture->context, fixture->crate_id,
            fixture->root_module, 70u);
    }
    fixture->alias = add_alias(&fixture->context, fixture->crate_id,
        fixture->root_module, fixture->wrapper, 90u);

    cm_hir_library_owned_data_init(&owned);
    if (reverse_order) {
        assert(cm_hir_library_owned_data_add_module(&owned,
            fixture->zeta_definition, &zeta_index) == CM_HIR_LIBRARY_OK);
        assert(cm_hir_library_owned_data_add_module(&owned,
            fixture->child_definition, &child_index) == CM_HIR_LIBRARY_OK);
        assert(cm_hir_library_owned_data_add_module(&owned,
            fixture->root_definition, &root_index) == CM_HIR_LIBRARY_OK);
        add_entry(&owned, child_index, "bool",
            primitive_binding(CM_HIR_PRIMITIVE_BOOL));
        add_entry(&owned, child_index, "Renamed",
            type_binding(fixture->alias,
                CM_HIR_TYPE_ALIAS_APPLICATION_KIND));
        add_entry(&owned, child_index, "ChildApi",
            extern_type_binding(fixture->child_api));
        add_entry(&owned, root_index, "usize",
            primitive_binding(CM_HIR_PRIMITIVE_USIZE));
        add_entry(&owned, root_index, "RootApi",
            extern_type_binding(fixture->root_api));
        add_entry(&owned, root_index, "Wrapper",
            type_binding(fixture->wrapper, CM_HIR_TYPE_ADT_KIND));
        add_entry(&owned, root_index, "Choice",
            type_binding(fixture->choice, CM_HIR_TYPE_ADT_KIND));
        add_entry(&owned, root_index, "Alias",
            type_binding(fixture->alias,
                CM_HIR_TYPE_ALIAS_APPLICATION_KIND));
        add_entry(&owned, root_index, "child",
            module_binding(fixture->child_definition));
        add_entry(&owned, root_index, "zeta",
            module_binding(fixture->zeta_definition));
    } else {
        assert(cm_hir_library_owned_data_add_module(&owned,
            fixture->root_definition, &root_index) == CM_HIR_LIBRARY_OK);
        assert(cm_hir_library_owned_data_add_module(&owned,
            fixture->child_definition, &child_index) == CM_HIR_LIBRARY_OK);
        assert(cm_hir_library_owned_data_add_module(&owned,
            fixture->zeta_definition, &zeta_index) == CM_HIR_LIBRARY_OK);
        add_entry(&owned, root_index, "child",
            module_binding(fixture->child_definition));
        add_entry(&owned, root_index, "RootApi",
            extern_type_binding(fixture->root_api));
        add_entry(&owned, root_index, "Alias",
            type_binding(fixture->alias,
                CM_HIR_TYPE_ALIAS_APPLICATION_KIND));
        add_entry(&owned, root_index, "Choice",
            type_binding(fixture->choice, CM_HIR_TYPE_ADT_KIND));
        add_entry(&owned, root_index, "Wrapper",
            type_binding(fixture->wrapper, CM_HIR_TYPE_ADT_KIND));
        add_entry(&owned, root_index, "usize",
            primitive_binding(CM_HIR_PRIMITIVE_USIZE));
        add_entry(&owned, child_index, "ChildApi",
            extern_type_binding(fixture->child_api));
        add_entry(&owned, child_index, "Renamed",
            type_binding(fixture->alias,
                CM_HIR_TYPE_ALIAS_APPLICATION_KIND));
        add_entry(&owned, child_index, "bool",
            primitive_binding(CM_HIR_PRIMITIVE_BOOL));
        add_entry(&owned, root_index, "zeta",
            module_binding(fixture->zeta_definition));
    }
    cm_hir_library_artifact_init(&fixture->artifact);
    restored = cm_hir_library_artifact_restore_owned(&fixture->artifact,
        &fixture->context, fixture->crate_id, fixture->root_definition,
        "producer", &owned);
    assert(restored.status == CM_HIR_LIBRARY_OK);
    assert(restored.module_count == 3u);
    assert(restored.public_type_entry_count == 8u);
    cm_hir_library_owned_data_destroy(&owned);
}

static void producer_destroy(ProducerFixture *fixture)
{
    cm_hir_library_artifact_destroy(&fixture->artifact);
    cm_hir_context_destroy(&fixture->context);
}

static int parsed_producer_build(ParsedProducerFixture *fixture,
    const unsigned char *source, size_t source_length,
    size_t expected_modules, size_t expected_entries,
    size_t expected_value_entries, int declaration)
{
    CmSourceSet sources;
    CmSourceId root_source;
    CmCfgSet cfg;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirModuleMap modules;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    CmHirLibraryArtifactResult artifact_result;
    int ok;

    memset(fixture, 0, sizeof(*fixture));
    cm_hir_context_init(&fixture->context);
    cm_hir_library_artifact_init(&fixture->artifact);
    cm_source_set_init(&sources);
    cm_cfg_set_init(&cfg);
    cm_module_graph_init(&graph);
    cm_import_resolver_init(&imports);
    cm_hir_module_map_init(&modules);
    root_source = 0u;
    memset(&graph_result, 0, sizeof(graph_result));
    memset(&import_result, 0, sizeof(import_result));
    memset(&lower_result, 0, sizeof(lower_result));
    memset(&artifact_result, 0, sizeof(artifact_result));
    ok = cm_source_add_memory(&sources, "parsed-metadata/lib.rs", source,
            source_length, &root_source) == CM_SOURCE_OK;
    cm_module_graph_options_init(&graph_options);
    graph_options.edition = CM_EDITION_2024;
    graph_options.cfg = &cfg;
    if (ok) {
        graph_result = cm_module_graph_build(&graph, &sources, root_source,
            &graph_options);
        ok = graph_result.root != CM_MODULE_NONE
            && graph_result.error_count == 0u;
        if (!ok) {
            fprintf(stderr, "parsed metadata graph failed: errors=%lu\n",
                (unsigned long)graph_result.error_count);
        }
    }
    if (ok) {
        import_result = cm_import_resolve(&imports, &graph,
            graph_result.revision);
        ok = import_result.error_count == 0u
            && import_result.revision == graph_result.revision;
        if (!ok) {
            fprintf(stderr, "parsed metadata imports failed: errors=%lu\n",
                (unsigned long)import_result.error_count);
        }
    }
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "wire_dep";
    lower_options.edition = CM_HIR_EDITION_2024;
    lower_options.source = root_source;
    if (ok) {
        lower_result = cm_hir_lower_module_graph(&fixture->context, &graph,
            graph_result.revision, &imports, &modules, &lower_options);
        ok = lower_result.error_count == 0u;
        if (!ok) {
            fprintf(stderr, "parsed metadata HIR failed: %s: %s\n",
                cm_hir_lower_error_kind_name(lower_result.first_error.kind),
                lower_result.first_error.message);
        }
    }
    if (ok) {
        artifact_result = declaration
            ? cm_hir_library_declaration_artifact_build(&fixture->artifact,
                &fixture->context, lower_result.crate_id, &graph,
                graph_result.revision, &modules, "producer")
            : cm_hir_library_artifact_build(&fixture->artifact,
                &fixture->context, lower_result.crate_id, &graph,
                graph_result.revision, &modules, "producer");
        ok = artifact_result.status == CM_HIR_LIBRARY_OK
            && artifact_result.module_count == expected_modules
            && artifact_result.public_type_entry_count == expected_entries
            && artifact_result.public_value_entry_count
                == expected_value_entries;
        if (!ok) {
            fprintf(stderr,
                "parsed metadata capture failed: %s modules=%lu "
                "type-entries=%lu value-entries=%lu\n",
                cm_hir_library_status_name(artifact_result.status),
                (unsigned long)artifact_result.module_count,
                (unsigned long)artifact_result.public_type_entry_count,
                (unsigned long)artifact_result.public_value_entry_count);
        }
    }
    cm_hir_module_map_destroy(&modules);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    if (!ok) {
        cm_hir_library_artifact_destroy(&fixture->artifact);
        cm_hir_context_destroy(&fixture->context);
    }
    return ok;
}

static int parsed_producer_init(ParsedProducerFixture *fixture,
    int reverse_order)
{
    static const unsigned char forward_source[] =
        "unsafe extern \"C\" { type RootApiRaw; }\n"
        "pub use self::RootApiRaw as RootApi;\n"
        "pub struct Wrapper<T = u32>(pub T);\n"
        "pub enum Choice<T> { None, Some(T) }\n"
        "pub union Storage<T> { pub value: T, pub marker: *const T }\n"
        "pub struct Borrowed<'a, T = u16> {\n"
        "    pub value: &'a T,\n"
        "    pub marker: *const T,\n"
        "    pub pair: (bool, u8),\n"
        "    pub array: [u16; 3],\n"
        "}\n"
        "pub type Alias = Wrapper<u32>;\n"
        "pub type BorrowedAlias<'a, T = u16> = Borrowed<'a, T>;\n"
        "pub mod child {\n"
        "    unsafe extern \"C\" { type ChildApiRaw; }\n"
        "    pub use self::ChildApiRaw as ChildApi;\n"
        "    pub use crate::Alias as Renamed;\n"
        "    pub use crate::BorrowedAlias as RenamedBorrowed;\n"
        "    pub use bool;\n"
        "}\n"
        "pub mod zeta {}\n"
        "pub use usize;\n";
    static const unsigned char reverse_source[] =
        "pub use usize;\n"
        "pub mod zeta {}\n"
        "pub mod child {\n"
        "    pub use bool;\n"
        "    pub use crate::BorrowedAlias as RenamedBorrowed;\n"
        "    pub use crate::Alias as Renamed;\n"
        "    pub use self::ChildApiRaw as ChildApi;\n"
        "    unsafe extern \"C\" { type ChildApiRaw; }\n"
        "}\n"
        "pub type BorrowedAlias<'a, T = u16> = Borrowed<'a, T>;\n"
        "pub type Alias = Wrapper<u32>;\n"
        "pub struct Borrowed<'a, T = u16> {\n"
        "    pub value: &'a T,\n"
        "    pub marker: *const T,\n"
        "    pub pair: (bool, u8),\n"
        "    pub array: [u16; 3],\n"
        "}\n"
        "pub union Storage<T> { pub value: T, pub marker: *const T }\n"
        "pub enum Choice<T> { None, Some(T) }\n"
        "pub struct Wrapper<T = u32>(pub T);\n"
        "pub use self::RootApiRaw as RootApi;\n"
        "unsafe extern \"C\" { type RootApiRaw; }\n";

    return reverse_order
        ? parsed_producer_build(fixture, reverse_source,
            sizeof(reverse_source) - 1u, 3u, 12u, 0u, 0)
        : parsed_producer_build(fixture, forward_source,
            sizeof(forward_source) - 1u, 3u, 12u, 0u, 0);
}

static void parsed_producer_destroy(ParsedProducerFixture *fixture)
{
    cm_hir_library_artifact_destroy(&fixture->artifact);
    cm_hir_context_destroy(&fixture->context);
}

static void add_sentinel_alias(CmHirContext *context,
    CmHirCrateId crate_id, CmHirModuleId owner)
{
    CmHirDefId definition;
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_id;
    CmHirType type;
    CmHirTypeId parameter_type;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(context, crate_id,
        CM_HIR_ITEM_TYPE_ALIAS, test_span(60u, 70u), &definition)
        == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = definition;
    parameter.name = cm_hir_intern(context, "SentinelT");
    parameter.span = test_span(61u, 62u);
    assert(cm_hir_add_generic_param(context, &parameter, &parameter_id)
        == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = test_span(61u, 62u);
    type.data.parameter_type.parameter = parameter_id;
    assert(cm_hir_add_type(context, &type, &parameter_type) == CM_HIR_OK);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_TYPE_ALIAS;
    item.definition = definition;
    item.owner_module = owner;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(context, "SentinelAlias");
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(60u, 70u);
    item.generic_parameter_start = parameter_id;
    item.generic_parameter_count = 1u;
    item.data.type_alias_item.target = parameter_type;
    assert(cm_hir_add_item(context, &item, &item_id) == CM_HIR_OK);
}

static void consumer_sentinel_init(CmHirContext *context,
    CmHirLibraryArtifact *artifact, CmHirCrateId *out_crate,
    CmHirModuleId *out_last_module)
{
    CmHirModuleId root;
    CmHirModuleId child;
    const CmHirModule *root_value;
    CmHirDefId keep;
    CmHirLibraryOwnedData owned;
    CmHirLibraryArtifactResult restored;
    size_t root_index;

    cm_hir_context_init(context);
    assert(cm_hir_create_crate(context, cm_hir_intern(context, "sentinel"),
        CM_HIR_EDITION_2021, test_span(0u, 100u), out_crate, &root)
        == CM_HIR_OK);
    assert(cm_hir_add_module(context, *out_crate, root,
        cm_hir_intern(context, "occupied"), test_span(10u, 20u), &child)
        == CM_HIR_OK);
    keep = add_extern_type(context, *out_crate, root, "Keep", 30u);
    add_sentinel_alias(context, *out_crate, root);
    root_value = cm_hir_get_module(context, root);
    assert(root_value != NULL);
    cm_hir_library_owned_data_init(&owned);
    assert(cm_hir_library_owned_data_add_module(&owned,
        root_value->definition, &root_index) == CM_HIR_LIBRARY_OK);
    add_entry(&owned, root_index, "Keep", extern_type_binding(keep));
    cm_hir_library_artifact_init(artifact);
    restored = cm_hir_library_artifact_restore_owned(artifact, context,
        *out_crate, root_value->definition, "sentinel", &owned);
    assert(restored.status == CM_HIR_LIBRARY_OK);
    cm_hir_library_owned_data_destroy(&owned);
    *out_last_module = child;
    assert(context->types.len != 0u);
    assert(context->generic_parameters.len != 0u);
}

static int write_metadata_file(const char *path, const CmByteBuf *encoded)
{
    FILE *file;
    int ok;

    if (path == NULL || encoded == NULL || encoded->len == 0u) return 0;
    file = fopen(path, "wb");
    if (file == NULL) return 0;
    ok = fwrite(encoded->data, 1u, encoded->len, file) == encoded->len;
    if (fclose(file) != 0) ok = 0;
    return ok;
}

static int read_metadata_file(const char *path, CmByteBuf *encoded)
{
    FILE *file;
    long file_length;
    size_t length;
    int ok;

    if (path == NULL || encoded == NULL) return 0;
    cm_byte_buf_init(encoded);
    file = fopen(path, "rb");
    if (file == NULL) return 0;
    if (fseek(file, 0L, SEEK_END) != 0) {
        (void)fclose(file);
        return 0;
    }
    file_length = ftell(file);
    if (file_length <= 0L
        || (uint64_t)file_length
            > (uint64_t)CM_HIR_METADATA_HEADER_SIZE
                + (uint64_t)CM_HIR_METADATA_MAX_PAYLOAD_SIZE
        || fseek(file, 0L, SEEK_SET) != 0) {
        (void)fclose(file);
        return 0;
    }
    length = (size_t)file_length;
    cm_byte_buf_resize(encoded, length);
    ok = fread(encoded->data, 1u, length, file) == length;
    if (fclose(file) != 0) ok = 0;
    if (!ok) cm_byte_buf_destroy(encoded);
    return ok;
}

static CmHirLibraryBinding lookup(const CmHirLibraryArtifact *artifact,
    const char *first, const char *second, const char *third)
{
    CmHirLibraryPathSegment path[3];
    CmHirLibraryBinding binding;
    size_t count;

    path[0].bytes = (const unsigned char *)first;
    path[0].length = strlen(first);
    path[1].bytes = (const unsigned char *)second;
    path[1].length = strlen(second);
    count = 2u;
    if (third != NULL) {
        path[2].bytes = (const unsigned char *)third;
        path[2].length = strlen(third);
        count = 3u;
    }
    memset(&binding, 0, sizeof(binding));
    assert(cm_hir_library_artifact_lookup_binding(artifact, path, count,
        &binding) == CM_HIR_LIBRARY_OK);
    return binding;
}

static CmHirLibraryValue lookup_value(
    const CmHirLibraryArtifact *artifact, const char *extern_name,
    const char *name)
{
    CmHirLibraryPathSegment path[2];
    CmHirLibraryValue value;

    path[0].bytes = (const unsigned char *)extern_name;
    path[0].length = strlen(extern_name);
    path[1].bytes = (const unsigned char *)name;
    path[1].length = strlen(name);
    memset(&value, 0, sizeof(value));
    assert(cm_hir_library_artifact_lookup_value(artifact, path, 2u,
        &value) == CM_HIR_LIBRARY_OK);
    return value;
}

static const CmHirLibraryNominalReference *find_nominal_reference(
    const CmHirLibraryValue *value, const char *name,
    CmHirLibraryNominalReferenceKind kind)
{
    uint32_t index;

    assert(value != NULL && value->kind == CM_HIR_LIBRARY_VALUE_FUNCTION);
    for (index = 0u;
            index < value->data.function.nominal_reference_count; ++index) {
        const CmHirLibraryNominalReference *reference;

        reference = &value->data.function.nominal_references[index];
        if (reference->kind != kind) continue;
        if (reference->name.length == strlen(name)
            && memcmp(reference->name.bytes, name,
                reference->name.length) == 0) {
            return reference;
        }
    }
    return NULL;
}

static int generic_function_valid(const CmHirContext *context,
    const CmHirLibraryArtifact *artifact, const char *extern_name)
{
    CmHirLibraryValue value;
    const CmHirGenericParam *lifetime;
    const CmHirGenericParam *type_parameter;
    const CmHirGenericParam *constant;
    const CmHirType *first_parameter;
    const CmHirType *first_pointee;
    const CmHirType *second_parameter;
    const CmHirType *return_type;
    const CmHirType *return_pointee;
    const CmHirType *const_type;

    value = lookup_value(artifact, extern_name, "select");
    if (value.kind != CM_HIR_LIBRARY_VALUE_FUNCTION
        || value.data.function.generic_parameter_start
            == CM_HIR_GENERIC_PARAM_NONE
        || value.data.function.generic_parameter_count != 3u
        || value.data.function.parameter_count != 2u) return 0;
    lifetime = cm_hir_get_generic_param(context,
        value.data.function.generic_parameter_start);
    type_parameter = cm_hir_get_generic_param(context,
        value.data.function.generic_parameter_start + 1u);
    constant = cm_hir_get_generic_param(context,
        value.data.function.generic_parameter_start + 2u);
    if (lifetime == NULL || lifetime->kind != CM_HIR_GENERIC_LIFETIME
        || lifetime->index != 0u
        || !cm_hir_def_id_equal(lifetime->owner, value.definition)
        || type_parameter == NULL
        || type_parameter->kind != CM_HIR_GENERIC_TYPE
        || type_parameter->index != 1u
        || !cm_hir_def_id_equal(type_parameter->owner, value.definition)
        || constant == NULL || constant->kind != CM_HIR_GENERIC_CONST
        || constant->index != 2u
        || !cm_hir_def_id_equal(constant->owner, value.definition)) return 0;
    const_type = cm_hir_get_type(context, constant->declared_type);
    first_parameter = cm_hir_get_type(context,
        value.data.function.parameter_types[0]);
    second_parameter = cm_hir_get_type(context,
        value.data.function.parameter_types[1]);
    return_type = cm_hir_get_type(context,
        value.data.function.return_type);
    if (const_type == NULL || const_type->kind != CM_HIR_TYPE_INTEGER_KIND
        || const_type->data.integer_type.kind != CM_HIR_INT_USIZE
        || first_parameter == NULL
        || first_parameter->kind != CM_HIR_TYPE_REFERENCE_KIND
        || first_parameter->data.reference_type.region.kind
            != CM_HIR_REGION_EARLY_BOUND
        || first_parameter->data.reference_type.region.data.parameter
            != value.data.function.generic_parameter_start
        || second_parameter == NULL
        || second_parameter->kind != CM_HIR_TYPE_ARRAY_KIND
        || second_parameter->data.array_type.length.kind
            != CM_HIR_CONST_PARAMETER
        || second_parameter->data.array_type.length.data.parameter
            != value.data.function.generic_parameter_start + 2u
        || return_type == NULL
        || return_type->kind != CM_HIR_TYPE_REFERENCE_KIND
        || return_type->data.reference_type.region.kind
            != CM_HIR_REGION_EARLY_BOUND
        || return_type->data.reference_type.region.data.parameter
            != value.data.function.generic_parameter_start) return 0;
    first_pointee = cm_hir_get_type(context,
        first_parameter->data.reference_type.pointee);
    return_pointee = cm_hir_get_type(context,
        return_type->data.reference_type.pointee);
    return first_pointee != NULL
        && first_pointee->kind == CM_HIR_TYPE_PARAMETER_KIND
        && first_pointee->data.parameter_type.parameter
            == value.data.function.generic_parameter_start + 1u
        && return_pointee != NULL
        && return_pointee->kind == CM_HIR_TYPE_PARAMETER_KIND
        && return_pointee->data.parameter_type.parameter
            == value.data.function.generic_parameter_start + 1u;
}

static const CmHirItem *binding_item(const CmHirContext *context,
    CmHirLibraryBinding binding)
{
    const CmHirDefinition *definition;

    definition = cm_hir_lookup_definition(context, binding.definition);
    return definition == NULL ? NULL
        : cm_hir_get_item(context, definition->entity.item_id);
}

static void assert_loaded_declarations(CmHirContext *context,
    const CmHirLibraryArtifact *artifact)
{
    CmHirLibraryBinding wrapper_binding;
    CmHirLibraryBinding choice_binding;
    CmHirLibraryBinding alias_binding;
    CmHirLibraryBinding renamed_binding;
    const CmHirItem *wrapper;
    const CmHirItem *choice;
    const CmHirItem *alias;
    const CmHirGenericParam *parameter;
    const CmHirType *type;
    CmHirType application;
    CmHirTypeId application_id;
    CmHirTypeAliasResult normalized;

    wrapper_binding = lookup(artifact, "dep", "Wrapper", NULL);
    choice_binding = lookup(artifact, "dep", "Choice", NULL);
    alias_binding = lookup(artifact, "dep", "Alias", NULL);
    renamed_binding = lookup(artifact, "dep", "child", "Renamed");
    assert(wrapper_binding.kind == CM_HIR_LIBRARY_BINDING_TYPE);
    assert(wrapper_binding.type_kind == CM_HIR_TYPE_ADT_KIND);
    assert(choice_binding.kind == CM_HIR_LIBRARY_BINDING_TYPE);
    assert(choice_binding.type_kind == CM_HIR_TYPE_ADT_KIND);
    assert(alias_binding.kind == CM_HIR_LIBRARY_BINDING_TYPE);
    assert(alias_binding.type_kind == CM_HIR_TYPE_ALIAS_APPLICATION_KIND);
    assert(cm_hir_def_id_equal(alias_binding.definition,
        renamed_binding.definition));

    wrapper = binding_item(context, wrapper_binding);
    assert(wrapper != NULL && wrapper->kind == CM_HIR_ITEM_STRUCT);
    assert(wrapper->data.aggregate_item.form == CM_HIR_AGGREGATE_TUPLE);
    assert(wrapper->data.aggregate_item.field_count == 1u);
    assert(wrapper->generic_parameter_count == 1u);
    parameter = cm_hir_get_generic_param(context,
        wrapper->generic_parameter_start);
    assert(parameter != NULL && parameter->kind == CM_HIR_GENERIC_TYPE);
    assert(cm_hir_def_id_equal(parameter->owner, wrapper->definition));
    assert(parameter->index == 0u && parameter->has_default);
    assert(parameter->default_argument.kind == CM_HIR_GENERIC_ARG_TYPE);
    type = cm_hir_get_type(context, parameter->default_argument.data.type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_U32);
    type = cm_hir_get_type(context,
        wrapper->data.aggregate_item.fields[0].type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_PARAMETER_KIND
        && type->data.parameter_type.parameter
            == wrapper->generic_parameter_start);

    choice = binding_item(context, choice_binding);
    assert(choice != NULL && choice->kind == CM_HIR_ITEM_ENUM);
    assert(choice->generic_parameter_count == 1u);
    assert(choice->data.enum_item.variant_count == 2u);
    assert(choice->data.enum_item.variants[0].form
        == CM_HIR_AGGREGATE_UNIT);
    assert(choice->data.enum_item.variants[0].field_count == 0u);
    assert(choice->data.enum_item.variants[1].form
        == CM_HIR_AGGREGATE_TUPLE);
    assert(choice->data.enum_item.variants[1].field_count == 1u);
    type = cm_hir_get_type(context,
        choice->data.enum_item.variants[1].fields[0].type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_PARAMETER_KIND
        && type->data.parameter_type.parameter
            == choice->generic_parameter_start);

    alias = binding_item(context, alias_binding);
    assert(alias != NULL && alias->kind == CM_HIR_ITEM_TYPE_ALIAS);
    type = cm_hir_get_type(context, alias->data.type_alias_item.target);
    assert(type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND);
    assert(cm_hir_def_id_equal(type->data.named_type.definition,
        wrapper_binding.definition));
    assert(type->data.named_type.argument_count == 1u);
    assert(type->data.named_type.arguments[0].kind
        == CM_HIR_GENERIC_ARG_TYPE);
    type = cm_hir_get_type(context,
        type->data.named_type.arguments[0].data.type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_U32);

    memset(&application, 0, sizeof(application));
    application.kind = CM_HIR_TYPE_ALIAS_APPLICATION_KIND;
    application.span = test_span(200u, 201u);
    application.data.named_type.definition = alias_binding.definition;
    assert(cm_hir_add_type(context, &application, &application_id)
        == CM_HIR_OK);
    normalized = cm_hir_normalize_type_aliases(context, application_id);
    assert(normalized.status == CM_HIR_TYPE_ALIAS_OK);
    type = cm_hir_get_type(context, normalized.type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND);
    assert(cm_hir_def_id_equal(type->data.named_type.definition,
        wrapper_binding.definition));
}

static void assert_loaded_parsed_declarations(CmHirContext *context,
    const CmHirLibraryArtifact *artifact)
{
    CmHirLibraryBinding storage_binding;
    CmHirLibraryBinding borrowed_binding;
    CmHirLibraryBinding alias_binding;
    CmHirLibraryBinding renamed_binding;
    const CmHirItem *storage;
    const CmHirItem *borrowed;
    const CmHirItem *alias;
    const CmHirGenericParam *lifetime;
    const CmHirGenericParam *parameter;
    const CmHirType *type;

    storage_binding = lookup(artifact, "dep", "Storage", NULL);
    borrowed_binding = lookup(artifact, "dep", "Borrowed", NULL);
    alias_binding = lookup(artifact, "dep", "BorrowedAlias", NULL);
    renamed_binding = lookup(artifact, "dep", "child",
        "RenamedBorrowed");
    assert(storage_binding.kind == CM_HIR_LIBRARY_BINDING_TYPE);
    assert(storage_binding.type_kind == CM_HIR_TYPE_ADT_KIND);
    assert(borrowed_binding.kind == CM_HIR_LIBRARY_BINDING_TYPE);
    assert(borrowed_binding.type_kind == CM_HIR_TYPE_ADT_KIND);
    assert(alias_binding.kind == CM_HIR_LIBRARY_BINDING_TYPE);
    assert(alias_binding.type_kind == CM_HIR_TYPE_ALIAS_APPLICATION_KIND);
    assert(cm_hir_def_id_equal(alias_binding.definition,
        renamed_binding.definition));

    storage = binding_item(context, storage_binding);
    assert(storage != NULL && storage->kind == CM_HIR_ITEM_UNION);
    assert(storage->generic_parameter_count == 1u);
    assert(storage->data.aggregate_item.form == CM_HIR_AGGREGATE_NAMED);
    assert(storage->data.aggregate_item.field_count == 2u);
    type = cm_hir_get_type(context,
        storage->data.aggregate_item.fields[1].type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_RAW_POINTER_KIND);
    type = cm_hir_get_type(context, type->data.raw_pointer_type.pointee);
    assert(type != NULL && type->kind == CM_HIR_TYPE_PARAMETER_KIND);

    borrowed = binding_item(context, borrowed_binding);
    assert(borrowed != NULL && borrowed->kind == CM_HIR_ITEM_STRUCT);
    assert(borrowed->generic_parameter_count == 2u);
    assert(borrowed->data.aggregate_item.form == CM_HIR_AGGREGATE_NAMED);
    assert(borrowed->data.aggregate_item.field_count == 4u);
    lifetime = cm_hir_get_generic_param(context,
        borrowed->generic_parameter_start);
    parameter = cm_hir_get_generic_param(context,
        borrowed->generic_parameter_start + 1u);
    assert(lifetime != NULL
        && lifetime->kind == CM_HIR_GENERIC_LIFETIME
        && lifetime->index == 0u && !lifetime->has_default);
    assert(parameter != NULL && parameter->kind == CM_HIR_GENERIC_TYPE
        && parameter->index == 1u && parameter->has_default
        && parameter->default_argument.kind == CM_HIR_GENERIC_ARG_TYPE);
    type = cm_hir_get_type(context, parameter->default_argument.data.type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_U16);
    type = cm_hir_get_type(context,
        borrowed->data.aggregate_item.fields[0].type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_REFERENCE_KIND
        && type->data.reference_type.region.kind
            == CM_HIR_REGION_EARLY_BOUND
        && type->data.reference_type.region.data.parameter
            == borrowed->generic_parameter_start);
    type = cm_hir_get_type(context,
        borrowed->data.aggregate_item.fields[2].type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_TUPLE_KIND
        && type->data.tuple_type.element_count == 2u);
    type = cm_hir_get_type(context,
        borrowed->data.aggregate_item.fields[3].type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_ARRAY_KIND
        && type->data.array_type.length.kind == CM_HIR_CONST_VALUE
        && type->data.array_type.length.data.value.low_bits == UINT64_C(3)
        && type->data.array_type.length.data.value.high_bits == UINT64_C(0));

    alias = binding_item(context, alias_binding);
    assert(alias != NULL && alias->kind == CM_HIR_ITEM_TYPE_ALIAS);
    assert(alias->generic_parameter_count == 2u);
    type = cm_hir_get_type(context, alias->data.type_alias_item.target);
    assert(type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND);
    assert(cm_hir_def_id_equal(type->data.named_type.definition,
        borrowed_binding.definition));
    assert(type->data.named_type.argument_count == 2u);
    assert(type->data.named_type.arguments[0].kind
        == CM_HIR_GENERIC_ARG_LIFETIME);
    assert(type->data.named_type.arguments[1].kind
        == CM_HIR_GENERIC_ARG_TYPE);
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
    lengths.generic_parameters = context->generic_parameters.len;
    lengths.definitions = context->definitions.len;
    lengths.prebound_associated_types =
        context->prebound_associated_types.len;
    lengths.strings = cm_interner_length(&context->strings);
    return lengths;
}

static void assert_context_lengths(const CmHirContext *context,
    ContextLengths expected)
{
    ContextLengths actual;

    actual = context_lengths(context);
    assert(actual.crates == expected.crates);
    assert(actual.modules == expected.modules);
    assert(actual.items == expected.items);
    assert(actual.bodies == expected.bodies);
    assert(actual.expressions == expected.expressions);
    assert(actual.types == expected.types);
    assert(actual.generic_parameters == expected.generic_parameters);
    assert(actual.definitions == expected.definitions);
    assert(actual.prebound_associated_types
        == expected.prebound_associated_types);
    assert(actual.strings == expected.strings);
}

static void recompute_metadata_crc(CmByteBuf *encoded)
{
    uint32_t crc;
    unsigned int index;

    assert(encoded->len >= CM_HIR_METADATA_HEADER_SIZE);
    crc = cm_hir_metadata_crc32(
        encoded->data + CM_HIR_METADATA_HEADER_SIZE,
        encoded->len - CM_HIR_METADATA_HEADER_SIZE);
    for (index = 0u; index < 4u; ++index) {
        encoded->data[TEST_METADATA_CRC_OFFSET + index]
            = (unsigned char)(crc & UINT32_C(0xff));
        crc >>= 8u;
    }
}

static void corrupt_v24_section_byte(CmByteBuf *encoded,
    const unsigned char tag[4], size_t section_offset, unsigned char value)
{
    CmHirMetadataEnvelope envelope;
    CmHirMetadataReader reader;
    CmHirMetadataSection section;
    CmHirMetadataStatus status;
    int found;

    memset(&envelope, 0, sizeof(envelope));
    status = cm_hir_metadata_decode_envelope_version(encoded->data,
        encoded->len, CM_HIR_METADATA_DECLARATION_MAJOR,
        CM_HIR_METADATA_DECLARATION_MINOR, &envelope);
    assert(status == CM_HIR_METADATA_OK);
    cm_hir_metadata_reader_init(&reader, envelope.payload,
        envelope.payload_length);
    found = 0;
    while (cm_hir_metadata_read_section(&reader, &section)
            == CM_HIR_METADATA_OK) {
        if (memcmp(section.tag, tag, 4u) == 0) {
            size_t absolute;

            assert(section_offset < section.length);
            absolute = (size_t)(section.data - encoded->data) + section_offset;
            encoded->data[absolute] = value;
            found = 1;
            break;
        }
    }
    assert(found);
    recompute_metadata_crc(encoded);
}

static void corrupt_first_predicate_modifier(CmByteBuf *encoded,
    unsigned char modifier)
{
    CmHirMetadataEnvelope envelope;
    CmHirMetadataReader sections;
    CmHirMetadataSection section;
    CmHirMetadataReader reader;
    uint32_t count;
    uint32_t ignored;
    uint32_t index;
    size_t modifier_offset;

    assert(cm_hir_metadata_decode_envelope_version(encoded->data,
        encoded->len, CM_HIR_METADATA_DECLARATION_MAJOR,
        CM_HIR_METADATA_DECLARATION_MINOR, &envelope) == CM_HIR_METADATA_OK);
    cm_hir_metadata_reader_init(&sections, envelope.payload,
        envelope.payload_length);
    for (index = 0u; index < 9u; ++index)
        assert(cm_hir_metadata_read_section(&sections, &section)
            == CM_HIR_METADATA_OK);
    assert(memcmp(section.tag, "PRED", 4u) == 0);
    cm_hir_metadata_reader_init(&reader, section.data, section.length);
    assert(cm_hir_metadata_read_u32(&reader, &count) == CM_HIR_METADATA_OK
        && count != 0u
        && cm_hir_metadata_read_u32(&reader, &ignored)
            == CM_HIR_METADATA_OK
        && cm_hir_metadata_read_u32(&reader, &count) == CM_HIR_METADATA_OK);
    for (index = 0u; index < count; ++index)
        assert(cm_hir_metadata_read_u32(&reader, &ignored)
            == CM_HIR_METADATA_OK);
    assert(cm_hir_metadata_read_u32(&reader, &count) == CM_HIR_METADATA_OK);
    for (index = 0u; index < count * 2u; ++index)
        assert(cm_hir_metadata_read_u32(&reader, &ignored)
            == CM_HIR_METADATA_OK);
    assert(cm_hir_metadata_read_u32(&reader, &count) == CM_HIR_METADATA_OK
        && count != 0u
        && cm_hir_metadata_read_u32(&reader, &ignored)
            == CM_HIR_METADATA_OK
        && cm_hir_metadata_read_u32(&reader, &ignored)
            == CM_HIR_METADATA_OK);
    modifier_offset = reader.cursor;
    assert(modifier_offset < section.length);
    encoded->data[(size_t)(section.data - encoded->data) + modifier_offset]
        = modifier;
    recompute_metadata_crc(encoded);
}

static void truncate_first_predicate_at_modifier(CmByteBuf *encoded)
{
    static const unsigned char pred_tag[4] = {
        (unsigned char)'P', (unsigned char)'R',
        (unsigned char)'E', (unsigned char)'D'
    };
    CmHirMetadataEnvelope envelope;
    CmHirMetadataReader sections;
    CmHirMetadataSection section;
    CmHirMetadataReader reader;
    CmByteBuf contents;
    uint32_t count;
    uint32_t ignored;
    uint32_t index;

    assert(cm_hir_metadata_decode_envelope_version(encoded->data,
        encoded->len, CM_HIR_METADATA_DECLARATION_MAJOR,
        CM_HIR_METADATA_DECLARATION_MINOR, &envelope) == CM_HIR_METADATA_OK);
    cm_hir_metadata_reader_init(&sections, envelope.payload,
        envelope.payload_length);
    for (index = 0u; index < 9u; ++index)
        assert(cm_hir_metadata_read_section(&sections, &section)
            == CM_HIR_METADATA_OK);
    assert(memcmp(section.tag, pred_tag, 4u) == 0);
    cm_hir_metadata_reader_init(&reader, section.data, section.length);
    assert(cm_hir_metadata_read_u32(&reader, &count) == CM_HIR_METADATA_OK
        && count != 0u
        && cm_hir_metadata_read_u32(&reader, &ignored)
            == CM_HIR_METADATA_OK
        && cm_hir_metadata_read_u32(&reader, &count) == CM_HIR_METADATA_OK);
    for (index = 0u; index < count; ++index)
        assert(cm_hir_metadata_read_u32(&reader, &ignored)
            == CM_HIR_METADATA_OK);
    assert(cm_hir_metadata_read_u32(&reader, &count) == CM_HIR_METADATA_OK);
    for (index = 0u; index < count * 2u; ++index)
        assert(cm_hir_metadata_read_u32(&reader, &ignored)
            == CM_HIR_METADATA_OK);
    assert(cm_hir_metadata_read_u32(&reader, &count) == CM_HIR_METADATA_OK
        && count != 0u
        && cm_hir_metadata_read_u32(&reader, &ignored)
            == CM_HIR_METADATA_OK
        && cm_hir_metadata_read_u32(&reader, &ignored)
            == CM_HIR_METADATA_OK);
    cm_byte_buf_init(&contents);
    cm_byte_buf_append(&contents, section.data, reader.cursor);
    replace_v24_section(encoded, pred_tag, &contents);
    cm_byte_buf_destroy(&contents);
}

static void corrupt_nominal_kind_named(CmByteBuf *encoded,
    const char *target_name, uint8_t replacement_kind)
{
    CmHirMetadataEnvelope envelope;
    CmHirMetadataReader sections;
    CmHirMetadataSection section;
    CmHirMetadataReader reader;
    uint32_t count;
    uint32_t index;
    int changed;

    assert(cm_hir_metadata_decode_envelope_version(encoded->data,
        encoded->len, CM_HIR_METADATA_DECLARATION_MAJOR,
        CM_HIR_METADATA_DECLARATION_MINOR, &envelope) == CM_HIR_METADATA_OK);
    cm_hir_metadata_reader_init(&sections, envelope.payload,
        envelope.payload_length);
    for (index = 0u; index < 8u; ++index)
        assert(cm_hir_metadata_read_section(&sections, &section)
            == CM_HIR_METADATA_OK);
    assert(memcmp(section.tag, "NREF", 4u) == 0);
    cm_hir_metadata_reader_init(&reader, section.data, section.length);
    assert(cm_hir_metadata_read_u32(&reader, &count) == CM_HIR_METADATA_OK);
    changed = 0;
    for (index = 0u; index < count; ++index) {
        size_t kind_offset;
        uint8_t kind;
        uint32_t ignored;
        uint32_t name_length;
        uint32_t generic_count;
        uint32_t generic;
        const unsigned char *name;

        kind_offset = reader.cursor;
        assert(cm_hir_metadata_read_u8(&reader, &kind) == CM_HIR_METADATA_OK
            && cm_hir_metadata_read_u32(&reader, &ignored)
                == CM_HIR_METADATA_OK
            && cm_hir_metadata_read_u32(&reader, &name_length)
                == CM_HIR_METADATA_OK
            && cm_hir_metadata_read_bytes(&reader, name_length, &name)
                == CM_HIR_METADATA_OK
            && cm_hir_metadata_read_u32(&reader, &ignored)
                == CM_HIR_METADATA_OK
            && cm_hir_metadata_read_u32(&reader, &generic_count)
                == CM_HIR_METADATA_OK);
        for (generic = 0u; generic < generic_count; ++generic)
            assert(cm_hir_metadata_read_u8(&reader, &kind)
                == CM_HIR_METADATA_OK);
        if (!changed && strlen(target_name) == (size_t)name_length
            && memcmp(name, target_name, name_length) == 0) {
            encoded->data[(size_t)(section.data - encoded->data)
                + kind_offset] = replacement_kind;
            changed = 1;
        }
    }
    assert(changed && cm_hir_metadata_reader_finish(&reader)
        == CM_HIR_METADATA_OK);
    recompute_metadata_crc(encoded);
}

static void replace_v24_section(CmByteBuf *encoded,
    const unsigned char tag[4], const CmByteBuf *replacement_contents)
{
    CmHirMetadataEnvelope envelope;
    CmHirMetadataReader reader;
    CmHirMetadataSection sections[9];
    CmHirMetadataWriter writer;
    CmByteBuf payload;
    CmByteBuf replacement;
    uint32_t index;
    int found;

    assert(cm_hir_metadata_decode_envelope_version(encoded->data,
        encoded->len, CM_HIR_METADATA_DECLARATION_MAJOR,
        CM_HIR_METADATA_DECLARATION_MINOR, &envelope) == CM_HIR_METADATA_OK);
    cm_hir_metadata_reader_init(&reader, envelope.payload,
        envelope.payload_length);
    for (index = 0u; index < 9u; ++index)
        assert(cm_hir_metadata_read_section(&reader, &sections[index])
            == CM_HIR_METADATA_OK);
    assert(cm_hir_metadata_read_section(&reader, &sections[0])
        == CM_HIR_METADATA_DONE);
    cm_byte_buf_init(&payload);
    cm_hir_metadata_writer_init(&writer, &payload,
        CM_HIR_METADATA_MAX_PAYLOAD_SIZE);
    found = 0;
    for (index = 0u; index < 9u; ++index) {
        const void *data;
        size_t length;

        data = sections[index].data;
        length = sections[index].length;
        if (memcmp(sections[index].tag, tag, 4u) == 0) {
            data = replacement_contents->data;
            length = replacement_contents->len;
            found = 1;
        }
        assert(cm_hir_metadata_write_section(&writer, sections[index].tag,
            data, length) == CM_HIR_METADATA_OK);
    }
    assert(found);
    cm_byte_buf_init(&replacement);
    assert(cm_hir_metadata_encode_envelope_version(&replacement,
        CM_HIR_METADATA_DECLARATION_MAJOR,
        CM_HIR_METADATA_DECLARATION_MINOR, UINT32_C(0), payload.data,
        payload.len) == CM_HIR_METADATA_OK);
    cm_byte_buf_destroy(&payload);
    cm_byte_buf_destroy(encoded);
    *encoded = replacement;
}

static uint32_t append_orphan_late_bound_type(CmByteBuf *encoded)
{
    static const unsigned char type_tag[4] = {
        (unsigned char)'T', (unsigned char)'Y',
        (unsigned char)'P', (unsigned char)'E'
    };
    CmHirMetadataEnvelope envelope;
    CmHirMetadataReader reader;
    CmHirMetadataSection section;
    CmByteBuf contents;
    CmHirMetadataWriter writer;
    uint32_t count;
    uint32_t index;

    assert(cm_hir_metadata_decode_envelope_version(encoded->data,
        encoded->len, CM_HIR_METADATA_DECLARATION_MAJOR,
        CM_HIR_METADATA_DECLARATION_MINOR, &envelope) == CM_HIR_METADATA_OK);
    cm_hir_metadata_reader_init(&reader, envelope.payload,
        envelope.payload_length);
    for (index = 0u; index < 4u; ++index)
        assert(cm_hir_metadata_read_section(&reader, &section)
            == CM_HIR_METADATA_OK);
    assert(memcmp(section.tag, type_tag, 4u) == 0 && section.length >= 4u);
    count = (uint32_t)section.data[0]
        | ((uint32_t)section.data[1] << 8u)
        | ((uint32_t)section.data[2] << 16u)
        | ((uint32_t)section.data[3] << 24u);
    assert(count != 0u && count != UINT32_MAX);
    cm_byte_buf_init(&contents);
    cm_byte_buf_append(&contents, section.data, section.length);
    count += 1u;
    contents.data[0] = (unsigned char)(count & UINT32_C(0xff));
    contents.data[1] = (unsigned char)((count >> 8u) & UINT32_C(0xff));
    contents.data[2] = (unsigned char)((count >> 16u) & UINT32_C(0xff));
    contents.data[3] = (unsigned char)((count >> 24u) & UINT32_C(0xff));
    cm_hir_metadata_writer_init(&writer, &contents,
        CM_HIR_METADATA_MAX_PAYLOAD_SIZE);
    assert(cm_hir_metadata_write_u8(&writer, UINT8_C(8))
            == CM_HIR_METADATA_OK
        && cm_hir_metadata_write_u8(&writer, UINT8_C(3))
            == CM_HIR_METADATA_OK
        && cm_hir_metadata_write_u32(&writer, UINT32_C(0))
            == CM_HIR_METADATA_OK
        && cm_hir_metadata_write_u32(&writer, UINT32_C(1))
            == CM_HIR_METADATA_OK
        && cm_hir_metadata_write_u8(&writer, UINT8_C(1))
            == CM_HIR_METADATA_OK);
    replace_v24_section(encoded, type_tag, &contents);
    cm_byte_buf_destroy(&contents);
    return count;
}

static void point_first_nonfunction_value_at_type(CmByteBuf *encoded,
    uint32_t type_local)
{
    CmHirMetadataEnvelope envelope;
    CmHirMetadataReader sections;
    CmHirMetadataSection section;
    CmHirMetadataReader reader;
    uint32_t section_index;
    uint32_t value_count;
    uint32_t value_index;
    int changed;

    assert(cm_hir_metadata_decode_envelope_version(encoded->data,
        encoded->len, CM_HIR_METADATA_DECLARATION_MAJOR,
        CM_HIR_METADATA_DECLARATION_MINOR, &envelope) == CM_HIR_METADATA_OK);
    cm_hir_metadata_reader_init(&sections, envelope.payload,
        envelope.payload_length);
    for (section_index = 0u; section_index < 6u; ++section_index)
        assert(cm_hir_metadata_read_section(&sections, &section)
            == CM_HIR_METADATA_OK);
    assert(memcmp(section.tag, "VALU", 4u) == 0);
    cm_hir_metadata_reader_init(&reader, section.data, section.length);
    assert(cm_hir_metadata_read_u32(&reader, &value_count)
        == CM_HIR_METADATA_OK);
    changed = 0;
    for (value_index = 0u; value_index < value_count; ++value_index) {
        uint8_t kind;

        assert(cm_hir_metadata_read_u8(&reader, &kind) == CM_HIR_METADATA_OK);
        if (kind == UINT8_C(1)) {
            uint32_t parameter_count;
            uint32_t ignored;
            uint32_t index;
            uint32_t abi_length;
            const unsigned char *abi;
            uint8_t flag;

            assert(cm_hir_metadata_read_u32(&reader, &parameter_count)
                == CM_HIR_METADATA_OK);
            for (index = 0u; index < parameter_count; ++index)
                assert(cm_hir_metadata_read_u32(&reader, &ignored)
                    == CM_HIR_METADATA_OK);
            for (index = 0u; index < 3u; ++index)
                assert(cm_hir_metadata_read_u32(&reader, &ignored)
                    == CM_HIR_METADATA_OK);
            assert(cm_hir_metadata_read_u32(&reader, &abi_length)
                    == CM_HIR_METADATA_OK
                && cm_hir_metadata_read_bytes(&reader, abi_length, &abi)
                    == CM_HIR_METADATA_OK);
            for (index = 0u; index < 4u; ++index)
                assert(cm_hir_metadata_read_u8(&reader, &flag)
                    == CM_HIR_METADATA_OK);
            (void)abi;
        } else {
            size_t offset;
            uint32_t ignored;
            uint8_t mutability;
            unsigned char *bytes;

            assert(kind == UINT8_C(2) || kind == UINT8_C(3));
            offset = reader.cursor;
            assert(cm_hir_metadata_read_u32(&reader, &ignored)
                    == CM_HIR_METADATA_OK
                && cm_hir_metadata_read_u8(&reader, &mutability)
                    == CM_HIR_METADATA_OK);
            bytes = (unsigned char *)section.data + offset;
            bytes[0] = (unsigned char)(type_local & UINT32_C(0xff));
            bytes[1] = (unsigned char)((type_local >> 8u) & UINT32_C(0xff));
            bytes[2] = (unsigned char)((type_local >> 16u) & UINT32_C(0xff));
            bytes[3] = (unsigned char)((type_local >> 24u) & UINT32_C(0xff));
            changed = 1;
            break;
        }
    }
    assert(changed);
    recompute_metadata_crc(encoded);
}

static void replace_with_aggregate_oversize_nominals(CmByteBuf *encoded)
{
    static const unsigned char nref_tag[4] = {
        (unsigned char)'N', (unsigned char)'R',
        (unsigned char)'E', (unsigned char)'F'
    };
    CmByteBuf contents;
    CmHirMetadataWriter writer;
    uint32_t record;
    uint32_t generic;

    cm_byte_buf_init(&contents);
    cm_hir_metadata_writer_init(&writer, &contents,
        CM_HIR_METADATA_MAX_PAYLOAD_SIZE);
    assert(cm_hir_metadata_write_u32(&writer, UINT32_C(2))
        == CM_HIR_METADATA_OK);
    for (record = 0u; record < 2u; ++record) {
        assert(cm_hir_metadata_write_u8(&writer, UINT8_C(1))
                == CM_HIR_METADATA_OK
            && cm_hir_metadata_write_u32(&writer, UINT32_C(1))
                == CM_HIR_METADATA_OK
            && cm_hir_metadata_write_u32(&writer, UINT32_C(1))
                == CM_HIR_METADATA_OK
            && cm_hir_metadata_write_u8(&writer,
                (unsigned char)('A' + record)) == CM_HIR_METADATA_OK
            && cm_hir_metadata_write_u32(&writer, UINT32_C(0))
                == CM_HIR_METADATA_OK
            && cm_hir_metadata_write_u32(&writer, UINT32_C(65537))
                == CM_HIR_METADATA_OK);
        for (generic = 0u; generic < UINT32_C(65537); ++generic)
            assert(cm_hir_metadata_write_u8(&writer, UINT8_C(2))
                == CM_HIR_METADATA_OK);
    }
    replace_v24_section(encoded, nref_tag, &contents);
    cm_byte_buf_destroy(&contents);
}

static void derive_legacy_declaration_v23(const CmByteBuf *current,
    CmByteBuf *legacy)
{
    CmHirMetadataEnvelope envelope;
    CmHirMetadataReader reader;
    CmHirMetadataSection section;
    CmHirMetadataStatus status;
    uint32_t index;

    memset(&envelope, 0, sizeof(envelope));
    status = cm_hir_metadata_decode_envelope_version(current->data,
        current->len, CM_HIR_METADATA_DECLARATION_MAJOR,
        CM_HIR_METADATA_DECLARATION_MINOR, &envelope);
    assert(status == CM_HIR_METADATA_OK);
    cm_hir_metadata_reader_init(&reader, envelope.payload,
        envelope.payload_length);
    for (index = 0u; index < 7u; ++index)
        assert(cm_hir_metadata_read_section(&reader, &section)
            == CM_HIR_METADATA_OK);
    cm_byte_buf_init(legacy);
    assert(cm_hir_metadata_encode_envelope_version(legacy,
        CM_HIR_METADATA_DECLARATION_MAJOR,
        CM_HIR_METADATA_DECLARATION_LEGACY_MINOR, UINT32_C(0),
        envelope.payload, reader.cursor) == CM_HIR_METADATA_OK);
}

static void derive_legacy_declaration_v25(const CmByteBuf *current,
    CmByteBuf *legacy)
{
    CmHirMetadataEnvelope envelope;

    memset(&envelope, 0, sizeof(envelope));
    assert(cm_hir_metadata_decode_envelope_version(current->data,
        current->len, CM_HIR_METADATA_DECLARATION_MAJOR,
        CM_HIR_METADATA_DECLARATION_MINOR, &envelope) == CM_HIR_METADATA_OK);
    cm_byte_buf_init(legacy);
    assert(cm_hir_metadata_encode_envelope_version(legacy,
        CM_HIR_METADATA_DECLARATION_MAJOR,
        CM_HIR_METADATA_DECLARATION_MODIFIER_MINOR, UINT32_C(0),
        envelope.payload, envelope.payload_length) == CM_HIR_METADATA_OK);
}

static void derive_legacy_declaration_v24(const CmByteBuf *current,
    CmByteBuf *legacy)
{
    CmHirMetadataEnvelope envelope;
    CmHirMetadataReader section_reader;
    CmHirMetadataSection sections[9];
    CmHirMetadataReader predicate_reader;
    CmHirMetadataWriter writer;
    CmByteBuf predicates;
    CmByteBuf payload;
    size_t modifier_offsets[64];
    size_t modifier_count;
    size_t copy_start;
    size_t offset_index;
    uint32_t group_count;
    uint32_t group;
    uint32_t count;
    uint32_t index;
    uint32_t ignored;
    uint8_t modifier;
    const unsigned char *bytes;

    assert(cm_hir_metadata_decode_envelope_version(current->data,
        current->len, CM_HIR_METADATA_DECLARATION_MAJOR,
        CM_HIR_METADATA_DECLARATION_MINOR, &envelope) == CM_HIR_METADATA_OK);
    cm_hir_metadata_reader_init(&section_reader, envelope.payload,
        envelope.payload_length);
    for (index = 0u; index < 9u; ++index)
        assert(cm_hir_metadata_read_section(&section_reader, &sections[index])
            == CM_HIR_METADATA_OK);
    assert(cm_hir_metadata_read_section(&section_reader, &sections[0])
            == CM_HIR_METADATA_DONE
        && memcmp(sections[8].tag, "PRED", 4u) == 0);
    cm_hir_metadata_reader_init(&predicate_reader, sections[8].data,
        sections[8].length);
    assert(cm_hir_metadata_read_u32(&predicate_reader, &group_count)
        == CM_HIR_METADATA_OK);
    modifier_count = 0u;
    for (group = 0u; group < group_count; ++group) {
        assert(cm_hir_metadata_read_u32(&predicate_reader, &ignored)
                == CM_HIR_METADATA_OK
            && cm_hir_metadata_read_u32(&predicate_reader, &count)
                == CM_HIR_METADATA_OK);
        for (index = 0u; index < count; ++index)
            assert(cm_hir_metadata_read_u32(&predicate_reader, &ignored)
                == CM_HIR_METADATA_OK);
        assert(cm_hir_metadata_read_u32(&predicate_reader, &count)
            == CM_HIR_METADATA_OK);
        for (index = 0u; index < count * 2u; ++index)
            assert(cm_hir_metadata_read_u32(&predicate_reader, &ignored)
                == CM_HIR_METADATA_OK);
        assert(cm_hir_metadata_read_u32(&predicate_reader, &count)
            == CM_HIR_METADATA_OK);
        for (index = 0u; index < count; ++index) {
            uint32_t child_count;
            uint32_t child;

            assert(cm_hir_metadata_read_u32(&predicate_reader, &ignored)
                    == CM_HIR_METADATA_OK
                && cm_hir_metadata_read_u32(&predicate_reader, &ignored)
                    == CM_HIR_METADATA_OK
                && modifier_count < sizeof(modifier_offsets)
                    / sizeof(modifier_offsets[0]));
            modifier_offsets[modifier_count++] = predicate_reader.cursor;
            assert(cm_hir_metadata_read_u8(&predicate_reader, &modifier)
                    == CM_HIR_METADATA_OK
                && modifier <= (uint8_t)CM_HIR_PREDICATE_CONST
                && cm_hir_metadata_read_u32(&predicate_reader, &child_count)
                    == CM_HIR_METADATA_OK);
            for (child = 0u; child < child_count; ++child) {
                uint32_t length;

                assert(cm_hir_metadata_read_u32(&predicate_reader, &length)
                        == CM_HIR_METADATA_OK
                    && cm_hir_metadata_read_bytes(&predicate_reader, length,
                        &bytes) == CM_HIR_METADATA_OK);
                (void)bytes;
            }
            assert(cm_hir_metadata_read_u32(&predicate_reader, &child_count)
                == CM_HIR_METADATA_OK);
            for (child = 0u; child < child_count; ++child)
                assert(cm_hir_metadata_read_u32(&predicate_reader, &ignored)
                    == CM_HIR_METADATA_OK);
            assert(cm_hir_metadata_read_u32(&predicate_reader, &child_count)
                == CM_HIR_METADATA_OK);
            for (child = 0u; child < child_count * 2u; ++child)
                assert(cm_hir_metadata_read_u32(&predicate_reader, &ignored)
                    == CM_HIR_METADATA_OK);
        }
        assert(cm_hir_metadata_read_u32(&predicate_reader, &count)
            == CM_HIR_METADATA_OK);
        for (index = 0u; index < count; ++index)
            assert(cm_hir_metadata_read_u32(&predicate_reader, &ignored)
                == CM_HIR_METADATA_OK);
    }
    assert(cm_hir_metadata_reader_finish(&predicate_reader)
            == CM_HIR_METADATA_OK
        && modifier_count != 0u);
    cm_byte_buf_init(&predicates);
    copy_start = 0u;
    for (offset_index = 0u; offset_index < modifier_count; ++offset_index) {
        assert(modifier_offsets[offset_index] >= copy_start);
        cm_byte_buf_append(&predicates, sections[8].data + copy_start,
            modifier_offsets[offset_index] - copy_start);
        copy_start = modifier_offsets[offset_index] + 1u;
    }
    cm_byte_buf_append(&predicates, sections[8].data + copy_start,
        sections[8].length - copy_start);
    cm_byte_buf_init(&payload);
    cm_hir_metadata_writer_init(&writer, &payload,
        CM_HIR_METADATA_MAX_PAYLOAD_SIZE);
    for (index = 0u; index < 9u; ++index)
        assert(cm_hir_metadata_write_section(&writer, sections[index].tag,
            index == 8u ? predicates.data : sections[index].data,
            index == 8u ? predicates.len : sections[index].length)
                == CM_HIR_METADATA_OK);
    cm_byte_buf_init(legacy);
    assert(cm_hir_metadata_encode_envelope_version(legacy,
        CM_HIR_METADATA_DECLARATION_MAJOR,
        CM_HIR_METADATA_DECLARATION_PREDICATE_MINOR, UINT32_C(0),
        payload.data, payload.len) == CM_HIR_METADATA_OK);
    cm_byte_buf_destroy(&payload);
    cm_byte_buf_destroy(&predicates);
}

static void corrupt_trait_universe(CmByteBuf *encoded,
    SemanticMetadataCorruption corruption)
{
    CmHirMetadataEnvelope envelope;
    CmHirMetadataReader section_reader;
    CmHirMetadataSection section;
    CmHirMetadataReader reader;
    uint8_t universe;
    uint32_t trait_count;
    uint32_t impl_count;
    uint32_t auto_trait;
    uint32_t ordinary_trait;
    uint32_t index;
    int changed;

    assert(cm_hir_metadata_decode_envelope_version(encoded->data,
        encoded->len, (uint16_t)CM_HIR_METADATA_MAJOR,
        (uint16_t)CM_HIR_METADATA_SEMANTIC_MINOR, &envelope)
        == CM_HIR_METADATA_OK);
    cm_hir_metadata_reader_init(&section_reader, envelope.payload,
        envelope.payload_length);
    for (index = 0u; index < 7u; ++index) {
        assert(cm_hir_metadata_read_section(&section_reader, &section)
            == CM_HIR_METADATA_OK);
    }
    assert(memcmp(section.tag, "TUNI", 4u) == 0 && section.length != 0u);
    if (corruption == SEMANTIC_CORRUPT_CLOSED) {
        encoded->data[(size_t)(section.data - encoded->data)] = UINT8_C(1);
        recompute_metadata_crc(encoded);
        return;
    }

    cm_hir_metadata_reader_init(&reader, section.data, section.length);
    assert(cm_hir_metadata_read_u8(&reader, &universe)
        == CM_HIR_METADATA_OK);
    assert(universe == UINT8_C(0));
    assert(cm_hir_metadata_read_u32(&reader, &trait_count)
        == CM_HIR_METADATA_OK);
    auto_trait = UINT32_C(0);
    ordinary_trait = UINT32_C(0);
    for (index = 0u; index < trait_count; ++index) {
        uint32_t owner;
        uint32_t name_length;
        const unsigned char *name;
        uint8_t visibility;
        uint32_t restriction;
        uint8_t safety;
        uint8_t is_auto;

        assert(cm_hir_metadata_read_u32(&reader, &owner)
            == CM_HIR_METADATA_OK);
        assert(cm_hir_metadata_read_u32(&reader, &name_length)
            == CM_HIR_METADATA_OK);
        assert(cm_hir_metadata_read_bytes(&reader, (size_t)name_length,
            &name) == CM_HIR_METADATA_OK);
        assert(cm_hir_metadata_read_u8(&reader, &visibility)
            == CM_HIR_METADATA_OK);
        assert(cm_hir_metadata_read_u32(&reader, &restriction)
            == CM_HIR_METADATA_OK);
        assert(cm_hir_metadata_read_u8(&reader, &safety)
            == CM_HIR_METADATA_OK);
        assert(cm_hir_metadata_read_u8(&reader, &is_auto)
            == CM_HIR_METADATA_OK);
        (void)owner;
        (void)name;
        (void)visibility;
        (void)restriction;
        (void)safety;
        if (is_auto != 0u) auto_trait = index + 1u;
        else ordinary_trait = index + 1u;
    }
    assert(auto_trait != 0u && ordinary_trait != 0u);
    assert(cm_hir_metadata_read_u32(&reader, &impl_count)
        == CM_HIR_METADATA_OK);
    changed = 0;
    for (index = 0u; index < impl_count; ++index) {
        uint32_t owner;
        uint32_t trait_local;
        uint32_t self_type;
        uint8_t safety;
        uint8_t is_negative;
        unsigned char *trait_bytes;
        unsigned char *self_type_bytes;
        unsigned char *safety_byte;
        unsigned char *negative_byte;

        assert(cm_hir_metadata_read_u32(&reader, &owner)
            == CM_HIR_METADATA_OK);
        trait_bytes = (unsigned char *)reader.data + reader.cursor;
        assert(cm_hir_metadata_read_u32(&reader, &trait_local)
            == CM_HIR_METADATA_OK);
        self_type_bytes = (unsigned char *)reader.data + reader.cursor;
        assert(cm_hir_metadata_read_u32(&reader, &self_type)
            == CM_HIR_METADATA_OK);
        safety_byte = (unsigned char *)reader.data + reader.cursor;
        assert(cm_hir_metadata_read_u8(&reader, &safety)
            == CM_HIR_METADATA_OK);
        negative_byte = (unsigned char *)reader.data + reader.cursor;
        assert(cm_hir_metadata_read_u8(&reader, &is_negative)
            == CM_HIR_METADATA_OK);
        (void)owner;
        (void)self_type;
        (void)safety;
        if (!changed && corruption == SEMANTIC_CORRUPT_TRAIT_HANDLE) {
            memset(trait_bytes, 0, 4u);
            changed = 1;
        } else if (!changed
            && corruption == SEMANTIC_CORRUPT_SELF_TYPE_HANDLE) {
            memset(self_type_bytes, 0, 4u);
            changed = 1;
        } else if (!changed
            && corruption == SEMANTIC_CORRUPT_NEGATIVE_POLARITY
            && trait_local == ordinary_trait && is_negative == 0u) {
            *negative_byte = UINT8_C(1);
            changed = 1;
        } else if (!changed
            && corruption == SEMANTIC_CORRUPT_NEGATIVE_SAFETY
            && trait_local == auto_trait && is_negative != 0u) {
            *safety_byte = (unsigned char)CM_HIR_UNSAFE;
            changed = 1;
        }
    }
    assert(cm_hir_metadata_reader_finish(&reader) == CM_HIR_METADATA_OK);
    assert(changed);
    recompute_metadata_crc(encoded);
}

static void replace_namespace_name(CmByteBuf *encoded, const char *from,
    const char *to)
{
    CmHirMetadataEnvelope envelope;
    CmHirMetadataReader sections_reader;
    CmHirMetadataSection section;
    CmHirMetadataReader reader;
    uint32_t count;
    uint32_t index;
    size_t name_length;

    name_length = strlen(from);
    assert(name_length == strlen(to));
    assert(cm_hir_metadata_decode_envelope(encoded->data, encoded->len,
        &envelope) == CM_HIR_METADATA_OK);
    cm_hir_metadata_reader_init(&sections_reader, envelope.payload,
        envelope.payload_length);
    for (index = 0u; index < 6u; ++index) {
        assert(cm_hir_metadata_read_section(&sections_reader, &section)
            == CM_HIR_METADATA_OK);
    }
    cm_hir_metadata_reader_init(&reader, section.data, section.length);
    assert(cm_hir_metadata_read_u32(&reader, &count)
        == CM_HIR_METADATA_OK);
    for (index = 0u; index < count; ++index) {
        uint32_t module;
        uint32_t wire_name_length;
        const unsigned char *name;
        uint8_t kind;
        uint32_t target;

        assert(cm_hir_metadata_read_u32(&reader, &module)
            == CM_HIR_METADATA_OK);
        assert(cm_hir_metadata_read_u32(&reader, &wire_name_length)
            == CM_HIR_METADATA_OK);
        assert(cm_hir_metadata_read_bytes(&reader,
            (size_t)wire_name_length, &name) == CM_HIR_METADATA_OK);
        assert(cm_hir_metadata_read_u8(&reader, &kind)
            == CM_HIR_METADATA_OK);
        assert(cm_hir_metadata_read_u32(&reader, &target)
            == CM_HIR_METADATA_OK);
        (void)module;
        (void)kind;
        (void)target;
        if ((size_t)wire_name_length == name_length
            && memcmp(name, from, name_length) == 0) {
            memcpy((unsigned char *)name, to, name_length);
            recompute_metadata_crc(encoded);
            return;
        }
    }
    assert(0 && "namespace name not found");
}

static void corrupt_default_to_self_parameter(CmByteBuf *encoded)
{
    CmHirMetadataEnvelope envelope;
    CmHirMetadataReader sections_reader;
    CmHirMetadataSection generic_section;
    CmHirMetadataSection type_section;
    CmHirMetadataReader reader;
    uint32_t generic_count;
    uint32_t type_count;
    uint32_t index;

    assert(cm_hir_metadata_decode_envelope(encoded->data, encoded->len,
        &envelope) == CM_HIR_METADATA_OK);
    cm_hir_metadata_reader_init(&sections_reader, envelope.payload,
        envelope.payload_length);
    for (index = 0u; index < 3u; ++index) {
        assert(cm_hir_metadata_read_section(&sections_reader,
            &generic_section) == CM_HIR_METADATA_OK);
    }
    assert(cm_hir_metadata_read_section(&sections_reader, &type_section)
        == CM_HIR_METADATA_OK);
    cm_hir_metadata_reader_init(&reader, type_section.data,
        type_section.length);
    assert(cm_hir_metadata_read_u32(&reader, &type_count)
        == CM_HIR_METADATA_OK);
    assert(type_count != 0u);
    cm_hir_metadata_reader_init(&reader, generic_section.data,
        generic_section.length);
    assert(cm_hir_metadata_read_u32(&reader, &generic_count)
        == CM_HIR_METADATA_OK);
    for (index = 0u; index < generic_count; ++index) {
        uint32_t owner;
        uint32_t parameter_index;
        uint8_t kind;
        uint32_t name_length;
        const unsigned char *name;
        uint8_t relaxed;
        uint8_t has_default;
        uint32_t default_type;
        unsigned char *default_bytes;

        assert(cm_hir_metadata_read_u32(&reader, &owner)
            == CM_HIR_METADATA_OK);
        assert(cm_hir_metadata_read_u32(&reader, &parameter_index)
            == CM_HIR_METADATA_OK);
        assert(cm_hir_metadata_read_u8(&reader, &kind)
            == CM_HIR_METADATA_OK);
        assert(cm_hir_metadata_read_u32(&reader, &name_length)
            == CM_HIR_METADATA_OK);
        assert(cm_hir_metadata_read_bytes(&reader, (size_t)name_length,
            &name) == CM_HIR_METADATA_OK);
        assert(cm_hir_metadata_read_u8(&reader, &relaxed)
            == CM_HIR_METADATA_OK);
        assert(cm_hir_metadata_read_u8(&reader, &has_default)
            == CM_HIR_METADATA_OK);
        default_bytes = (unsigned char *)reader.data + reader.cursor;
        assert(cm_hir_metadata_read_u32(&reader, &default_type)
            == CM_HIR_METADATA_OK);
        (void)owner;
        (void)parameter_index;
        (void)kind;
        (void)name;
        (void)relaxed;
        (void)default_type;
        if (has_default != 0u) {
            uint32_t value;
            unsigned int byte_index;

            value = type_count;
            for (byte_index = 0u; byte_index < 4u; ++byte_index) {
                default_bytes[byte_index] =
                    (unsigned char)(value & UINT32_C(0xff));
                value >>= 8u;
            }
            recompute_metadata_crc(encoded);
            return;
        }
    }
    assert(0 && "defaulted generic not found");
}

static void corrupt_lifetime_generic_name(CmByteBuf *encoded)
{
    CmHirMetadataEnvelope envelope;
    CmHirMetadataReader sections_reader;
    CmHirMetadataSection generic_section;
    CmHirMetadataReader reader;
    uint32_t generic_count;
    uint32_t index;

    assert(cm_hir_metadata_decode_envelope(encoded->data, encoded->len,
        &envelope) == CM_HIR_METADATA_OK);
    cm_hir_metadata_reader_init(&sections_reader, envelope.payload,
        envelope.payload_length);
    for (index = 0u; index < 3u; ++index) {
        assert(cm_hir_metadata_read_section(&sections_reader,
            &generic_section) == CM_HIR_METADATA_OK);
    }
    cm_hir_metadata_reader_init(&reader, generic_section.data,
        generic_section.length);
    assert(cm_hir_metadata_read_u32(&reader, &generic_count)
        == CM_HIR_METADATA_OK);
    for (index = 0u; index < generic_count; ++index) {
        uint32_t owner;
        uint32_t parameter_index;
        uint8_t kind;
        uint32_t name_length;
        const unsigned char *name;
        uint8_t relaxed;
        uint8_t has_default;
        uint32_t default_type;

        assert(cm_hir_metadata_read_u32(&reader, &owner)
            == CM_HIR_METADATA_OK);
        assert(cm_hir_metadata_read_u32(&reader, &parameter_index)
            == CM_HIR_METADATA_OK);
        assert(cm_hir_metadata_read_u8(&reader, &kind)
            == CM_HIR_METADATA_OK);
        assert(cm_hir_metadata_read_u32(&reader, &name_length)
            == CM_HIR_METADATA_OK);
        assert(cm_hir_metadata_read_bytes(&reader, (size_t)name_length,
            &name) == CM_HIR_METADATA_OK);
        assert(cm_hir_metadata_read_u8(&reader, &relaxed)
            == CM_HIR_METADATA_OK);
        assert(cm_hir_metadata_read_u8(&reader, &has_default)
            == CM_HIR_METADATA_OK);
        assert(cm_hir_metadata_read_u32(&reader, &default_type)
            == CM_HIR_METADATA_OK);
        (void)owner;
        (void)parameter_index;
        (void)relaxed;
        (void)has_default;
        (void)default_type;
        if (kind == UINT8_C(1)) {
            assert(name_length > 1u && name[0] == (unsigned char)'\'');
            ((unsigned char *)name)[0] = (unsigned char)'!';
            recompute_metadata_crc(encoded);
            return;
        }
    }
    assert(0 && "lifetime generic not found");
}

static void test_primitive_only_round_trip(void)
{
    CmHirContext producer_context;
    CmHirCrateId producer_crate;
    CmHirModuleId producer_root;
    const CmHirModule *root;
    CmHirLibraryOwnedData owned;
    size_t root_index;
    CmHirLibraryArtifact producer_artifact;
    CmHirLibraryArtifactResult restored;
    CmByteBuf encoded;
    CmHirMetadataArtifactResult metadata_result;
    CmHirContext consumer_context;
    CmHirLibraryArtifact consumer_artifact;
    CmHirLibraryBinding primitive;

    cm_hir_context_init(&producer_context);
    assert(cm_hir_create_crate(&producer_context,
        cm_hir_intern(&producer_context, "primitives"),
        CM_HIR_EDITION_2021, test_span(0u, 1u), &producer_crate,
        &producer_root) == CM_HIR_OK);
    root = cm_hir_get_module(&producer_context, producer_root);
    assert(root != NULL);
    cm_hir_library_owned_data_init(&owned);
    assert(cm_hir_library_owned_data_add_module(&owned, root->definition,
        &root_index) == CM_HIR_LIBRARY_OK);
    add_entry(&owned, root_index, "bool",
        primitive_binding(CM_HIR_PRIMITIVE_BOOL));
    cm_hir_library_artifact_init(&producer_artifact);
    restored = cm_hir_library_artifact_restore_owned(&producer_artifact,
        &producer_context, producer_crate, root->definition, "producer",
        &owned);
    assert(restored.status == CM_HIR_LIBRARY_OK);
    cm_hir_library_owned_data_destroy(&owned);
    cm_byte_buf_init(&encoded);
    metadata_result = cm_hir_metadata_encode_artifact(&encoded,
        &producer_artifact);
    assert(metadata_result.status == CM_HIR_METADATA_ARTIFACT_OK);
    cm_hir_library_artifact_destroy(&producer_artifact);
    cm_hir_context_destroy(&producer_context);

    cm_hir_context_init(&consumer_context);
    cm_hir_library_artifact_init(&consumer_artifact);
    metadata_result = cm_hir_metadata_decode_semantic_artifact(
        &consumer_context, &consumer_artifact, encoded.data, encoded.len,
        "semantic", 90u);
    assert(metadata_result.status == CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT);
    assert(consumer_context.crates.len == 0u
        && consumer_context.items.len == 0u);
    metadata_result = cm_hir_metadata_decode_artifact(&consumer_context,
        &consumer_artifact, encoded.data, encoded.len, "dep", 91u);
    assert(metadata_result.status == CM_HIR_METADATA_ARTIFACT_OK);
    assert(metadata_result.module_count == 1u);
    assert(metadata_result.public_entry_count == 1u);
    primitive = lookup(&consumer_artifact, "dep", "bool", NULL);
    assert(primitive.kind == CM_HIR_LIBRARY_BINDING_PRIMITIVE);
    assert(primitive.primitive_kind == CM_HIR_PRIMITIVE_BOOL);
    assert(consumer_context.items.len == 0u);

    cm_hir_library_artifact_destroy(&consumer_artifact);
    cm_hir_context_destroy(&consumer_context);
    cm_byte_buf_destroy(&encoded);
}

static void init_empty_artifact(CmHirContext *context, CmHirCrateId crate_id,
    CmHirModuleId root_module, CmHirLibraryArtifact *artifact)
{
    const CmHirModule *root;
    CmHirLibraryOwnedData owned;
    CmHirLibraryArtifactResult restored;
    size_t root_index;

    root = cm_hir_get_module(context, root_module);
    assert(root != NULL);
    cm_hir_library_owned_data_init(&owned);
    assert(cm_hir_library_owned_data_add_module(&owned, root->definition,
        &root_index) == CM_HIR_LIBRARY_OK);
    (void)root_index;
    cm_hir_library_artifact_init(artifact);
    restored = cm_hir_library_artifact_restore_owned(artifact, context,
        crate_id, root->definition, "producer", &owned);
    assert(restored.status == CM_HIR_LIBRARY_OK);
    cm_hir_library_owned_data_destroy(&owned);
}

static void assert_semantic_encode_unsupported(CmHirContext *context,
    CmHirCrateId crate_id, CmHirModuleId root_module)
{
    static const unsigned char sentinel[] = { 'k', 'e', 'e', 'p' };
    CmHirLibraryArtifact artifact;
    CmByteBuf encoded;
    CmHirMetadataArtifactResult result;

    init_empty_artifact(context, crate_id, root_module, &artifact);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_append(&encoded, sentinel, sizeof(sentinel));
    result = cm_hir_metadata_encode_semantic_artifact(&encoded, &artifact);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_UNSUPPORTED_HIR);
    assert(encoded.len == sizeof(sentinel)
        && memcmp(encoded.data, sentinel, sizeof(sentinel)) == 0);
    cm_byte_buf_destroy(&encoded);
    cm_hir_library_artifact_destroy(&artifact);
}

static void test_semantic_unsupported_producers(void)
{
    CmHirContext context;
    CmHirCrateId foreign_crate;
    CmHirCrateId local_crate;
    CmHirModuleId foreign_root;
    CmHirModuleId local_root;
    CmHirDefId foreign_trait;
    CmHirDefId implemented_trait;
    CmHirDefId impl_definition;
    CmHirDefId trait_member_definition;
    CmHirDefId impl_member_definition;
    CmHirDefId predicate_trait;
    CmHirTypeId u8_type;
    CmHirItem item;
    CmHirItemId item_id;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "default_method"), CM_HIR_EDITION_2024,
        test_span(1u, 10u), &local_crate, &local_root) == CM_HIR_OK);
    u8_type = add_integer_type(&context, CM_HIR_INT_U8, 2u);
    implemented_trait = add_metadata_trait(&context, local_crate,
        local_root, "Defaulted", 0, 3u);
    assert(cm_hir_reserve_item_definition_as(&context, local_crate,
        CM_HIR_ITEM_FUNCTION, test_span(4u, 5u),
        &trait_member_definition) == CM_HIR_OK);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_FUNCTION;
    item.definition = trait_member_definition;
    item.owner_module = local_root;
    item.parent_definition = implemented_trait;
    item.name = cm_hir_intern(&context, "provided");
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(4u, 5u);
    item.data.function_item.signature.return_type = u8_type;
    item.data.function_item.signature.abi = cm_hir_intern(&context, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.has_default_body = 1;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    assert_semantic_encode_unsupported(&context, local_crate, local_root);
    cm_hir_context_destroy(&context);

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "default_const"), CM_HIR_EDITION_2024,
        test_span(1u, 10u), &local_crate, &local_root) == CM_HIR_OK);
    u8_type = add_integer_type(&context, CM_HIR_INT_U8, 2u);
    implemented_trait = add_metadata_trait(&context, local_crate,
        local_root, "Defaulted", 0, 3u);
    assert(cm_hir_reserve_item_definition_as(&context, local_crate,
        CM_HIR_ITEM_CONST, test_span(4u, 5u), &trait_member_definition)
        == CM_HIR_OK);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_CONST;
    item.definition = trait_member_definition;
    item.owner_module = local_root;
    item.parent_definition = implemented_trait;
    item.name = cm_hir_intern(&context, "PROVIDED");
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(4u, 5u);
    item.data.value_item.type = u8_type;
    item.data.value_item.mutability = CM_HIR_IMMUTABLE;
    item.data.value_item.has_default_body = 1;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    assert_semantic_encode_unsupported(&context, local_crate, local_root);
    cm_hir_context_destroy(&context);

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "foreign_traits"), CM_HIR_EDITION_2024,
        test_span(1u, 10u), &foreign_crate, &foreign_root) == CM_HIR_OK);
    foreign_trait = add_metadata_trait(&context, foreign_crate,
        foreign_root, "Foreign", 0, 2u);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "local_impl"), CM_HIR_EDITION_2024,
        test_span(11u, 20u), &local_crate, &local_root) == CM_HIR_OK);
    u8_type = add_integer_type(&context, CM_HIR_INT_U8, 12u);
    (void)add_metadata_impl(&context, local_crate, local_root,
        foreign_trait, u8_type, 0, 13u);
    assert_semantic_encode_unsupported(&context, local_crate, local_root);
    cm_hir_context_destroy(&context);

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "generic_impl"), CM_HIR_EDITION_2024,
        test_span(1u, 10u), &local_crate, &local_root) == CM_HIR_OK);
    implemented_trait = add_metadata_trait(&context, local_crate,
        local_root, "Generic", 0, 2u);
    add_metadata_generic_impl(&context, local_crate, local_root,
        implemented_trait, 3u);
    assert_semantic_encode_unsupported(&context, local_crate, local_root);
    cm_hir_context_destroy(&context);

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "predicate_impl"), CM_HIR_EDITION_2024,
        test_span(1u, 12u), &local_crate, &local_root) == CM_HIR_OK);
    u8_type = add_integer_type(&context, CM_HIR_INT_U8, 2u);
    implemented_trait = add_metadata_trait(&context, local_crate,
        local_root, "Implemented", 0, 3u);
    predicate_trait = add_metadata_trait(&context, local_crate,
        local_root, "Predicate", 0, 4u);
    add_metadata_predicate_impl(&context, local_crate, local_root,
        implemented_trait, predicate_trait, u8_type, 5u);
    assert_semantic_encode_unsupported(&context, local_crate, local_root);
    cm_hir_context_destroy(&context);

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "const_trait"), CM_HIR_EDITION_2024,
        test_span(1u, 10u), &local_crate, &local_root) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&context, local_crate,
        CM_HIR_ITEM_TRAIT, test_span(2u, 3u), &implemented_trait)
        == CM_HIR_OK);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_TRAIT;
    item.definition = implemented_trait;
    item.owner_module = local_root;
    item.name = cm_hir_intern(&context, "ConstTrait");
    item.visibility.kind = CM_HIR_VIS_PUBLIC;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(2u, 3u);
    item.data.trait_item.safety = CM_HIR_SAFE;
    item.data.trait_item.is_const = 1;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    assert_semantic_encode_unsupported(&context, local_crate, local_root);
    cm_hir_context_destroy(&context);

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "foreign_const_trait"),
        CM_HIR_EDITION_2024, test_span(1u, 10u), &foreign_crate,
        &foreign_root) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&context, foreign_crate,
        CM_HIR_ITEM_TRAIT, test_span(2u, 3u), &implemented_trait)
        == CM_HIR_OK);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_TRAIT;
    item.definition = implemented_trait;
    item.owner_module = foreign_root;
    item.name = cm_hir_intern(&context, "ConstTrait");
    item.visibility.kind = CM_HIR_VIS_PUBLIC;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(2u, 3u);
    item.data.trait_item.safety = CM_HIR_SAFE;
    item.data.trait_item.is_const = 1;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "const_impl"), CM_HIR_EDITION_2024,
        test_span(11u, 20u), &local_crate, &local_root) == CM_HIR_OK);
    u8_type = add_integer_type(&context, CM_HIR_INT_U8, 12u);
    assert(cm_hir_reserve_item_definition_as(&context, local_crate,
        CM_HIR_ITEM_IMPL, test_span(13u, 14u), &impl_definition)
        == CM_HIR_OK);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_IMPL;
    item.definition = impl_definition;
    item.owner_module = local_root;
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(13u, 14u);
    item.data.impl_item.self_type = u8_type;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = implemented_trait;
    item.data.impl_item.safety = CM_HIR_SAFE;
    item.data.impl_item.is_const = 1;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    assert_semantic_encode_unsupported(&context, local_crate, local_root);
    cm_hir_context_destroy(&context);

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "specializable_impl"),
        CM_HIR_EDITION_2024, test_span(1u, 20u), &local_crate,
        &local_root) == CM_HIR_OK);
    u8_type = add_integer_type(&context, CM_HIR_INT_U8, 2u);
    implemented_trait = add_metadata_trait(&context, local_crate,
        local_root, "Specialize", 0, 3u);
    assert(cm_hir_reserve_item_definition_as(&context, local_crate,
        CM_HIR_ITEM_TYPE_ALIAS, test_span(4u, 5u),
        &trait_member_definition) == CM_HIR_OK);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_TYPE_ALIAS;
    item.definition = trait_member_definition;
    item.owner_module = local_root;
    item.parent_definition = implemented_trait;
    item.name = cm_hir_intern(&context, "Output");
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(4u, 5u);
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    impl_definition = add_metadata_impl(&context, local_crate, local_root,
        implemented_trait, u8_type, 0, 6u);
    assert(cm_hir_reserve_item_definition_as(&context, local_crate,
        CM_HIR_ITEM_TYPE_ALIAS, test_span(7u, 8u),
        &impl_member_definition) == CM_HIR_OK);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_TYPE_ALIAS;
    item.definition = impl_member_definition;
    item.owner_module = local_root;
    item.parent_definition = impl_definition;
    item.is_specializable = 1;
    item.name = cm_hir_intern(&context, "Output");
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(7u, 8u);
    item.data.type_alias_item.target = u8_type;
    item.data.type_alias_item.trait_item_definition =
        trait_member_definition;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    assert_semantic_encode_unsupported(&context, local_crate, local_root);
    cm_hir_context_destroy(&context);
}

static void test_semantic_trait_universe_round_trip(void)
{
    CmHirContext producer;
    CmHirCrateId producer_crate;
    CmHirModuleId producer_root;
    const CmHirModule *root_value;
    CmHirTypeId u8_type;
    CmHirDefId ordinary_trait;
    CmHirDefId auto_trait;
    CmHirLibraryOwnedData owned;
    size_t root_index;
    CmHirLibraryBinding binding;
    CmHirLibraryArtifact producer_artifact;
    CmHirLibraryArtifactResult restored;
    CmByteBuf encoded;
    CmByteBuf reencoded;
    CmByteBuf corrupted;
    CmHirMetadataArtifactResult metadata_result;
    CmHirContext consumer;
    CmHirLibraryArtifact consumer_artifact;
    CmHirCrateId sentinel_crate;
    CmHirModuleId sentinel_last_module;
    ContextLengths before_failure;
    CmHirLibraryArtifactIdentity sentinel_identity;
    CmHirLibraryBinding loaded_ordinary;
    CmHirLibraryBinding loaded_auto;
    CmTraitImplIndex impl_index;
    const CmTraitImplIndexEntry *entry;
    size_t entry_index;
    size_t positive_count;
    size_t negative_count;
    SemanticMetadataCorruption corruption;

    cm_hir_context_init(&producer);
    assert(cm_hir_create_crate(&producer,
        cm_hir_intern(&producer, "semantic_dep"), CM_HIR_EDITION_2024,
        test_span(1u, 20u), &producer_crate, &producer_root) == CM_HIR_OK);
    u8_type = add_integer_type(&producer, CM_HIR_INT_U8, 2u);
    ordinary_trait = add_metadata_trait(&producer, producer_crate,
        producer_root, "Ordinary", 0, 3u);
    auto_trait = add_metadata_trait(&producer, producer_crate,
        producer_root, "Auto", 1, 4u);
    (void)add_metadata_impl(&producer, producer_crate, producer_root,
        ordinary_trait, u8_type, 0, 5u);
    (void)add_metadata_impl(&producer, producer_crate, producer_root,
        auto_trait, u8_type, 1, 6u);

    root_value = cm_hir_get_module(&producer, producer_root);
    assert(root_value != NULL);
    cm_hir_library_owned_data_init(&owned);
    assert(cm_hir_library_owned_data_add_module(&owned,
        root_value->definition, &root_index) == CM_HIR_LIBRARY_OK);
    memset(&binding, 0, sizeof(binding));
    binding.kind = CM_HIR_LIBRARY_BINDING_TRAIT;
    binding.definition = ordinary_trait;
    binding.type_kind = CM_HIR_TYPE_ERROR_KIND;
    assert(cm_hir_library_owned_data_add_entry(&owned, root_index,
        (const unsigned char *)"Ordinary", strlen("Ordinary"), &binding)
        == CM_HIR_LIBRARY_OK);
    binding.definition = auto_trait;
    assert(cm_hir_library_owned_data_add_entry(&owned, root_index,
        (const unsigned char *)"Auto", strlen("Auto"), &binding)
        == CM_HIR_LIBRARY_OK);
    cm_hir_library_artifact_init(&producer_artifact);
    restored = cm_hir_library_artifact_restore_owned(&producer_artifact,
        &producer, producer_crate, root_value->definition, "producer",
        &owned);
    assert(restored.status == CM_HIR_LIBRARY_OK);
    cm_hir_library_owned_data_destroy(&owned);

    cm_byte_buf_init(&encoded);
    metadata_result = cm_hir_metadata_encode_artifact(&encoded,
        &producer_artifact);
    assert(metadata_result.status
        == CM_HIR_METADATA_ARTIFACT_UNSUPPORTED_HIR);
    assert(encoded.len == 0u);
    metadata_result = cm_hir_metadata_encode_semantic_artifact(&encoded,
        &producer_artifact);
    assert(metadata_result.status == CM_HIR_METADATA_ARTIFACT_OK);
    assert(encoded.len > CM_HIR_METADATA_HEADER_SIZE);
    assert(encoded.data[10] == UINT8_C(1) && encoded.data[11] == UINT8_C(0));

    consumer_sentinel_init(&consumer, &consumer_artifact, &sentinel_crate,
        &sentinel_last_module);
    before_failure = context_lengths(&consumer);
    assert(cm_hir_library_artifact_identity(&consumer_artifact,
        &sentinel_identity));
    metadata_result = cm_hir_metadata_decode_artifact(&consumer,
        &consumer_artifact, encoded.data, encoded.len, "legacy", 9u);
    assert(metadata_result.status == CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT);
    assert_sentinel_preserved(&consumer, &consumer_artifact,
        before_failure, &sentinel_identity);
    for (corruption = SEMANTIC_CORRUPT_CLOSED;
            corruption <= SEMANTIC_CORRUPT_NEGATIVE_SAFETY;
            corruption = (SemanticMetadataCorruption)(corruption + 1)) {
        cm_byte_buf_init(&corrupted);
        cm_byte_buf_append(&corrupted, encoded.data, encoded.len);
        corrupt_trait_universe(&corrupted, corruption);
        metadata_result = cm_hir_metadata_decode_semantic_artifact(&consumer,
            &consumer_artifact, corrupted.data, corrupted.len, "broken",
            9u);
        assert(metadata_result.status
            == CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT);
        assert_sentinel_preserved(&consumer, &consumer_artifact,
            before_failure, &sentinel_identity);
        cm_byte_buf_destroy(&corrupted);
    }
    metadata_result = cm_hir_metadata_decode_semantic_artifact(&consumer,
        &consumer_artifact, encoded.data, encoded.len, "dep", 10u);
    assert(metadata_result.status == CM_HIR_METADATA_ARTIFACT_OK);
    assert(metadata_result.crate_id > sentinel_crate);
    assert(metadata_result.root_module > sentinel_last_module);
    loaded_ordinary = lookup(&consumer_artifact, "dep", "Ordinary", NULL);
    loaded_auto = lookup(&consumer_artifact, "dep", "Auto", NULL);
    assert(loaded_ordinary.kind == CM_HIR_LIBRARY_BINDING_TRAIT);
    assert(loaded_auto.kind == CM_HIR_LIBRARY_BINDING_TRAIT);
    assert(!cm_hir_def_id_equal(loaded_ordinary.definition,
        loaded_auto.definition));

    memset(&impl_index, 0, sizeof(impl_index));
    assert(cm_trait_impl_index_init(&impl_index, &consumer,
        metadata_result.crate_id, CM_TRAIT_IMPL_UNIVERSE_OPEN)
        == CM_TRAIT_SOLVER_PROVEN);
    assert(cm_trait_impl_index_entry_count(&impl_index) == 2u);
    positive_count = 0u;
    negative_count = 0u;
    for (entry_index = 0u;
            entry_index < cm_trait_impl_index_entry_count(&impl_index);
            ++entry_index) {
        entry = cm_trait_impl_index_entry(&impl_index, entry_index);
        assert(entry != NULL);
        if (cm_hir_def_id_equal(entry->trait_definition,
                loaded_ordinary.definition)) {
            assert(entry->unsupported_flags == CM_TRAIT_IMPL_UNSUPPORTED_NONE);
            positive_count += 1u;
        } else if (cm_hir_def_id_equal(entry->trait_definition,
                loaded_auto.definition)) {
            assert((entry->unsupported_flags
                & CM_TRAIT_IMPL_UNSUPPORTED_AUTO_TRAIT) != 0u);
            assert((entry->unsupported_flags
                & CM_TRAIT_IMPL_UNSUPPORTED_NEGATIVE) != 0u);
            negative_count += 1u;
        }
    }
    assert(positive_count == 1u && negative_count == 1u);
    cm_trait_impl_index_destroy(&impl_index);
    cm_byte_buf_init(&reencoded);
    metadata_result = cm_hir_metadata_encode_semantic_artifact(&reencoded,
        &consumer_artifact);
    assert(metadata_result.status == CM_HIR_METADATA_ARTIFACT_OK
        && reencoded.len == encoded.len
        && memcmp(reencoded.data, encoded.data, encoded.len) == 0);
    cm_byte_buf_destroy(&reencoded);
    cm_hir_library_artifact_destroy(&consumer_artifact);
    cm_hir_context_destroy(&consumer);
    cm_byte_buf_destroy(&encoded);
    cm_hir_library_artifact_destroy(&producer_artifact);
    cm_hir_context_destroy(&producer);
}

static void assert_encode_unsupported(CmHirContext *context,
    CmHirCrateId crate_id, CmHirModuleId root_module)
{
    CmHirLibraryArtifact artifact;
    CmByteBuf encoded;
    CmHirMetadataArtifactResult result;

    init_empty_artifact(context, crate_id, root_module, &artifact);
    cm_byte_buf_init(&encoded);
    result = cm_hir_metadata_encode_artifact(&encoded, &artifact);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_UNSUPPORTED_HIR);
    assert(encoded.len == 0u);
    result = cm_hir_metadata_encode_declaration_artifact(&encoded,
        &artifact);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_UNSUPPORTED_HIR);
    assert(encoded.len == 0u);
    cm_byte_buf_destroy(&encoded);
    cm_hir_library_artifact_destroy(&artifact);
}

static void test_unsupported_hir_rejected(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root;
    CmHirDefId definition;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirTypeId u32_type;
    CmHirType type;
    CmHirTypeId array_type;
    CmHirField field;
    CmHirDefId alias_a;
    CmHirDefId alias_b;
    CmHirTypeId target_a;
    CmHirTypeId target_b;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context, cm_hir_intern(&context, "badtrait"),
        CM_HIR_EDITION_2021, test_span(1u, 2u), &crate_id, &root)
        == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TRAIT, test_span(2u, 3u), &definition) == CM_HIR_OK);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_TRAIT;
    item.definition = definition;
    item.owner_module = root;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(&context, "Trait");
    item.visibility.kind = CM_HIR_VIS_PUBLIC;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(2u, 3u);
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    assert_encode_unsupported(&context, crate_id, root);
    cm_hir_context_destroy(&context);

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context, cm_hir_intern(&context, "badconst"),
        CM_HIR_EDITION_2021, test_span(1u, 2u), &crate_id, &root)
        == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TRAIT_ALIAS, test_span(2u, 3u), &definition)
        == CM_HIR_OK);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_TRAIT_ALIAS;
    item.definition = definition;
    item.owner_module = root;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(&context, "Alias");
    item.visibility.kind = CM_HIR_VIS_PUBLIC;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(2u, 3u);
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    {
        CmHirLibraryArtifact alias_artifact;
        CmByteBuf alias_encoded;
        CmHirMetadataArtifactResult alias_result;

        init_empty_artifact(&context, crate_id, root, &alias_artifact);
        cm_byte_buf_init(&alias_encoded);
        alias_result = cm_hir_metadata_encode_declaration_artifact(
            &alias_encoded, &alias_artifact);
        assert(alias_result.status
            == CM_HIR_METADATA_ARTIFACT_UNSUPPORTED_HIR);
        assert(alias_encoded.len == 0u);
        cm_byte_buf_destroy(&alias_encoded);
        cm_hir_library_artifact_destroy(&alias_artifact);
    }
    cm_hir_context_destroy(&context);

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "badunevaluated"), CM_HIR_EDITION_2021,
        test_span(1u, 2u), &crate_id, &root) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_STRUCT, test_span(2u, 8u), &definition) == CM_HIR_OK);
    u32_type = add_integer_type(&context, CM_HIR_INT_U32, 3u);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ARRAY_KIND;
    type.span = test_span(4u, 5u);
    type.data.array_type.element = u32_type;
    type.data.array_type.length.kind = CM_HIR_CONST_UNEVALUATED;
    type.data.array_type.length.type = u32_type;
    type.data.array_type.length.data.definition = definition;
    assert(cm_hir_add_type(&context, &type, &array_type) == CM_HIR_OK);
    memset(&field, 0, sizeof(field));
    field.name = cm_hir_intern(&context, "values");
    field.type = array_type;
    field.visibility.kind = CM_HIR_VIS_PRIVATE;
    field.visibility.restriction = cm_hir_def_id_none();
    field.span = test_span(5u, 6u);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_STRUCT;
    item.definition = definition;
    item.owner_module = root;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(&context, "BadArray");
    item.visibility.kind = CM_HIR_VIS_PUBLIC;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(2u, 8u);
    item.data.aggregate_item.form = CM_HIR_AGGREGATE_NAMED;
    item.data.aggregate_item.fields = &field;
    item.data.aggregate_item.field_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    assert_encode_unsupported(&context, crate_id, root);
    cm_hir_context_destroy(&context);

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context, cm_hir_intern(&context, "badcycle"),
        CM_HIR_EDITION_2021, test_span(1u, 2u), &crate_id, &root)
        == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TYPE_ALIAS, test_span(2u, 3u), &alias_a) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TYPE_ALIAS, test_span(3u, 4u), &alias_b) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ALIAS_APPLICATION_KIND;
    type.span = test_span(4u, 5u);
    type.data.named_type.definition = alias_b;
    assert(cm_hir_add_type(&context, &type, &target_a) == CM_HIR_OK);
    type.data.named_type.definition = alias_a;
    assert(cm_hir_add_type(&context, &type, &target_b) == CM_HIR_OK);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_TYPE_ALIAS;
    item.definition = alias_a;
    item.owner_module = root;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(&context, "A");
    item.visibility.kind = CM_HIR_VIS_PUBLIC;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(2u, 3u);
    item.data.type_alias_item.target = target_a;
    item.data.type_alias_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    item.definition = alias_b;
    item.name = cm_hir_intern(&context, "B");
    item.span = test_span(3u, 4u);
    item.data.type_alias_item.target = target_b;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    assert_encode_unsupported(&context, crate_id, root);
    cm_hir_context_destroy(&context);
}

static void test_parsed_unsupported_hir_rejected(void)
{
    static const unsigned char source[] =
        "pub struct Supported;\n"
        "pub trait Unsupported {}\n";
    static const unsigned char sentinel[] = { 'k', 'e', 'e', 'p' };
    ParsedProducerFixture producer;
    CmByteBuf encoded;
    CmHirMetadataArtifactResult result;

    assert(parsed_producer_build(&producer, source, sizeof(source) - 1u,
        1u, 2u, 0u, 0));
    cm_byte_buf_init(&encoded);
    cm_byte_buf_append(&encoded, sentinel, sizeof(sentinel));
    result = cm_hir_metadata_encode_artifact(&encoded, &producer.artifact);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_UNSUPPORTED_HIR);
    assert(encoded.len == sizeof(sentinel));
    assert(memcmp(encoded.data, sentinel, sizeof(sentinel)) == 0);
    cm_byte_buf_destroy(&encoded);
    parsed_producer_destroy(&producer);
}

static void test_bound_function_pointer_v2_rejected_transactionally(void)
{
    static const unsigned char source[] =
        "pub fn bound(callback: for<'a> fn(&'a u8)) { let _ = callback; }\n";
    static const unsigned char sentinel[] = { 'k', 'e', 'e', 'p' };
    ParsedProducerFixture producer;
    CmByteBuf encoded;
    CmHirMetadataArtifactResult result;

    assert(parsed_producer_build(&producer, source, sizeof(source) - 1u,
        1u, 0u, 1u, 1));
    cm_byte_buf_init(&encoded);
    cm_byte_buf_append(&encoded, sentinel, sizeof(sentinel));
    result = cm_hir_metadata_encode_declaration_artifact(&encoded,
        &producer.artifact);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_UNSUPPORTED_HIR
        && encoded.len == sizeof(sentinel)
        && memcmp(encoded.data, sentinel, sizeof(sentinel)) == 0);
    cm_byte_buf_destroy(&encoded);
    parsed_producer_destroy(&producer);
}

static void test_semantic_round_trip(void)
{
    ProducerFixture producer;
    CmByteBuf first;
    CmByteBuf second;
    CmByteBuf corrupted;
    CmHirMetadataArtifactResult encoded;
    CmHirContext consumer;
    CmHirLibraryArtifact artifact;
    CmHirMetadataArtifactResult decoded;
    CmHirLibraryArtifactIdentity identity_before_failure;
    CmHirLibraryArtifactIdentity identity_after_failure;
    CmHirLibraryBinding root_api;
    CmHirLibraryBinding child_api;
    CmHirLibraryBinding alias;
    CmHirLibraryBinding renamed;
    CmHirLibraryBinding primitive;
    CmHirLibraryBinding zeta;
    const CmHirDefinition *definition;
    const CmHirItem *item;
    ContextLengths before_failure;

    producer_init(&producer, 0);
    cm_byte_buf_init(&first);
    cm_byte_buf_init(&second);
    encoded = cm_hir_metadata_encode_artifact(&first, &producer.artifact);
    assert(encoded.status == CM_HIR_METADATA_ARTIFACT_OK);
    assert(encoded.module_count == 3u);
    assert(encoded.public_entry_count == 8u);
    assert(first.len != 0u);
    encoded = cm_hir_metadata_encode_artifact(&second, &producer.artifact);
    assert(encoded.status == CM_HIR_METADATA_ARTIFACT_OK);
    assert(second.len == first.len);
    assert(memcmp(second.data, first.data, first.len) == 0);
    producer_destroy(&producer);

    cm_hir_context_init(&consumer);
    cm_hir_library_artifact_init(&artifact);
    decoded = cm_hir_metadata_decode_artifact(&consumer, &artifact,
        first.data, first.len, "dep", 77u);
    assert(decoded.status == CM_HIR_METADATA_ARTIFACT_OK);
    assert(decoded.crate_id == 1u);
    assert(decoded.module_count == 3u);
    assert(decoded.public_entry_count == 8u);
    root_api = lookup(&artifact, "dep", "RootApi", NULL);
    child_api = lookup(&artifact, "dep", "child", "ChildApi");
    alias = lookup(&artifact, "dep", "Alias", NULL);
    renamed = lookup(&artifact, "dep", "child", "Renamed");
    primitive = lookup(&artifact, "dep", "usize", NULL);
    assert(root_api.kind == CM_HIR_LIBRARY_BINDING_TYPE);
    assert(child_api.kind == CM_HIR_LIBRARY_BINDING_TYPE);
    assert(alias.kind == CM_HIR_LIBRARY_BINDING_TYPE);
    assert(cm_hir_def_id_equal(alias.definition, renamed.definition));
    assert(!cm_hir_def_id_equal(child_api.definition,
        root_api.definition));
    assert(primitive.kind == CM_HIR_LIBRARY_BINDING_PRIMITIVE);
    assert(primitive.primitive_kind == CM_HIR_PRIMITIVE_USIZE);
    primitive = lookup(&artifact, "dep", "child", "bool");
    assert(primitive.kind == CM_HIR_LIBRARY_BINDING_PRIMITIVE);
    assert(primitive.primitive_kind == CM_HIR_PRIMITIVE_BOOL);
    zeta = lookup(&artifact, "dep", "zeta", NULL);
    assert(zeta.kind == CM_HIR_LIBRARY_BINDING_MODULE);
    definition = cm_hir_lookup_definition(&consumer, root_api.definition);
    item = definition == NULL ? NULL
        : cm_hir_get_item(&consumer, definition->entity.item_id);
    assert(item != NULL && item->kind == CM_HIR_ITEM_EXTERN_TYPE);
    assert(item->span.source == 77u && item->span.start == 0u
        && item->span.end == 0u);
    assert_loaded_declarations(&consumer, &artifact);

    assert(cm_hir_library_artifact_identity(&artifact,
        &identity_before_failure));
    before_failure = context_lengths(&consumer);
    corrupted = first;
    corrupted.data = (unsigned char *)cm_alloc(first.len);
    memcpy(corrupted.data, first.data, first.len);
    corrupted.cap = first.len;
    corrupted.data[first.len - 1u] ^= UINT8_C(0x80);
    decoded = cm_hir_metadata_decode_artifact(&consumer, &artifact,
        corrupted.data, corrupted.len, "broken", 88u);
    assert(decoded.status == CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT);
    assert_context_lengths(&consumer, before_failure);
    assert(cm_hir_library_artifact_identity(&artifact,
        &identity_after_failure));
    assert(identity_after_failure.context == identity_before_failure.context);
    assert(identity_after_failure.extern_name
        == identity_before_failure.extern_name);
    root_api = lookup(&artifact, "dep", "RootApi", NULL);
    assert(root_api.kind == CM_HIR_LIBRARY_BINDING_TYPE);
    decoded = cm_hir_metadata_decode_artifact(&consumer, &artifact,
        first.data, first.len - 1u, "broken", 88u);
    assert(decoded.status == CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT);
    assert_context_lengths(&consumer, before_failure);

    memcpy(corrupted.data, first.data, first.len);
    replace_namespace_name(&corrupted, "usize", "child");
    decoded = cm_hir_metadata_decode_artifact(&consumer, &artifact,
        corrupted.data, corrupted.len, "broken", 88u);
    assert(decoded.status == CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT);
    assert_context_lengths(&consumer, before_failure);
    assert(cm_hir_library_artifact_identity(&artifact,
        &identity_after_failure));
    assert(identity_after_failure.context == identity_before_failure.context);
    assert(identity_after_failure.extern_name
        == identity_before_failure.extern_name);
    root_api = lookup(&artifact, "dep", "RootApi", NULL);
    assert(root_api.kind == CM_HIR_LIBRARY_BINDING_TYPE);

    cm_byte_buf_destroy(&corrupted);
    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&consumer);
    cm_byte_buf_destroy(&second);
    cm_byte_buf_destroy(&first);
}

static void assert_sentinel_preserved(const CmHirContext *context,
    const CmHirLibraryArtifact *artifact, ContextLengths lengths,
    const CmHirLibraryArtifactIdentity *identity)
{
    CmHirLibraryArtifactIdentity current;
    CmHirLibraryBinding keep;

    assert_context_lengths(context, lengths);
    assert(cm_hir_library_artifact_identity(artifact, &current));
    assert(current.context == identity->context);
    assert(current.crate_id == identity->crate_id);
    assert(cm_hir_def_id_equal(current.root_definition,
        identity->root_definition));
    assert(strcmp(current.extern_name, identity->extern_name) == 0);
    keep = lookup(artifact, "sentinel", "Keep", NULL);
    assert(keep.kind == CM_HIR_LIBRARY_BINDING_TYPE);
    assert(keep.type_kind == CM_HIR_TYPE_FOREIGN_KIND);
}

static void test_declaration_v2_value_round_trip(void)
{
    DeclarationProducerFixture producer;
    CmByteBuf encoded;
    CmByteBuf reencoded;
    CmByteBuf legacy_encoded;
    CmByteBuf corrupted;
    CmHirMetadataArtifactResult result;
    CmHirMetadataEnvelope envelope;
    CmHirContext consumer;
    CmHirLibraryArtifact artifact;
    CmHirCrateId sentinel_crate;
    CmHirModuleId sentinel_root;
    CmHirTypeId sentinel_type;
    ContextLengths before;
    CmHirLibraryBinding shared_type;
    CmHirLibraryValue function_value;
    CmHirLibraryValue const_value;
    CmHirLibraryValue static_value;
    const CmHirType *parameter_type;
    const CmHirType *return_type;
    const CmHirDefinition *definition;
    const CmInternedString *abi;
    CmHirLibraryOwnedData legacy_owned;
    CmHirLibraryArtifact legacy_artifact;
    CmHirLibraryArtifactIdentity producer_identity;
    size_t root_index;

    declaration_producer_init(&producer);
    cm_byte_buf_init(&encoded);
    result = cm_hir_metadata_encode_declaration_artifact(&encoded,
        &producer.artifact);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK);
    assert(result.module_count == 1u);
    assert(result.public_entry_count == 4u);
    memset(&envelope, 0, sizeof(envelope));
    assert(cm_hir_metadata_decode_envelope_version(encoded.data,
        encoded.len, (uint16_t)CM_HIR_METADATA_DECLARATION_MAJOR,
        (uint16_t)CM_HIR_METADATA_DECLARATION_MINOR, &envelope)
        == CM_HIR_METADATA_OK);
    assert(cm_hir_metadata_decode_envelope(encoded.data, encoded.len,
        &envelope) == CM_HIR_METADATA_UNSUPPORTED_VERSION);

    cm_byte_buf_init(&reencoded);
    reencoded.data = (unsigned char *)cm_alloc(1u);
    reencoded.data[0] = UINT8_C(0x5a);
    reencoded.len = 1u;
    reencoded.cap = 1u;
    result = cm_hir_metadata_encode_artifact(&reencoded,
        &producer.artifact);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_INVALID_HIR);
    assert(reencoded.len == 1u && reencoded.data[0] == UINT8_C(0x5a));
    cm_byte_buf_destroy(&reencoded);

    assert(cm_hir_library_artifact_identity(&producer.artifact,
        &producer_identity));
    cm_hir_library_owned_data_init(&legacy_owned);
    assert(cm_hir_library_owned_data_add_module(&legacy_owned,
        producer_identity.root_definition, &root_index)
        == CM_HIR_LIBRARY_OK);
    add_entry(&legacy_owned, root_index, "Shared", extern_type_binding(
        producer.shared_type));
    cm_hir_library_artifact_init(&legacy_artifact);
    assert(cm_hir_library_artifact_restore_owned(&legacy_artifact,
        &producer.context, producer_identity.crate_id,
        producer_identity.root_definition, "legacy", &legacy_owned).status
        == CM_HIR_LIBRARY_OK);
    cm_hir_library_owned_data_destroy(&legacy_owned);
    cm_byte_buf_init(&legacy_encoded);
    assert(cm_hir_metadata_encode_artifact(&legacy_encoded,
        &legacy_artifact).status == CM_HIR_METADATA_ARTIFACT_OK);

    cm_hir_context_init(&consumer);
    assert(cm_hir_create_crate(&consumer, cm_hir_intern(&consumer,
        "sentinel"), CM_HIR_EDITION_2021, test_span(0u, 1u),
        &sentinel_crate, &sentinel_root) == CM_HIR_OK);
    sentinel_type = add_integer_type(&consumer, CM_HIR_INT_I16, 2u);
    assert(sentinel_type != CM_HIR_TYPE_NONE);
    cm_hir_library_artifact_init(&artifact);
    before = context_lengths(&consumer);
    result = cm_hir_metadata_decode_artifact(&consumer, &artifact,
        encoded.data, encoded.len, "wrong_v1", 90u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT);
    assert_context_lengths(&consumer, before);
    result = cm_hir_metadata_decode_declaration_artifact(&consumer,
        &artifact, legacy_encoded.data, legacy_encoded.len, "wrong_v2", 90u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT);
    assert_context_lengths(&consumer, before);
    cm_byte_buf_init(&corrupted);
    cm_byte_buf_append(&corrupted, encoded.data, encoded.len);
    point_first_nonfunction_value_at_type(&corrupted,
        append_orphan_late_bound_type(&corrupted));
    result = cm_hir_metadata_decode_declaration_artifact(&consumer,
        &artifact, corrupted.data, corrupted.len, "broken", 90u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT);
    assert_context_lengths(&consumer, before);
    cm_byte_buf_destroy(&corrupted);

    result = cm_hir_metadata_decode_declaration_artifact(&consumer,
        &artifact, encoded.data, encoded.len, "dep", 91u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK);
    assert(result.crate_id > sentinel_crate);
    assert(result.root_module > sentinel_root);
    assert(result.module_count == 1u);
    assert(result.public_entry_count == 4u);
    shared_type = lookup(&artifact, "dep", "Shared", NULL);
    assert(shared_type.kind == CM_HIR_LIBRARY_BINDING_TYPE);
    assert(shared_type.type_kind == CM_HIR_TYPE_FOREIGN_KIND);
    function_value = lookup_value(&artifact, "dep", "Shared");
    assert(function_value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION);
    assert(function_value.data.function.parameter_count == 1u);
    assert(function_value.data.function.safety == CM_HIR_UNSAFE);
    parameter_type = cm_hir_get_type(&consumer,
        function_value.data.function.parameter_types[0]);
    return_type = cm_hir_get_type(&consumer,
        function_value.data.function.return_type);
    assert(parameter_type != NULL
        && parameter_type->kind == CM_HIR_TYPE_INTEGER_KIND
        && parameter_type->data.integer_type.kind == CM_HIR_INT_U32);
    assert(return_type != NULL && return_type->kind == CM_HIR_TYPE_BOOL_KIND);
    assert(function_value.data.function.parameter_types[0]
        != producer.u32_type);
    abi = cm_interner_get(&consumer.strings,
        function_value.data.function.abi);
    assert(abi != NULL && abi->len == 1u && abi->bytes[0] == 'C');
    definition = cm_hir_lookup_definition(&consumer,
        function_value.definition);
    assert(definition != NULL
        && definition->state == CM_HIR_DEFINITION_RESERVED
        && definition->has_reserved_item_kind
        && definition->reserved_item_kind == CM_HIR_ITEM_FUNCTION);
    const_value = lookup_value(&artifact, "dep", "LIMIT");
    assert(const_value.kind == CM_HIR_LIBRARY_VALUE_CONST);
    assert(const_value.data.value.mutability == CM_HIR_IMMUTABLE);
    static_value = lookup_value(&artifact, "dep", "COUNTER");
    assert(static_value.kind == CM_HIR_LIBRARY_VALUE_STATIC);
    assert(static_value.data.value.mutability == CM_HIR_MUTABLE);

    cm_byte_buf_init(&reencoded);
    assert(cm_hir_metadata_encode_declaration_artifact(&reencoded,
        &artifact).status == CM_HIR_METADATA_ARTIFACT_OK);
    assert(reencoded.len == encoded.len);
    assert(memcmp(reencoded.data, encoded.data, encoded.len) == 0);

    cm_byte_buf_destroy(&reencoded);
    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&consumer);
    cm_byte_buf_destroy(&legacy_encoded);
    cm_hir_library_artifact_destroy(&legacy_artifact);
    cm_byte_buf_destroy(&encoded);
    declaration_producer_destroy(&producer);
}

static void test_parsed_declaration_v2_capture(void)
{
    static const unsigned char source[] =
        "pub struct Api;\n"
        "pub unsafe extern \"C\" fn call(value: u32) -> bool { "
            "value == 0 }\n"
        "pub const LIMIT: u32 = 7;\n"
        "pub static COUNTER: u32 = 0;\n";
    ParsedProducerFixture producer;
    CmHirLibraryValue function_value;
    CmHirLibraryValue const_value;
    CmHirLibraryValue static_value;
    CmByteBuf encoded;
    size_t item_index;

    assert(parsed_producer_build(&producer, source, sizeof(source) - 1u,
        1u, 1u, 3u, 1));
    function_value = lookup_value(&producer.artifact, "producer", "call");
    assert(function_value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION);
    assert(function_value.data.function.parameter_count == 1u);
    assert(function_value.data.function.safety == CM_HIR_UNSAFE);
    const_value = lookup_value(&producer.artifact, "producer", "LIMIT");
    assert(const_value.kind == CM_HIR_LIBRARY_VALUE_CONST);
    static_value = lookup_value(&producer.artifact, "producer", "COUNTER");
    assert(static_value.kind == CM_HIR_LIBRARY_VALUE_STATIC);
    assert(static_value.data.value.mutability == CM_HIR_IMMUTABLE);
    cm_byte_buf_init(&encoded);
    assert(cm_hir_metadata_encode_declaration_artifact(&encoded,
        &producer.artifact).status == CM_HIR_METADATA_ARTIFACT_OK);
    cm_byte_buf_destroy(&encoded);
    for (item_index = 0u; item_index < producer.context.items.len;
            ++item_index) {
        CmHirItem *item;
        CmHirLibraryPathSegment path[2];
        CmHirLibraryValue forged_value;
        const CmInternedString *item_name;

        item = (CmHirItem *)cm_vec_at(&producer.context.items, item_index);
        if (item == NULL || (item->kind != CM_HIR_ITEM_FUNCTION
                && item->kind != CM_HIR_ITEM_CONST
                && item->kind != CM_HIR_ITEM_STATIC)) continue;
        if (item->kind == CM_HIR_ITEM_FUNCTION) {
            item->data.function_item.has_default_body = 1;
        } else {
            item->data.value_item.has_default_body = 1;
        }
        item_name = cm_interner_get(&producer.context.strings, item->name);
        assert(item_name != NULL);
        path[0].bytes = (const unsigned char *)"producer";
        path[0].length = strlen("producer");
        path[1].bytes = item_name->bytes;
        path[1].length = item_name->len;
        memset(&forged_value, 0, sizeof(forged_value));
        assert(cm_hir_library_artifact_lookup_value(&producer.artifact,
            path, 2u, &forged_value) == CM_HIR_LIBRARY_INVALID_HIR);
        if (item->kind == CM_HIR_ITEM_FUNCTION) {
            item->data.function_item.has_default_body = 0;
        } else {
            item->data.value_item.has_default_body = 0;
        }
    }
    parsed_producer_destroy(&producer);
}

static int produce_process_artifact(const char *path, int reverse_order)
{
    ParsedProducerFixture producer;
    CmByteBuf encoded;
    CmHirMetadataArtifactResult result;
    int ok;

    if (!parsed_producer_init(&producer, reverse_order)) return 0;
    cm_byte_buf_init(&encoded);
    result = cm_hir_metadata_encode_artifact(&encoded, &producer.artifact);
    ok = result.status == CM_HIR_METADATA_ARTIFACT_OK
        && result.module_count == 3u
        && result.public_entry_count == 12u
        && write_metadata_file(path, &encoded);
    if (!ok) {
        fprintf(stderr,
            "parsed metadata encode failed: %s modules=%lu entries=%lu\n",
            cm_hir_metadata_artifact_status_name(result.status),
            (unsigned long)result.module_count,
            (unsigned long)result.public_entry_count);
    }
    cm_byte_buf_destroy(&encoded);
    parsed_producer_destroy(&producer);
    return ok;
}

static int produce_declaration_process_artifact(const char *path)
{
    DeclarationProducerFixture producer;
    CmByteBuf encoded;
    CmHirMetadataArtifactResult result;
    int ok;

    declaration_producer_init(&producer);
    cm_byte_buf_init(&encoded);
    result = cm_hir_metadata_encode_declaration_artifact(&encoded,
        &producer.artifact);
    ok = result.status == CM_HIR_METADATA_ARTIFACT_OK
        && result.public_entry_count == 4u
        && write_metadata_file(path, &encoded);
    cm_byte_buf_destroy(&encoded);
    declaration_producer_destroy(&producer);
    return ok;
}

static int consume_declaration_process_artifact(const char *path)
{
    CmByteBuf encoded;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirCrateId sentinel_crate;
    CmHirModuleId sentinel_root;
    CmHirTypeId sentinel_type;
    CmHirMetadataArtifactResult result;
    CmHirLibraryValue function_value;
    const CmHirType *parameter_type;
    const CmHirDefinition *definition;
    int ok;

    if (!read_metadata_file(path, &encoded)) return 0;
    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context, cm_hir_intern(&context,
        "process_sentinel"), CM_HIR_EDITION_2021, test_span(0u, 1u),
        &sentinel_crate, &sentinel_root) == CM_HIR_OK);
    sentinel_type = add_integer_type(&context, CM_HIR_INT_I8, 2u);
    assert(sentinel_type != CM_HIR_TYPE_NONE);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_metadata_decode_declaration_artifact(&context,
        &artifact, encoded.data, encoded.len, "dep", 101u);
    ok = result.status == CM_HIR_METADATA_ARTIFACT_OK
        && result.crate_id > sentinel_crate
        && result.root_module > sentinel_root
        && result.public_entry_count == 4u;
    if (ok) {
        function_value = lookup_value(&artifact, "dep", "Shared");
        parameter_type = cm_hir_get_type(&context,
            function_value.data.function.parameter_types[0]);
        definition = cm_hir_lookup_definition(&context,
            function_value.definition);
        ok = function_value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
            && parameter_type != NULL
            && parameter_type->kind == CM_HIR_TYPE_INTEGER_KIND
            && parameter_type->data.integer_type.kind == CM_HIR_INT_U32
            && definition != NULL
            && definition->state == CM_HIR_DEFINITION_RESERVED
            && definition->reserved_item_kind == CM_HIR_ITEM_FUNCTION;
    }
    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_byte_buf_destroy(&encoded);
    return ok;
}

static int produce_generic_function_process_artifact(const char *path)
{
    ParsedProducerFixture producer;
    CmByteBuf encoded;
    CmHirMetadataArtifactResult result;
    int ok;

    if (!parsed_producer_build(&producer, generic_function_source,
            sizeof(generic_function_source) - 1u, 1u, 0u, 1u, 1)) {
        return 0;
    }
    cm_byte_buf_init(&encoded);
    result = cm_hir_metadata_encode_declaration_artifact(&encoded,
        &producer.artifact);
    ok = result.status == CM_HIR_METADATA_ARTIFACT_OK
        && result.public_entry_count == 1u
        && generic_function_valid(&producer.context, &producer.artifact,
            "producer")
        && write_metadata_file(path, &encoded);
    cm_byte_buf_destroy(&encoded);
    parsed_producer_destroy(&producer);
    return ok;
}

static int consume_generic_function_process_artifact(const char *path)
{
    CmByteBuf encoded;
    CmByteBuf reencoded;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirMetadataArtifactResult result;
    int ok;

    if (!read_metadata_file(path, &encoded)) return 0;
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_metadata_decode_declaration_artifact(&context,
        &artifact, encoded.data, encoded.len, "dep", 102u);
    ok = result.status == CM_HIR_METADATA_ARTIFACT_OK
        && result.public_entry_count == 1u
        && generic_function_valid(&context, &artifact, "dep");
    cm_byte_buf_init(&reencoded);
    if (ok) {
        result = cm_hir_metadata_encode_declaration_artifact(&reencoded,
            &artifact);
        ok = result.status == CM_HIR_METADATA_ARTIFACT_OK
            && reencoded.len == encoded.len
            && memcmp(reencoded.data, encoded.data, encoded.len) == 0;
    }
    cm_byte_buf_destroy(&reencoded);
    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_byte_buf_destroy(&encoded);
    return ok;
}

static int produce_predicate_function_process_artifact(const char *path)
{
    ParsedProducerFixture producer;
    CmByteBuf encoded;
    CmHirMetadataArtifactResult result;
    int ok;

    if (!parsed_producer_build(&producer, constrained_function_source,
            sizeof(constrained_function_source) - 1u, 1u, 0u, 1u, 1)) {
        return 0;
    }
    cm_byte_buf_init(&encoded);
    result = cm_hir_metadata_encode_declaration_artifact(&encoded,
        &producer.artifact);
    ok = result.status == CM_HIR_METADATA_ARTIFACT_OK
        && result.public_entry_count == 1u
        && write_metadata_file(path, &encoded);
    cm_byte_buf_destroy(&encoded);
    parsed_producer_destroy(&producer);
    return ok;
}

static int consume_predicate_function_process_artifact(const char *path)
{
    CmByteBuf encoded;
    CmByteBuf reencoded;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirMetadataArtifactResult result;
    CmHirLibraryValue value;
    const CmHirLibraryNominalReference *output;
    const CmHirLibraryNominalReference *fn_once;
    int ok;

    if (!read_metadata_file(path, &encoded)) return 0;
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_metadata_decode_declaration_artifact(&context,
        &artifact, encoded.data, encoded.len, "dep", 106u);
    value = result.status == CM_HIR_METADATA_ARTIFACT_OK
        ? lookup_value(&artifact, "dep", "constrained")
        : (CmHirLibraryValue){0};
    output = value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        ? find_nominal_reference(&value, "Output",
            CM_HIR_LIBRARY_NOMINAL_ASSOCIATED_TYPE) : NULL;
    fn_once = value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        ? find_nominal_reference(&value, "FnOnce",
            CM_HIR_LIBRARY_NOMINAL_TRAIT) : NULL;
    ok = result.status == CM_HIR_METADATA_ARTIFACT_OK
        && result.public_entry_count == 1u
        && value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        && value.data.function.predicate_count == 2u
        && value.data.function.outlives_predicate_count == 1u
        && value.data.function.nominal_reference_count == 5u
        && output != NULL && fn_once != NULL
        && cm_hir_def_id_equal(output->declaring_trait,
            fn_once->definition);
    cm_byte_buf_init(&reencoded);
    if (ok) {
        result = cm_hir_metadata_encode_declaration_artifact(&reencoded,
            &artifact);
        ok = result.status == CM_HIR_METADATA_ARTIFACT_OK
            && reencoded.len == encoded.len
            && memcmp(reencoded.data, encoded.data, encoded.len) == 0;
    }
    cm_byte_buf_destroy(&reencoded);
    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_byte_buf_destroy(&encoded);
    return ok;
}

static int produce_modifier_function_process_artifact(const char *path)
{
    ParsedProducerFixture producer;
    CmByteBuf encoded;
    CmHirMetadataArtifactResult result;
    int ok;

    if (!parsed_producer_build(&producer, carrying_mul_add_source,
            sizeof(carrying_mul_add_source) - 1u, 1u, 0u, 1u, 1)) {
        return 0;
    }
    cm_byte_buf_init(&encoded);
    result = cm_hir_metadata_encode_declaration_artifact(&encoded,
        &producer.artifact);
    ok = result.status == CM_HIR_METADATA_ARTIFACT_OK
        && result.public_entry_count == 1u
        && write_metadata_file(path, &encoded);
    cm_byte_buf_destroy(&encoded);
    parsed_producer_destroy(&producer);
    return ok;
}

static int consume_modifier_function_process_artifact(const char *path)
{
    CmByteBuf encoded;
    CmByteBuf reencoded;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirMetadataArtifactResult result;
    CmHirLibraryValue value;
    const CmHirLibraryNominalReference *carrying;
    const CmHirLibraryNominalReference *unsigned_type;
    int ok;

    if (!read_metadata_file(path, &encoded)) return 0;
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_metadata_decode_declaration_artifact(&context,
        &artifact, encoded.data, encoded.len, "dep", 112u);
    value = result.status == CM_HIR_METADATA_ARTIFACT_OK
        ? lookup_value(&artifact, "dep", "carrying_mul_add")
        : (CmHirLibraryValue){0};
    carrying = value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        ? find_nominal_reference(&value, "CarryingMulAdd",
            CM_HIR_LIBRARY_NOMINAL_TRAIT) : NULL;
    unsigned_type = value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        ? find_nominal_reference(&value, "Unsigned",
            CM_HIR_LIBRARY_NOMINAL_ASSOCIATED_TYPE) : NULL;
    ok = result.status == CM_HIR_METADATA_ARTIFACT_OK
        && result.public_entry_count == 1u
        && value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        && value.data.function.predicate_count == 1u
        && value.data.function.predicates[0].modifier
            == CM_HIR_PREDICATE_CONST_IF_CONST
        && value.data.function.predicates[0].equality_count == 1u
        && carrying != NULL && unsigned_type != NULL
        && cm_hir_def_id_equal(value.data.function.predicates[0]
            .trait_type.definition, carrying->definition)
        && cm_hir_def_id_equal(value.data.function.predicates[0]
            .equalities[0].associated_type, unsigned_type->definition);
    cm_byte_buf_init(&reencoded);
    if (ok) {
        result = cm_hir_metadata_encode_declaration_artifact(&reencoded,
            &artifact);
        ok = result.status == CM_HIR_METADATA_ARTIFACT_OK
            && reencoded.len == encoded.len
            && memcmp(reencoded.data, encoded.data, encoded.len) == 0;
    }
    cm_byte_buf_destroy(&reencoded);
    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_byte_buf_destroy(&encoded);
    return ok;
}

static int produce_trait_alias_function_process_artifact(const char *path)
{
    ParsedProducerFixture producer;
    CmByteBuf encoded;
    CmHirMetadataArtifactResult result;
    int ok;

    if (!parsed_producer_build(&producer, thin_predicate_source,
            sizeof(thin_predicate_source) - 1u, 1u, 0u, 1u, 1)) {
        return 0;
    }
    cm_byte_buf_init(&encoded);
    result = cm_hir_metadata_encode_declaration_artifact(&encoded,
        &producer.artifact);
    ok = result.status == CM_HIR_METADATA_ARTIFACT_OK
        && result.public_entry_count == 1u
        && write_metadata_file(path, &encoded);
    cm_byte_buf_destroy(&encoded);
    parsed_producer_destroy(&producer);
    return ok;
}

static int consume_trait_alias_function_process_artifact(const char *path)
{
    CmByteBuf encoded;
    CmByteBuf reencoded;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirMetadataArtifactResult result;
    CmHirLibraryValue value;
    const CmHirLibraryNominalReference *thin;
    const CmHirLibraryNominalReference *pointee;
    const CmHirLibraryNominalReference *pointee_sized;
    uint32_t alias_count;
    uint32_t index;
    int ok;

    if (!read_metadata_file(path, &encoded)) return 0;
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_metadata_decode_declaration_artifact(&context,
        &artifact, encoded.data, encoded.len, "dep", 115u);
    value = result.status == CM_HIR_METADATA_ARTIFACT_OK
        ? lookup_value(&artifact, "dep", "null")
        : (CmHirLibraryValue){0};
    thin = value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        ? find_nominal_reference(&value, "Thin",
            CM_HIR_LIBRARY_NOMINAL_TRAIT_ALIAS) : NULL;
    pointee = value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        ? find_nominal_reference(&value, "Pointee",
            CM_HIR_LIBRARY_NOMINAL_TRAIT) : NULL;
    pointee_sized = value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        ? find_nominal_reference(&value, "PointeeSized",
            CM_HIR_LIBRARY_NOMINAL_TRAIT) : NULL;
    alias_count = 0u;
    if (value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION && thin != NULL)
        for (index = 0u; index < value.data.function.predicate_count;
                ++index)
            if (cm_hir_def_id_equal(value.data.function.predicates[index]
                    .trait_type.definition, thin->definition)
                && value.data.function.predicates[index].equality_count == 0u)
                alias_count += 1u;
    ok = result.status == CM_HIR_METADATA_ARTIFACT_OK
        && result.public_entry_count == 1u
        && value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        && value.data.function.predicate_count == 2u
        && value.data.function.nominal_reference_count == 3u
        && value.data.function.associated_availability_count == 0u
        && thin != NULL && pointee != NULL && pointee_sized != NULL
        && alias_count == 1u
        && cm_hir_lookup_definition(&context, thin->definition) != NULL;
    cm_byte_buf_init(&reencoded);
    if (ok) {
        result = cm_hir_metadata_encode_declaration_artifact(&reencoded,
            &artifact);
        ok = result.status == CM_HIR_METADATA_ARTIFACT_OK
            && reencoded.len == encoded.len
            && memcmp(reencoded.data, encoded.data, encoded.len) == 0;
    }
    cm_byte_buf_destroy(&reencoded);
    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_byte_buf_destroy(&encoded);
    return ok;
}

static int consume_process_artifact(const char *path)
{
    CmByteBuf encoded;
    CmByteBuf corrupted;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirCrateId sentinel_crate;
    CmHirModuleId sentinel_last_module;
    ContextLengths before;
    CmHirLibraryArtifactIdentity sentinel_identity;
    CmHirMetadataArtifactResult result;
    CmHirLibraryBinding root_api;
    CmHirLibraryBinding child_api;
    CmHirLibraryBinding alias;
    CmHirLibraryBinding renamed;
    CmHirLibraryBinding primitive;
    CmHirLibraryBinding zeta;
    CmHirLibraryArtifactIdentity loaded_identity;
    const CmHirDefinition *definition;
    const CmHirItem *item;
    int ok;

    if (!read_metadata_file(path, &encoded)) return 0;
    consumer_sentinel_init(&context, &artifact, &sentinel_crate,
        &sentinel_last_module);
    before = context_lengths(&context);
    assert(cm_hir_library_artifact_identity(&artifact,
        &sentinel_identity));

    cm_byte_buf_init(&corrupted);
    cm_byte_buf_append(&corrupted, encoded.data, encoded.len);
    assert(corrupted.len > CM_HIR_METADATA_HEADER_SIZE);
    corrupted.data[corrupted.len - 1u] ^= UINT8_C(0x80);
    result = cm_hir_metadata_decode_artifact(&context, &artifact,
        corrupted.data, corrupted.len, "broken", 88u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT);
    assert_sentinel_preserved(&context, &artifact, before,
        &sentinel_identity);

    result = cm_hir_metadata_decode_artifact(&context, &artifact,
        encoded.data, encoded.len - 1u, "broken", 88u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT);
    assert_sentinel_preserved(&context, &artifact, before,
        &sentinel_identity);

    memcpy(corrupted.data, encoded.data, encoded.len);
    replace_namespace_name(&corrupted, "usize", "child");
    result = cm_hir_metadata_decode_artifact(&context, &artifact,
        corrupted.data, corrupted.len, "broken", 88u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT);
    assert_sentinel_preserved(&context, &artifact, before,
        &sentinel_identity);

    memcpy(corrupted.data, encoded.data, encoded.len);
    corrupt_default_to_self_parameter(&corrupted);
    result = cm_hir_metadata_decode_artifact(&context, &artifact,
        corrupted.data, corrupted.len, "broken", 88u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_INVALID_HIR);
    assert_sentinel_preserved(&context, &artifact, before,
        &sentinel_identity);

    memcpy(corrupted.data, encoded.data, encoded.len);
    corrupt_lifetime_generic_name(&corrupted);
    result = cm_hir_metadata_decode_artifact(&context, &artifact,
        corrupted.data, corrupted.len, "broken", 88u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT);
    assert_sentinel_preserved(&context, &artifact, before,
        &sentinel_identity);

    result = cm_hir_metadata_decode_artifact(&context, &artifact,
        encoded.data, encoded.len, "dep", 77u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK);
    assert(result.crate_id > sentinel_crate);
    assert(result.root_module > sentinel_last_module);
    assert(result.module_count == 3u);
    assert(result.public_entry_count == 12u);
    assert(cm_hir_library_artifact_identity(&artifact, &loaded_identity));
    assert(loaded_identity.context == &context);
    assert(loaded_identity.crate_id == result.crate_id);
    assert(strcmp(loaded_identity.extern_name, "dep") == 0);

    root_api = lookup(&artifact, "dep", "RootApi", NULL);
    child_api = lookup(&artifact, "dep", "child", "ChildApi");
    alias = lookup(&artifact, "dep", "Alias", NULL);
    renamed = lookup(&artifact, "dep", "child", "Renamed");
    primitive = lookup(&artifact, "dep", "usize", NULL);
    assert(root_api.kind == CM_HIR_LIBRARY_BINDING_TYPE);
    assert(child_api.kind == CM_HIR_LIBRARY_BINDING_TYPE);
    assert(alias.kind == CM_HIR_LIBRARY_BINDING_TYPE);
    assert(cm_hir_def_id_equal(alias.definition, renamed.definition));
    assert(!cm_hir_def_id_equal(child_api.definition,
        root_api.definition));
    assert(root_api.definition.crate_id == result.crate_id);
    assert(root_api.definition.index != UINT32_C(1));
    assert(primitive.kind == CM_HIR_LIBRARY_BINDING_PRIMITIVE);
    assert(primitive.primitive_kind == CM_HIR_PRIMITIVE_USIZE);
    primitive = lookup(&artifact, "dep", "child", "bool");
    assert(primitive.kind == CM_HIR_LIBRARY_BINDING_PRIMITIVE);
    assert(primitive.primitive_kind == CM_HIR_PRIMITIVE_BOOL);
    zeta = lookup(&artifact, "dep", "zeta", NULL);
    assert(zeta.kind == CM_HIR_LIBRARY_BINDING_MODULE);

    definition = cm_hir_lookup_definition(&context, root_api.definition);
    item = definition == NULL ? NULL
        : cm_hir_get_item(&context, definition->entity.item_id);
    assert(item != NULL && item->kind == CM_HIR_ITEM_EXTERN_TYPE);
    assert(definition->entity.item_id > before.items);
    assert(item->span.source == 77u);
    assert(context.types.len > before.types);
    assert(context.generic_parameters.len > before.generic_parameters);
    assert_loaded_declarations(&context, &artifact);
    assert_loaded_parsed_declarations(&context, &artifact);

    ok = 1;
    cm_byte_buf_destroy(&corrupted);
    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_byte_buf_destroy(&encoded);
    return ok;
}

static void test_declaration_v2_const_generic_round_trip(void)
{
    CmHirContext producer;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    const CmHirModule *module;
    CmHirDefId struct_definition;
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_id;
    CmHirType declared;
    CmHirTypeId usize_type;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirLibraryOwnedData owned;
    size_t root_index;
    CmHirLibraryArtifact artifact;
    CmHirLibraryArtifactResult restored;
    CmByteBuf encoded;
    CmHirMetadataArtifactResult result;
    CmHirContext consumer;
    CmHirLibraryArtifact consumer_artifact;
    size_t item_index;
    const CmHirItem *decoded = NULL;
    CmHirGenericParamId decoded_start;

    cm_hir_context_init(&producer);
    assert(cm_hir_create_crate(&producer,
        cm_hir_intern(&producer, "const_wire"), CM_HIR_EDITION_2024,
        test_span(1u, 40u), &crate_id, &root_module) == CM_HIR_OK);
    module = cm_hir_get_module(&producer, root_module);
    assert(module != NULL);
    assert(cm_hir_reserve_item_definition_as(&producer, crate_id,
        CM_HIR_ITEM_STRUCT, test_span(10u, 14u), &struct_definition)
        == CM_HIR_OK);
    memset(&declared, 0, sizeof(declared));
    declared.kind = CM_HIR_TYPE_INTEGER_KIND;
    declared.span = test_span(11u, 13u);
    declared.data.integer_type.kind = CM_HIR_INT_USIZE;
    assert(cm_hir_add_type(&producer, &declared, &usize_type)
        == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_CONST;
    parameter.owner = struct_definition;
    parameter.name = cm_hir_intern(&producer, "N");
    parameter.declared_type = usize_type;
    parameter.span = test_span(11u, 12u);
    assert(cm_hir_add_generic_param(&producer, &parameter, &parameter_id)
        == CM_HIR_OK);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_STRUCT;
    item.definition = struct_definition;
    item.owner_module = root_module;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(&producer, "Cap");
    item.visibility.kind = CM_HIR_VIS_PUBLIC;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(10u, 14u);
    item.generic_parameter_start = parameter_id;
    item.generic_parameter_count = 1u;
    assert(cm_hir_add_item(&producer, &item, &item_id) == CM_HIR_OK);

    cm_hir_library_owned_data_init(&owned);
    assert(cm_hir_library_owned_data_add_module(&owned,
        module->definition, &root_index) == CM_HIR_LIBRARY_OK);
    add_entry(&owned, root_index, "Cap", type_binding(struct_definition,
        CM_HIR_TYPE_ADT_KIND));
    cm_hir_library_artifact_init(&artifact);
    restored = cm_hir_library_artifact_restore_owned(&artifact, &producer,
        crate_id, module->definition, "producer", &owned);
    assert(restored.status == CM_HIR_LIBRARY_OK);
    cm_hir_library_owned_data_destroy(&owned);

    cm_byte_buf_init(&encoded);
    result = cm_hir_metadata_encode_declaration_artifact(&encoded,
        &artifact);
    if (result.status != CM_HIR_METADATA_ARTIFACT_OK) {
        fprintf(stderr, "const-generic encode got %s\n",
            cm_hir_metadata_artifact_status_name(result.status));
    }
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK);

    cm_hir_context_init(&consumer);
    cm_hir_library_artifact_init(&consumer_artifact);
    result = cm_hir_metadata_decode_declaration_artifact(&consumer,
        &consumer_artifact, encoded.data, encoded.len, "dep", 90u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK);
    for (item_index = 0u; item_index < consumer.items.len; ++item_index) {
        const CmHirItem *candidate = (const CmHirItem *)cm_vec_at_const(
            &consumer.items, item_index);
        const CmInternedString *name;

        if (candidate == NULL || candidate->kind != CM_HIR_ITEM_STRUCT) {
            continue;
        }
        name = cm_interner_get(&consumer.strings, candidate->name);
        if (name != NULL && name->len == 3u
            && memcmp(name->bytes, "Cap", 3u) == 0) {
            decoded = candidate;
            break;
        }
    }
    assert(decoded != NULL
        && decoded->generic_parameter_count == 1u
        && decoded->generic_parameter_start
            != CM_HIR_GENERIC_PARAM_NONE);
    decoded_start = decoded->generic_parameter_start;
    {
        const CmHirGenericParam *decoded_parameter = cm_hir_get_generic_param(
            &consumer, decoded_start);
        const CmInternedString *parameter_name = decoded_parameter == NULL
            ? NULL : cm_interner_get(&consumer.strings,
                decoded_parameter->name);

        {
            const CmHirType *decoded_declared = decoded_parameter == NULL
                ? NULL : cm_hir_get_type(&consumer,
                    decoded_parameter->declared_type);

            assert(decoded_parameter != NULL
                && decoded_parameter->kind == CM_HIR_GENERIC_CONST
                && decoded_parameter->index == 0u
                && !decoded_parameter->has_default
                && parameter_name != NULL
                && parameter_name->len == 1u
                && parameter_name->bytes[0] == (unsigned char)'N'
                && decoded_declared != NULL
                && decoded_declared->kind == CM_HIR_TYPE_INTEGER_KIND
                && decoded_declared->data.integer_type.kind
                    == CM_HIR_INT_USIZE);
        }
    }

    cm_byte_buf_destroy(&encoded);
    cm_hir_library_artifact_destroy(&consumer_artifact);
    cm_hir_context_destroy(&consumer);
    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&producer);
}

static void corrupt_first_const_parameter(CmByteBuf *encoded,
    int corrupt_kind)
{
    CmHirMetadataEnvelope envelope;
    CmHirMetadataReader sections;
    CmHirMetadataSection section;
    uint32_t section_index;
    size_t index;
    int changed;

    assert(cm_hir_metadata_decode_envelope_version(encoded->data,
        encoded->len, (uint16_t)CM_HIR_METADATA_DECLARATION_MAJOR,
        (uint16_t)CM_HIR_METADATA_DECLARATION_MINOR, &envelope)
        == CM_HIR_METADATA_OK);
    cm_hir_metadata_reader_init(&sections, envelope.payload,
        envelope.payload_length);
    memset(&section, 0, sizeof(section));
    for (section_index = 0u; section_index < 4u; ++section_index) {
        assert(cm_hir_metadata_read_section(&sections, &section)
            == CM_HIR_METADATA_OK);
    }
    assert(memcmp(section.tag, "TYPE", 4u) == 0);
    changed = 0;
    for (index = 0u; index + 10u <= section.length; ++index) {
        unsigned char *bytes;

        bytes = (unsigned char *)section.data;
        if (bytes[index] != UINT8_C(3)
            || bytes[index + 1u] != UINT8_C(2)) continue;
        if (corrupt_kind) {
            bytes[index + 1u] = UINT8_C(0);
        } else {
            bytes[index + 6u] = UINT8_C(0);
            bytes[index + 7u] = UINT8_C(0);
            bytes[index + 8u] = UINT8_C(0);
            bytes[index + 9u] = UINT8_C(0);
        }
        changed = 1;
        break;
    }
    assert(changed);
    recompute_metadata_crc(encoded);
}

static void test_declaration_v2_const_terms_round_trip(void)
{
    static const unsigned char source[] =
        "pub struct Buffer<const N: usize> { pub bytes: [u8; N] }\n"
        "pub struct Wrap<const N: usize>(pub Buffer<N>);\n"
        "pub type Four = Buffer<4>;\n";
    ParsedProducerFixture producer;
    CmByteBuf encoded;
    CmByteBuf reencoded;
    CmByteBuf corrupted;
    CmHirMetadataArtifactResult result;
    CmHirContext consumer;
    CmHirLibraryArtifact artifact;
    CmHirLibraryBinding buffer_binding;
    CmHirLibraryBinding wrap_binding;
    CmHirLibraryBinding four_binding;
    const CmHirItem *buffer;
    const CmHirItem *wrap;
    const CmHirItem *four;
    const CmHirType *type;
    const CmHirGenericParam *parameter;
    CmHirCrateId sentinel_crate;
    CmHirModuleId sentinel_module;
    ContextLengths before;
    CmHirLibraryArtifactIdentity sentinel_identity;

    assert(parsed_producer_build(&producer, source, sizeof(source) - 1u,
        1u, 3u, 0u, 0));
    cm_byte_buf_init(&encoded);
    result = cm_hir_metadata_encode_declaration_artifact(&encoded,
        &producer.artifact);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK
        && encoded.len != 0u);

    consumer_sentinel_init(&consumer, &artifact, &sentinel_crate,
        &sentinel_module);
    before = context_lengths(&consumer);
    assert(cm_hir_library_artifact_identity(&artifact, &sentinel_identity));
    cm_byte_buf_init(&corrupted);
    cm_byte_buf_append(&corrupted, encoded.data, encoded.len);
    corrupt_first_const_parameter(&corrupted, 0);
    result = cm_hir_metadata_decode_declaration_artifact(&consumer,
        &artifact, corrupted.data, corrupted.len, "broken", 91u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT);
    assert_sentinel_preserved(&consumer, &artifact, before,
        &sentinel_identity);
    cm_byte_buf_destroy(&corrupted);

    cm_byte_buf_init(&corrupted);
    cm_byte_buf_append(&corrupted, encoded.data, encoded.len);
    corrupt_first_const_parameter(&corrupted, 1);
    result = cm_hir_metadata_decode_declaration_artifact(&consumer,
        &artifact, corrupted.data, corrupted.len, "broken", 91u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT);
    assert_sentinel_preserved(&consumer, &artifact, before,
        &sentinel_identity);
    cm_byte_buf_destroy(&corrupted);
    cm_byte_buf_init(&corrupted);
    cm_byte_buf_append(&corrupted, encoded.data, encoded.len);
    (void)append_orphan_late_bound_type(&corrupted);
    result = cm_hir_metadata_decode_declaration_artifact(&consumer,
        &artifact, corrupted.data, corrupted.len, "broken", 105u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT);
    assert_sentinel_preserved(&consumer, &artifact, before,
        &sentinel_identity);
    cm_byte_buf_destroy(&corrupted);
    cm_byte_buf_init(&corrupted);
    cm_byte_buf_append(&corrupted, encoded.data, encoded.len);
    replace_with_aggregate_oversize_nominals(&corrupted);
    result = cm_hir_metadata_decode_declaration_artifact(&consumer,
        &artifact, corrupted.data, corrupted.len, "broken", 105u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT);
    assert_sentinel_preserved(&consumer, &artifact, before,
        &sentinel_identity);
    cm_byte_buf_destroy(&corrupted);
    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&consumer);

    cm_hir_context_init(&consumer);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_metadata_decode_declaration_artifact(&consumer,
        &artifact, encoded.data, encoded.len, "dep", 92u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK);
    buffer_binding = lookup(&artifact, "dep", "Buffer", NULL);
    wrap_binding = lookup(&artifact, "dep", "Wrap", NULL);
    four_binding = lookup(&artifact, "dep", "Four", NULL);
    buffer = binding_item(&consumer, buffer_binding);
    wrap = binding_item(&consumer, wrap_binding);
    four = binding_item(&consumer, four_binding);
    assert(buffer != NULL && buffer->kind == CM_HIR_ITEM_STRUCT
        && buffer->generic_parameter_count == 1u
        && buffer->data.aggregate_item.field_count == 1u);
    parameter = cm_hir_get_generic_param(&consumer,
        buffer->generic_parameter_start);
    assert(parameter != NULL && parameter->kind == CM_HIR_GENERIC_CONST);
    type = cm_hir_get_type(&consumer,
        buffer->data.aggregate_item.fields[0].type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_ARRAY_KIND
        && type->data.array_type.length.kind == CM_HIR_CONST_PARAMETER
        && type->data.array_type.length.data.parameter
            == buffer->generic_parameter_start);
    assert(wrap != NULL && wrap->kind == CM_HIR_ITEM_STRUCT
        && wrap->generic_parameter_count == 1u
        && wrap->data.aggregate_item.field_count == 1u);
    type = cm_hir_get_type(&consumer,
        wrap->data.aggregate_item.fields[0].type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(type->data.named_type.definition,
            buffer->definition)
        && type->data.named_type.argument_count == 1u
        && type->data.named_type.arguments[0].kind
            == CM_HIR_GENERIC_ARG_CONST
        && type->data.named_type.arguments[0].data.constant.kind
            == CM_HIR_CONST_PARAMETER
        && type->data.named_type.arguments[0].data.constant.data.parameter
            == wrap->generic_parameter_start);
    assert(four != NULL && four->kind == CM_HIR_ITEM_TYPE_ALIAS);
    type = cm_hir_get_type(&consumer, four->data.type_alias_item.target);
    assert(type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(type->data.named_type.definition,
            buffer->definition)
        && type->data.named_type.argument_count == 1u
        && type->data.named_type.arguments[0].kind
            == CM_HIR_GENERIC_ARG_CONST
        && type->data.named_type.arguments[0].data.constant.kind
            == CM_HIR_CONST_VALUE
        && type->data.named_type.arguments[0].data.constant.data.value.low_bits
            == UINT64_C(4)
        && type->data.named_type.arguments[0].data.constant.data.value.high_bits
            == UINT64_C(0));

    cm_byte_buf_init(&reencoded);
    result = cm_hir_metadata_encode_declaration_artifact(&reencoded,
        &artifact);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK
        && reencoded.len == encoded.len
        && memcmp(reencoded.data, encoded.data, encoded.len) == 0);

    cm_byte_buf_destroy(&reencoded);
    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&consumer);
    cm_byte_buf_destroy(&encoded);
    parsed_producer_destroy(&producer);
    (void)sentinel_crate;
    (void)sentinel_module;
}

static void corrupt_first_generic_owner_kind(CmByteBuf *encoded)
{
    CmHirMetadataEnvelope envelope;
    CmHirMetadataReader sections;
    CmHirMetadataSection section;
    uint32_t index;
    unsigned char *bytes;

    assert(cm_hir_metadata_decode_envelope_version(encoded->data,
        encoded->len, (uint16_t)CM_HIR_METADATA_DECLARATION_MAJOR,
        (uint16_t)CM_HIR_METADATA_DECLARATION_MINOR, &envelope)
        == CM_HIR_METADATA_OK);
    cm_hir_metadata_reader_init(&sections, envelope.payload,
        envelope.payload_length);
    memset(&section, 0, sizeof(section));
    for (index = 0u; index < 3u; ++index) {
        assert(cm_hir_metadata_read_section(&sections, &section)
            == CM_HIR_METADATA_OK);
    }
    assert(memcmp(section.tag, "GPAR", 4u) == 0 && section.length > 4u);
    bytes = (unsigned char *)section.data;
    bytes[4] = UINT8_C(0);
    recompute_metadata_crc(encoded);
}

static void test_declaration_v2_generic_function_round_trip(void)
{
    static const unsigned char rejected_sentinel[] = {
        UINT8_C(0xa5), UINT8_C(0x5a), UINT8_C(0xc3)
    };
    ParsedProducerFixture producer;
    ParsedProducerFixture rejected;
    CmByteBuf encoded;
    CmByteBuf corrupted;
    CmByteBuf reencoded;
    CmByteBuf legacy;
    CmHirMetadataArtifactResult result;
    CmHirContext consumer;
    CmHirLibraryArtifact artifact;
    CmHirCrateId sentinel_crate;
    CmHirModuleId sentinel_module;
    ContextLengths before;
    CmHirLibraryArtifactIdentity sentinel_identity;
    CmHirLibraryValue rejected_value;
    CmHirLibraryValue scoped_value;
    CmHirLibraryValue alias_value;
    const CmHirType *predicate_subject;
    const CmHirGenericParam *predicate_parameter;
    const CmHirType *callable_argument;
    const CmHirType *callable_input;
    const CmHirType *callable_output;
    const CmHirDefinition *source_definition;
    const CmHirItem *source_item;
    unsigned char *rejected_buffer_data;
    CmHirLibraryOwnedData predicate_owned;
    CmHirLibraryOwnedValue *owned_constrained;
    CmHirLibraryOwnedValue *owned_scoped;
    CmHirLibraryOwnedValue *owned_alias;
    CmHirLibraryArtifact predicate_artifact;
    CmHirLibraryArtifactResult library_result;
    CmHirLibraryArtifactIdentity predicate_identity;
    CmHirLibraryArtifactIdentity predicate_identity_after;
    size_t predicate_root_index;
    CmHirGenericArg *owned_arguments;
    uint32_t owned_scope_span_end;
    const CmHirLibraryNominalReference *fn_reference;
    const CmHirLibraryNominalReference *fn_mut_reference;
    const CmHirLibraryNominalReference *fn_once_reference;
    const CmHirLibraryNominalReference *copy_reference;
    const CmHirLibraryNominalReference *output_reference;
    const CmHirLibraryNominalReference *thin_reference;
    const CmHirLibraryNominalReference *pointee_reference;
    const CmHirLibraryNominalReference *pointee_sized_reference;
    const CmHirLibraryNominalReference *metadata_reference;
    const CmHirLibraryNominalReference *owned_fn_reference;
    CmHirLibraryNominalReference *owned_fn_mutable;
    CmHirLibraryNominalReference *owned_copy_mutable;
    CmHirLibraryNominalReference *owned_output_mutable;
    CmHirLibraryNominalReference *owned_thin_mutable;
    CmHirLibraryNominalReferenceKind owned_nominal_kind;
    CmHirDefId owned_nominal_definition;
    CmHirDefId owned_declaring_trait;
    CmHirDefId owned_available_trait;
    CmHirGenericParamKind owned_schema_kind;
    uint32_t nominal_index;
    CmHirDefId owned_owner_module;
    CmHirLibraryPathSegment owned_reference_name;
    CmInternId owned_reference_name_id;
    const CmInternedString *source_reference_name;
    CmHirLibraryPathSegment hidden_path[2];
    CmHirLibraryBinding hidden_binding;

    assert(parsed_producer_build(&producer, generic_function_source,
        sizeof(generic_function_source) - 1u, 1u, 0u, 1u, 1));
    assert(generic_function_valid(&producer.context, &producer.artifact,
        "producer"));
    cm_byte_buf_init(&encoded);
    result = cm_hir_metadata_encode_declaration_artifact(&encoded,
        &producer.artifact);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK
        && result.public_entry_count == 1u && encoded.len != 0u);

    derive_legacy_declaration_v23(&encoded, &legacy);
    cm_hir_context_init(&consumer);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_metadata_decode_declaration_artifact(&consumer,
        &artifact, legacy.data, legacy.len, "legacy", 103u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK
        && generic_function_valid(&consumer, &artifact, "legacy"));
    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&consumer);
    cm_byte_buf_destroy(&legacy);

    consumer_sentinel_init(&consumer, &artifact, &sentinel_crate,
        &sentinel_module);
    before = context_lengths(&consumer);
    assert(cm_hir_library_artifact_identity(&artifact,
        &sentinel_identity));
    cm_byte_buf_init(&corrupted);
    cm_byte_buf_append(&corrupted, encoded.data, encoded.len);
    corrupt_first_generic_owner_kind(&corrupted);
    result = cm_hir_metadata_decode_declaration_artifact(&consumer,
        &artifact, corrupted.data, corrupted.len, "broken", 103u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT);
    assert_sentinel_preserved(&consumer, &artifact, before,
        &sentinel_identity);
    cm_byte_buf_destroy(&corrupted);
    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&consumer);

    cm_hir_context_init(&consumer);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_metadata_decode_declaration_artifact(&consumer,
        &artifact, encoded.data, encoded.len, "dep", 104u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK
        && generic_function_valid(&consumer, &artifact, "dep"));
    cm_byte_buf_init(&reencoded);
    result = cm_hir_metadata_encode_declaration_artifact(&reencoded,
        &artifact);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK
        && reencoded.len == encoded.len
        && memcmp(reencoded.data, encoded.data, encoded.len) == 0);

    assert(parsed_producer_build(&rejected, predicate_function_source,
        sizeof(predicate_function_source) - 1u, 1u, 0u, 3u, 1));
    rejected_value = lookup_value(&rejected.artifact, "producer",
        "constrained");
    assert(rejected_value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        && rejected_value.data.function.generic_parameter_count == 2u
        && rejected_value.data.function.predicate_scope_count == 0u
        && rejected_value.data.function.predicate_scopes == NULL
        && rejected_value.data.function.predicate_count == 2u
        && rejected_value.data.function.predicates != NULL
        && rejected_value.data.function.outlives_predicate_count == 1u
        && rejected_value.data.function.outlives_predicates != NULL);
    fn_reference = find_nominal_reference(&rejected_value, "Fn",
        CM_HIR_LIBRARY_NOMINAL_TRAIT);
    fn_mut_reference = find_nominal_reference(&rejected_value, "FnMut",
        CM_HIR_LIBRARY_NOMINAL_TRAIT);
    fn_once_reference = find_nominal_reference(&rejected_value, "FnOnce",
        CM_HIR_LIBRARY_NOMINAL_TRAIT);
    copy_reference = find_nominal_reference(&rejected_value, "Copy",
        CM_HIR_LIBRARY_NOMINAL_TRAIT);
    output_reference = find_nominal_reference(&rejected_value, "Output",
        CM_HIR_LIBRARY_NOMINAL_ASSOCIATED_TYPE);
    assert(cm_hir_library_artifact_identity(&rejected.artifact,
        &predicate_identity));
    assert(fn_reference != NULL);
    source_definition = cm_hir_lookup_definition(&rejected.context,
        fn_reference->definition);
    source_item = source_definition == NULL ? NULL : cm_hir_get_item(
        &rejected.context, source_definition->entity.item_id);
    source_reference_name = source_item == NULL ? NULL : cm_interner_get(
        &rejected.context.strings, source_item->name);
    assert(rejected_value.data.function.nominal_reference_count == 5u
        && fn_reference != NULL && fn_mut_reference != NULL
        && fn_once_reference != NULL && copy_reference != NULL
        && output_reference != NULL
        && fn_reference->use == CM_HIR_LIBRARY_REFERENCE_ONLY
        && fn_reference->generic_parameter_count == 1u
        && fn_reference->generic_parameter_kinds[0] == CM_HIR_GENERIC_TYPE
        && fn_mut_reference->generic_parameter_count == 1u
        && fn_mut_reference->generic_parameter_kinds[0]
            == CM_HIR_GENERIC_TYPE
        && fn_once_reference->generic_parameter_count == 1u
        && fn_once_reference->generic_parameter_kinds[0]
            == CM_HIR_GENERIC_TYPE
        && copy_reference->generic_parameter_count == 0u
        && copy_reference->generic_parameter_kinds == NULL
        && output_reference->generic_parameter_count == 0u
        && output_reference->generic_parameter_kinds == NULL
        && cm_hir_def_id_equal(fn_reference->owner_module,
            predicate_identity.root_definition)
        && cm_hir_def_id_equal(output_reference->owner_module,
            predicate_identity.root_definition)
        && source_reference_name != NULL
        && fn_reference->name.length == 2u
        && memcmp(fn_reference->name.bytes, "Fn", 2u) == 0
        && fn_reference->name.bytes != source_reference_name->bytes
        && cm_hir_def_id_equal(output_reference->declaring_trait,
            fn_once_reference->definition)
        && rejected_value.data.function.associated_availability_count == 1u
        && cm_hir_def_id_equal(rejected_value.data.function
                .associated_availability[0].direct_trait,
            fn_reference->definition)
        && cm_hir_def_id_equal(rejected_value.data.function
                .associated_availability[0].associated_type,
            output_reference->definition));
    for (nominal_index = 1u;
            nominal_index < rejected_value.data.function
                .nominal_reference_count;
            ++nominal_index) {
        const CmHirDefId prior = rejected_value.data.function
            .nominal_references[nominal_index - 1u].definition;
        const CmHirDefId current = rejected_value.data.function
            .nominal_references[nominal_index].definition;

        assert(prior.crate_id < current.crate_id
            || (prior.crate_id == current.crate_id
                && prior.index < current.index));
    }
    hidden_path[0].bytes = (const unsigned char *)"producer";
    hidden_path[0].length = 8u;
    hidden_path[1].bytes = (const unsigned char *)"Fn";
    hidden_path[1].length = 2u;
    memset(&hidden_binding, 0, sizeof(hidden_binding));
    assert(cm_hir_library_artifact_lookup_binding(&rejected.artifact,
        hidden_path, 2u, &hidden_binding) == CM_HIR_LIBRARY_NOT_FOUND);
    source_definition = cm_hir_lookup_definition(&rejected.context,
        output_reference->definition);
    assert(source_definition != NULL
        && source_definition->state == CM_HIR_DEFINITION_BOUND
        && source_definition->reserved_item_kind == CM_HIR_ITEM_TYPE_ALIAS);
    predicate_parameter = cm_hir_get_generic_param(&rejected.context,
        rejected_value.data.function.generic_parameter_start + 1u);
    predicate_subject = cm_hir_get_type(&rejected.context,
        rejected_value.data.function.outlives_predicates[0].subject.type);
    assert(predicate_parameter != NULL
        && predicate_parameter->kind == CM_HIR_GENERIC_TYPE
        && cm_hir_def_id_equal(predicate_parameter->owner,
            rejected_value.definition)
        && predicate_subject != NULL
        && predicate_subject->kind == CM_HIR_TYPE_PARAMETER_KIND
        && predicate_subject->data.parameter_type.parameter
            == rejected_value.data.function.generic_parameter_start + 1u
        && rejected_value.data.function.outlives_predicates[0].subject_kind
            == CM_HIR_OUTLIVES_TYPE
        && rejected_value.data.function.outlives_predicates[0].bound.kind
            == CM_HIR_REGION_STATIC
        && rejected_value.data.function.outlives_predicates[0].scope
            == CM_HIR_PREDICATE_SCOPE_NONE);
    assert(rejected_value.data.function.predicates[0].scope
            == CM_HIR_PREDICATE_SCOPE_NONE
        && rejected_value.data.function.predicates[0].subject
            == rejected_value.data.function.outlives_predicates[0]
                .subject.type
        && rejected_value.data.function.predicates[0].binder.lifetime_count
            == 1u
        && rejected_value.data.function.predicates[0].binder.lifetimes != NULL
        && rejected_value.data.function.predicates[0].trait_type.argument_count
            == 1u
        && rejected_value.data.function.predicates[0].trait_type.arguments
            != NULL
        && rejected_value.data.function.predicates[0].trait_type.arguments[0]
            .kind == CM_HIR_GENERIC_ARG_TYPE
        && rejected_value.data.function.predicates[0].equality_count == 1u
        && rejected_value.data.function.predicates[0].equalities != NULL
        && rejected_value.data.function.predicates[0].modifier
            == CM_HIR_PREDICATE_REQUIRED
        && rejected_value.data.function.predicates[1].scope
            == CM_HIR_PREDICATE_SCOPE_NONE
        && rejected_value.data.function.predicates[1].binder.lifetime_count
            == 0u
        && rejected_value.data.function.predicates[1].binder.lifetimes == NULL
        && rejected_value.data.function.predicates[1].trait_type.argument_count
            == 0u
        && rejected_value.data.function.predicates[1].trait_type.arguments
            == NULL
        && rejected_value.data.function.predicates[1].equality_count == 0u
        && rejected_value.data.function.predicates[1].equalities == NULL);
    callable_argument = cm_hir_get_type(&rejected.context,
        rejected_value.data.function.predicates[0].trait_type.arguments[0]
            .data.type);
    callable_input = callable_argument == NULL
        || callable_argument->kind != CM_HIR_TYPE_TUPLE_KIND
        || callable_argument->data.tuple_type.element_count != 1u
        ? NULL : cm_hir_get_type(&rejected.context,
            callable_argument->data.tuple_type.elements[0]);
    callable_output = cm_hir_get_type(&rejected.context,
        rejected_value.data.function.predicates[0].equalities[0].value);
    assert(callable_argument != NULL
        && callable_argument->kind == CM_HIR_TYPE_TUPLE_KIND
        && callable_input != NULL
        && callable_input->kind == CM_HIR_TYPE_REFERENCE_KIND
        && callable_input->data.reference_type.region.kind
            == CM_HIR_REGION_LATE_BOUND
        && callable_input->data.reference_type.region.data.binder_index == 0u
        && callable_output != NULL
        && callable_output->kind == CM_HIR_TYPE_BOOL_KIND);
    source_definition = cm_hir_lookup_definition(&rejected.context,
        rejected_value.definition);
    source_item = source_definition == NULL ? NULL : cm_hir_get_item(
        &rejected.context, source_definition->entity.item_id);
    assert(source_item != NULL
        && rejected_value.data.function.predicates != source_item->predicates
        && rejected_value.data.function.outlives_predicates
            != source_item->outlives_predicates
        && rejected_value.data.function.predicates[0].binder.lifetimes
            != source_item->predicates[0].binder.lifetimes
        && rejected_value.data.function.predicates[0].trait_type.arguments
            != source_item->predicates[0].trait_type.arguments
        && rejected_value.data.function.predicates[0].equalities
            != source_item->predicates[0].equalities);
    scoped_value = lookup_value(&rejected.artifact, "producer", "scoped");
    assert(scoped_value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        && scoped_value.data.function.predicate_scope_count == 1u
        && scoped_value.data.function.predicate_scopes != NULL
        && scoped_value.data.function.predicate_scopes[0].binder.lifetime_count
            == 1u
        && scoped_value.data.function.predicate_scopes[0].binder.lifetimes
            != NULL
        && scoped_value.data.function.predicate_scopes[0]
            .trait_predicate_count == 1u
        && scoped_value.data.function.predicate_scopes[0]
            .outlives_predicate_count == 0u
        && scoped_value.data.function.predicate_count == 1u
        && scoped_value.data.function.predicates[0].scope == 1u
        && scoped_value.data.function.predicates[0].binder.lifetime_count == 0u
        && scoped_value.data.function.outlives_predicate_count == 0u
        && scoped_value.data.function.nominal_reference_count == 4u
        && scoped_value.data.function.associated_availability_count == 1u);
    source_definition = cm_hir_lookup_definition(&rejected.context,
        scoped_value.definition);
    source_item = source_definition == NULL ? NULL : cm_hir_get_item(
        &rejected.context, source_definition->entity.item_id);
    assert(source_item != NULL
        && scoped_value.data.function.predicate_scopes
            != source_item->predicate_scopes
        && scoped_value.data.function.predicate_scopes[0].binder.lifetimes
            != source_item->predicate_scopes[0].binder.lifetimes);
    alias_value = lookup_value(&rejected.artifact, "producer", "null");
    thin_reference = find_nominal_reference(&alias_value, "Thin",
        CM_HIR_LIBRARY_NOMINAL_TRAIT_ALIAS);
    pointee_reference = find_nominal_reference(&alias_value, "Pointee",
        CM_HIR_LIBRARY_NOMINAL_TRAIT);
    pointee_sized_reference = find_nominal_reference(&alias_value,
        "PointeeSized", CM_HIR_LIBRARY_NOMINAL_TRAIT);
    metadata_reference = find_nominal_reference(&alias_value, "Metadata",
        CM_HIR_LIBRARY_NOMINAL_ASSOCIATED_TYPE);
    assert(alias_value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        && alias_value.data.function.is_const == 1
        && alias_value.data.function.parameter_count == 0u
        && alias_value.data.function.generic_parameter_count == 1u
        && alias_value.data.function.predicate_count == 2u
        && alias_value.data.function.outlives_predicate_count == 0u
        && alias_value.data.function.nominal_reference_count == 3u
        && alias_value.data.function.associated_availability_count == 0u
        && thin_reference != NULL && pointee_reference != NULL
        && pointee_sized_reference != NULL && metadata_reference == NULL
        && thin_reference->generic_parameter_count == 0u
        && thin_reference->generic_parameter_kinds == NULL
        && pointee_reference->generic_parameter_count == 0u
        && pointee_sized_reference->generic_parameter_count == 0u
        && cm_hir_def_id_equal(thin_reference->owner_module,
            predicate_identity.root_definition)
        && ((cm_hir_def_id_equal(alias_value.data.function.predicates[0]
                    .trait_type.definition, thin_reference->definition)
                && cm_hir_def_id_equal(alias_value.data.function.predicates[1]
                    .trait_type.definition,
                    pointee_sized_reference->definition))
            || (cm_hir_def_id_equal(alias_value.data.function.predicates[1]
                    .trait_type.definition, thin_reference->definition)
                && cm_hir_def_id_equal(alias_value.data.function.predicates[0]
                    .trait_type.definition,
                    pointee_sized_reference->definition))));
    source_definition = cm_hir_lookup_definition(&rejected.context,
        thin_reference->definition);
    source_item = source_definition == NULL ? NULL : cm_hir_get_item(
        &rejected.context, source_definition->entity.item_id);
    assert(source_definition != NULL
        && source_definition->state == CM_HIR_DEFINITION_BOUND
        && source_definition->reserved_item_kind == CM_HIR_ITEM_TRAIT_ALIAS
        && source_item != NULL && source_item->kind == CM_HIR_ITEM_TRAIT_ALIAS
        && source_item->data.trait_alias_item.bound_count == 2u);
    hidden_path[0].bytes = (const unsigned char *)"producer";
    hidden_path[0].length = 8u;
    hidden_path[1].bytes = (const unsigned char *)"Thin";
    hidden_path[1].length = 4u;
    memset(&hidden_binding, 0, sizeof(hidden_binding));
    assert(cm_hir_library_artifact_lookup_binding(&rejected.artifact,
        hidden_path, 2u, &hidden_binding) == CM_HIR_LIBRARY_NOT_FOUND);
    cm_hir_library_owned_data_init(&predicate_owned);
    assert(cm_interner_length(&predicate_owned.names) == 0u);
    assert(cm_hir_library_owned_data_add_module(&predicate_owned,
        predicate_identity.root_definition, &predicate_root_index)
        == CM_HIR_LIBRARY_OK);
    assert(cm_hir_library_owned_data_add_value(&predicate_owned,
        &rejected_value) == CM_HIR_LIBRARY_OK
        && cm_interner_length(&predicate_owned.names) == 5u);
    assert(cm_hir_library_owned_data_add_value(&predicate_owned,
        &scoped_value) == CM_HIR_LIBRARY_OK);
    assert(cm_hir_library_owned_data_add_value(&predicate_owned,
        &alias_value) == CM_HIR_LIBRARY_OK
        && cm_interner_length(&predicate_owned.names) == 8u);
    add_entry(&predicate_owned, predicate_root_index, "constrained",
        value_binding(rejected_value.definition,
            CM_HIR_LIBRARY_VALUE_FUNCTION));
    add_entry(&predicate_owned, predicate_root_index, "scoped",
        value_binding(scoped_value.definition, CM_HIR_LIBRARY_VALUE_FUNCTION));
    add_entry(&predicate_owned, predicate_root_index, "null",
        value_binding(alias_value.definition, CM_HIR_LIBRARY_VALUE_FUNCTION));
    cm_hir_library_artifact_init(&predicate_artifact);
    library_result = cm_hir_library_artifact_restore_owned(
        &predicate_artifact, &rejected.context, predicate_identity.crate_id,
        predicate_identity.root_definition, "bounded", &predicate_owned);
    assert(library_result.status == CM_HIR_LIBRARY_OK
        && library_result.public_value_entry_count == 3u
        && predicate_owned.values.len == 0u);
    cm_hir_library_owned_data_destroy(&predicate_owned);

    cm_hir_library_owned_data_init(&predicate_owned);
    assert(cm_hir_library_owned_data_add_module(&predicate_owned,
        predicate_identity.root_definition, &predicate_root_index)
        == CM_HIR_LIBRARY_OK);
    assert(cm_hir_library_owned_data_add_value(&predicate_owned,
        &rejected_value) == CM_HIR_LIBRARY_OK);
    assert(cm_hir_library_owned_data_add_value(&predicate_owned,
        &scoped_value) == CM_HIR_LIBRARY_OK);
    assert(cm_hir_library_owned_data_add_value(&predicate_owned,
        &alias_value) == CM_HIR_LIBRARY_OK);
    add_entry(&predicate_owned, predicate_root_index, "constrained",
        value_binding(rejected_value.definition,
            CM_HIR_LIBRARY_VALUE_FUNCTION));
    add_entry(&predicate_owned, predicate_root_index, "scoped",
        value_binding(scoped_value.definition, CM_HIR_LIBRARY_VALUE_FUNCTION));
    add_entry(&predicate_owned, predicate_root_index, "null",
        value_binding(alias_value.definition, CM_HIR_LIBRARY_VALUE_FUNCTION));
    owned_constrained = (CmHirLibraryOwnedValue *)cm_vec_at(
        &predicate_owned.values, 0u);
    owned_scoped = (CmHirLibraryOwnedValue *)cm_vec_at(
        &predicate_owned.values, 1u);
    owned_alias = (CmHirLibraryOwnedValue *)cm_vec_at(
        &predicate_owned.values, 2u);
    assert(owned_constrained != NULL && owned_scoped != NULL
        && owned_alias != NULL
        && cm_hir_library_artifact_identity(&predicate_artifact,
            &predicate_identity_after));
    owned_thin_mutable = NULL;
    for (nominal_index = 0u;
            nominal_index < owned_alias->nominal_reference_count;
            ++nominal_index) {
        if (cm_hir_def_id_equal(
                owned_alias->nominal_references[nominal_index].definition,
                thin_reference->definition)) {
            owned_thin_mutable =
                &owned_alias->nominal_references[nominal_index];
        }
    }
    assert(owned_alias->nominal_references
            != alias_value.data.function.nominal_references
        && owned_alias->associated_availability == NULL
        && owned_thin_mutable != NULL
        && owned_thin_mutable->name.bytes != thin_reference->name.bytes
        && owned_thin_mutable->name.length == thin_reference->name.length
        && memcmp(owned_thin_mutable->name.bytes, thin_reference->name.bytes,
            thin_reference->name.length) == 0);
    owned_fn_reference = find_nominal_reference(
        &owned_constrained->declaration, "Fn",
        CM_HIR_LIBRARY_NOMINAL_TRAIT);
    owned_fn_mutable = NULL;
    owned_copy_mutable = NULL;
    owned_output_mutable = NULL;
    for (nominal_index = 0u;
            nominal_index < owned_constrained->nominal_reference_count;
            ++nominal_index) {
        if (cm_hir_def_id_equal(
                owned_constrained->nominal_references[nominal_index]
                    .definition,
                fn_reference->definition)) {
            owned_fn_mutable =
                &owned_constrained->nominal_references[nominal_index];
        }
        if (cm_hir_def_id_equal(
                owned_constrained->nominal_references[nominal_index]
                    .definition,
                copy_reference->definition)) {
            owned_copy_mutable =
                &owned_constrained->nominal_references[nominal_index];
        }
        if (cm_hir_def_id_equal(
                owned_constrained->nominal_references[nominal_index]
                    .definition,
                output_reference->definition)) {
            owned_output_mutable =
                &owned_constrained->nominal_references[nominal_index];
        }
    }
    assert(owned_constrained->nominal_references
            != rejected_value.data.function.nominal_references
        && owned_constrained->associated_availability
            != rejected_value.data.function.associated_availability
        && owned_fn_reference != NULL
        && owned_fn_mutable != NULL && owned_copy_mutable != NULL
        && owned_output_mutable != NULL
        && owned_fn_reference->name.bytes != fn_reference->name.bytes
        && owned_fn_reference->name.length == fn_reference->name.length
        && memcmp(owned_fn_reference->name.bytes, fn_reference->name.bytes,
            fn_reference->name.length) == 0
        && owned_fn_reference->generic_parameter_kinds
            != fn_reference->generic_parameter_kinds);
    owned_arguments = owned_constrained->predicates[0].trait_type.arguments;
    owned_constrained->predicates[0].trait_type.arguments =
        rejected_value.data.function.predicates[0].trait_type.arguments;
    library_result = cm_hir_library_artifact_restore_owned(
        &predicate_artifact, &rejected.context, predicate_identity.crate_id,
        predicate_identity.root_definition, "broken", &predicate_owned);
    assert(library_result.status == CM_HIR_LIBRARY_INVALID_HIR
        && predicate_owned.values.len == 3u);
    owned_constrained->predicates[0].trait_type.arguments = owned_arguments;
    owned_constrained->predicates[0].modifier = CM_HIR_PREDICATE_CONST;
    library_result = cm_hir_library_artifact_restore_owned(
        &predicate_artifact, &rejected.context, predicate_identity.crate_id,
        predicate_identity.root_definition, "broken", &predicate_owned);
    assert(library_result.status == CM_HIR_LIBRARY_INVALID_HIR
        && predicate_owned.values.len == 3u);
    owned_constrained->predicates[0].modifier = CM_HIR_PREDICATE_REQUIRED;
    owned_constrained->outlives_predicates[0].bound.kind =
        CM_HIR_REGION_ERASED;
    library_result = cm_hir_library_artifact_restore_owned(
        &predicate_artifact, &rejected.context, predicate_identity.crate_id,
        predicate_identity.root_definition, "broken", &predicate_owned);
    assert(library_result.status == CM_HIR_LIBRARY_INVALID_HIR
        && predicate_owned.values.len == 3u);
    owned_constrained->outlives_predicates[0].bound.kind =
        CM_HIR_REGION_STATIC;
    owned_scope_span_end = owned_scoped->predicate_scopes[0].span.end;
    owned_scoped->predicate_scopes[0].span.end = owned_scope_span_end - 1u;
    library_result = cm_hir_library_artifact_restore_owned(
        &predicate_artifact, &rejected.context, predicate_identity.crate_id,
        predicate_identity.root_definition, "broken", &predicate_owned);
    assert(library_result.status == CM_HIR_LIBRARY_INVALID_HIR
        && predicate_owned.values.len == 3u);
    owned_scoped->predicate_scopes[0].span.end = owned_scope_span_end;
    owned_schema_kind = owned_fn_mutable->generic_parameter_kinds[0];
    owned_constrained->nominal_reference_generic_kinds[
        (uint32_t)(owned_fn_mutable - owned_constrained->nominal_references)]
            [0] = CM_HIR_GENERIC_CONST;
    library_result = cm_hir_library_artifact_restore_owned(
        &predicate_artifact, &rejected.context, predicate_identity.crate_id,
        predicate_identity.root_definition, "broken", &predicate_owned);
    assert(library_result.status == CM_HIR_LIBRARY_INVALID_HIR
        && predicate_owned.values.len == 3u);
    owned_constrained->nominal_reference_generic_kinds[
        (uint32_t)(owned_fn_mutable - owned_constrained->nominal_references)]
            [0] = owned_schema_kind;
    owned_owner_module = owned_fn_mutable->owner_module;
    owned_fn_mutable->owner_module = rejected_value.definition;
    library_result = cm_hir_library_artifact_restore_owned(
        &predicate_artifact, &rejected.context, predicate_identity.crate_id,
        predicate_identity.root_definition, "broken", &predicate_owned);
    assert(library_result.status == CM_HIR_LIBRARY_INVALID_HIR
        && predicate_owned.values.len == 3u);
    owned_fn_mutable->owner_module = owned_owner_module;
    owned_reference_name = owned_fn_mutable->name;
    owned_reference_name_id = owned_constrained->nominal_reference_names[
        (uint32_t)(owned_fn_mutable - owned_constrained->nominal_references)];
    owned_fn_mutable->name = owned_copy_mutable->name;
    owned_constrained->nominal_reference_names[
        (uint32_t)(owned_fn_mutable - owned_constrained->nominal_references)] =
            owned_constrained->nominal_reference_names[(uint32_t)(
                owned_copy_mutable - owned_constrained->nominal_references)];
    library_result = cm_hir_library_artifact_restore_owned(
        &predicate_artifact, &rejected.context, predicate_identity.crate_id,
        predicate_identity.root_definition, "broken", &predicate_owned);
    assert(library_result.status == CM_HIR_LIBRARY_INVALID_HIR
        && predicate_owned.values.len == 3u);
    owned_fn_mutable->name = owned_reference_name;
    owned_constrained->nominal_reference_names[
        (uint32_t)(owned_fn_mutable - owned_constrained->nominal_references)] =
            owned_reference_name_id;
    owned_declaring_trait = owned_output_mutable->declaring_trait;
    owned_output_mutable->declaring_trait = copy_reference->definition;
    library_result = cm_hir_library_artifact_restore_owned(
        &predicate_artifact, &rejected.context, predicate_identity.crate_id,
        predicate_identity.root_definition, "broken", &predicate_owned);
    assert(library_result.status == CM_HIR_LIBRARY_INVALID_HIR
        && predicate_owned.values.len == 3u);
    owned_output_mutable->declaring_trait = owned_declaring_trait;
    owned_available_trait = owned_constrained->associated_availability[0]
        .direct_trait;
    owned_constrained->associated_availability[0].direct_trait =
        copy_reference->definition;
    library_result = cm_hir_library_artifact_restore_owned(
        &predicate_artifact, &rejected.context, predicate_identity.crate_id,
        predicate_identity.root_definition, "broken", &predicate_owned);
    assert(library_result.status == CM_HIR_LIBRARY_INVALID_HIR
        && predicate_owned.values.len == 3u);
    owned_constrained->associated_availability[0].direct_trait =
        owned_available_trait;
    owned_nominal_kind = owned_thin_mutable->kind;
    owned_thin_mutable->kind = CM_HIR_LIBRARY_NOMINAL_TRAIT;
    library_result = cm_hir_library_artifact_restore_owned(
        &predicate_artifact, &rejected.context, predicate_identity.crate_id,
        predicate_identity.root_definition, "broken", &predicate_owned);
    assert(library_result.status == CM_HIR_LIBRARY_INVALID_HIR
        && predicate_owned.values.len == 3u);
    owned_thin_mutable->kind = owned_nominal_kind;
    owned_nominal_definition = owned_thin_mutable->definition;
    owned_thin_mutable->definition = pointee_reference->definition;
    library_result = cm_hir_library_artifact_restore_owned(
        &predicate_artifact, &rejected.context, predicate_identity.crate_id,
        predicate_identity.root_definition, "broken", &predicate_owned);
    assert(library_result.status == CM_HIR_LIBRARY_INVALID_HIR
        && predicate_owned.values.len == 3u);
    owned_thin_mutable->definition = owned_nominal_definition;
    assert(cm_hir_library_artifact_identity(&predicate_artifact,
        &predicate_identity)
        && predicate_identity.context == predicate_identity_after.context
        && predicate_identity.crate_id == predicate_identity_after.crate_id
        && cm_hir_def_id_equal(predicate_identity.root_definition,
            predicate_identity_after.root_definition)
        && strcmp(predicate_identity.extern_name, "bounded") == 0);
    cm_hir_library_owned_data_destroy(&predicate_owned);
    cm_hir_library_artifact_destroy(&predicate_artifact);
    cm_byte_buf_destroy(&reencoded);
    cm_byte_buf_init(&reencoded);
    cm_byte_buf_append(&reencoded, rejected_sentinel,
        sizeof(rejected_sentinel));
    rejected_buffer_data = reencoded.data;
    result = cm_hir_metadata_encode_declaration_artifact(&reencoded,
        &rejected.artifact);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_UNSUPPORTED_HIR
        && reencoded.data == rejected_buffer_data
        && reencoded.len == sizeof(rejected_sentinel)
        && memcmp(reencoded.data, rejected_sentinel,
            sizeof(rejected_sentinel)) == 0);
    parsed_producer_destroy(&rejected);

    cm_byte_buf_destroy(&reencoded);
    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&consumer);
    cm_byte_buf_destroy(&encoded);
    parsed_producer_destroy(&producer);
    (void)sentinel_crate;
    (void)sentinel_module;
}

static void test_declaration_v24_predicate_function_round_trip(void)
{
    static const unsigned char unsupported_sentinel[] = {
        UINT8_C(0x42), UINT8_C(0x24), UINT8_C(0xa5)
    };
    ParsedProducerFixture producer;
    ParsedProducerFixture unsupported;
    CmHirContext consumer;
    CmHirLibraryArtifact artifact;
    CmByteBuf encoded;
    CmByteBuf legacy_v25;
    CmByteBuf legacy_v24;
    CmByteBuf reencoded;
    CmByteBuf corrupted;
    CmHirMetadataArtifactResult result;
    CmHirLibraryValue value;
    const CmHirLibraryNominalReference *fn_reference;
    const CmHirLibraryNominalReference *fn_mut_reference;
    const CmHirLibraryNominalReference *fn_once_reference;
    const CmHirLibraryNominalReference *copy_reference;
    const CmHirLibraryNominalReference *output_reference;
    const CmHirTraitPredicate *fn_predicate;
    const CmHirTraitPredicate *copy_predicate;
    const CmHirDefinition *definition;
    const CmHirType *tuple;
    const CmHirType *input;
    const CmHirType *subject;
    const CmHirType *ret_type;
    const CmHirType *output_type;
    const CmHirGenericParam *ret_parameter;
    const CmHirGenericParam *c_parameter;
    const CmInternedString *binder_name;
    size_t item_index;
    CmHirLibraryPathSegment hidden[2];
    CmHirLibraryBinding binding;
    CmHirCrateId sentinel_crate;
    CmHirModuleId sentinel_module;
    ContextLengths before;
    CmHirLibraryArtifactIdentity sentinel_identity;
    unsigned char *sentinel_data;
    static const unsigned char nref_tag[4] = {
        (unsigned char)'N', (unsigned char)'R',
        (unsigned char)'E', (unsigned char)'F'
    };
    static const unsigned char pred_tag[4] = {
        (unsigned char)'P', (unsigned char)'R',
        (unsigned char)'E', (unsigned char)'D'
    };

    assert(parsed_producer_build(&producer, constrained_function_source,
        sizeof(constrained_function_source) - 1u, 1u, 0u, 1u, 1));
    cm_byte_buf_init(&encoded);
    result = cm_hir_metadata_encode_declaration_artifact(&encoded,
        &producer.artifact);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK
        && encoded.len != 0u);

    derive_legacy_declaration_v25(&encoded, &legacy_v25);
    cm_hir_context_init(&consumer);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_metadata_decode_declaration_artifact(&consumer,
        &artifact, legacy_v25.data, legacy_v25.len, "legacy5", 105u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK);
    value = lookup_value(&artifact, "legacy5", "constrained");
    assert(value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        && value.data.function.predicate_count == 2u);
    cm_byte_buf_init(&reencoded);
    result = cm_hir_metadata_encode_declaration_artifact(&reencoded,
        &artifact);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK
        && reencoded.len == encoded.len
        && memcmp(reencoded.data, encoded.data, encoded.len) == 0);
    cm_byte_buf_destroy(&reencoded);
    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&consumer);
    cm_byte_buf_destroy(&legacy_v25);

    derive_legacy_declaration_v24(&encoded, &legacy_v24);
    cm_hir_context_init(&consumer);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_metadata_decode_declaration_artifact(&consumer,
        &artifact, legacy_v24.data, legacy_v24.len, "legacy", 105u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK);
    value = lookup_value(&artifact, "legacy", "constrained");
    assert(value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        && value.data.function.predicate_count == 2u
        && value.data.function.predicates[0].modifier
            == CM_HIR_PREDICATE_REQUIRED
        && value.data.function.predicates[1].modifier
            == CM_HIR_PREDICATE_REQUIRED);
    cm_byte_buf_init(&reencoded);
    result = cm_hir_metadata_encode_declaration_artifact(&reencoded,
        &artifact);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK
        && reencoded.len == encoded.len
        && memcmp(reencoded.data, encoded.data, encoded.len) == 0);
    cm_byte_buf_destroy(&reencoded);
    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&consumer);
    cm_byte_buf_destroy(&legacy_v24);

    consumer_sentinel_init(&consumer, &artifact, &sentinel_crate,
        &sentinel_module);
    before = context_lengths(&consumer);
    assert(cm_hir_library_artifact_identity(&artifact, &sentinel_identity));
    cm_byte_buf_init(&corrupted);
    cm_byte_buf_append(&corrupted, encoded.data, encoded.len);
    corrupt_v24_section_byte(&corrupted, nref_tag, 4u, UINT8_C(0xff));
    result = cm_hir_metadata_decode_declaration_artifact(&consumer,
        &artifact, corrupted.data, corrupted.len, "broken", 105u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT);
    assert_sentinel_preserved(&consumer, &artifact, before,
        &sentinel_identity);
    cm_byte_buf_destroy(&corrupted);
    cm_byte_buf_init(&corrupted);
    cm_byte_buf_append(&corrupted, encoded.data, encoded.len);
    corrupt_v24_section_byte(&corrupted, pred_tag, 4u, UINT8_C(0));
    result = cm_hir_metadata_decode_declaration_artifact(&consumer,
        &artifact, corrupted.data, corrupted.len, "broken", 105u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT);
    assert_sentinel_preserved(&consumer, &artifact, before,
        &sentinel_identity);
    cm_byte_buf_destroy(&corrupted);
    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&consumer);

    cm_hir_context_init(&consumer);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_metadata_decode_declaration_artifact(&consumer,
        &artifact, encoded.data, encoded.len, "dep", 105u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK);
    value = lookup_value(&artifact, "dep", "constrained");
    assert(value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        && value.data.function.generic_parameter_count == 2u
        && value.data.function.predicate_scope_count == 0u
        && value.data.function.predicate_count == 2u
        && value.data.function.outlives_predicate_count == 1u
        && value.data.function.nominal_reference_count == 5u
        && value.data.function.associated_availability_count == 1u);
    fn_reference = find_nominal_reference(&value, "Fn",
        CM_HIR_LIBRARY_NOMINAL_TRAIT);
    fn_mut_reference = find_nominal_reference(&value, "FnMut",
        CM_HIR_LIBRARY_NOMINAL_TRAIT);
    fn_once_reference = find_nominal_reference(&value, "FnOnce",
        CM_HIR_LIBRARY_NOMINAL_TRAIT);
    copy_reference = find_nominal_reference(&value, "Copy",
        CM_HIR_LIBRARY_NOMINAL_TRAIT);
    output_reference = find_nominal_reference(&value, "Output",
        CM_HIR_LIBRARY_NOMINAL_ASSOCIATED_TYPE);
    assert(fn_reference != NULL && fn_mut_reference != NULL
        && fn_once_reference != NULL && copy_reference != NULL
        && output_reference != NULL
        && fn_reference->generic_parameter_count == 1u
        && fn_reference->generic_parameter_kinds[0] == CM_HIR_GENERIC_TYPE
        && fn_mut_reference->generic_parameter_count == 1u
        && fn_once_reference->generic_parameter_count == 1u
        && copy_reference->generic_parameter_count == 0u
        && output_reference->generic_parameter_count == 0u
        && cm_hir_def_id_equal(output_reference->declaring_trait,
            fn_once_reference->definition)
        && cm_hir_def_id_equal(value.data.function
                .associated_availability[0].direct_trait,
            fn_reference->definition)
        && cm_hir_def_id_equal(value.data.function
                .associated_availability[0].associated_type,
            output_reference->definition));
    fn_predicate = NULL;
    copy_predicate = NULL;
    for (item_index = 0u; item_index < value.data.function.predicate_count;
            ++item_index) {
        const CmHirTraitPredicate *predicate;

        predicate = &value.data.function.predicates[item_index];
        if (cm_hir_def_id_equal(predicate->trait_type.definition,
                fn_reference->definition)) fn_predicate = predicate;
        if (cm_hir_def_id_equal(predicate->trait_type.definition,
                copy_reference->definition)) copy_predicate = predicate;
    }
    assert(fn_predicate != NULL && copy_predicate != NULL
        && fn_predicate->binder.lifetime_count == 1u
        && fn_predicate->trait_type.argument_count == 1u
        && fn_predicate->equality_count == 1u);
    definition = cm_hir_lookup_definition(&consumer,
        fn_reference->definition);
    assert(definition != NULL
        && definition->state == CM_HIR_DEFINITION_RESERVED
        && definition->has_reserved_item_kind
        && definition->reserved_item_kind == CM_HIR_ITEM_TRAIT);
    for (item_index = 0u; item_index < consumer.items.len; ++item_index) {
        const CmHirItem *item;

        item = (const CmHirItem *)cm_vec_at_const(&consumer.items,
            item_index);
        assert(item == NULL || !cm_hir_def_id_equal(item->definition,
            fn_reference->definition));
    }
    definition = cm_hir_lookup_definition(&consumer,
        output_reference->definition);
    assert(definition != NULL
        && definition->state == CM_HIR_DEFINITION_RESERVED
        && definition->reserved_item_kind == CM_HIR_ITEM_TYPE_ALIAS);
    tuple = cm_hir_get_type(&consumer,
        fn_predicate->trait_type.arguments[0].data.type);
    input = tuple == NULL || tuple->kind != CM_HIR_TYPE_TUPLE_KIND
        ? NULL : cm_hir_get_type(&consumer,
            tuple->data.tuple_type.elements[0]);
    subject = cm_hir_get_type(&consumer,
        fn_predicate->subject);
    ret_parameter = cm_hir_get_generic_param(&consumer,
        value.data.function.generic_parameter_start);
    c_parameter = cm_hir_get_generic_param(&consumer,
        value.data.function.generic_parameter_start + 1u);
    ret_type = input == NULL ? NULL : cm_hir_get_type(&consumer,
        input->data.reference_type.pointee);
    output_type = cm_hir_get_type(&consumer,
        fn_predicate->equalities[0].value);
    binder_name = cm_interner_get(&consumer.strings,
        fn_predicate->binder.lifetimes[0]);
    assert(fn_predicate->binder.lifetime_count == 1u
        && binder_name != NULL && binder_name->len == 8u
        && memcmp(binder_name->bytes, "elided#0", 8u) == 0
        && ret_parameter != NULL && ret_parameter->index == 0u
        && ret_parameter->kind == CM_HIR_GENERIC_TYPE
        && cm_hir_def_id_equal(ret_parameter->owner, value.definition)
        && c_parameter != NULL && c_parameter->index == 1u
        && c_parameter->kind == CM_HIR_GENERIC_TYPE
        && cm_hir_def_id_equal(c_parameter->owner, value.definition)
        && subject != NULL && subject->kind == CM_HIR_TYPE_PARAMETER_KIND
        && subject->data.parameter_type.parameter
            == value.data.function.generic_parameter_start + 1u
        && input != NULL && input->kind == CM_HIR_TYPE_REFERENCE_KIND
        && input->data.reference_type.region.kind
            == CM_HIR_REGION_LATE_BOUND
        && input->data.reference_type.region.data.binder_index == 0u
        && ret_type != NULL && ret_type->kind == CM_HIR_TYPE_PARAMETER_KIND
        && ret_type->data.parameter_type.parameter
            == value.data.function.generic_parameter_start
        && fn_predicate->equality_count == 1u
        && cm_hir_def_id_equal(fn_predicate->equalities[0].associated_type,
            output_reference->definition)
        && output_type != NULL && output_type->kind == CM_HIR_TYPE_BOOL_KIND
        && copy_predicate->subject == fn_predicate->subject
        && copy_predicate->binder.lifetime_count == 0u
        && copy_predicate->trait_type.argument_count == 0u
        && copy_predicate->equality_count == 0u
        && value.data.function.outlives_predicates[0].subject.type
            == fn_predicate->subject
        && value.data.function.outlives_predicates[0].bound.kind
            == CM_HIR_REGION_STATIC);
    hidden[0].bytes = (const unsigned char *)"dep";
    hidden[0].length = 3u;
    hidden[1].bytes = (const unsigned char *)"Fn";
    hidden[1].length = 2u;
    memset(&binding, 0, sizeof(binding));
    assert(cm_hir_library_artifact_lookup_binding(&artifact, hidden, 2u,
        &binding) == CM_HIR_LIBRARY_NOT_FOUND);

    cm_byte_buf_init(&reencoded);
    result = cm_hir_metadata_encode_declaration_artifact(&reencoded,
        &artifact);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK
        && reencoded.len == encoded.len
        && memcmp(reencoded.data, encoded.data, encoded.len) == 0);
    cm_byte_buf_destroy(&reencoded);
    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&consumer);
    cm_byte_buf_destroy(&encoded);
    parsed_producer_destroy(&producer);

    assert(parsed_producer_build(&unsupported,
        early_predicate_function_source,
        sizeof(early_predicate_function_source) - 1u, 1u, 0u, 1u, 1));
    cm_byte_buf_init(&encoded);
    cm_byte_buf_append(&encoded, unsupported_sentinel,
        sizeof(unsupported_sentinel));
    sentinel_data = encoded.data;
    result = cm_hir_metadata_encode_declaration_artifact(&encoded,
        &unsupported.artifact);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_UNSUPPORTED_HIR
        && encoded.data == sentinel_data
        && encoded.len == sizeof(unsupported_sentinel)
        && memcmp(encoded.data, unsupported_sentinel,
            sizeof(unsupported_sentinel)) == 0);
    cm_byte_buf_destroy(&encoded);
    parsed_producer_destroy(&unsupported);
    (void)sentinel_crate;
    (void)sentinel_module;
}

static void test_declaration_v24_multi_fact_canonical_order(void)
{
    ParsedProducerFixture first;
    ParsedProducerFixture second;
    CmByteBuf first_bytes;
    CmByteBuf second_bytes;
    CmByteBuf reencoded;
    CmHirMetadataArtifactResult result;
    CmHirContext consumer;
    CmHirLibraryArtifact artifact;
    CmHirLibraryValue multi;
    CmHirLibraryValue both;
    const CmHirLibraryNominalReference *item_references[2];
    uint32_t item_count;
    CmHirDefId marker_definition;
    CmHirDefId marker_item_definition;
    const CmHirLibraryNominalReference *marker_reference;
    CmHirTypeId marker_subjects[2];
    uint32_t marker_count;
    uint32_t equality_predicate_count;
    uint32_t index;

    assert(parsed_producer_build(&first, multi_predicate_source_a,
        sizeof(multi_predicate_source_a) - 1u, 1u, 0u, 2u, 1));
    assert(parsed_producer_build(&second, multi_predicate_source_b,
        sizeof(multi_predicate_source_b) - 1u, 1u, 0u, 2u, 1));
    cm_byte_buf_init(&first_bytes);
    cm_byte_buf_init(&second_bytes);
    result = cm_hir_metadata_encode_declaration_artifact(&first_bytes,
        &first.artifact);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK);
    result = cm_hir_metadata_encode_declaration_artifact(&second_bytes,
        &second.artifact);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK
        && second_bytes.len == first_bytes.len
        && memcmp(second_bytes.data, first_bytes.data, first_bytes.len) == 0);

    cm_hir_context_init(&consumer);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_metadata_decode_declaration_artifact(&consumer,
        &artifact, first_bytes.data, first_bytes.len, "dep", 107u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK);
    multi = lookup_value(&artifact, "dep", "multi");
    both = lookup_value(&artifact, "dep", "both");
    assert(multi.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        && multi.data.function.predicate_count == 3u
        && multi.data.function.associated_availability_count == 3u
        && multi.data.function.outlives_predicate_count == 2u
        && both.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        && both.data.function.predicate_count == 2u
        && both.data.function.associated_availability_count == 2u);
    marker_reference = find_nominal_reference(&multi, "Marker",
        CM_HIR_LIBRARY_NOMINAL_TRAIT);
    assert(marker_reference != NULL);
    marker_definition = marker_reference->definition;
    marker_item_definition = cm_hir_def_id_none();
    for (index = 0u; index < multi.data.function.nominal_reference_count;
            ++index) {
        const CmHirLibraryNominalReference *reference;

        reference = &multi.data.function.nominal_references[index];
        if (reference->kind == CM_HIR_LIBRARY_NOMINAL_ASSOCIATED_TYPE
            && reference->name.length == 4u
            && memcmp(reference->name.bytes, "Item", 4u) == 0
            && cm_hir_def_id_equal(reference->declaring_trait,
                marker_definition))
            marker_item_definition = reference->definition;
    }
    assert(!cm_hir_def_id_is_none(marker_definition));
    marker_count = 0u;
    equality_predicate_count = 0u;
    for (index = 0u; index < multi.data.function.predicate_count; ++index) {
        const CmHirTraitPredicate *predicate;

        predicate = &multi.data.function.predicates[index];
        if (predicate->equality_count == 2u) equality_predicate_count += 1u;
        if (cm_hir_def_id_equal(predicate->trait_type.definition,
                marker_definition)) {
            const CmHirType *subject_type;
            const CmHirType *equality_type;

            assert(marker_count < 2u);
            marker_subjects[marker_count++] = predicate->subject;
            subject_type = cm_hir_get_type(&consumer, predicate->subject);
            equality_type = predicate->equality_count == 1u
                ? cm_hir_get_type(&consumer,
                    predicate->equalities[0].value) : NULL;
            assert(predicate->equality_count == 1u
                && cm_hir_def_id_equal(
                    predicate->equalities[0].associated_type,
                    marker_item_definition)
                && subject_type != NULL && equality_type != NULL
                && subject_type->kind == CM_HIR_TYPE_PARAMETER_KIND
                && equality_type->kind == CM_HIR_TYPE_PARAMETER_KIND
                && subject_type->data.parameter_type.parameter
                    == equality_type->data.parameter_type.parameter);
        }
    }
    assert(!cm_hir_def_id_is_none(marker_item_definition)
        && equality_predicate_count == 1u && marker_count == 2u
        && marker_subjects[0] != marker_subjects[1]);
    {
        uint32_t marker_availability_count;

        marker_availability_count = 0u;
        for (index = 0u;
                index < multi.data.function.associated_availability_count;
                ++index) {
            const CmHirLibraryAssociatedAvailability *availability;

            availability = &multi.data.function.associated_availability[
                index];
            if (cm_hir_def_id_equal(availability->direct_trait,
                    marker_definition)
                && cm_hir_def_id_equal(availability->associated_type,
                    marker_item_definition)) marker_availability_count += 1u;
        }
        assert(marker_availability_count == 1u);
    }
    item_count = 0u;
    for (index = 0u; index < both.data.function.nominal_reference_count;
            ++index) {
        const CmHirLibraryNominalReference *reference;

        reference = &both.data.function.nominal_references[index];
        if (reference->kind == CM_HIR_LIBRARY_NOMINAL_ASSOCIATED_TYPE
            && reference->name.length == 4u
            && memcmp(reference->name.bytes, "Item", 4u) == 0) {
            assert(item_count < 2u);
            item_references[item_count++] = reference;
        }
    }
    assert(item_count == 2u
        && !cm_hir_def_id_equal(item_references[0]->definition,
            item_references[1]->definition)
        && !cm_hir_def_id_equal(item_references[0]->declaring_trait,
            item_references[1]->declaring_trait)
        && cm_hir_def_id_equal(item_references[0]->owner_module,
            item_references[1]->owner_module));
    cm_byte_buf_init(&reencoded);
    result = cm_hir_metadata_encode_declaration_artifact(&reencoded,
        &artifact);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK
        && reencoded.len == first_bytes.len
        && memcmp(reencoded.data, first_bytes.data, first_bytes.len) == 0);

    cm_byte_buf_destroy(&reencoded);
    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&consumer);
    cm_byte_buf_destroy(&second_bytes);
    cm_byte_buf_destroy(&first_bytes);
    parsed_producer_destroy(&second);
    parsed_producer_destroy(&first);
}

static void test_carrying_mul_add_modifier_round_trip(void)
{
    ParsedProducerFixture producer;
    CmHirContext consumer;
    CmHirLibraryArtifact artifact;
    CmHirLibraryValue value;
    const CmHirLibraryNominalReference *carrying;
    const CmHirLibraryNominalReference *copy;
    const CmHirLibraryNominalReference *clone;
    const CmHirLibraryNominalReference *sized;
    const CmHirLibraryNominalReference *unsigned_type;
    const CmHirTraitPredicate *predicate;
    const CmHirType *subject;
    const CmHirType *equality;
    const CmHirGenericParam *t_parameter;
    const CmHirGenericParam *u_parameter;
    CmByteBuf encoded;
    CmByteBuf reencoded;
    CmByteBuf malformed;
    CmByteBuf unsupported_version;
    CmHirMetadataEnvelope envelope;
    CmHirMetadataEnvelope unsupported_envelope;
    CmHirMetadataArtifactResult result;
    ContextLengths before;
    CmHirLibraryArtifactIdentity sentinel_identity;
    CmHirCrateId sentinel_crate;
    CmHirModuleId sentinel_module;

    assert(parsed_producer_build(&producer, carrying_mul_add_source,
        sizeof(carrying_mul_add_source) - 1u, 1u, 0u, 1u, 1));
    cm_byte_buf_init(&encoded);
    result = cm_hir_metadata_encode_declaration_artifact(&encoded,
        &producer.artifact);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK
        && result.public_entry_count == 1u && encoded.len != 0u);
    cm_hir_context_init(&consumer);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_metadata_decode_declaration_artifact(&consumer,
        &artifact, encoded.data, encoded.len, "dep", 110u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK
        && result.public_entry_count == 1u);
    value = lookup_value(&artifact, "dep", "carrying_mul_add");
    carrying = find_nominal_reference(&value, "CarryingMulAdd",
        CM_HIR_LIBRARY_NOMINAL_TRAIT);
    copy = find_nominal_reference(&value, "Copy",
        CM_HIR_LIBRARY_NOMINAL_TRAIT);
    clone = find_nominal_reference(&value, "Clone",
        CM_HIR_LIBRARY_NOMINAL_TRAIT);
    sized = find_nominal_reference(&value, "Sized",
        CM_HIR_LIBRARY_NOMINAL_TRAIT);
    unsigned_type = find_nominal_reference(&value, "Unsigned",
        CM_HIR_LIBRARY_NOMINAL_ASSOCIATED_TYPE);
    assert(value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        && value.data.function.generic_parameter_count == 2u
        && value.data.function.predicate_count == 1u
        && value.data.function.outlives_predicate_count == 0u
        && value.data.function.nominal_reference_count == 5u
        && value.data.function.associated_availability_count == 1u
        && carrying != NULL && copy != NULL && clone != NULL
        && sized != NULL && unsigned_type != NULL
        && cm_hir_def_id_equal(unsigned_type->declaring_trait,
            carrying->definition)
        && cm_hir_def_id_equal(value.data.function
            .associated_availability[0].direct_trait, carrying->definition)
        && cm_hir_def_id_equal(value.data.function
            .associated_availability[0].associated_type,
            unsigned_type->definition));
    predicate = &value.data.function.predicates[0];
    subject = cm_hir_get_type(&consumer, predicate->subject);
    equality = predicate->equality_count == 1u
        ? cm_hir_get_type(&consumer, predicate->equalities[0].value) : NULL;
    t_parameter = cm_hir_get_generic_param(&consumer,
        value.data.function.generic_parameter_start);
    u_parameter = cm_hir_get_generic_param(&consumer,
        value.data.function.generic_parameter_start + 1u);
    assert(predicate->modifier == CM_HIR_PREDICATE_CONST_IF_CONST
        && predicate->scope == CM_HIR_PREDICATE_SCOPE_NONE
        && predicate->binder.lifetime_count == 0u
        && predicate->trait_type.argument_count == 0u
        && predicate->equality_count == 1u
        && cm_hir_def_id_equal(predicate->trait_type.definition,
            carrying->definition)
        && cm_hir_def_id_equal(predicate->equalities[0].associated_type,
            unsigned_type->definition)
        && subject != NULL && subject->kind == CM_HIR_TYPE_PARAMETER_KIND
        && equality != NULL && equality->kind == CM_HIR_TYPE_PARAMETER_KIND
        && subject->data.parameter_type.parameter
            == value.data.function.generic_parameter_start
        && equality->data.parameter_type.parameter
            == value.data.function.generic_parameter_start + 1u
        && t_parameter != NULL && t_parameter->index == 0u
        && cm_hir_def_id_equal(t_parameter->owner, value.definition)
        && u_parameter != NULL && u_parameter->index == 1u
        && cm_hir_def_id_equal(u_parameter->owner, value.definition));
    cm_byte_buf_init(&reencoded);
    result = cm_hir_metadata_encode_declaration_artifact(&reencoded,
        &artifact);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK
        && reencoded.len == encoded.len
        && memcmp(reencoded.data, encoded.data, encoded.len) == 0);
    cm_byte_buf_destroy(&reencoded);
    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&consumer);

    consumer_sentinel_init(&consumer, &artifact, &sentinel_crate,
        &sentinel_module);
    before = context_lengths(&consumer);
    assert(cm_hir_library_artifact_identity(&artifact,
        &sentinel_identity));
    cm_byte_buf_init(&malformed);
    cm_byte_buf_append(&malformed, encoded.data, encoded.len);
    corrupt_first_predicate_modifier(&malformed, UINT8_C(3));
    result = cm_hir_metadata_decode_declaration_artifact(&consumer,
        &artifact, malformed.data, malformed.len, "broken", 111u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT);
    assert_sentinel_preserved(&consumer, &artifact, before,
        &sentinel_identity);
    cm_byte_buf_destroy(&malformed);

    cm_byte_buf_init(&malformed);
    cm_byte_buf_append(&malformed, encoded.data, encoded.len);
    truncate_first_predicate_at_modifier(&malformed);
    result = cm_hir_metadata_decode_declaration_artifact(&consumer,
        &artifact, malformed.data, malformed.len, "truncated", 111u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT);
    assert_sentinel_preserved(&consumer, &artifact, before,
        &sentinel_identity);
    cm_byte_buf_destroy(&malformed);

    memset(&envelope, 0, sizeof(envelope));
    assert(cm_hir_metadata_decode_envelope_version(encoded.data, encoded.len,
        CM_HIR_METADATA_DECLARATION_MAJOR,
        CM_HIR_METADATA_DECLARATION_MINOR, &envelope) == CM_HIR_METADATA_OK);
    cm_byte_buf_init(&unsupported_version);
    assert(cm_hir_metadata_encode_envelope_version(&unsupported_version,
        CM_HIR_METADATA_DECLARATION_MAJOR,
        (uint16_t)(CM_HIR_METADATA_DECLARATION_MINOR + 1u), UINT32_C(0),
        envelope.payload, envelope.payload_length) == CM_HIR_METADATA_OK);
    memset(&unsupported_envelope, 0, sizeof(unsupported_envelope));
    assert(cm_hir_metadata_decode_envelope_version(unsupported_version.data,
        unsupported_version.len, CM_HIR_METADATA_DECLARATION_MAJOR,
        (uint16_t)(CM_HIR_METADATA_DECLARATION_MINOR + 1u),
        &unsupported_envelope) == CM_HIR_METADATA_OK);
    result = cm_hir_metadata_decode_declaration_artifact(&consumer,
        &artifact, unsupported_version.data, unsupported_version.len,
        "future", 111u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT);
    assert_sentinel_preserved(&consumer, &artifact, before,
        &sentinel_identity);
    cm_byte_buf_destroy(&unsupported_version);
    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&consumer);
    cm_byte_buf_destroy(&encoded);
    parsed_producer_destroy(&producer);
}

static void test_thin_trait_alias_predicate_round_trip(void)
{
    ParsedProducerFixture producer;
    ParsedProducerFixture swapped;
    CmHirContext consumer;
    CmHirLibraryArtifact artifact;
    CmHirLibraryValue value;
    const CmHirLibraryNominalReference *thin;
    const CmHirLibraryNominalReference *pointee;
    const CmHirLibraryNominalReference *pointee_sized;
    const CmHirLibraryNominalReference *metadata;
    const CmHirTraitPredicate *thin_predicate;
    const CmHirTraitPredicate *sized_predicate;
    const CmHirGenericParam *parameter;
    const CmHirType *subject;
    const CmHirType *return_type;
    const CmHirType *return_pointee;
    const CmHirDefinition *definition;
    CmHirLibraryPathSegment hidden[2];
    CmHirLibraryBinding binding;
    CmByteBuf encoded;
    CmByteBuf swapped_encoded;
    CmByteBuf reencoded;
    CmByteBuf malformed;
    CmByteBuf legacy_v25;
    CmHirMetadataArtifactResult result;
    ContextLengths before;
    CmHirLibraryArtifactIdentity sentinel_identity;
    CmHirCrateId sentinel_crate;
    CmHirModuleId sentinel_module;
    uint32_t index;

    assert(parsed_producer_build(&producer, thin_predicate_source,
        sizeof(thin_predicate_source) - 1u, 1u, 0u, 1u, 1));
    cm_byte_buf_init(&encoded);
    result = cm_hir_metadata_encode_declaration_artifact(&encoded,
        &producer.artifact);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK
        && result.public_entry_count == 1u && encoded.len != 0u);
    assert(parsed_producer_build(&swapped, thin_predicate_swapped_source,
        sizeof(thin_predicate_swapped_source) - 1u, 1u, 0u, 1u, 1));
    cm_byte_buf_init(&swapped_encoded);
    result = cm_hir_metadata_encode_declaration_artifact(&swapped_encoded,
        &swapped.artifact);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK
        && swapped_encoded.len == encoded.len
        && memcmp(swapped_encoded.data, encoded.data, encoded.len) == 0);
    cm_byte_buf_destroy(&swapped_encoded);
    parsed_producer_destroy(&swapped);
    cm_hir_context_init(&consumer);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_metadata_decode_declaration_artifact(&consumer,
        &artifact, encoded.data, encoded.len, "dep", 113u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK
        && result.public_entry_count == 1u);
    value = lookup_value(&artifact, "dep", "null");
    thin = find_nominal_reference(&value, "Thin",
        CM_HIR_LIBRARY_NOMINAL_TRAIT_ALIAS);
    pointee = find_nominal_reference(&value, "Pointee",
        CM_HIR_LIBRARY_NOMINAL_TRAIT);
    pointee_sized = find_nominal_reference(&value, "PointeeSized",
        CM_HIR_LIBRARY_NOMINAL_TRAIT);
    metadata = find_nominal_reference(&value, "Metadata",
        CM_HIR_LIBRARY_NOMINAL_ASSOCIATED_TYPE);
    assert(value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        && value.data.function.is_const == 1
        && value.data.function.parameter_count == 0u
        && value.data.function.generic_parameter_count == 1u
        && value.data.function.predicate_count == 2u
        && value.data.function.outlives_predicate_count == 0u
        && value.data.function.nominal_reference_count == 3u
        && value.data.function.associated_availability_count == 0u
        && thin != NULL && pointee != NULL && pointee_sized != NULL
        && metadata == NULL);
    thin_predicate = NULL;
    sized_predicate = NULL;
    for (index = 0u; index < value.data.function.predicate_count; ++index) {
        const CmHirTraitPredicate *predicate;

        predicate = &value.data.function.predicates[index];
        if (cm_hir_def_id_equal(predicate->trait_type.definition,
                thin->definition)) thin_predicate = predicate;
        if (cm_hir_def_id_equal(predicate->trait_type.definition,
                pointee_sized->definition)) sized_predicate = predicate;
    }
    assert(thin_predicate != NULL && sized_predicate != NULL
        && thin_predicate->subject == sized_predicate->subject
        && thin_predicate->modifier == CM_HIR_PREDICATE_REQUIRED
        && thin_predicate->scope == CM_HIR_PREDICATE_SCOPE_NONE
        && thin_predicate->binder.lifetime_count == 0u
        && thin_predicate->trait_type.argument_count == 0u
        && thin_predicate->equality_count == 0u
        && sized_predicate->modifier == CM_HIR_PREDICATE_REQUIRED
        && sized_predicate->binder.lifetime_count == 0u
        && sized_predicate->trait_type.argument_count == 0u
        && sized_predicate->equality_count == 0u);
    parameter = cm_hir_get_generic_param(&consumer,
        value.data.function.generic_parameter_start);
    subject = cm_hir_get_type(&consumer, thin_predicate->subject);
    return_type = cm_hir_get_type(&consumer,
        value.data.function.return_type);
    return_pointee = return_type == NULL
            || return_type->kind != CM_HIR_TYPE_RAW_POINTER_KIND
        ? NULL : cm_hir_get_type(&consumer,
            return_type->data.raw_pointer_type.pointee);
    assert(parameter != NULL && parameter->kind == CM_HIR_GENERIC_TYPE
        && parameter->index == 0u
        && cm_hir_def_id_equal(parameter->owner, value.definition)
        && subject != NULL && subject->kind == CM_HIR_TYPE_PARAMETER_KIND
        && subject->data.parameter_type.parameter
            == value.data.function.generic_parameter_start
        && return_type != NULL
        && return_type->kind == CM_HIR_TYPE_RAW_POINTER_KIND
        && return_type->data.raw_pointer_type.mutability == CM_HIR_IMMUTABLE
        && return_pointee != NULL
        && return_pointee->kind == CM_HIR_TYPE_PARAMETER_KIND
        && return_pointee->data.parameter_type.parameter
            == value.data.function.generic_parameter_start);
    definition = cm_hir_lookup_definition(&consumer, thin->definition);
    assert(definition != NULL
        && definition->state == CM_HIR_DEFINITION_RESERVED
        && definition->has_reserved_item_kind
        && definition->reserved_item_kind == CM_HIR_ITEM_TRAIT_ALIAS);
    for (index = 0u; index < (uint32_t)consumer.items.len; ++index) {
        const CmHirItem *item;

        item = (const CmHirItem *)cm_vec_at_const(&consumer.items, index);
        assert(item == NULL || (!cm_hir_def_id_equal(item->definition,
                thin->definition)
            && !cm_hir_def_id_equal(item->definition, pointee->definition)
            && !cm_hir_def_id_equal(item->definition,
                pointee_sized->definition)));
    }
    hidden[0].bytes = (const unsigned char *)"dep";
    hidden[0].length = 3u;
    hidden[1].bytes = (const unsigned char *)"Thin";
    hidden[1].length = 4u;
    memset(&binding, 0, sizeof(binding));
    assert(cm_hir_library_artifact_lookup_binding(&artifact, hidden, 2u,
        &binding) == CM_HIR_LIBRARY_NOT_FOUND);
    cm_byte_buf_init(&reencoded);
    result = cm_hir_metadata_encode_declaration_artifact(&reencoded,
        &artifact);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK
        && reencoded.len == encoded.len
        && memcmp(reencoded.data, encoded.data, encoded.len) == 0);
    cm_byte_buf_destroy(&reencoded);
    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&consumer);

    consumer_sentinel_init(&consumer, &artifact, &sentinel_crate,
        &sentinel_module);
    before = context_lengths(&consumer);
    assert(cm_hir_library_artifact_identity(&artifact,
        &sentinel_identity));
    cm_byte_buf_init(&malformed);
    cm_byte_buf_append(&malformed, encoded.data, encoded.len);
    corrupt_nominal_kind_named(&malformed, "Thin", UINT8_C(0));
    result = cm_hir_metadata_decode_declaration_artifact(&consumer,
        &artifact, malformed.data, malformed.len, "broken", 114u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT);
    assert_sentinel_preserved(&consumer, &artifact, before,
        &sentinel_identity);
    cm_byte_buf_destroy(&malformed);

    derive_legacy_declaration_v25(&encoded, &legacy_v25);
    result = cm_hir_metadata_decode_declaration_artifact(&consumer,
        &artifact, legacy_v25.data, legacy_v25.len, "legacy5", 114u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT);
    assert_sentinel_preserved(&consumer, &artifact, before,
        &sentinel_identity);
    cm_byte_buf_destroy(&legacy_v25);
    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&consumer);

    {
        static const unsigned char equality_sentinel[] = {
            UINT8_C(0x26), UINT8_C(0xea), UINT8_C(0x51)
        };
        const CmHirLibraryOwnedData *producer_owned_const;
        CmHirLibraryOwnedValue *producer_owned_value;
        CmHirLibraryValue producer_value;
        const CmHirLibraryNominalReference *producer_thin;
        CmHirAssociatedTypeEquality fake_equality;
        CmHirAssociatedTypeEquality *saved_equalities;
        uint32_t saved_equality_count;
        uint32_t predicate_index;
        unsigned char *sentinel_data;

        producer_value = lookup_value(&producer.artifact, "producer", "null");
        producer_thin = find_nominal_reference(&producer_value, "Thin",
            CM_HIR_LIBRARY_NOMINAL_TRAIT_ALIAS);
        producer_owned_const = cm_hir_library_artifact_owned_data_const(
            &producer.artifact);
        assert(producer_thin != NULL && producer_owned_const != NULL
            && producer_owned_const->values.len == 1u);
        producer_owned_value = (CmHirLibraryOwnedValue *)(void *)
            cm_vec_at_const(&producer_owned_const->values, 0u);
        assert(producer_owned_value != NULL);
        predicate_index = UINT32_MAX;
        for (index = 0u; index < producer_owned_value->predicate_count;
                ++index)
            if (cm_hir_def_id_equal(producer_owned_value->predicates[index]
                    .trait_type.definition, producer_thin->definition))
                predicate_index = index;
        assert(predicate_index != UINT32_MAX);
        saved_equalities = producer_owned_value->predicates[predicate_index]
            .equalities;
        saved_equality_count = producer_owned_value
            ->predicates[predicate_index].equality_count;
        memset(&fake_equality, 0, sizeof(fake_equality));
        producer_owned_value->predicates[predicate_index].equalities =
            &fake_equality;
        producer_owned_value->predicates[predicate_index].equality_count = 1u;
        cm_byte_buf_init(&malformed);
        cm_byte_buf_append(&malformed, equality_sentinel,
            sizeof(equality_sentinel));
        sentinel_data = malformed.data;
        result = cm_hir_metadata_encode_declaration_artifact(&malformed,
            &producer.artifact);
        assert(result.status == CM_HIR_METADATA_ARTIFACT_UNSUPPORTED_HIR
            && malformed.data == sentinel_data
            && malformed.len == sizeof(equality_sentinel)
            && memcmp(malformed.data, equality_sentinel,
                sizeof(equality_sentinel)) == 0);
        cm_byte_buf_destroy(&malformed);
        producer_owned_value->predicates[predicate_index].equalities =
            saved_equalities;
        producer_owned_value->predicates[predicate_index].equality_count =
            saved_equality_count;
    }
    cm_byte_buf_destroy(&encoded);
    parsed_producer_destroy(&producer);
}

static void test_duplicate_const_callable_library_capture(void)
{
    ParsedProducerFixture producer;
    CmHirContext consumer;
    CmHirLibraryArtifact artifact;
    CmHirLibraryValue value;
    const CmHirLibraryNominalReference *fn_once;
    const CmHirLibraryNominalReference *output;
    CmHirTypeId subjects[2];
    CmHirGenericParamId equality_parameter;
    uint32_t index;
    uint32_t const_count;
    CmByteBuf encoded;
    CmByteBuf reencoded;
    CmHirMetadataArtifactResult result;

    assert(parsed_producer_build(&producer, duplicate_const_callable_source,
        sizeof(duplicate_const_callable_source) - 1u, 1u, 0u, 1u, 1));
    value = lookup_value(&producer.artifact, "producer",
        "const_eval_select");
    fn_once = find_nominal_reference(&value, "FnOnce",
        CM_HIR_LIBRARY_NOMINAL_TRAIT);
    output = find_nominal_reference(&value, "Output",
        CM_HIR_LIBRARY_NOMINAL_ASSOCIATED_TYPE);
    assert(fn_once != NULL && output != NULL
        && cm_hir_def_id_equal(output->declaring_trait,
            fn_once->definition)
        && value.data.function.predicate_count == 2u
        && value.data.function.associated_availability_count == 1u
        && cm_hir_def_id_equal(value.data.function
            .associated_availability[0].direct_trait, fn_once->definition)
        && cm_hir_def_id_equal(value.data.function
            .associated_availability[0].associated_type,
            output->definition));
    equality_parameter = CM_HIR_GENERIC_PARAM_NONE;
    const_count = 0u;
    for (index = 0u; index < value.data.function.predicate_count; ++index) {
        const CmHirTraitPredicate *predicate;
        const CmHirType *equality_type;

        predicate = &value.data.function.predicates[index];
        equality_type = predicate->equality_count == 1u
            ? cm_hir_get_type(&producer.context,
                predicate->equalities[0].value) : NULL;
        subjects[index] = predicate->subject;
        assert(cm_hir_def_id_equal(predicate->trait_type.definition,
                fn_once->definition)
            && predicate->equality_count == 1u
            && cm_hir_def_id_equal(predicate->equalities[0].associated_type,
                output->definition)
            && equality_type != NULL
            && equality_type->kind == CM_HIR_TYPE_PARAMETER_KIND);
        if (index == 0u) equality_parameter =
            equality_type->data.parameter_type.parameter;
        else assert(equality_type->data.parameter_type.parameter
            == equality_parameter);
        if (predicate->modifier == CM_HIR_PREDICATE_CONST) const_count += 1u;
        else assert(predicate->modifier == CM_HIR_PREDICATE_REQUIRED);
    }
    assert(subjects[0] != subjects[1] && const_count == 1u);

    cm_byte_buf_init(&encoded);
    result = cm_hir_metadata_encode_declaration_artifact(&encoded,
        &producer.artifact);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK
        && result.public_entry_count == 1u && encoded.len != 0u);
    cm_hir_context_init(&consumer);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_metadata_decode_declaration_artifact(&consumer,
        &artifact, encoded.data, encoded.len, "dep", 109u);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK
        && result.public_entry_count == 1u);
    value = lookup_value(&artifact, "dep", "const_eval_select");
    assert(value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        && value.data.function.predicate_count == 2u
        && value.data.function.associated_availability_count == 1u);
    const_count = 0u;
    for (index = 0u; index < value.data.function.predicate_count; ++index) {
        const CmHirTraitPredicate *predicate;

        predicate = &value.data.function.predicates[index];
        assert(predicate->equality_count == 1u);
        if (predicate->modifier == CM_HIR_PREDICATE_CONST) const_count += 1u;
        else assert(predicate->modifier == CM_HIR_PREDICATE_REQUIRED);
    }
    assert(const_count == 1u);
    cm_byte_buf_init(&reencoded);
    result = cm_hir_metadata_encode_declaration_artifact(&reencoded,
        &artifact);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK
        && reencoded.len == encoded.len
        && memcmp(reencoded.data, encoded.data, encoded.len) == 0);
    cm_byte_buf_destroy(&reencoded);
    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&consumer);
    cm_byte_buf_destroy(&encoded);
    parsed_producer_destroy(&producer);
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "produce-forward") == 0) {
        return produce_process_artifact(argv[2], 0) ? 0 : 1;
    }
    if (argc == 3 && strcmp(argv[1], "produce-reverse") == 0) {
        return produce_process_artifact(argv[2], 1) ? 0 : 1;
    }
    if (argc == 3 && strcmp(argv[1], "consume") == 0) {
        return consume_process_artifact(argv[2]) ? 0 : 1;
    }
    if (argc == 3 && strcmp(argv[1], "produce-declaration") == 0) {
        return produce_declaration_process_artifact(argv[2]) ? 0 : 1;
    }
    if (argc == 3 && strcmp(argv[1], "consume-declaration") == 0) {
        return consume_declaration_process_artifact(argv[2]) ? 0 : 1;
    }
    if (argc == 3 && strcmp(argv[1], "produce-generic-function") == 0) {
        return produce_generic_function_process_artifact(argv[2]) ? 0 : 1;
    }
    if (argc == 3 && strcmp(argv[1], "consume-generic-function") == 0) {
        return consume_generic_function_process_artifact(argv[2]) ? 0 : 1;
    }
    if (argc == 3 && strcmp(argv[1], "produce-predicate-function") == 0) {
        return produce_predicate_function_process_artifact(argv[2]) ? 0 : 1;
    }
    if (argc == 3 && strcmp(argv[1], "consume-predicate-function") == 0) {
        return consume_predicate_function_process_artifact(argv[2]) ? 0 : 1;
    }
    if (argc == 3 && strcmp(argv[1], "produce-modifier-function") == 0) {
        return produce_modifier_function_process_artifact(argv[2]) ? 0 : 1;
    }
    if (argc == 3 && strcmp(argv[1], "consume-modifier-function") == 0) {
        return consume_modifier_function_process_artifact(argv[2]) ? 0 : 1;
    }
    if (argc == 3 && strcmp(argv[1], "produce-trait-alias-function") == 0) {
        return produce_trait_alias_function_process_artifact(argv[2])
            ? 0 : 1;
    }
    if (argc == 3 && strcmp(argv[1], "consume-trait-alias-function") == 0) {
        return consume_trait_alias_function_process_artifact(argv[2])
            ? 0 : 1;
    }
    if (argc != 1) {
        fputs("usage: test_hir_metadata "
            "[produce-forward|produce-reverse|consume|produce-declaration|"
            "consume-declaration|produce-generic-function|"
            "consume-generic-function|produce-predicate-function|"
            "consume-predicate-function|produce-modifier-function|"
            "consume-modifier-function|produce-trait-alias-function|"
            "consume-trait-alias-function FILE]\n", stderr);
        return 2;
    }
    test_primitive_only_round_trip();
    test_semantic_unsupported_producers();
    test_semantic_trait_universe_round_trip();
    test_unsupported_hir_rejected();
    test_parsed_unsupported_hir_rejected();
    test_bound_function_pointer_v2_rejected_transactionally();
    test_semantic_round_trip();
    test_declaration_v2_value_round_trip();
    test_declaration_v2_const_generic_round_trip();
    test_declaration_v2_const_terms_round_trip();
    test_declaration_v2_generic_function_round_trip();
    test_declaration_v24_predicate_function_round_trip();
    test_declaration_v24_multi_fact_canonical_order();
    test_carrying_mul_add_modifier_round_trip();
    test_thin_trait_alias_predicate_round_trip();
    test_duplicate_const_callable_library_capture();
    test_parsed_declaration_v2_capture();
    assert(strcmp(cm_hir_metadata_artifact_status_name(
        CM_HIR_METADATA_ARTIFACT_OK), "ok") == 0);
    return 0;
}
