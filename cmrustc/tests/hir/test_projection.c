#include "cm/alloc.h"
#include "cm/hir/projection.h"
#include "cm/hir/type_alias.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static CmSpan test_span(uint32_t start, uint32_t end)
{
    CmSpan span;

    span.source = 1u;
    span.start = start;
    span.end = end;
    return span;
}

static void init_item(CmHirItem *item, CmHirItemKind kind,
    CmHirDefId definition, CmHirModuleId module,
    CmHirDefId parent_definition, CmInternId name)
{
    memset(item, 0, sizeof(*item));
    item->kind = kind;
    item->definition = definition;
    item->owner_module = module;
    item->parent_definition = parent_definition;
    item->name = name;
    item->visibility.kind = CM_HIR_VIS_PRIVATE;
    item->visibility.restriction = cm_hir_def_id_none();
    item->span = test_span(1u, 2u);
}

static CmHirTypeId add_scalar(CmHirContext *context, CmHirTypeKind kind,
    unsigned int subtype)
{
    CmHirType type;
    CmHirTypeId type_id;

    memset(&type, 0, sizeof(type));
    type.kind = kind;
    type.span = test_span(1u, 2u);
    if (kind == CM_HIR_TYPE_INTEGER_KIND) {
        type.data.integer_type.kind = (CmHirIntType)subtype;
    } else if (kind == CM_HIR_TYPE_FLOAT_KIND) {
        type.data.float_type.kind = (CmHirFloatType)subtype;
    }
    assert(cm_hir_add_type(context, &type, &type_id) == CM_HIR_OK);
    return type_id;
}

