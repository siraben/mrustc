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
    "pub fn constrained<T>(value: T) -> T where T: 'static { value }\n";

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
    ParsedProducerFixture producer;
    ParsedProducerFixture rejected;
    CmByteBuf encoded;
    CmByteBuf corrupted;
    CmByteBuf reencoded;
    CmHirMetadataArtifactResult result;
    CmHirContext consumer;
    CmHirLibraryArtifact artifact;
    CmHirCrateId sentinel_crate;
    CmHirModuleId sentinel_module;
    ContextLengths before;
    CmHirLibraryArtifactIdentity sentinel_identity;

    assert(parsed_producer_build(&producer, generic_function_source,
        sizeof(generic_function_source) - 1u, 1u, 0u, 1u, 1));
    assert(generic_function_valid(&producer.context, &producer.artifact,
        "producer"));
    cm_byte_buf_init(&encoded);
    result = cm_hir_metadata_encode_declaration_artifact(&encoded,
        &producer.artifact);
    assert(result.status == CM_HIR_METADATA_ARTIFACT_OK
        && result.public_entry_count == 1u && encoded.len != 0u);

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

    assert(!parsed_producer_build(&rejected, predicate_function_source,
        sizeof(predicate_function_source) - 1u, 1u, 0u, 1u, 1));

    cm_byte_buf_destroy(&reencoded);
    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&consumer);
    cm_byte_buf_destroy(&encoded);
    parsed_producer_destroy(&producer);
    (void)sentinel_crate;
    (void)sentinel_module;
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
    if (argc != 1) {
        fputs("usage: test_hir_metadata "
            "[produce-forward|produce-reverse|consume|produce-declaration|"
            "consume-declaration|produce-generic-function|"
            "consume-generic-function FILE]\n", stderr);
        return 2;
    }
    test_primitive_only_round_trip();
    test_semantic_unsupported_producers();
    test_semantic_trait_universe_round_trip();
    test_unsupported_hir_rejected();
    test_parsed_unsupported_hir_rejected();
    test_semantic_round_trip();
    test_declaration_v2_value_round_trip();
    test_declaration_v2_const_generic_round_trip();
    test_declaration_v2_const_terms_round_trip();
    test_declaration_v2_generic_function_round_trip();
    test_parsed_declaration_v2_capture();
    assert(strcmp(cm_hir_metadata_artifact_status_name(
        CM_HIR_METADATA_ARTIFACT_OK), "ok") == 0);
    return 0;
}