static CmHirDefId add_trait(CmHirContext *context, CmHirCrateId crate_id,
    CmHirModuleId module, const char *name)
{
    CmHirItem item;
    CmHirItemId item_id;
    CmHirDefId definition;

    assert(cm_hir_reserve_item_definition(context, crate_id,
        test_span(1u, 2u), &definition) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_TRAIT, definition, module,
        cm_hir_def_id_none(), cm_hir_intern(context, name));
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(context, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_trait_associated(CmHirContext *context,
    CmHirCrateId crate_id, CmHirModuleId module, CmHirDefId trait_definition,
    const char *name)
{
    CmHirItem item;
    CmHirItemId item_id;
    CmHirDefId definition;

    assert(cm_hir_reserve_item_definition(context, crate_id,
        test_span(1u, 2u), &definition) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_TYPE_ALIAS, definition, module,
        trait_definition, cm_hir_intern(context, name));
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    item.data.type_alias_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(context, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_generic_trait_associated(CmHirContext *context,
    CmHirCrateId crate_id, CmHirModuleId module,
    CmHirDefId trait_definition, const char *name)
{
    CmHirItem item;
    CmHirItemId item_id;
    CmHirDefId definition;
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_id;

    assert(cm_hir_reserve_item_definition(context, crate_id,
        test_span(1u, 2u), &definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = definition;
    parameter.name = cm_hir_intern(context, "X");
    parameter.span = test_span(1u, 2u);
    assert(cm_hir_add_generic_param(context, &parameter, &parameter_id)
        == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_TYPE_ALIAS, definition, module,
        trait_definition, cm_hir_intern(context, name));
    item.generic_parameter_start = parameter_id;
    item.generic_parameter_count = 1u;
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    item.data.type_alias_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(context, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirTypeId add_adt(CmHirContext *context, CmHirCrateId crate_id,
    CmHirModuleId module, const char *name, CmHirDefId *out_definition)
{
    CmHirItem item;
    CmHirItemId item_id;
    CmHirType type;
    CmHirTypeId type_id;

    assert(cm_hir_reserve_item_definition(context, crate_id,
        test_span(1u, 2u), out_definition) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_STRUCT, *out_definition, module,
        cm_hir_def_id_none(), cm_hir_intern(context, name));
    item.data.aggregate_item.form = CM_HIR_AGGREGATE_UNIT;
    assert(cm_hir_add_item(context, &item, &item_id) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ADT_KIND;
    type.span = test_span(1u, 2u);
    type.data.named_type.definition = *out_definition;
    assert(cm_hir_add_type(context, &type, &type_id) == CM_HIR_OK);
    return type_id;
}

static CmHirTypeId add_union(CmHirContext *context, CmHirCrateId crate_id,
    CmHirModuleId module, const char *name, CmHirDefId *out_definition)
{
    CmHirItem item;
    CmHirItemId item_id;
    CmHirType type;
    CmHirTypeId type_id;

    assert(cm_hir_reserve_item_definition(context, crate_id,
        test_span(1u, 2u), out_definition) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_UNION, *out_definition, module,
        cm_hir_def_id_none(), cm_hir_intern(context, name));
    item.data.aggregate_item.form = CM_HIR_AGGREGATE_UNIT;
    assert(cm_hir_add_item(context, &item, &item_id) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ADT_KIND;
    type.span = test_span(1u, 2u);
    type.data.named_type.definition = *out_definition;
    assert(cm_hir_add_type(context, &type, &type_id) == CM_HIR_OK);
    return type_id;
}

static CmHirTypeId add_parameter_type(CmHirContext *context,
    CmHirGenericParamId parameter)
{
    CmHirType type;
    CmHirTypeId type_id;

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = test_span(1u, 2u);
    type.data.parameter_type.parameter = parameter;
    assert(cm_hir_add_type(context, &type, &type_id) == CM_HIR_OK);
    return type_id;
}

static CmHirDefId add_generic_struct(CmHirContext *context,
    CmHirCrateId crate_id, CmHirModuleId module, const char *name,
    uint32_t parameter_count, CmHirGenericParamId *out_parameters,
    CmHirTypeId *out_parameter_types)
{
    CmHirItem item;
    CmHirItemId item_id;
    CmHirDefId definition;
    uint32_t index;

    assert(parameter_count != 0u);
    assert(cm_hir_reserve_item_definition(context, crate_id,
        test_span(1u, 2u), &definition) == CM_HIR_OK);
    for (index = 0u; index < parameter_count; ++index) {
        CmHirGenericParam parameter;
        const char *parameter_name;

        memset(&parameter, 0, sizeof(parameter));
        parameter.kind = CM_HIR_GENERIC_TYPE;
        parameter.owner = definition;
        parameter.index = index;
        parameter_name = index == 0u ? "T" : "U";
        parameter.name = cm_hir_intern(context, parameter_name);
        parameter.span = test_span(1u, 2u);
        assert(cm_hir_add_generic_param(context, &parameter,
            &out_parameters[index]) == CM_HIR_OK);
        out_parameter_types[index] = add_parameter_type(context,
            out_parameters[index]);
    }
    init_item(&item, CM_HIR_ITEM_STRUCT, definition, module,
        cm_hir_def_id_none(), cm_hir_intern(context, name));
    item.generic_parameter_start = out_parameters[0];
    item.generic_parameter_count = parameter_count;
    item.data.aggregate_item.form = CM_HIR_AGGREGATE_UNIT;
    assert(cm_hir_add_item(context, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirTypeId add_named_application(CmHirContext *context,
    CmHirDefId definition, const CmHirTypeId *arguments,
    uint32_t argument_count)
{
    CmHirGenericArg generic_arguments[2];
    CmHirType type;
    CmHirTypeId type_id;
    uint32_t index;

    assert(argument_count <= 2u);
    memset(generic_arguments, 0, sizeof(generic_arguments));
    for (index = 0u; index < argument_count; ++index) {
        generic_arguments[index].kind = CM_HIR_GENERIC_ARG_TYPE;
        generic_arguments[index].data.type = arguments[index];
    }
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ADT_KIND;
    type.span = test_span(1u, 2u);
    type.data.named_type.definition = definition;
    type.data.named_type.arguments = generic_arguments;
    type.data.named_type.argument_count = argument_count;
    assert(cm_hir_add_type(context, &type, &type_id) == CM_HIR_OK);
    return type_id;
}

static CmHirDefId add_single_parameter_impl(CmHirContext *context,
    CmHirCrateId crate_id, CmHirModuleId module,
    CmHirDefId trait_definition, CmHirDefId self_definition,
    int is_blanket, CmHirTypeId *out_parameter_type,
    CmHirTypeId *out_self_template)
{
    CmHirItem item;
    CmHirItemId item_id;
    CmHirDefId definition;
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_id;

    assert(cm_hir_reserve_item_definition(context, crate_id,
        test_span(1u, 2u), &definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = definition;
    parameter.name = cm_hir_intern(context, "T");
    parameter.span = test_span(1u, 2u);
    assert(cm_hir_add_generic_param(context, &parameter, &parameter_id)
        == CM_HIR_OK);
    *out_parameter_type = add_parameter_type(context, parameter_id);
    if (is_blanket) {
        *out_self_template = *out_parameter_type;
    } else {
        *out_self_template = add_named_application(context, self_definition,
            out_parameter_type, 1u);
    }
    init_item(&item, CM_HIR_ITEM_IMPL, definition, module,
        cm_hir_def_id_none(), CM_INTERN_ID_NONE);
    item.generic_parameter_start = parameter_id;
    item.generic_parameter_count = 1u;
    item.data.impl_item.self_type = *out_self_template;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = trait_definition;
    item.data.impl_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(context, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_impl(CmHirContext *context, CmHirCrateId crate_id,
    CmHirModuleId module, CmHirDefId trait_definition,
    CmHirTypeId self_type)
{
    CmHirItem item;
    CmHirItemId item_id;
    CmHirDefId definition;

    assert(cm_hir_reserve_item_definition(context, crate_id,
        test_span(1u, 2u), &definition) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_IMPL, definition, module,
        cm_hir_def_id_none(), CM_INTERN_ID_NONE);
    item.data.impl_item.self_type = self_type;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = trait_definition;
    item.data.impl_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(context, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_impl_associated(CmHirContext *context,
    CmHirCrateId crate_id, CmHirModuleId module, CmHirDefId impl_definition,
    CmHirDefId trait_item_definition, const char *name,
    CmHirTypeId target)
{
    CmHirItem item;
    CmHirItemId item_id;
    CmHirDefId definition;

    assert(cm_hir_reserve_item_definition(context, crate_id,
        test_span(1u, 2u), &definition) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_TYPE_ALIAS, definition, module,
        impl_definition, cm_hir_intern(context, name));
    item.data.type_alias_item.target = target;
    item.data.type_alias_item.trait_item_definition = trait_item_definition;
    assert(cm_hir_add_item(context, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_generic_impl_associated(CmHirContext *context,
    CmHirCrateId crate_id, CmHirModuleId module, CmHirDefId impl_definition,
    CmHirDefId trait_item_definition, const char *name)
{
    CmHirItem item;
    CmHirItemId item_id;
    CmHirDefId definition;
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_id;
    CmHirType parameter_type;
    CmHirTypeId parameter_type_id;

    assert(cm_hir_reserve_item_definition(context, crate_id,
        test_span(1u, 2u), &definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = definition;
    parameter.name = cm_hir_intern(context, "X");
    parameter.span = test_span(1u, 2u);
    assert(cm_hir_add_generic_param(context, &parameter, &parameter_id)
        == CM_HIR_OK);
    memset(&parameter_type, 0, sizeof(parameter_type));
    parameter_type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    parameter_type.span = test_span(1u, 2u);
    parameter_type.data.parameter_type.parameter = parameter_id;
    assert(cm_hir_add_type(context, &parameter_type, &parameter_type_id)
        == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_TYPE_ALIAS, definition, module,
        impl_definition, cm_hir_intern(context, name));
    item.generic_parameter_start = parameter_id;
    item.generic_parameter_count = 1u;
    item.data.type_alias_item.target = parameter_type_id;
    item.data.type_alias_item.trait_item_definition = trait_item_definition;
    assert(cm_hir_add_item(context, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirTypeId add_and_normalize_alias(CmHirContext *context,
    CmHirCrateId crate_id, CmHirModuleId module, const char *name,
    CmHirTypeId target, CmHirDefId *out_definition)
{
    CmHirItem item;
    CmHirItemId item_id;
    CmHirType application;
    CmHirTypeId application_id;
    CmHirTypeAliasResult result;

    assert(cm_hir_reserve_item_definition(context, crate_id,
        test_span(1u, 2u), out_definition) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_TYPE_ALIAS, *out_definition, module,
        cm_hir_def_id_none(), cm_hir_intern(context, name));
    item.data.type_alias_item.target = target;
    item.data.type_alias_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(context, &item, &item_id) == CM_HIR_OK);
    memset(&application, 0, sizeof(application));
    application.kind = CM_HIR_TYPE_ALIAS_APPLICATION_KIND;
    application.span = test_span(1u, 2u);
    application.data.named_type.definition = *out_definition;
    assert(cm_hir_add_type(context, &application, &application_id)
        == CM_HIR_OK);
    result = cm_hir_normalize_type_aliases(context, application_id);
    assert(result.status == CM_HIR_TYPE_ALIAS_OK
        && result.type != CM_HIR_TYPE_NONE);
    return result.type;
}

static CmHirTypeId add_projection(CmHirContext *context,
    CmHirTypeId self_type, CmHirDefId trait_definition,
    CmHirDefId associated_definition, CmHirGenericArg *trait_arguments,
    uint32_t trait_argument_count, CmHirGenericArg *associated_arguments,
    uint32_t associated_argument_count)
{
    CmHirType type;
    CmHirTypeId type_id;

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PROJECTION_KIND;
    type.span = test_span(1u, 2u);
    type.data.projection_type.self_type = self_type;
    type.data.projection_type.trait_type.definition = trait_definition;
    type.data.projection_type.trait_type.arguments = trait_arguments;
    type.data.projection_type.trait_type.argument_count =
        trait_argument_count;
    type.data.projection_type.associated_type.definition =
        associated_definition;
    type.data.projection_type.associated_type.arguments =
        associated_arguments;
    type.data.projection_type.associated_type.argument_count =
        associated_argument_count;
    assert(cm_hir_add_type(context, &type, &type_id) == CM_HIR_OK);
    return type_id;
}

static void check_empty_result(CmHirProjectionResult result,
    CmHirProjectionStatus status)
{
    assert(result.status == status);
    assert(result.target == CM_HIR_TYPE_NONE);
    assert(cm_hir_def_id_is_none(result.impl_definition));
    assert(cm_hir_def_id_is_none(result.impl_associated_definition));
    if (status != CM_HIR_PROJECTION_SUBSTITUTION_FAILURE) {
        assert(result.hir_status == CM_HIR_OK);
    }
    assert(result.allocated_type_count == 0u);
}

static void check_selected(CmHirProjectionResult result,
    CmHirTypeId target, CmHirDefId impl_definition,
    CmHirDefId impl_item_definition)
{
    assert(result.status == CM_HIR_PROJECTION_SELECTED);
    assert(result.target == target);
    assert(cm_hir_def_id_equal(result.impl_definition, impl_definition));
    assert(cm_hir_def_id_equal(result.impl_associated_definition,
        impl_item_definition));
    assert(result.hir_status == CM_HIR_OK);
}

static void check_empty_match(CmHirProjectionMatch match,
    CmHirProjectionStatus status)
{
    assert(match.status == status);
    assert(match.target_template == CM_HIR_TYPE_NONE);
    assert(match.query_self == CM_HIR_TYPE_NONE);
    assert(cm_hir_def_id_is_none(match.impl_definition));
    assert(cm_hir_def_id_is_none(match.impl_associated_definition));
}

static void check_match(CmHirProjectionMatch match,
    CmHirTypeId target_template, CmHirTypeId query_self,
    CmHirDefId impl_definition, CmHirDefId impl_item_definition)
{
    assert(match.status == CM_HIR_PROJECTION_SELECTED);
    assert(match.target_template == target_template);
    assert(match.query_self == query_self);
    assert(cm_hir_def_id_equal(match.impl_definition, impl_definition));
    assert(cm_hir_def_id_equal(match.impl_associated_definition,
        impl_item_definition));
}

static void test_generic_projection(void)
{
    CmHirContext context;
    CmHirContext snapshot;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    CmHirDefId wrapper_definition;
    CmHirDefId pair_definition;
    CmHirDefId trait_definition;
    CmHirDefId direct_definition;
    CmHirDefId pair_associated_definition;
    CmHirDefId impl_definition;
    CmHirDefId direct_impl_definition;
    CmHirDefId pair_impl_definition;
    CmHirDefId blanket_trait_definition;
    CmHirDefId blanket_associated_definition;
    CmHirDefId blanket_impl_definition;
    CmHirDefId malformed_trait_definition;
    CmHirDefId malformed_associated_definition;
    CmHirDefId malformed_impl_definition;
    CmHirDefId malformed_impl_item_definition;
    CmHirDefId ambiguous_trait_definition;
    CmHirDefId ambiguous_associated_definition;
    CmHirDefId first_ambiguous_impl;
    CmHirDefId second_ambiguous_impl;
    CmHirGenericParamId wrapper_parameters[1];
    CmHirGenericParamId pair_parameters[2];
    CmHirTypeId wrapper_parameter_types[1];
    CmHirTypeId pair_parameter_types[2];
    CmHirTypeId impl_parameter_type;
    CmHirTypeId impl_self_template;
    CmHirTypeId pair_template_arguments[2];
    CmHirTypeId pair_target_template;
    CmHirTypeId blanket_parameter_type;
    CmHirTypeId blanket_self_template;
    CmHirTypeId malformed_parameter_type;
    CmHirTypeId malformed_self_template;
    CmHirTypeId first_ambiguous_parameter_type;
    CmHirTypeId first_ambiguous_self_template;
    CmHirTypeId second_ambiguous_parameter_type;
    CmHirTypeId second_ambiguous_self_template;
    CmHirTypeId u8_type;
    CmHirTypeId bool_type;
    CmHirTypeId wrapper_u8;
    CmHirTypeId wrapper_bool;
    CmHirTypeId direct_u8_projection;
    CmHirTypeId direct_bool_projection;
    CmHirTypeId pair_u8_projection;
    CmHirTypeId blanket_projection;
    CmHirTypeId malformed_projection;
    CmHirTypeId ambiguous_projection;
    CmHirProjectionMatch match;
    CmHirProjectionResult result;
    const CmHirType *selected_type;
    unsigned char *item_bytes;
    unsigned char *type_bytes;
    size_t item_byte_count;
    size_t type_byte_count;
    size_t arena_bytes;
    size_t arena_capacity;
    size_t type_count;
    unsigned int repeat;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "generic_projection_test"),
        CM_HIR_EDITION_2024, test_span(0u, 100u), &crate_id,
        &root_module) == CM_HIR_OK);
    u8_type = add_scalar(&context, CM_HIR_TYPE_INTEGER_KIND,
        (unsigned int)CM_HIR_INT_U8);
    bool_type = add_scalar(&context, CM_HIR_TYPE_BOOL_KIND, 0u);
    wrapper_definition = add_generic_struct(&context, crate_id, root_module,
        "Wrapper", 1u, wrapper_parameters, wrapper_parameter_types);
    pair_definition = add_generic_struct(&context, crate_id, root_module,
        "Pair", 2u, pair_parameters, pair_parameter_types);
    wrapper_u8 = add_named_application(&context, wrapper_definition,
        &u8_type, 1u);
    wrapper_bool = add_named_application(&context, wrapper_definition,
        &bool_type, 1u);

    trait_definition = add_trait(&context, crate_id, root_module,
        "GenericTrait");
    direct_definition = add_trait_associated(&context, crate_id,
        root_module, trait_definition, "Direct");
    pair_associated_definition = add_trait_associated(&context, crate_id,
        root_module, trait_definition, "PairOut");
    impl_definition = add_single_parameter_impl(&context, crate_id,
        root_module, trait_definition, wrapper_definition, 0,
        &impl_parameter_type, &impl_self_template);
    direct_impl_definition = add_impl_associated(&context, crate_id,
        root_module, impl_definition, direct_definition, "Direct",
        impl_parameter_type);
    pair_template_arguments[0] = impl_parameter_type;
    pair_template_arguments[1] = impl_parameter_type;
    pair_target_template = add_named_application(&context, pair_definition,
        pair_template_arguments, 2u);
    pair_impl_definition = add_impl_associated(&context, crate_id,
        root_module, impl_definition, pair_associated_definition, "PairOut",
        pair_target_template);
    direct_u8_projection = add_projection(&context, wrapper_u8,
        trait_definition, direct_definition, NULL, 0u, NULL, 0u);
    direct_bool_projection = add_projection(&context, wrapper_bool,
        trait_definition, direct_definition, NULL, 0u, NULL, 0u);
    pair_u8_projection = add_projection(&context, wrapper_u8,
        trait_definition, pair_associated_definition, NULL, 0u, NULL, 0u);

    match = cm_hir_match_projection(&context, crate_id, direct_u8_projection);
    check_match(match, impl_parameter_type, wrapper_u8, impl_definition,
        direct_impl_definition);
    match = cm_hir_match_projection(&context, crate_id, direct_bool_projection);
    check_match(match, impl_parameter_type, wrapper_bool, impl_definition,
        direct_impl_definition);
    match = cm_hir_match_projection(&context, crate_id, pair_u8_projection);
    check_match(match, pair_target_template, wrapper_u8, impl_definition,
        pair_impl_definition);

    type_count = context.types.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    result = cm_hir_select_projection(&context, crate_id, direct_u8_projection);
    check_selected(result, u8_type, impl_definition,
        direct_impl_definition);
    assert(result.allocated_type_count == 0u);
    assert(context.types.len == type_count);
    assert(cm_arena_bytes_used(&context.storage) == arena_bytes);
    result = cm_hir_select_projection(&context, crate_id, direct_bool_projection);
    check_selected(result, bool_type, impl_definition,
        direct_impl_definition);
    assert(result.allocated_type_count == 0u);
    assert(context.types.len == type_count);
    assert(cm_arena_bytes_used(&context.storage) == arena_bytes);

    result = cm_hir_select_projection(&context, crate_id, pair_u8_projection);
    assert(result.status == CM_HIR_PROJECTION_SELECTED);
    assert(result.target != CM_HIR_TYPE_NONE
        && result.target != pair_target_template);
    assert(cm_hir_def_id_equal(result.impl_definition, impl_definition));
    assert(cm_hir_def_id_equal(result.impl_associated_definition,
        pair_impl_definition));
    assert(result.hir_status == CM_HIR_OK);
    assert(result.allocated_type_count == 1u);
    assert(context.types.len == type_count + 1u);
    selected_type = cm_hir_get_type(&context, result.target);
    assert(selected_type != NULL
        && selected_type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(selected_type->data.named_type.definition,
            pair_definition)
        && selected_type->data.named_type.argument_count == 2u
        && selected_type->data.named_type.arguments != NULL
        && selected_type->data.named_type.arguments[0].kind
            == CM_HIR_GENERIC_ARG_TYPE
        && selected_type->data.named_type.arguments[0].data.type == u8_type
        && selected_type->data.named_type.arguments[1].kind
            == CM_HIR_GENERIC_ARG_TYPE
        && selected_type->data.named_type.arguments[1].data.type == u8_type);

    blanket_trait_definition = add_trait(&context, crate_id, root_module,
        "BlanketTrait");
    blanket_associated_definition = add_trait_associated(&context, crate_id,
        root_module, blanket_trait_definition, "Assoc");
    blanket_impl_definition = add_single_parameter_impl(&context, crate_id,
        root_module, blanket_trait_definition, wrapper_definition, 1,
        &blanket_parameter_type, &blanket_self_template);
    (void)add_impl_associated(&context, crate_id, root_module,
        blanket_impl_definition, blanket_associated_definition, "Assoc",
        blanket_parameter_type);
    blanket_projection = add_projection(&context, u8_type,
        blanket_trait_definition, blanket_associated_definition, NULL, 0u,
        NULL, 0u);
    check_empty_match(cm_hir_match_projection(&context, crate_id, blanket_projection),
        CM_HIR_PROJECTION_DEFERRED_ARGUMENTS);
    check_empty_result(cm_hir_select_projection(&context, crate_id,
        blanket_projection), CM_HIR_PROJECTION_DEFERRED_ARGUMENTS);

    malformed_trait_definition = add_trait(&context, crate_id, root_module,
        "MalformedTrait");
    malformed_associated_definition = add_trait_associated(&context,
        crate_id, root_module, malformed_trait_definition, "Assoc");
    malformed_impl_definition = add_single_parameter_impl(&context,
        crate_id, root_module, malformed_trait_definition,
        wrapper_definition, 0, &malformed_parameter_type,
        &malformed_self_template);
    malformed_impl_item_definition = add_impl_associated(&context, crate_id,
        root_module, malformed_impl_definition,
        malformed_associated_definition, "Assoc",
        wrapper_parameter_types[0]);
    malformed_projection = add_projection(&context, wrapper_u8,
        malformed_trait_definition, malformed_associated_definition, NULL,
        0u, NULL, 0u);
    check_match(cm_hir_match_projection(&context, crate_id, malformed_projection),
        wrapper_parameter_types[0], wrapper_u8, malformed_impl_definition,
        malformed_impl_item_definition);
    result = cm_hir_select_projection(&context, crate_id, malformed_projection);
    check_empty_result(result, CM_HIR_PROJECTION_SUBSTITUTION_FAILURE);
    assert(result.hir_status != CM_HIR_OK);

    ambiguous_trait_definition = add_trait(&context, crate_id, root_module,
        "AmbiguousTrait");
    ambiguous_associated_definition = add_trait_associated(&context,
        crate_id, root_module, ambiguous_trait_definition, "Assoc");
    first_ambiguous_impl = add_single_parameter_impl(&context, crate_id,
        root_module, ambiguous_trait_definition, wrapper_definition, 0,
        &first_ambiguous_parameter_type, &first_ambiguous_self_template);
    (void)add_impl_associated(&context, crate_id, root_module,
        first_ambiguous_impl, ambiguous_associated_definition, "Assoc",
        first_ambiguous_parameter_type);
    second_ambiguous_impl = add_single_parameter_impl(&context, crate_id,
        root_module, ambiguous_trait_definition, wrapper_definition, 0,
        &second_ambiguous_parameter_type, &second_ambiguous_self_template);
    (void)add_impl_associated(&context, crate_id, root_module,
        second_ambiguous_impl, ambiguous_associated_definition, "Assoc",
        second_ambiguous_parameter_type);
    ambiguous_projection = add_projection(&context, wrapper_u8,
        ambiguous_trait_definition, ambiguous_associated_definition, NULL,
        0u, NULL, 0u);
    check_empty_match(cm_hir_match_projection(&context, crate_id,
        ambiguous_projection), CM_HIR_PROJECTION_AMBIGUOUS);
    check_empty_result(cm_hir_select_projection(&context, crate_id,
        ambiguous_projection), CM_HIR_PROJECTION_AMBIGUOUS);

    snapshot = context;
    item_byte_count = context.items.len * context.items.elem_size;
    type_byte_count = context.types.len * context.types.elem_size;
    item_bytes = (unsigned char *)malloc(item_byte_count);
    type_bytes = (unsigned char *)malloc(type_byte_count);
    assert(item_bytes != NULL && type_bytes != NULL);
    memcpy(item_bytes, context.items.data, item_byte_count);
    memcpy(type_bytes, context.types.data, type_byte_count);
    arena_bytes = cm_arena_bytes_used(&context.storage);
    arena_capacity = cm_arena_capacity(&context.storage);
    cm_alloc_fail_after(0u);
    for (repeat = 0u; repeat < 16u; ++repeat) {
        check_match(cm_hir_match_projection(&context, crate_id,
            direct_u8_projection), impl_parameter_type, wrapper_u8,
            impl_definition, direct_impl_definition);
        check_match(cm_hir_match_projection(&context, crate_id, pair_u8_projection),
            pair_target_template, wrapper_u8, impl_definition,
            pair_impl_definition);
        check_empty_match(cm_hir_match_projection(&context, crate_id,
            blanket_projection), CM_HIR_PROJECTION_DEFERRED_ARGUMENTS);
        check_empty_match(cm_hir_match_projection(&context, crate_id,
            ambiguous_projection), CM_HIR_PROJECTION_AMBIGUOUS);
    }
    cm_alloc_fail_never();
    assert(memcmp(&context, &snapshot, sizeof(context)) == 0);
    assert(memcmp(context.items.data, item_bytes, item_byte_count) == 0);
    assert(memcmp(context.types.data, type_bytes, type_byte_count) == 0);
    assert(cm_arena_bytes_used(&context.storage) == arena_bytes);
    assert(cm_arena_capacity(&context.storage) == arena_capacity);
    free(type_bytes);
    free(item_bytes);

    cm_hir_context_destroy(&context);
}

static void test_cross_crate_projection_boundary(void)
{
    CmHirContext context;
    CmHirContext snapshot;
    CmHirCrateId local_crate;
    CmHirCrateId foreign_crate;
    CmHirModuleId local_module;
    CmHirModuleId foreign_module;
    CmHirDefId foreign_wrapper_definition;
    CmHirDefId foreign_trait_definition;
    CmHirDefId foreign_associated_definition;
    CmHirDefId foreign_impl_definition;
    CmHirDefId foreign_impl_associated_definition;
    CmHirDefId local_foreign_self_trait;
    CmHirDefId local_foreign_self_associated;
    CmHirDefId scalar_trait_definition;
    CmHirDefId scalar_associated_definition;
    CmHirDefId scalar_local_impl;
    CmHirDefId scalar_local_associated;
    CmHirDefId scalar_foreign_impl;
    CmHirDefId scalar_second_local_impl;
    CmHirDefId unrelated_trait_definition;
    CmHirDefId unrelated_associated_definition;
    CmHirDefId unrelated_local_impl;
    CmHirDefId unrelated_local_associated;
    CmHirDefId unrelated_foreign_impl;
    CmHirDefId local_wrapper_definition;
    CmHirDefId generic_trait_definition;
    CmHirDefId generic_associated_definition;
    CmHirDefId generic_local_impl;
    CmHirDefId generic_foreign_impl;
    CmHirGenericParamId foreign_wrapper_parameters[1];
    CmHirGenericParamId local_wrapper_parameters[1];
    CmHirTypeId foreign_wrapper_parameter_types[1];
    CmHirTypeId local_wrapper_parameter_types[1];
    CmHirTypeId foreign_impl_parameter_type;
    CmHirTypeId foreign_impl_self;
    CmHirTypeId generic_local_parameter_type;
    CmHirTypeId generic_local_self;
    CmHirTypeId generic_foreign_parameter_type;
    CmHirTypeId generic_foreign_self;
    CmHirTypeId u8_type;
    CmHirTypeId bool_type;
    CmHirTypeId foreign_wrapper_u8;
    CmHirTypeId local_wrapper_u8;
    CmHirTypeId foreign_projection;
    CmHirTypeId foreign_self_projection;
    CmHirTypeId scalar_projection;
    CmHirTypeId unrelated_projection;
    CmHirTypeId generic_projection;
    CmHirProjectionResult result;
    unsigned char *item_bytes;
    unsigned char *type_bytes;
    size_t item_byte_count;
    size_t type_byte_count;
    size_t arena_bytes;
    size_t arena_capacity;
    unsigned int repeat;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "cross_crate_local"),
        CM_HIR_EDITION_2024, test_span(0u, 100u), &local_crate,
        &local_module) == CM_HIR_OK);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "cross_crate_foreign"),
        CM_HIR_EDITION_2024, test_span(0u, 100u), &foreign_crate,
        &foreign_module) == CM_HIR_OK);
    u8_type = add_scalar(&context, CM_HIR_TYPE_INTEGER_KIND,
        (unsigned int)CM_HIR_INT_U8);
    bool_type = add_scalar(&context, CM_HIR_TYPE_BOOL_KIND, 0u);

    foreign_wrapper_definition = add_generic_struct(&context,
        foreign_crate, foreign_module, "ForeignWrapper", 1u,
        foreign_wrapper_parameters, foreign_wrapper_parameter_types);
    foreign_wrapper_u8 = add_named_application(&context,
        foreign_wrapper_definition, &u8_type, 1u);
    foreign_trait_definition = add_trait(&context, foreign_crate,
        foreign_module, "ForeignTrait");
    foreign_associated_definition = add_trait_associated(&context,
        foreign_crate, foreign_module, foreign_trait_definition, "Assoc");
    foreign_impl_definition = add_single_parameter_impl(&context,
        foreign_crate, foreign_module, foreign_trait_definition,
        foreign_wrapper_definition, 0, &foreign_impl_parameter_type,
        &foreign_impl_self);
    foreign_impl_associated_definition = add_impl_associated(&context,
        foreign_crate, foreign_module, foreign_impl_definition,
        foreign_associated_definition, "Assoc", foreign_impl_parameter_type);
    foreign_projection = add_projection(&context, foreign_wrapper_u8,
        foreign_trait_definition, foreign_associated_definition, NULL, 0u,
        NULL, 0u);
    check_empty_match(cm_hir_match_projection(&context, local_crate,
        foreign_projection), CM_HIR_PROJECTION_DEFERRED_CRATE);
    check_empty_result(cm_hir_select_projection(&context, local_crate,
        foreign_projection), CM_HIR_PROJECTION_DEFERRED_CRATE);
    result = cm_hir_select_projection(&context, foreign_crate,
        foreign_projection);
    check_selected(result, u8_type, foreign_impl_definition,
        foreign_impl_associated_definition);
    assert(result.allocated_type_count == 0u);

    local_foreign_self_trait = add_trait(&context, local_crate,
        local_module, "LocalForeignSelfTrait");
    local_foreign_self_associated = add_trait_associated(&context,
        local_crate, local_module, local_foreign_self_trait, "Assoc");
    foreign_self_projection = add_projection(&context, foreign_wrapper_u8,
        local_foreign_self_trait, local_foreign_self_associated, NULL, 0u,
        NULL, 0u);
    check_empty_match(cm_hir_match_projection(&context, local_crate,
        foreign_self_projection), CM_HIR_PROJECTION_DEFERRED_CRATE);

    scalar_trait_definition = add_trait(&context, local_crate,
        local_module, "ScalarConflictTrait");
    scalar_associated_definition = add_trait_associated(&context,
        local_crate, local_module, scalar_trait_definition, "Assoc");
    scalar_local_impl = add_impl(&context, local_crate, local_module,
        scalar_trait_definition, u8_type);
    scalar_local_associated = add_impl_associated(&context, local_crate,
        local_module, scalar_local_impl, scalar_associated_definition,
        "Assoc", bool_type);
    scalar_foreign_impl = add_impl(&context, foreign_crate, foreign_module,
        scalar_trait_definition, u8_type);
    (void)add_impl_associated(&context, foreign_crate, foreign_module,
        scalar_foreign_impl, scalar_associated_definition, "Assoc", u8_type);
    scalar_projection = add_projection(&context, u8_type,
        scalar_trait_definition, scalar_associated_definition, NULL, 0u,
        NULL, 0u);
    check_empty_match(cm_hir_match_projection(&context, local_crate,
        scalar_projection), CM_HIR_PROJECTION_DEFERRED_CRATE);
    check_empty_result(cm_hir_select_projection(&context, local_crate,
        scalar_projection), CM_HIR_PROJECTION_DEFERRED_CRATE);
    scalar_second_local_impl = add_impl(&context, local_crate, local_module,
        scalar_trait_definition, u8_type);
    (void)add_impl_associated(&context, local_crate, local_module,
        scalar_second_local_impl, scalar_associated_definition, "Assoc",
        u8_type);
    check_empty_match(cm_hir_match_projection(&context, local_crate,
        scalar_projection), CM_HIR_PROJECTION_AMBIGUOUS);
    check_empty_result(cm_hir_select_projection(&context, local_crate,
        scalar_projection), CM_HIR_PROJECTION_AMBIGUOUS);

    unrelated_trait_definition = add_trait(&context, local_crate,
        local_module, "UnrelatedForeignTrait");
    unrelated_associated_definition = add_trait_associated(&context,
        local_crate, local_module, unrelated_trait_definition, "Assoc");
    unrelated_local_impl = add_impl(&context, local_crate, local_module,
        unrelated_trait_definition, u8_type);
    unrelated_local_associated = add_impl_associated(&context, local_crate,
        local_module, unrelated_local_impl, unrelated_associated_definition,
        "Assoc", bool_type);
    unrelated_foreign_impl = add_impl(&context, foreign_crate,
        foreign_module, unrelated_trait_definition, bool_type);
    (void)add_impl_associated(&context, foreign_crate, foreign_module,
        unrelated_foreign_impl, unrelated_associated_definition, "Assoc",
        u8_type);
    unrelated_projection = add_projection(&context, u8_type,
        unrelated_trait_definition, unrelated_associated_definition, NULL,
        0u, NULL, 0u);
    result = cm_hir_select_projection(&context, local_crate,
        unrelated_projection);
    check_selected(result, bool_type, unrelated_local_impl,
        unrelated_local_associated);

    local_wrapper_definition = add_generic_struct(&context, local_crate,
        local_module, "LocalWrapper", 1u, local_wrapper_parameters,
        local_wrapper_parameter_types);
    local_wrapper_u8 = add_named_application(&context,
        local_wrapper_definition, &u8_type, 1u);
    generic_trait_definition = add_trait(&context, local_crate,
        local_module, "GenericConflictTrait");
    generic_associated_definition = add_trait_associated(&context,
        local_crate, local_module, generic_trait_definition, "Assoc");
    generic_local_impl = add_single_parameter_impl(&context, local_crate,
        local_module, generic_trait_definition, local_wrapper_definition, 0,
        &generic_local_parameter_type, &generic_local_self);
    (void)add_impl_associated(&context, local_crate, local_module,
        generic_local_impl, generic_associated_definition, "Assoc",
        generic_local_parameter_type);
    generic_foreign_impl = add_single_parameter_impl(&context,
        foreign_crate, foreign_module, generic_trait_definition,
        local_wrapper_definition, 0, &generic_foreign_parameter_type,
        &generic_foreign_self);
    (void)add_impl_associated(&context, foreign_crate, foreign_module,
        generic_foreign_impl, generic_associated_definition, "Assoc",
        generic_foreign_parameter_type);
    generic_projection = add_projection(&context, local_wrapper_u8,
        generic_trait_definition, generic_associated_definition, NULL, 0u,
        NULL, 0u);
    check_empty_match(cm_hir_match_projection(&context, local_crate,
        generic_projection), CM_HIR_PROJECTION_DEFERRED_CRATE);
    check_empty_result(cm_hir_select_projection(&context, local_crate,
        generic_projection), CM_HIR_PROJECTION_DEFERRED_CRATE);

    check_empty_match(cm_hir_match_projection(&context, CM_HIR_CRATE_NONE,
        unrelated_projection), CM_HIR_PROJECTION_INVALID_ASSOCIATION);
    check_empty_result(cm_hir_select_projection(&context,
        CM_HIR_CRATE_NONE, unrelated_projection),
        CM_HIR_PROJECTION_INVALID_ASSOCIATION);

    snapshot = context;
    item_byte_count = context.items.len * context.items.elem_size;
    type_byte_count = context.types.len * context.types.elem_size;
    item_bytes = (unsigned char *)malloc(item_byte_count);
    type_bytes = (unsigned char *)malloc(type_byte_count);
    assert(item_bytes != NULL && type_bytes != NULL);
    memcpy(item_bytes, context.items.data, item_byte_count);
    memcpy(type_bytes, context.types.data, type_byte_count);
    arena_bytes = cm_arena_bytes_used(&context.storage);
    arena_capacity = cm_arena_capacity(&context.storage);
    cm_alloc_fail_after(0u);
    for (repeat = 0u; repeat < 16u; ++repeat) {
        check_empty_match(cm_hir_match_projection(&context, local_crate,
            foreign_projection), CM_HIR_PROJECTION_DEFERRED_CRATE);
        check_match(cm_hir_match_projection(&context, foreign_crate,
            foreign_projection), foreign_impl_parameter_type,
            foreign_wrapper_u8, foreign_impl_definition,
            foreign_impl_associated_definition);
        check_empty_match(cm_hir_match_projection(&context, local_crate,
            scalar_projection), CM_HIR_PROJECTION_AMBIGUOUS);
        check_match(cm_hir_match_projection(&context, local_crate,
            unrelated_projection), bool_type, u8_type, unrelated_local_impl,
            unrelated_local_associated);
        check_empty_match(cm_hir_match_projection(&context, local_crate,
            generic_projection), CM_HIR_PROJECTION_DEFERRED_CRATE);
    }
    cm_alloc_fail_never();
    assert(memcmp(&context, &snapshot, sizeof(context)) == 0);
    assert(memcmp(context.items.data, item_bytes, item_byte_count) == 0);
    assert(memcmp(context.types.data, type_bytes, type_byte_count) == 0);
    assert(cm_arena_bytes_used(&context.storage) == arena_bytes);
    assert(cm_arena_capacity(&context.storage) == arena_capacity);
    free(type_bytes);
    free(item_bytes);

    (void)scalar_local_associated;
    (void)generic_local_self;
    (void)generic_foreign_self;
    cm_hir_context_destroy(&context);
}

static void test_symbolic_self_projection(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    CmHirDefId trait_definition;
    CmHirDefId associated_definition;
    CmHirDefId impl_definition;
    CmHirType type;
    CmHirTypeId self_type;
    CmHirTypeId u8_type;
    CmHirTypeId projection_type;
    size_t type_count;
    size_t arena_bytes;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "symbolic_self_projection"),
        CM_HIR_EDITION_2024, test_span(0u, 100u), &crate_id,
        &root_module) == CM_HIR_OK);
    trait_definition = add_trait(&context, crate_id, root_module,
        "SymbolicTrait");
    associated_definition = add_trait_associated(&context, crate_id,
        root_module, trait_definition, "Assoc");

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_SELF_KIND;
    type.span = test_span(10u, 14u);
    type.data.self_type.owner = trait_definition;
    assert(cm_hir_add_type(&context, &type, &self_type) == CM_HIR_OK);
    projection_type = add_projection(&context, self_type,
        trait_definition, associated_definition, NULL, 0u, NULL, 0u);

    type_count = context.types.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    check_empty_match(cm_hir_match_projection(&context, crate_id,
        projection_type), CM_HIR_PROJECTION_DEFERRED_SELF);
    check_empty_result(cm_hir_select_projection(&context, crate_id,
        projection_type), CM_HIR_PROJECTION_DEFERRED_SELF);
    assert(context.types.len == type_count);
    assert(cm_arena_bytes_used(&context.storage) == arena_bytes);

    u8_type = add_scalar(&context, CM_HIR_TYPE_INTEGER_KIND,
        (unsigned int)CM_HIR_INT_U8);
    impl_definition = add_impl(&context, crate_id, root_module,
        trait_definition, u8_type);
    (void)add_impl_associated(&context, crate_id, root_module,
        impl_definition, associated_definition, "Assoc", u8_type);
    check_empty_match(cm_hir_match_projection(&context, crate_id,
        projection_type), CM_HIR_PROJECTION_DEFERRED_SELF);
    check_empty_result(cm_hir_select_projection(&context, crate_id,
        projection_type), CM_HIR_PROJECTION_DEFERRED_SELF);

    cm_hir_context_destroy(&context);
}

int main(void)
{
    CmHirContext context;
    CmHirContext snapshot;
    CmHirCrateId crate_id;
    CmHirCrateId foreign_crate_id;
    CmHirModuleId root_module;
    CmHirModuleId foreign_root_module;
    CmHirDefId trait_definition;
    CmHirDefId associated_definition;
    CmHirDefId missing_definition;
    CmHirDefId struct_definition;
    CmHirDefId other_struct_definition;
    CmHirDefId foreign_struct_definition;
    CmHirDefId union_definition;
    CmHirDefId struct_alias_definition;
    CmHirDefId generic_child_trait_definition;
    CmHirDefId generic_child_associated_definition;
    CmHirDefId u8_impl;
    CmHirDefId u8_impl_item;
    CmHirDefId bool_impl;
    CmHirDefId bool_impl_item;
    CmHirDefId f32_impl;
    CmHirDefId f32_impl_item;
    CmHirDefId struct_impl;
    CmHirDefId struct_impl_item;
    CmHirDefId first_u32_impl;
    CmHirDefId second_u32_impl;
    CmHirDefId union_impl;
    CmHirDefId generic_child_impl;
    CmHirTypeId u8_type;
    CmHirTypeId u16_type;
    CmHirTypeId u32_type;
    CmHirTypeId bool_type;
    CmHirTypeId char_type;
    CmHirTypeId f32_type;
    CmHirTypeId f64_type;
    CmHirTypeId struct_type;
    CmHirTypeId other_struct_type;
    CmHirTypeId foreign_struct_type;
    CmHirTypeId union_type;
    CmHirTypeId normalized_struct_type;
    CmHirTypeId tuple_type;
    CmHirTypeId u8_projection;
    CmHirTypeId u16_projection;
    CmHirTypeId u32_projection;
    CmHirTypeId bool_projection;
    CmHirTypeId f32_projection;
    CmHirTypeId f64_projection;
    CmHirTypeId struct_projection;
    CmHirTypeId other_struct_projection;
    CmHirTypeId foreign_struct_projection;
    CmHirTypeId missing_projection;
    CmHirTypeId argument_projection;
    CmHirTypeId associated_argument_projection;
    CmHirTypeId tuple_projection;
    CmHirTypeId union_projection;
    CmHirTypeId generic_child_projection;
    CmHirType tuple;
    CmHirType *malformed_projection_type;
    CmHirGenericArg *malformed_argument;
    CmHirTypeId tuple_elements[2];
    CmHirGenericArg argument;
    CmHirProjectionResult result;
    CmHirProjectionImplTarget impl_target;
    unsigned char *item_bytes;
    unsigned char *type_bytes;
    size_t item_byte_count;
    size_t type_byte_count;
    size_t arena_bytes;
    size_t arena_capacity;
    unsigned int repeat;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "projection_test"), CM_HIR_EDITION_2024,
        test_span(0u, 100u), &crate_id, &root_module) == CM_HIR_OK);
    u8_type = add_scalar(&context, CM_HIR_TYPE_INTEGER_KIND,
        (unsigned int)CM_HIR_INT_U8);
    u16_type = add_scalar(&context, CM_HIR_TYPE_INTEGER_KIND,
        (unsigned int)CM_HIR_INT_U16);
    u32_type = add_scalar(&context, CM_HIR_TYPE_INTEGER_KIND,
        (unsigned int)CM_HIR_INT_U32);
    bool_type = add_scalar(&context, CM_HIR_TYPE_BOOL_KIND, 0u);
    char_type = add_scalar(&context, CM_HIR_TYPE_CHAR_KIND, 0u);
    f32_type = add_scalar(&context, CM_HIR_TYPE_FLOAT_KIND,
        (unsigned int)CM_HIR_FLOAT_F32);
    f64_type = add_scalar(&context, CM_HIR_TYPE_FLOAT_KIND,
        (unsigned int)CM_HIR_FLOAT_F64);
    struct_type = add_adt(&context, crate_id, root_module, "Local",
        &struct_definition);
    other_struct_type = add_adt(&context, crate_id, root_module, "Other",
        &other_struct_definition);
    union_type = add_union(&context, crate_id, root_module, "Union",
        &union_definition);
    normalized_struct_type = add_and_normalize_alias(&context, crate_id,
        root_module, "LocalAlias", struct_type, &struct_alias_definition);
    assert(cm_hir_get_type(&context, normalized_struct_type) != NULL
        && cm_hir_get_type(&context, normalized_struct_type)->kind
            == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(cm_hir_get_type(&context,
                normalized_struct_type)->data.named_type.definition,
            struct_definition));
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "foreign"), CM_HIR_EDITION_2024,
        test_span(0u, 100u), &foreign_crate_id,
        &foreign_root_module) == CM_HIR_OK);
    foreign_struct_type = add_adt(&context, foreign_crate_id,
        foreign_root_module, "Foreign", &foreign_struct_definition);

    trait_definition = add_trait(&context, crate_id, root_module, "Trait");
    associated_definition = add_trait_associated(&context, crate_id,
        root_module, trait_definition, "Assoc");
    missing_definition = add_trait_associated(&context, crate_id,
        root_module, trait_definition, "Missing");
    generic_child_trait_definition = add_trait(&context, crate_id,
        root_module, "GenericChildTrait");
    generic_child_associated_definition = add_generic_trait_associated(
        &context, crate_id, root_module, generic_child_trait_definition,
        "Item");

    u8_projection = add_projection(&context, u8_type, trait_definition,
        associated_definition, NULL, 0u, NULL, 0u);
    u16_projection = add_projection(&context, u16_type, trait_definition,
        associated_definition, NULL, 0u, NULL, 0u);
    u32_projection = add_projection(&context, u32_type, trait_definition,
        associated_definition, NULL, 0u, NULL, 0u);
    bool_projection = add_projection(&context, bool_type, trait_definition,
        associated_definition, NULL, 0u, NULL, 0u);
    f32_projection = add_projection(&context, f32_type, trait_definition,
        associated_definition, NULL, 0u, NULL, 0u);
    f64_projection = add_projection(&context, f64_type, trait_definition,
        associated_definition, NULL, 0u, NULL, 0u);
    struct_projection = add_projection(&context, normalized_struct_type,
        trait_definition, associated_definition, NULL, 0u, NULL, 0u);
    other_struct_projection = add_projection(&context, other_struct_type,
        trait_definition, associated_definition, NULL, 0u, NULL, 0u);
    foreign_struct_projection = add_projection(&context,
        foreign_struct_type, trait_definition, associated_definition,
        NULL, 0u, NULL, 0u);
    missing_projection = add_projection(&context, u8_type, trait_definition,
        missing_definition, NULL, 0u, NULL, 0u);
    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    argument.data.type = u8_type;
    argument_projection = add_projection(&context, u8_type,
        trait_definition, associated_definition, NULL, 0u, NULL, 0u);
    malformed_projection_type = (CmHirType *)cm_vec_at(&context.types,
        (size_t)argument_projection - 1u);
    malformed_argument = (CmHirGenericArg *)cm_arena_alloc(
        &context.storage, sizeof(*malformed_argument), 16u);
    *malformed_argument = argument;
    assert(malformed_projection_type != NULL);
    malformed_projection_type->data.projection_type.trait_type.arguments =
        malformed_argument;
    malformed_projection_type->data.projection_type.trait_type.argument_count =
        1u;
    associated_argument_projection = add_projection(&context, u8_type,
        trait_definition, associated_definition, NULL, 0u, NULL, 0u);
    malformed_projection_type = (CmHirType *)cm_vec_at(&context.types,
        (size_t)associated_argument_projection - 1u);
    malformed_argument = (CmHirGenericArg *)cm_arena_alloc(
        &context.storage, sizeof(*malformed_argument), 16u);
    *malformed_argument = argument;
    assert(malformed_projection_type != NULL);
    malformed_projection_type->data.projection_type.associated_type.arguments =
        malformed_argument;
    malformed_projection_type->data.projection_type.associated_type
        .argument_count = 1u;
    memset(&tuple, 0, sizeof(tuple));
    tuple.kind = CM_HIR_TYPE_TUPLE_KIND;
    tuple.span = test_span(1u, 2u);
    tuple_elements[0] = u8_type;
    tuple_elements[1] = u16_type;
    tuple.data.tuple_type.elements = tuple_elements;
    tuple.data.tuple_type.element_count = 2u;
    assert(cm_hir_add_type(&context, &tuple, &tuple_type) == CM_HIR_OK);
    tuple_projection = add_projection(&context, tuple_type,
        trait_definition, associated_definition, NULL, 0u, NULL, 0u);
    union_projection = add_projection(&context, union_type, trait_definition,
        associated_definition, NULL, 0u, NULL, 0u);
    generic_child_projection = add_projection(&context, char_type,
        generic_child_trait_definition, generic_child_associated_definition,
        NULL, 0u, &argument, 1u);

    u8_impl = add_impl(&context, crate_id, root_module, trait_definition,
        u8_type);
    u8_impl_item = add_impl_associated(&context, crate_id, root_module,
        u8_impl, associated_definition, "Assoc", u16_type);
    bool_impl = add_impl(&context, crate_id, root_module, trait_definition,
        bool_type);
    bool_impl_item = add_impl_associated(&context, crate_id, root_module,
        bool_impl, associated_definition, "Assoc", u32_type);
    f32_impl = add_impl(&context, crate_id, root_module, trait_definition,
        f32_type);
    f32_impl_item = add_impl_associated(&context, crate_id, root_module,
        f32_impl, associated_definition, "Assoc", u8_projection);
    struct_impl = add_impl(&context, crate_id, root_module,
        trait_definition, struct_type);
    struct_impl_item = add_impl_associated(&context, crate_id, root_module,
        struct_impl, associated_definition, "Assoc", bool_type);
    first_u32_impl = add_impl(&context, crate_id, root_module,
        trait_definition, u32_type);
    (void)add_impl_associated(&context, crate_id, root_module,
        first_u32_impl, associated_definition, "Assoc", u8_type);
    second_u32_impl = add_impl(&context, crate_id, root_module,
        trait_definition, u32_type);
    (void)add_impl_associated(&context, crate_id, root_module,
        second_u32_impl, associated_definition, "Assoc", u16_type);
    union_impl = add_impl(&context, crate_id, root_module, trait_definition,
        union_type);
    (void)add_impl_associated(&context, crate_id, root_module, union_impl,
        associated_definition, "Assoc", u8_type);
    generic_child_impl = add_impl(&context, crate_id, root_module,
        generic_child_trait_definition, char_type);
    (void)add_generic_impl_associated(&context, crate_id, root_module,
        generic_child_impl, generic_child_associated_definition, "Item");

    impl_target = cm_hir_projection_impl_target(&context, crate_id,
        u8_impl, trait_definition, associated_definition);
    assert(impl_target.status == CM_HIR_PROJECTION_SELECTED);
    assert(impl_target.target_template == u16_type);
    assert(cm_hir_def_id_equal(impl_target.impl_associated_definition,
        u8_impl_item));
    impl_target = cm_hir_projection_impl_target(&context, crate_id,
        u8_impl, trait_definition, missing_definition);
    assert(impl_target.status == CM_HIR_PROJECTION_INVALID_ASSOCIATION);
    assert(impl_target.target_template == CM_HIR_TYPE_NONE);
    assert(cm_hir_def_id_is_none(
        impl_target.impl_associated_definition));
    impl_target = cm_hir_projection_impl_target(&context, crate_id,
        generic_child_impl, generic_child_trait_definition,
        generic_child_associated_definition);
    assert(impl_target.status == CM_HIR_PROJECTION_DEFERRED_ARGUMENTS);
    assert(impl_target.target_template == CM_HIR_TYPE_NONE);
    assert(cm_hir_def_id_is_none(
        impl_target.impl_associated_definition));
    impl_target = cm_hir_projection_impl_target(&context, crate_id,
        u8_impl, generic_child_trait_definition,
        generic_child_associated_definition);
    assert(impl_target.status == CM_HIR_PROJECTION_INVALID_ASSOCIATION);

    check_selected(cm_hir_select_projection(&context, crate_id, u8_projection),
        u16_type, u8_impl, u8_impl_item);
    check_selected(cm_hir_select_projection(&context, crate_id, bool_projection),
        u32_type, bool_impl, bool_impl_item);
    check_selected(cm_hir_select_projection(&context, crate_id, f32_projection),
        u8_projection, f32_impl, f32_impl_item);
    check_selected(cm_hir_select_projection(&context, crate_id, struct_projection),
        bool_type, struct_impl, struct_impl_item);
    check_empty_result(cm_hir_select_projection(&context, crate_id, u16_projection),
        CM_HIR_PROJECTION_NO_IMPL);
    check_empty_result(cm_hir_select_projection(&context, crate_id, f64_projection),
        CM_HIR_PROJECTION_NO_IMPL);
    check_empty_result(cm_hir_select_projection(&context, crate_id,
        other_struct_projection), CM_HIR_PROJECTION_NO_IMPL);
    check_empty_result(cm_hir_select_projection(&context, crate_id, u32_projection),
        CM_HIR_PROJECTION_AMBIGUOUS);
    check_empty_result(cm_hir_select_projection(&context, crate_id,
        argument_projection), CM_HIR_PROJECTION_DEFERRED_ARGUMENTS);
    check_empty_result(cm_hir_select_projection(&context, crate_id,
        associated_argument_projection),
        CM_HIR_PROJECTION_DEFERRED_ARGUMENTS);
    check_empty_result(cm_hir_select_projection(&context, crate_id, tuple_projection),
        CM_HIR_PROJECTION_DEFERRED_SELF);
    check_empty_result(cm_hir_select_projection(&context, crate_id, union_projection),
        CM_HIR_PROJECTION_DEFERRED_SELF);
    check_empty_result(cm_hir_select_projection(&context, crate_id,
        generic_child_projection), CM_HIR_PROJECTION_DEFERRED_ARGUMENTS);
    check_empty_result(cm_hir_select_projection(&context, crate_id,
        foreign_struct_projection), CM_HIR_PROJECTION_DEFERRED_CRATE);
    check_empty_result(cm_hir_select_projection(&context, crate_id,
        missing_projection), CM_HIR_PROJECTION_INVALID_ASSOCIATION);
    check_empty_result(cm_hir_select_projection(&context, crate_id, u8_type),
        CM_HIR_PROJECTION_INVALID_ASSOCIATION);
    check_empty_result(cm_hir_select_projection(NULL, crate_id, u8_projection),
        CM_HIR_PROJECTION_INVALID_ASSOCIATION);
    check_empty_result(cm_hir_select_projection(&context, crate_id, CM_HIR_TYPE_NONE),
        CM_HIR_PROJECTION_INVALID_ASSOCIATION);

    assert(strcmp(cm_hir_projection_status_name(
        CM_HIR_PROJECTION_SELECTED), "selected") == 0);
    assert(strcmp(cm_hir_projection_status_name(
        CM_HIR_PROJECTION_DEFERRED_ARGUMENTS), "deferred arguments") == 0);
    assert(strcmp(cm_hir_projection_status_name(
        CM_HIR_PROJECTION_DEFERRED_SELF), "deferred self") == 0);
    assert(strcmp(cm_hir_projection_status_name(
        CM_HIR_PROJECTION_DEFERRED_CRATE), "deferred crate") == 0);
    assert(strcmp(cm_hir_projection_status_name(CM_HIR_PROJECTION_NO_IMPL),
        "no impl") == 0);
    assert(strcmp(cm_hir_projection_status_name(CM_HIR_PROJECTION_AMBIGUOUS),
        "ambiguous") == 0);
    assert(strcmp(cm_hir_projection_status_name(
        CM_HIR_PROJECTION_INVALID_ASSOCIATION),
        "invalid association") == 0);
    assert(strcmp(cm_hir_projection_status_name(
        CM_HIR_PROJECTION_SUBSTITUTION_FAILURE),
        "substitution failure") == 0);
    assert(strcmp(cm_hir_projection_status_name(
        (CmHirProjectionStatus)99), "unknown projection status") == 0);

    snapshot = context;
    item_byte_count = context.items.len * context.items.elem_size;
    type_byte_count = context.types.len * context.types.elem_size;
    item_bytes = (unsigned char *)malloc(item_byte_count);
    type_bytes = (unsigned char *)malloc(type_byte_count);
    assert(item_bytes != NULL && type_bytes != NULL);
    memcpy(item_bytes, context.items.data, item_byte_count);
    memcpy(type_bytes, context.types.data, type_byte_count);
    arena_bytes = cm_arena_bytes_used(&context.storage);
    arena_capacity = cm_arena_capacity(&context.storage);
    cm_alloc_fail_after(0u);
    for (repeat = 0u; repeat < 16u; ++repeat) {
        result = cm_hir_select_projection(&context, crate_id, u8_projection);
        check_selected(result, u16_type, u8_impl, u8_impl_item);
        result = cm_hir_select_projection(&context, crate_id, u32_projection);
        check_empty_result(result, CM_HIR_PROJECTION_AMBIGUOUS);
    }
    cm_alloc_fail_never();
    assert(memcmp(&context, &snapshot, sizeof(context)) == 0);
    assert(memcmp(context.items.data, item_bytes, item_byte_count) == 0);
    assert(memcmp(context.types.data, type_bytes, type_byte_count) == 0);
    assert(cm_arena_bytes_used(&context.storage) == arena_bytes);
    assert(cm_arena_capacity(&context.storage) == arena_capacity);
    free(type_bytes);
    free(item_bytes);

    cm_hir_context_destroy(&context);
    test_generic_projection();
    test_cross_crate_projection_boundary();
    test_symbolic_self_projection();
    return 0;
}
