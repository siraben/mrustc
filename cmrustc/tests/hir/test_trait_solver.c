#include "cm/hir/trait_solver.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct TestFixture {
    CmHirContext hir;
    CmHirCrateId crate_id;
    CmHirModuleId root;
    CmHirDefId exact_trait;
    CmHirDefId empty_trait;
    CmHirDefId generic_trait;
    CmHirDefId ambiguous_trait;
    CmHirDefId auto_trait;
    CmHirDefId empty_auto_trait;
    CmHirDefId lifetime_trait;
    CmHirDefId const_trait;
    CmHirDefId unary_generic_trait;
    CmHirDefId repeated_generic_trait;
    CmHirDefId nested_generic_trait;
    CmHirDefId unused_generic_trait;
    CmHirDefId overlap_generic_trait;
    CmHirDefId blocked_generic_trait;
    CmHirGenericParamId lifetime_parameter;
    CmHirGenericParamId const_parameter;
    CmHirDefId exact_impl;
    CmHirTypeId u8_hir;
    CmHirTypeId u16_hir;
    CmHirTypeId bool_hir;
} TestFixture;

static CmSpan test_span(uint32_t start, uint32_t end)
{
    CmSpan span;

    span.source = 1u;
    span.start = start;
    span.end = end;
    return span;
}

static void init_item(CmHirItem *item, CmHirItemKind kind,
    CmHirDefId definition, CmHirModuleId module, CmInternId name)
{
    memset(item, 0, sizeof(*item));
    item->kind = kind;
    item->definition = definition;
    item->owner_module = module;
    item->parent_definition = cm_hir_def_id_none();
    item->name = name;
    item->visibility.kind = CM_HIR_VIS_PRIVATE;
    item->visibility.restriction = cm_hir_def_id_none();
    item->span = test_span(1u, 2u);
}

static CmHirTypeId add_scalar(CmHirContext *hir, CmHirTypeKind kind,
    CmHirIntType integer_kind)
{
    CmHirType type;
    CmHirTypeId id;

    memset(&type, 0, sizeof(type));
    type.kind = kind;
    type.span = test_span(1u, 2u);
    if (kind == CM_HIR_TYPE_INTEGER_KIND) {
        type.data.integer_type.kind = integer_kind;
    }
    assert(cm_hir_add_type(hir, &type, &id) == CM_HIR_OK);
    return id;
}

static CmHirDefId add_trait(CmHirContext *hir, CmHirCrateId crate_id,
    CmHirModuleId module, const char *name, int is_auto)
{
    CmHirDefId definition;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(hir, crate_id,
        CM_HIR_ITEM_TRAIT, test_span(1u, 2u), &definition) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_TRAIT, definition, module,
        cm_hir_intern(hir, name));
    item.data.trait_item.safety = CM_HIR_SAFE;
    item.data.trait_item.is_auto = is_auto;
    assert(cm_hir_add_item(hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_type_trait(CmHirContext *hir,
    CmHirCrateId crate_id, CmHirModuleId module, const char *name,
    uint32_t parameter_count)
{
    CmHirDefId definition;
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_start;
    CmHirItem item;
    CmHirItemId item_id;
    uint32_t index;

    assert(parameter_count != 0u);
    assert(cm_hir_reserve_item_definition_as(hir, crate_id,
        CM_HIR_ITEM_TRAIT, test_span(1u, 2u), &definition) == CM_HIR_OK);
    parameter_start = CM_HIR_GENERIC_PARAM_NONE;
    for (index = 0u; index < parameter_count; ++index) {
        CmHirGenericParamId parameter_id;

        memset(&parameter, 0, sizeof(parameter));
        parameter.kind = CM_HIR_GENERIC_TYPE;
        parameter.owner = definition;
        parameter.index = index;
        parameter.name = cm_hir_intern(hir, index == 0u ? "T" : "U");
        parameter.span = test_span(1u, 2u);
        assert(cm_hir_add_generic_param(hir, &parameter, &parameter_id)
            == CM_HIR_OK);
        if (index == 0u) parameter_start = parameter_id;
    }
    init_item(&item, CM_HIR_ITEM_TRAIT, definition, module,
        cm_hir_intern(hir, name));
    item.generic_parameter_start = parameter_start;
    item.generic_parameter_count = parameter_count;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_lifetime_trait(CmHirContext *hir,
    CmHirCrateId crate_id, CmHirModuleId module,
    CmHirGenericParamId *out_parameter)
{
    CmHirDefId definition;
    CmHirGenericParam parameter;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(hir, crate_id,
        CM_HIR_ITEM_TRAIT, test_span(1u, 2u), &definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_LIFETIME;
    parameter.owner = definition;
    parameter.name = cm_hir_intern(hir, "a");
    parameter.span = test_span(1u, 2u);
    assert(cm_hir_add_generic_param(hir, &parameter, out_parameter)
        == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_TRAIT, definition, module,
        cm_hir_intern(hir, "Lifetime"));
    item.generic_parameter_start = *out_parameter;
    item.generic_parameter_count = 1u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_const_trait(CmHirContext *hir,
    CmHirCrateId crate_id, CmHirModuleId module, CmHirTypeId const_type,
    CmHirGenericParamId *out_parameter)
{
    CmHirDefId definition;
    CmHirGenericParam parameter;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(hir, crate_id,
        CM_HIR_ITEM_TRAIT, test_span(1u, 2u), &definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_CONST;
    parameter.owner = definition;
    parameter.name = cm_hir_intern(hir, "N");
    parameter.span = test_span(1u, 2u);
    parameter.declared_type = const_type;
    assert(cm_hir_add_generic_param(hir, &parameter, out_parameter)
        == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_TRAIT, definition, module,
        cm_hir_intern(hir, "Const"));
    item.generic_parameter_start = *out_parameter;
    item.generic_parameter_count = 1u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_impl(CmHirContext *hir, CmHirCrateId crate_id,
    CmHirModuleId module, CmHirDefId trait_definition,
    CmHirTypeId self_type, int is_negative)
{
    CmHirDefId definition;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(hir, crate_id,
        CM_HIR_ITEM_IMPL, test_span(1u, 2u), &definition) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_IMPL, definition, module,
        CM_INTERN_ID_NONE);
    item.data.impl_item.self_type = self_type;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = trait_definition;
    item.data.impl_item.safety = CM_HIR_SAFE;
    item.data.impl_item.is_negative = is_negative;
    assert(cm_hir_add_item(hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirItem *find_mutable_item(TestFixture *fixture,
    CmHirDefId definition)
{
    size_t index;

    for (index = 0u; index < fixture->hir.items.len; ++index) {
        CmHirItem *item;

        item = (CmHirItem *)cm_vec_at(&fixture->hir.items, index);
        if (item != NULL
            && cm_hir_def_id_equal(item->definition, definition)) {
            return item;
        }
    }
    return NULL;
}

static CmHirDefId add_constrained_bool_impl(TestFixture *fixture,
    CmHirDefId trait_definition, int with_predicate,
    int with_outlives)
{
    CmHirDefId definition;
    CmHirTraitPredicate predicate;
    CmHirOutlivesPredicate outlives;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_IMPL, test_span(1u, 2u),
        &definition) == CM_HIR_OK);
    memset(&predicate, 0, sizeof(predicate));
    predicate.subject = fixture->bool_hir;
    predicate.trait_type.definition = fixture->exact_trait;
    predicate.span = test_span(1u, 2u);
    predicate.modifier = CM_HIR_PREDICATE_REQUIRED;
    memset(&outlives, 0, sizeof(outlives));
    outlives.subject_kind = CM_HIR_OUTLIVES_TYPE;
    outlives.subject.type = fixture->bool_hir;
    outlives.bound.kind = CM_HIR_REGION_STATIC;
    outlives.span = test_span(1u, 2u);
    init_item(&item, CM_HIR_ITEM_IMPL, definition, fixture->root,
        CM_INTERN_ID_NONE);
    item.data.impl_item.self_type = fixture->bool_hir;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = trait_definition;
    item.data.impl_item.safety = CM_HIR_SAFE;
    if (with_predicate) {
        item.predicates = &predicate;
        item.predicate_count = 1u;
    }
    if (with_outlives) {
        item.outlives_predicates = &outlives;
        item.outlives_predicate_count = 1u;
    }
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_impl_with_type_argument(CmHirContext *hir,
    CmHirCrateId crate_id, CmHirModuleId module,
    CmHirDefId trait_definition, CmHirTypeId self_type,
    CmHirTypeId trait_argument, int is_negative)
{
    CmHirGenericArg argument;
    CmHirDefId definition;
    CmHirItem item;
    CmHirItemId item_id;

    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    argument.data.type = trait_argument;
    assert(cm_hir_reserve_item_definition_as(hir, crate_id,
        CM_HIR_ITEM_IMPL, test_span(1u, 2u), &definition) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_IMPL, definition, module,
        CM_INTERN_ID_NONE);
    item.data.impl_item.self_type = self_type;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = trait_definition;
    item.data.impl_item.trait_type.arguments = &argument;
    item.data.impl_item.trait_type.argument_count = 1u;
    item.data.impl_item.safety = CM_HIR_SAFE;
    item.data.impl_item.is_negative = is_negative;
    assert(cm_hir_add_item(hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_blanket_impl(CmHirContext *hir,
    CmHirCrateId crate_id, CmHirModuleId module,
    CmHirDefId trait_definition, int is_negative)
{
    CmHirDefId definition;
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_id;
    CmHirType parameter_type;
    CmHirTypeId parameter_type_id;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(hir, crate_id,
        CM_HIR_ITEM_IMPL, test_span(1u, 2u), &definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = definition;
    parameter.name = cm_hir_intern(hir, "T");
    parameter.span = test_span(1u, 2u);
    assert(cm_hir_add_generic_param(hir, &parameter, &parameter_id)
        == CM_HIR_OK);
    memset(&parameter_type, 0, sizeof(parameter_type));
    parameter_type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    parameter_type.span = test_span(1u, 2u);
    parameter_type.data.parameter_type.parameter = parameter_id;
    assert(cm_hir_add_type(hir, &parameter_type, &parameter_type_id)
        == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_IMPL, definition, module,
        CM_INTERN_ID_NONE);
    item.generic_parameter_start = parameter_id;
    item.generic_parameter_count = 1u;
    item.data.impl_item.self_type = parameter_type_id;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = trait_definition;
    item.data.impl_item.safety = CM_HIR_SAFE;
    item.data.impl_item.is_negative = is_negative;
    assert(cm_hir_add_item(hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_associated_declaration(CmHirContext *hir,
    CmHirCrateId crate_id, CmHirModuleId module,
    CmHirDefId trait_definition, const char *name)
{
    CmHirDefId definition;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(hir, crate_id,
        CM_HIR_ITEM_TYPE_ALIAS, test_span(1u, 2u), &definition)
        == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_TYPE_ALIAS, definition, module,
        cm_hir_intern(hir, name));
    item.parent_definition = trait_definition;
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    item.data.type_alias_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_impl_associated_type(CmHirContext *hir,
    CmHirCrateId crate_id, CmHirModuleId module,
    CmHirDefId impl_definition, CmHirDefId trait_item_definition,
    const char *name, CmHirTypeId target, int is_specializable)
{
    CmHirDefId definition;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(hir, crate_id,
        CM_HIR_ITEM_TYPE_ALIAS, test_span(1u, 2u), &definition)
        == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_TYPE_ALIAS, definition, module,
        cm_hir_intern(hir, name));
    item.parent_definition = impl_definition;
    item.is_specializable = is_specializable;
    item.data.type_alias_item.target = target;
    item.data.type_alias_item.trait_item_definition = trait_item_definition;
    assert(cm_hir_add_item(hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

typedef enum TestGenericSelfShape {
    TEST_GENERIC_SELF_TUPLE = 0,
    TEST_GENERIC_SELF_NESTED_REFERENCE,
    TEST_GENERIC_SELF_BOOL
} TestGenericSelfShape;

typedef enum TestGenericArgShape {
    TEST_GENERIC_ARGS_NONE = 0,
    TEST_GENERIC_ARGS_PARAMETER,
    TEST_GENERIC_ARGS_REPEATED_PARAMETER,
    TEST_GENERIC_ARGS_NESTED_PARAMETER
} TestGenericArgShape;

static CmHirTypeId add_hir_tuple(CmHirContext *hir,
    const CmHirTypeId *elements, uint32_t element_count)
{
    CmHirType type;
    CmHirTypeId result;

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_TUPLE_KIND;
    type.span = test_span(1u, 2u);
    type.data.tuple_type.elements = (CmHirTypeId *)elements;
    type.data.tuple_type.element_count = element_count;
    assert(cm_hir_add_type(hir, &type, &result) == CM_HIR_OK);
    return result;
}

static CmHirDefId add_type_only_generic_impl(TestFixture *fixture,
    CmHirDefId trait_definition, TestGenericSelfShape self_shape,
    TestGenericArgShape argument_shape, uint32_t impl_parameter_count)
{
    CmHirDefId definition;
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_id;
    CmHirType parameter_type;
    CmHirTypeId parameter_type_id;
    CmHirTypeId tuple_type;
    CmHirTypeId tuple_elements[1];
    CmHirType reference_type;
    CmHirTypeId self_type;
    CmHirGenericArg trait_arguments[2];
    CmHirItem item;
    CmHirItemId item_id;
    uint32_t trait_argument_count;
    uint32_t index;

    assert(impl_parameter_count != 0u);
    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_IMPL, test_span(1u, 2u),
        &definition) == CM_HIR_OK);
    parameter_id = CM_HIR_GENERIC_PARAM_NONE;
    for (index = 0u; index < impl_parameter_count; ++index) {
        CmHirGenericParamId added;

        memset(&parameter, 0, sizeof(parameter));
        parameter.kind = CM_HIR_GENERIC_TYPE;
        parameter.owner = definition;
        parameter.index = index;
        parameter.name = cm_hir_intern(&fixture->hir,
            index == 0u ? "T" : "U");
        parameter.span = test_span(1u, 2u);
        assert(cm_hir_add_generic_param(&fixture->hir, &parameter,
            &added) == CM_HIR_OK);
        if (index == 0u) parameter_id = added;
    }
    memset(&parameter_type, 0, sizeof(parameter_type));
    parameter_type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    parameter_type.span = test_span(1u, 2u);
    parameter_type.data.parameter_type.parameter = parameter_id;
    assert(cm_hir_add_type(&fixture->hir, &parameter_type,
        &parameter_type_id) == CM_HIR_OK);
    tuple_elements[0] = parameter_type_id;
    tuple_type = add_hir_tuple(&fixture->hir, tuple_elements, 1u);
    if (self_shape == TEST_GENERIC_SELF_TUPLE) {
        self_type = tuple_type;
    } else if (self_shape == TEST_GENERIC_SELF_NESTED_REFERENCE) {
        memset(&reference_type, 0, sizeof(reference_type));
        reference_type.kind = CM_HIR_TYPE_REFERENCE_KIND;
        reference_type.span = test_span(1u, 2u);
        reference_type.data.reference_type.region.kind =
            CM_HIR_REGION_STATIC;
        reference_type.data.reference_type.pointee = tuple_type;
        reference_type.data.reference_type.mutability = CM_HIR_IMMUTABLE;
        assert(cm_hir_add_type(&fixture->hir, &reference_type, &self_type)
            == CM_HIR_OK);
    } else {
        self_type = fixture->bool_hir;
    }
    memset(trait_arguments, 0, sizeof(trait_arguments));
    trait_argument_count = 0u;
    if (argument_shape == TEST_GENERIC_ARGS_PARAMETER) {
        trait_arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
        trait_arguments[0].data.type = parameter_type_id;
        trait_argument_count = 1u;
    } else if (argument_shape == TEST_GENERIC_ARGS_REPEATED_PARAMETER) {
        trait_arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
        trait_arguments[0].data.type = parameter_type_id;
        trait_arguments[1] = trait_arguments[0];
        trait_argument_count = 2u;
    } else if (argument_shape == TEST_GENERIC_ARGS_NESTED_PARAMETER) {
        trait_arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
        trait_arguments[0].data.type = tuple_type;
        trait_argument_count = 1u;
    }
    init_item(&item, CM_HIR_ITEM_IMPL, definition, fixture->root,
        CM_INTERN_ID_NONE);
    item.generic_parameter_start = parameter_id;
    item.generic_parameter_count = impl_parameter_count;
    item.data.impl_item.self_type = self_type;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = trait_definition;
    item.data.impl_item.trait_type.arguments = trait_arguments;
    item.data.impl_item.trait_type.argument_count = trait_argument_count;
    item.data.impl_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_ordered_generic_impl(TestFixture *fixture,
    CmHirDefId trait_definition)
{
    CmHirDefId definition;
    CmHirGenericParam parameters[2];
    CmHirGenericParamId parameter_ids[2];
    CmHirType parameter_type;
    CmHirTypeId parameter_types[2];
    CmHirTypeId self_type;
    CmHirGenericArg trait_arguments[2];
    CmHirItem item;
    CmHirItemId item_id;
    uint32_t index;

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_IMPL, test_span(1u, 2u),
        &definition) == CM_HIR_OK);
    memset(parameters, 0, sizeof(parameters));
    memset(parameter_ids, 0, sizeof(parameter_ids));
    memset(parameter_types, 0, sizeof(parameter_types));
    for (index = 0u; index < 2u; ++index) {
        parameters[index].kind = CM_HIR_GENERIC_TYPE;
        parameters[index].owner = definition;
        parameters[index].index = index;
        parameters[index].name = cm_hir_intern(&fixture->hir,
            index == 0u ? "T" : "U");
        parameters[index].span = test_span(1u, 2u);
        assert(cm_hir_add_generic_param(&fixture->hir, &parameters[index],
            &parameter_ids[index]) == CM_HIR_OK);
        memset(&parameter_type, 0, sizeof(parameter_type));
        parameter_type.kind = CM_HIR_TYPE_PARAMETER_KIND;
        parameter_type.span = test_span(1u, 2u);
        parameter_type.data.parameter_type.parameter = parameter_ids[index];
        assert(cm_hir_add_type(&fixture->hir, &parameter_type,
            &parameter_types[index]) == CM_HIR_OK);
    }
    self_type = add_hir_tuple(&fixture->hir, parameter_types, 2u);
    memset(trait_arguments, 0, sizeof(trait_arguments));
    trait_arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    trait_arguments[0].data.type = parameter_types[1];
    trait_arguments[1].kind = CM_HIR_GENERIC_ARG_TYPE;
    trait_arguments[1].data.type = parameter_types[0];
    init_item(&item, CM_HIR_ITEM_IMPL, definition, fixture->root,
        CM_INTERN_ID_NONE);
    item.generic_parameter_start = parameter_ids[0];
    item.generic_parameter_count = 2u;
    item.data.impl_item.self_type = self_type;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = trait_definition;
    item.data.impl_item.trait_type.arguments = trait_arguments;
    item.data.impl_item.trait_type.argument_count = 2u;
    item.data.impl_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_monomorphic_tuple_impl(TestFixture *fixture,
    CmHirDefId trait_definition)
{
    CmHirTypeId tuple_elements[1];
    CmHirTypeId self_type;
    CmHirGenericArg trait_argument;
    CmHirDefId definition;
    CmHirItem item;
    CmHirItemId item_id;

    tuple_elements[0] = fixture->u8_hir;
    self_type = add_hir_tuple(&fixture->hir, tuple_elements, 1u);
    memset(&trait_argument, 0, sizeof(trait_argument));
    trait_argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    trait_argument.data.type = fixture->u8_hir;
    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_IMPL, test_span(1u, 2u),
        &definition) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_IMPL, definition, fixture->root,
        CM_INTERN_ID_NONE);
    item.data.impl_item.self_type = self_type;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = trait_definition;
    item.data.impl_item.trait_type.arguments = &trait_argument;
    item.data.impl_item.trait_type.argument_count = 1u;
    item.data.impl_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static void add_blocked_lifetime_generic_impl(TestFixture *fixture,
    CmHirDefId trait_definition)
{
    CmHirDefId definition;
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_id;
    CmHirType reference_type;
    CmHirTypeId self_type;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_IMPL, test_span(1u, 2u),
        &definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_LIFETIME;
    parameter.owner = definition;
    parameter.name = cm_hir_intern(&fixture->hir, "a");
    parameter.span = test_span(1u, 2u);
    assert(cm_hir_add_generic_param(&fixture->hir, &parameter,
        &parameter_id) == CM_HIR_OK);
    memset(&reference_type, 0, sizeof(reference_type));
    reference_type.kind = CM_HIR_TYPE_REFERENCE_KIND;
    reference_type.span = test_span(1u, 2u);
    reference_type.data.reference_type.region.kind =
        CM_HIR_REGION_EARLY_BOUND;
    reference_type.data.reference_type.region.data.parameter = parameter_id;
    reference_type.data.reference_type.pointee = fixture->bool_hir;
    reference_type.data.reference_type.mutability = CM_HIR_IMMUTABLE;
    assert(cm_hir_add_type(&fixture->hir, &reference_type, &self_type)
        == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_IMPL, definition, fixture->root,
        CM_INTERN_ID_NONE);
    item.generic_parameter_start = parameter_id;
    item.generic_parameter_count = 1u;
    item.data.impl_item.self_type = self_type;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = trait_definition;
    item.data.impl_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
}

static void fixture_init_ordered(TestFixture *fixture,
    int reverse_overlap_order)
{
    memset(fixture, 0, sizeof(*fixture));
    cm_hir_context_init(&fixture->hir);
    assert(cm_hir_create_crate(&fixture->hir,
        cm_hir_intern(&fixture->hir, "solver_test"), CM_HIR_EDITION_2024,
        test_span(0u, 100u), &fixture->crate_id, &fixture->root)
        == CM_HIR_OK);
    fixture->u8_hir = add_scalar(&fixture->hir,
        CM_HIR_TYPE_INTEGER_KIND, CM_HIR_INT_U8);
    fixture->u16_hir = add_scalar(&fixture->hir,
        CM_HIR_TYPE_INTEGER_KIND, CM_HIR_INT_U16);
    fixture->bool_hir = add_scalar(&fixture->hir,
        CM_HIR_TYPE_BOOL_KIND, CM_HIR_INT_U8);
    fixture->exact_trait = add_trait(&fixture->hir, fixture->crate_id,
        fixture->root, "Exact", 0);
    fixture->empty_trait = add_trait(&fixture->hir, fixture->crate_id,
        fixture->root, "Empty", 0);
    fixture->generic_trait = add_trait(&fixture->hir, fixture->crate_id,
        fixture->root, "Generic", 0);
    fixture->ambiguous_trait = add_trait(&fixture->hir, fixture->crate_id,
        fixture->root, "Ambiguous", 0);
    fixture->auto_trait = add_trait(&fixture->hir, fixture->crate_id,
        fixture->root, "Auto", 1);
    fixture->empty_auto_trait = add_trait(&fixture->hir, fixture->crate_id,
        fixture->root, "EmptyAuto", 1);
    fixture->lifetime_trait = add_lifetime_trait(&fixture->hir,
        fixture->crate_id, fixture->root, &fixture->lifetime_parameter);
    fixture->const_trait = add_const_trait(&fixture->hir,
        fixture->crate_id, fixture->root, fixture->u8_hir,
        &fixture->const_parameter);
    fixture->unary_generic_trait = add_type_trait(&fixture->hir,
        fixture->crate_id, fixture->root, "UnaryGeneric", 1u);
    fixture->repeated_generic_trait = add_type_trait(&fixture->hir,
        fixture->crate_id, fixture->root, "RepeatedGeneric", 2u);
    fixture->nested_generic_trait = add_type_trait(&fixture->hir,
        fixture->crate_id, fixture->root, "NestedGeneric", 1u);
    fixture->unused_generic_trait = add_trait(&fixture->hir,
        fixture->crate_id, fixture->root, "UnusedGeneric", 0);
    fixture->overlap_generic_trait = add_type_trait(&fixture->hir,
        fixture->crate_id, fixture->root, "OverlapGeneric", 1u);
    fixture->blocked_generic_trait = add_trait(&fixture->hir,
        fixture->crate_id, fixture->root, "BlockedGeneric", 0);

    /* Deliberately not in trait/head order: the index must sort it. */
    (void)add_impl(&fixture->hir, fixture->crate_id, fixture->root,
        fixture->ambiguous_trait, fixture->u16_hir, 0);
    fixture->exact_impl = add_impl(&fixture->hir, fixture->crate_id,
        fixture->root, fixture->exact_trait, fixture->u8_hir, 0);
    (void)add_blanket_impl(&fixture->hir, fixture->crate_id,
        fixture->root, fixture->generic_trait, 0);
    (void)add_impl(&fixture->hir, fixture->crate_id, fixture->root,
        fixture->ambiguous_trait, fixture->u16_hir, 0);
    (void)add_impl(&fixture->hir, fixture->crate_id, fixture->root,
        fixture->auto_trait, fixture->u8_hir, 0);
    (void)add_impl(&fixture->hir, fixture->crate_id, fixture->root,
        fixture->auto_trait, fixture->bool_hir, 1);
    (void)add_impl(&fixture->hir, fixture->crate_id, fixture->root,
        fixture->auto_trait, fixture->bool_hir, 1);
    (void)add_impl(&fixture->hir, fixture->crate_id, fixture->root,
        fixture->auto_trait, fixture->u16_hir, 1);
    (void)add_impl(&fixture->hir, fixture->crate_id, fixture->root,
        fixture->auto_trait, fixture->u8_hir, 1);
    (void)add_type_only_generic_impl(fixture,
        fixture->unary_generic_trait, TEST_GENERIC_SELF_TUPLE,
        TEST_GENERIC_ARGS_PARAMETER, 1u);
    (void)add_type_only_generic_impl(fixture,
        fixture->repeated_generic_trait, TEST_GENERIC_SELF_BOOL,
        TEST_GENERIC_ARGS_REPEATED_PARAMETER, 1u);
    (void)add_type_only_generic_impl(fixture,
        fixture->nested_generic_trait,
        TEST_GENERIC_SELF_NESTED_REFERENCE,
        TEST_GENERIC_ARGS_NESTED_PARAMETER, 1u);
    (void)add_type_only_generic_impl(fixture,
        fixture->unused_generic_trait, TEST_GENERIC_SELF_BOOL,
        TEST_GENERIC_ARGS_NONE, 2u);
    if (reverse_overlap_order) {
        (void)add_monomorphic_tuple_impl(fixture,
            fixture->overlap_generic_trait);
        (void)add_type_only_generic_impl(fixture,
            fixture->overlap_generic_trait, TEST_GENERIC_SELF_TUPLE,
            TEST_GENERIC_ARGS_PARAMETER, 1u);
    } else {
        (void)add_type_only_generic_impl(fixture,
            fixture->overlap_generic_trait, TEST_GENERIC_SELF_TUPLE,
            TEST_GENERIC_ARGS_PARAMETER, 1u);
        (void)add_monomorphic_tuple_impl(fixture,
            fixture->overlap_generic_trait);
    }
    add_blocked_lifetime_generic_impl(fixture,
        fixture->blocked_generic_trait);
}

static void fixture_init(TestFixture *fixture)
{
    fixture_init_ordered(fixture, 0);
}

static void fixture_destroy(TestFixture *fixture)
{
    cm_hir_context_destroy(&fixture->hir);
}

static CmTypeckNamedType trait_query(CmHirDefId definition)
{
    CmTypeckNamedType query;

    memset(&query, 0, sizeof(query));
    query.definition = definition;
    return query;
}

static CmTypeckTypeId add_typeck_tuple(CmTypeckContext *typeck,
    const CmTypeckTypeId *elements, uint32_t element_count)
{
    CmTypeckType type;
    CmTypeckTypeId result;

    memset(&type, 0, sizeof(type));
    type.kind = CM_TYPECK_TYPE_TUPLE;
    type.span = test_span(1u, 2u);
    type.data.tuple_type.elements = (CmTypeckTypeId *)elements;
    type.data.tuple_type.element_count = element_count;
    assert(cm_typeck_add_type(typeck, &type, &result) == CM_TYPECK_OK);
    return result;
}

static CmTypeckTypeId add_typeck_static_reference(
    CmTypeckContext *typeck, CmTypeckTypeId pointee)
{
    CmTypeckType type;
    CmTypeckTypeId result;

    memset(&type, 0, sizeof(type));
    type.kind = CM_TYPECK_TYPE_REFERENCE;
    type.span = test_span(1u, 2u);
    type.data.reference_type.region.kind = CM_HIR_REGION_STATIC;
    type.data.reference_type.pointee = pointee;
    type.data.reference_type.mutability = CM_HIR_IMMUTABLE;
    assert(cm_typeck_add_type(typeck, &type, &result) == CM_TYPECK_OK);
    return result;
}

static void test_type_only_generic_selection(void)
{
    TestFixture fixture;
    CmHirDefId unconstrained_trait;
    TestFixture reversed_fixture;
    CmTraitImplIndex impl_index;
    CmTraitImplIndex reversed_index;
    CmTypeckContext typeck;
    CmTypeckContext reversed_typeck;
    CmTypeckTypeId u8_type;
    CmTypeckTypeId bool_type;
    CmTypeckTypeId tuple_elements[1];
    CmTypeckTypeId tuple_type;
    CmTypeckTypeId nested_type;
    CmTypeckTypeId variable;
    CmTypeckTypeId trait_variable;
    CmTypeckTypeId unconstrained_variable;
    CmTypeckTypeId resolved;
    CmTypeckGenericArg arguments[2];
    CmTypeckNamedType query;
    CmTraitSelectionResult result;
    size_t type_count;
    size_t entry_index;
    int saw_supported_generic;
    int saw_unused_blocker;
    int saw_lifetime_blocker;

    fixture_init(&fixture);
    unconstrained_trait = add_type_trait(&fixture.hir,
        fixture.crate_id, fixture.root, "Unconstrained", 1u);
    (void)add_type_only_generic_impl(&fixture, unconstrained_trait,
        TEST_GENERIC_SELF_BOOL, TEST_GENERIC_ARGS_PARAMETER, 1u);
    memset(&impl_index, 0, sizeof(impl_index));
    assert(cm_trait_impl_index_init(&impl_index, &fixture.hir,
        fixture.crate_id, CM_TRAIT_IMPL_UNIVERSE_OPEN)
        == CM_TRAIT_SOLVER_PROVEN);
    saw_supported_generic = 0;
    saw_unused_blocker = 0;
    saw_lifetime_blocker = 0;
    for (entry_index = 0u;
         entry_index < cm_trait_impl_index_entry_count(&impl_index);
         ++entry_index) {
        const CmTraitImplIndexEntry *entry;

        entry = cm_trait_impl_index_entry(&impl_index, entry_index);
        assert(entry != NULL);
        if (cm_hir_def_id_equal(entry->trait_definition,
                fixture.unary_generic_trait)) {
            assert(entry->unsupported_flags ==
                CM_TRAIT_IMPL_UNSUPPORTED_NONE);
            saw_supported_generic = 1;
        } else if (cm_hir_def_id_equal(entry->trait_definition,
                fixture.unused_generic_trait)) {
            assert((entry->unsupported_flags
                & CM_TRAIT_IMPL_UNSUPPORTED_GENERIC) != 0u);
            saw_unused_blocker = 1;
        } else if (cm_hir_def_id_equal(entry->trait_definition,
                fixture.blocked_generic_trait)) {
            assert((entry->unsupported_flags
                & CM_TRAIT_IMPL_UNSUPPORTED_GENERIC) != 0u);
            saw_lifetime_blocker = 1;
        }
    }
    assert(saw_supported_generic && saw_unused_blocker
        && saw_lifetime_blocker);
    cm_typeck_context_init(&typeck, &fixture.hir);
    assert(cm_typeck_import_hir_type(&typeck, fixture.u8_hir, &u8_type)
        == CM_TYPECK_OK);
    assert(cm_typeck_import_hir_type(&typeck, fixture.bool_hir, &bool_type)
        == CM_TYPECK_OK);
    tuple_elements[0] = u8_type;
    tuple_type = add_typeck_tuple(&typeck, tuple_elements, 1u);
    memset(arguments, 0, sizeof(arguments));
    arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    arguments[0].data.type = u8_type;

    query = trait_query(fixture.unary_generic_trait);
    query.arguments = arguments;
    query.argument_count = 1u;
    result = cm_trait_solver_select(&impl_index, &typeck, tuple_type,
        &query);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && result.proof_origin == CM_TRAIT_PROOF_IMPL
        && result.param_env_fact_index == CM_TRAIT_PROOF_FACT_NONE
        && result.param_env_equality_index
            == CM_TRAIT_PROOF_EQUALITY_NONE
        && result.supported_match_count == 1u
        && !cm_hir_def_id_is_none(result.impl_definition));

    /* A concrete receiver can infer an omitted type trait argument. */
    assert(cm_typeck_new_variable(&typeck, CM_HIR_INFER_GENERAL,
        test_span(2u, 3u), &trait_variable) == CM_TYPECK_OK);
    arguments[0].data.type = trait_variable;
    result = cm_trait_solver_select(&impl_index, &typeck, tuple_type,
        &query);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && result.supported_match_count == 1u);
    assert(cm_typeck_resolve(&typeck, trait_variable, &resolved)
        == CM_TYPECK_OK);
    assert(resolved == u8_type);

    /* A trait argument with no receiver/predicate constraint must defer. */
    assert(cm_typeck_new_variable(&typeck, CM_HIR_INFER_GENERAL,
        test_span(3u, 4u), &unconstrained_variable) == CM_TYPECK_OK);
    arguments[0].data.type = unconstrained_variable;
    query = trait_query(unconstrained_trait);
    query.arguments = arguments;
    query.argument_count = 1u;
    result = cm_trait_solver_select(&impl_index, &typeck, bool_type,
        &query);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_INFERENCE);
    assert(cm_typeck_resolve(&typeck, unconstrained_variable, &resolved)
        == CM_TYPECK_OK);
    assert(resolved == unconstrained_variable);

    arguments[0].data.type = bool_type;
    type_count = cm_typeck_type_count(&typeck);
    result = cm_trait_solver_select(&impl_index, &typeck, tuple_type,
        &query);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_METADATA
        && cm_typeck_type_count(&typeck) == type_count);

    arguments[0].data.type = u8_type;
    arguments[1] = arguments[0];
    query = trait_query(fixture.repeated_generic_trait);
    query.arguments = arguments;
    query.argument_count = 2u;
    result = cm_trait_solver_select(&impl_index, &typeck, bool_type,
        &query);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && result.supported_match_count == 1u);
    arguments[1].data.type = bool_type;
    type_count = cm_typeck_type_count(&typeck);
    result = cm_trait_solver_select(&impl_index, &typeck, bool_type,
        &query);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_METADATA
        && cm_typeck_type_count(&typeck) == type_count);

    arguments[0].data.type = tuple_type;
    query = trait_query(fixture.nested_generic_trait);
    query.arguments = arguments;
    query.argument_count = 1u;
    nested_type = add_typeck_static_reference(&typeck, tuple_type);
    result = cm_trait_solver_select(&impl_index, &typeck, nested_type,
        &query);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && result.supported_match_count == 1u);

    query = trait_query(fixture.unused_generic_trait);
    type_count = cm_typeck_type_count(&typeck);
    result = cm_trait_solver_select(&impl_index, &typeck, bool_type,
        &query);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED
        && result.supported_match_count == 0u
        && result.blocking_match_count == 1u
        && cm_typeck_type_count(&typeck) == type_count);

    arguments[0].data.type = u8_type;
    query = trait_query(fixture.overlap_generic_trait);
    query.arguments = arguments;
    query.argument_count = 1u;
    type_count = cm_typeck_type_count(&typeck);
    result = cm_trait_solver_select(&impl_index, &typeck, tuple_type,
        &query);
    assert(result.kind == CM_TRAIT_SOLVER_AMBIGUOUS
        && result.supported_match_count == 2u
        && cm_typeck_type_count(&typeck) == type_count);

    assert(cm_typeck_new_variable(&typeck, CM_HIR_INFER_GENERAL,
        test_span(1u, 2u), &variable) == CM_TYPECK_OK);
    query = trait_query(fixture.unary_generic_trait);
    query.arguments = arguments;
    query.argument_count = 1u;
    type_count = cm_typeck_type_count(&typeck);
    result = cm_trait_solver_select(&impl_index, &typeck, variable,
        &query);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_INFERENCE
        && cm_typeck_type_count(&typeck) == type_count);
    assert(cm_typeck_resolve(&typeck, variable, &resolved) == CM_TYPECK_OK
        && resolved == variable);

    nested_type = add_typeck_static_reference(&typeck, bool_type);
    query = trait_query(fixture.blocked_generic_trait);
    type_count = cm_typeck_type_count(&typeck);
    result = cm_trait_solver_select(&impl_index, &typeck, nested_type,
        &query);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED
        && result.blocking_match_count == 1u
        && cm_typeck_type_count(&typeck) == type_count);

    fixture_init_ordered(&reversed_fixture, 1);
    memset(&reversed_index, 0, sizeof(reversed_index));
    assert(cm_trait_impl_index_init(&reversed_index,
        &reversed_fixture.hir, reversed_fixture.crate_id,
        CM_TRAIT_IMPL_UNIVERSE_OPEN) == CM_TRAIT_SOLVER_PROVEN);
    cm_typeck_context_init(&reversed_typeck, &reversed_fixture.hir);
    assert(cm_typeck_import_hir_type(&reversed_typeck,
        reversed_fixture.u8_hir, &u8_type) == CM_TYPECK_OK);
    tuple_elements[0] = u8_type;
    tuple_type = add_typeck_tuple(&reversed_typeck, tuple_elements, 1u);
    arguments[0].data.type = u8_type;
    query = trait_query(reversed_fixture.overlap_generic_trait);
    query.arguments = arguments;
    query.argument_count = 1u;
    type_count = cm_typeck_type_count(&reversed_typeck);
    result = cm_trait_solver_select(&reversed_index, &reversed_typeck,
        tuple_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_AMBIGUOUS
        && result.supported_match_count == 2u
        && cm_typeck_type_count(&reversed_typeck) == type_count);

    cm_typeck_context_destroy(&reversed_typeck);
    cm_trait_impl_index_destroy(&reversed_index);
    fixture_destroy(&reversed_fixture);
    cm_typeck_context_destroy(&typeck);
    cm_trait_impl_index_destroy(&impl_index);
    fixture_destroy(&fixture);
}

static void test_impl_selection_witness(void)
{
    TestFixture fixture;
    CmTraitImplIndex impl_index;
    CmTraitImplSelectionWitness witness;
    CmTypeckContext typeck;
    CmTypeckSnapshot snapshot;
    CmTypeckInstantiation instantiation;
    CmTypeckTypeId u8_type;
    CmTypeckTypeId bool_type;
    CmTypeckTypeId tuple_elements[2];
    CmTypeckTypeId tuple_type;
    CmTypeckTypeId unary_tuple_type;
    CmTypeckTypeId resolved;
    CmTypeckGenericArg arguments[2];
    CmTypeckNamedType query;
    CmTraitSelectionResult result;
    CmHirDefId ordered_trait;
    CmHirDefId ordered_impl;

    fixture_init(&fixture);
    ordered_trait = add_type_trait(&fixture.hir, fixture.crate_id,
        fixture.root, "Ordered", 2u);
    ordered_impl = add_ordered_generic_impl(&fixture, ordered_trait);
    memset(&impl_index, 0, sizeof(impl_index));
    assert(cm_trait_impl_index_init(&impl_index, &fixture.hir,
        fixture.crate_id, CM_TRAIT_IMPL_UNIVERSE_OPEN)
        == CM_TRAIT_SOLVER_PROVEN);
    cm_typeck_context_init(&typeck, &fixture.hir);
    cm_trait_impl_selection_witness_init(&witness);
    assert(!cm_trait_impl_selection_witness_is_current(&witness, &typeck)
        && !cm_trait_impl_selection_witness_instantiation(&witness,
            &typeck, &instantiation));
    assert(cm_typeck_import_hir_type(&typeck, fixture.u8_hir, &u8_type)
            == CM_TYPECK_OK
        && cm_typeck_import_hir_type(&typeck, fixture.bool_hir, &bool_type)
            == CM_TYPECK_OK);
    tuple_elements[0] = u8_type;
    tuple_elements[1] = bool_type;
    tuple_type = add_typeck_tuple(&typeck, tuple_elements, 2u);
    memset(arguments, 0, sizeof(arguments));
    arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    arguments[0].data.type = bool_type;
    arguments[1].kind = CM_HIR_GENERIC_ARG_TYPE;
    arguments[1].data.type = u8_type;
    query = trait_query(ordered_trait);
    query.arguments = arguments;
    query.argument_count = 2u;
    result = cm_trait_solver_select_with_witness(&impl_index, &typeck,
        tuple_type, &query, &witness);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && result.proof_origin == CM_TRAIT_PROOF_IMPL
        && cm_hir_def_id_equal(result.impl_definition, ordered_impl)
        && cm_trait_impl_selection_witness_is_current(&witness, &typeck)
        && cm_trait_impl_selection_witness_instantiation(&witness,
            &typeck, &instantiation)
        && cm_hir_def_id_equal(instantiation.parameter_owner, ordered_impl)
        && instantiation.argument_count == 2u
        && instantiation.arguments != NULL
        && instantiation.arguments[0].kind == CM_HIR_GENERIC_ARG_TYPE
        && instantiation.arguments[1].kind == CM_HIR_GENERIC_ARG_TYPE
        && cm_typeck_resolve(&typeck,
            instantiation.arguments[0].data.type, &resolved) == CM_TYPECK_OK
        && resolved == u8_type
        && cm_typeck_resolve(&typeck,
            instantiation.arguments[1].data.type, &resolved) == CM_TYPECK_OK
        && resolved == bool_type);

    assert(cm_typeck_snapshot(&typeck, &snapshot) == CM_TYPECK_OK
        && cm_typeck_rollback(&typeck, &snapshot) == CM_TYPECK_OK
        && !cm_trait_impl_selection_witness_is_current(&witness, &typeck)
        && !cm_trait_impl_selection_witness_instantiation(&witness,
            &typeck, &instantiation));
    result = cm_trait_solver_select_with_witness(&impl_index, &typeck,
        tuple_type, &query, &witness);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && cm_trait_impl_selection_witness_is_current(&witness, &typeck));

    query = trait_query(fixture.empty_trait);
    result = cm_trait_solver_select_with_witness(&impl_index, &typeck,
        bool_type, &query, &witness);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_METADATA
        && !cm_trait_impl_selection_witness_is_current(&witness, &typeck));

    query = trait_query(fixture.exact_trait);
    result = cm_trait_solver_select_with_witness(&impl_index, &typeck,
        u8_type, &query, &witness);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && cm_hir_def_id_equal(result.impl_definition, fixture.exact_impl)
        && cm_trait_impl_selection_witness_instantiation(&witness,
            &typeck, &instantiation)
        && cm_hir_def_id_equal(instantiation.parameter_owner,
            fixture.exact_impl)
        && instantiation.argument_count == 0u
        && instantiation.arguments == NULL);

    unary_tuple_type = add_typeck_tuple(&typeck, tuple_elements, 1u);
    arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    arguments[0].data.type = u8_type;
    query = trait_query(fixture.overlap_generic_trait);
    query.arguments = arguments;
    query.argument_count = 1u;
    result = cm_trait_solver_select_with_witness(&impl_index, &typeck,
        unary_tuple_type, &query, &witness);
    assert(result.kind == CM_TRAIT_SOLVER_AMBIGUOUS
        && result.supported_match_count == 2u
        && !cm_trait_impl_selection_witness_is_current(&witness, &typeck)
        && !cm_trait_impl_selection_witness_instantiation(&witness,
            &typeck, &instantiation));

    cm_typeck_context_destroy(&typeck);
    cm_typeck_context_init(&typeck, &fixture.hir);
    assert(!cm_trait_impl_selection_witness_is_current(&witness, &typeck)
        && !cm_trait_impl_selection_witness_instantiation(&witness,
            &typeck, &instantiation));
    cm_trait_impl_selection_witness_destroy(&witness);
    cm_typeck_context_destroy(&typeck);
    cm_trait_impl_index_destroy(&impl_index);
    fixture_destroy(&fixture);
}

static void test_impl_selection_witness_hir_staleness(void)
{
    TestFixture fixture;
    CmTraitImplIndex impl_index;
    CmTraitImplSelectionWitness witness;
    CmTypeckContext typeck;
    CmTypeckInstantiation instantiation;
    CmTypeckTypeId u8_type;
    CmTypeckNamedType query;
    CmTraitSelectionResult result;

    fixture_init(&fixture);
    memset(&impl_index, 0, sizeof(impl_index));
    assert(cm_trait_impl_index_init(&impl_index, &fixture.hir,
        fixture.crate_id, CM_TRAIT_IMPL_UNIVERSE_OPEN)
        == CM_TRAIT_SOLVER_PROVEN);
    cm_typeck_context_init(&typeck, &fixture.hir);
    cm_trait_impl_selection_witness_init(&witness);
    assert(cm_typeck_import_hir_type(&typeck, fixture.u8_hir, &u8_type)
        == CM_TYPECK_OK);
    query = trait_query(fixture.generic_trait);
    result = cm_trait_solver_select_with_witness(&impl_index, &typeck,
        u8_type, &query, &witness);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && cm_trait_impl_selection_witness_instantiation(&witness,
            &typeck, &instantiation)
        && instantiation.argument_count == 1u);
    (void)add_blanket_impl(&fixture.hir, fixture.crate_id, fixture.root,
        fixture.generic_trait, 0);
    assert(!cm_trait_impl_selection_witness_is_current(&witness, &typeck)
        && !cm_trait_impl_selection_witness_instantiation(&witness,
            &typeck, &instantiation));
    cm_trait_impl_selection_witness_destroy(&witness);
    cm_typeck_context_destroy(&typeck);
    cm_trait_impl_index_destroy(&impl_index);
    fixture_destroy(&fixture);

    fixture_init(&fixture);
    memset(&impl_index, 0, sizeof(impl_index));
    assert(cm_trait_impl_index_init(&impl_index, &fixture.hir,
        fixture.crate_id, CM_TRAIT_IMPL_UNIVERSE_OPEN)
        == CM_TRAIT_SOLVER_PROVEN);
    cm_typeck_context_init(&typeck, &fixture.hir);
    cm_trait_impl_selection_witness_init(&witness);
    assert(cm_typeck_import_hir_type(&typeck, fixture.u8_hir, &u8_type)
        == CM_TYPECK_OK);
    query = trait_query(fixture.exact_trait);
    result = cm_trait_solver_select_with_witness(&impl_index, &typeck,
        u8_type, &query, &witness);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && cm_trait_impl_selection_witness_instantiation(&witness,
            &typeck, &instantiation)
        && instantiation.argument_count == 0u);
    (void)add_impl(&fixture.hir, fixture.crate_id, fixture.root,
        fixture.exact_trait, fixture.u8_hir, 0);
    assert(!cm_trait_impl_selection_witness_is_current(&witness, &typeck)
        && !cm_trait_impl_selection_witness_instantiation(&witness,
            &typeck, &instantiation));
    cm_trait_impl_selection_witness_destroy(&witness);
    cm_typeck_context_destroy(&typeck);
    cm_trait_impl_index_destroy(&impl_index);
    fixture_destroy(&fixture);
}

static void test_specializable_impl_is_a_solver_blocker(void)
{
    TestFixture fixture;
    CmHirDefId trait_definition;
    CmHirDefId selected_associated;
    CmHirDefId specializable_associated;
    CmHirDefId impl_definition;
    CmHirDefId owner;
    CmTraitImplIndex index;
    CmTraitImplSelectionWitness witness;
    CmParamEnv environment;
    CmParamEnvSubstitution substitution;
    CmTypeckInstantiation owner_instantiation;
    CmTypeckInstantiation witness_instantiation;
    CmTypeckContext typeck;
    CmTypeckNamedType query;
    CmTypeckType projection;
    CmTypeckTypeId bool_type;
    CmTypeckTypeId u8_type;
    CmTypeckTypeId projection_type;
    CmProjectionEqualityGoal projection_goal;
    CmTraitSelectionResult result;
    size_t entry_index;
    size_t type_count;
    int saw_specialization_blocker;

    fixture_init(&fixture);
    trait_definition = add_trait(&fixture.hir, fixture.crate_id,
        fixture.root, "SpecializationBlocked", 0);
    selected_associated = add_associated_declaration(&fixture.hir,
        fixture.crate_id, fixture.root, trait_definition, "Selected");
    specializable_associated = add_associated_declaration(&fixture.hir,
        fixture.crate_id, fixture.root, trait_definition, "Specializable");
    impl_definition = add_impl(&fixture.hir, fixture.crate_id, fixture.root,
        trait_definition, fixture.bool_hir, 0);
    (void)add_impl_associated_type(&fixture.hir, fixture.crate_id,
        fixture.root, impl_definition, selected_associated, "Selected",
        fixture.u8_hir, 0);
    (void)add_impl_associated_type(&fixture.hir, fixture.crate_id,
        fixture.root, impl_definition, specializable_associated,
        "Specializable", fixture.bool_hir, 1);
    owner = add_trait(&fixture.hir, fixture.crate_id, fixture.root,
        "SpecializationGoalOwner", 0);

    memset(&index, 0, sizeof(index));
    assert(cm_trait_impl_index_init(&index, &fixture.hir, fixture.crate_id,
        CM_TRAIT_IMPL_UNIVERSE_OPEN) == CM_TRAIT_SOLVER_PROVEN);
    saw_specialization_blocker = 0;
    for (entry_index = 0u;
         entry_index < cm_trait_impl_index_entry_count(&index);
         ++entry_index) {
        const CmTraitImplIndexEntry *entry;

        entry = cm_trait_impl_index_entry(&index, entry_index);
        if (entry != NULL && cm_hir_def_id_equal(entry->impl_definition,
                impl_definition)) {
            assert((entry->unsupported_flags
                & CM_TRAIT_IMPL_UNSUPPORTED_SPECIALIZATION) != 0u);
            saw_specialization_blocker = 1;
        }
    }
    assert(saw_specialization_blocker);
    memset(&environment, 0, sizeof(environment));
    assert(cm_param_env_init(&environment, &fixture.hir, owner)
        == CM_PARAM_ENV_READY);
    cm_typeck_context_init(&typeck, &fixture.hir);
    assert(cm_typeck_import_hir_type(&typeck, fixture.bool_hir, &bool_type)
            == CM_TYPECK_OK
        && cm_typeck_import_hir_type(&typeck, fixture.u8_hir, &u8_type)
            == CM_TYPECK_OK);
    cm_trait_impl_selection_witness_init(&witness);

    query = trait_query(fixture.exact_trait);
    result = cm_trait_solver_select_with_witness(&index, &typeck, u8_type,
        &query, &witness);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && cm_trait_impl_selection_witness_is_current(&witness, &typeck));
    query = trait_query(trait_definition);
    type_count = cm_typeck_type_count(&typeck);
    result = cm_trait_solver_select_with_witness(&index, &typeck, bool_type,
        &query, &witness);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED
        && result.supported_match_count == 0u
        && result.blocking_match_count == 1u
        && result.proof_origin == CM_TRAIT_PROOF_NONE
        && cm_hir_def_id_is_none(result.impl_definition)
        && result.impl_item == CM_HIR_ITEM_NONE
        && cm_hir_def_id_is_none(result.impl_associated_definition)
        && !cm_trait_impl_selection_witness_is_current(&witness, &typeck)
        && !cm_trait_impl_selection_witness_instantiation(&witness,
            &typeck, &witness_instantiation)
        && cm_typeck_type_count(&typeck) == type_count);

    cm_typeck_instantiation_init(&typeck, &owner_instantiation);
    owner_instantiation.parameter_owner = owner;
    owner_instantiation.self_owner = owner;
    owner_instantiation.self_type = bool_type;
    memset(&substitution, 0, sizeof(substitution));
    substitution.exact = &owner_instantiation;
    memset(&projection, 0, sizeof(projection));
    projection.kind = CM_TYPECK_TYPE_PROJECTION;
    projection.span = test_span(1u, 2u);
    projection.data.projection_type.self_type = bool_type;
    projection.data.projection_type.trait_type.definition =
        trait_definition;
    projection.data.projection_type.associated_type.definition =
        selected_associated;
    assert(cm_typeck_add_type(&typeck, &projection, &projection_type)
        == CM_TYPECK_OK);
    memset(&projection_goal, 0, sizeof(projection_goal));
    projection_goal.owner = owner;
    projection_goal.projection_type = projection_type;
    projection_goal.expected_type = u8_type;
    type_count = cm_typeck_type_count(&typeck);
    result = cm_trait_solver_solve_projection_equality(&index,
        &environment, &typeck, &substitution, &projection_goal, NULL);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED
        && result.supported_match_count == 0u
        && result.blocking_match_count == 1u
        && result.proof_origin == CM_TRAIT_PROOF_NONE
        && cm_hir_def_id_is_none(result.impl_definition)
        && result.impl_item == CM_HIR_ITEM_NONE
        && cm_hir_def_id_is_none(result.impl_associated_definition)
        && cm_typeck_type_count(&typeck) == type_count);

    cm_trait_impl_selection_witness_destroy(&witness);
    cm_typeck_context_destroy(&typeck);
    cm_param_env_destroy(&environment);
    cm_trait_impl_index_destroy(&index);
    fixture_destroy(&fixture);
}

static void test_exact_negative_selection(void)
{
    TestFixture fixture;
    CmHirDefId ordinary_trait;
    CmHirDefId argument_negative_trait;
    CmHirDefId generic_negative_trait;
    CmHirDefId duplicate_negative_trait;
    CmHirDefId overlap_trait;
    CmHirDefId projection_trait;
    CmHirDefId associated_type;
    CmHirDefId foreign_trait;
    CmHirDefId owner;
    CmHirCrateId foreign_crate;
    CmHirModuleId foreign_root;
    CmTraitImplIndex index;
    CmTraitImplSelectionWitness witness;
    CmTypeckContext typeck;
    CmTypeckInstantiation witness_instantiation;
    CmTypeckInstantiation owner_instantiation;
    CmParamEnv environment;
    CmParamEnvSubstitution substitution;
    CmImplementedTraitGoal goal;
    CmProjectionEqualityGoal projection_goal;
    CmTraitSelectionResult result;
    CmTypeckGenericArg trait_argument;
    CmTypeckNamedType query;
    CmTypeckType projection;
    CmTypeckTypeId bool_type;
    CmTypeckTypeId u8_type;
    CmTypeckTypeId u16_type;
    CmTypeckTypeId projection_type;
    CmTypeckTypeId variable;
    CmTypeckTypeId resolved;
    size_t type_count;

    fixture_init(&fixture);
    ordinary_trait = add_trait(&fixture.hir, fixture.crate_id,
        fixture.root, "NegativeOrdinary", 0);
    (void)add_impl(&fixture.hir, fixture.crate_id, fixture.root,
        ordinary_trait, fixture.bool_hir, 1);
    argument_negative_trait = add_type_trait(&fixture.hir,
        fixture.crate_id, fixture.root, "NegativeArgument", 1u);
    (void)add_impl_with_type_argument(&fixture.hir, fixture.crate_id,
        fixture.root, argument_negative_trait, fixture.bool_hir,
        fixture.u8_hir, 1);
    generic_negative_trait = add_trait(&fixture.hir, fixture.crate_id,
        fixture.root, "NegativeGeneric", 0);
    (void)add_blanket_impl(&fixture.hir, fixture.crate_id, fixture.root,
        generic_negative_trait, 1);
    duplicate_negative_trait = add_trait(&fixture.hir, fixture.crate_id,
        fixture.root, "NegativeDuplicate", 0);
    (void)add_impl(&fixture.hir, fixture.crate_id, fixture.root,
        duplicate_negative_trait, fixture.bool_hir, 1);
    (void)add_impl(&fixture.hir, fixture.crate_id, fixture.root,
        duplicate_negative_trait, fixture.bool_hir, 1);
    overlap_trait = add_trait(&fixture.hir, fixture.crate_id,
        fixture.root, "NegativeOverlap", 0);
    (void)add_impl(&fixture.hir, fixture.crate_id, fixture.root,
        overlap_trait, fixture.bool_hir, 1);
    (void)add_impl(&fixture.hir, fixture.crate_id, fixture.root,
        overlap_trait, fixture.bool_hir, 0);
    projection_trait = add_trait(&fixture.hir, fixture.crate_id,
        fixture.root, "NegativeProjection", 0);
    associated_type = add_associated_declaration(&fixture.hir,
        fixture.crate_id, fixture.root, projection_trait, "Item");
    (void)add_impl(&fixture.hir, fixture.crate_id, fixture.root,
        projection_trait, fixture.bool_hir, 1);
    assert(cm_hir_create_crate(&fixture.hir,
        cm_hir_intern(&fixture.hir, "solver_dependency"),
        CM_HIR_EDITION_2024, test_span(0u, 100u), &foreign_crate,
        &foreign_root) == CM_HIR_OK);
    foreign_trait = add_trait(&fixture.hir, foreign_crate, foreign_root,
        "ForeignNegative", 0);
    (void)add_impl(&fixture.hir, fixture.crate_id, fixture.root,
        foreign_trait, fixture.bool_hir, 1);
    owner = add_trait(&fixture.hir, fixture.crate_id, fixture.root,
        "NegativeGoalOwner", 0);

    memset(&environment, 0, sizeof(environment));
    assert(cm_param_env_init(&environment, &fixture.hir, owner)
        == CM_PARAM_ENV_READY);
    memset(&index, 0, sizeof(index));
    assert(cm_trait_impl_index_init(&index, &fixture.hir,
        fixture.crate_id, CM_TRAIT_IMPL_UNIVERSE_OPEN)
        == CM_TRAIT_SOLVER_PROVEN);
    cm_typeck_context_init(&typeck, &fixture.hir);
    assert(cm_typeck_import_hir_type(&typeck, fixture.bool_hir,
        &bool_type) == CM_TYPECK_OK);
    assert(cm_typeck_import_hir_type(&typeck, fixture.u8_hir,
        &u8_type) == CM_TYPECK_OK);
    assert(cm_typeck_import_hir_type(&typeck, fixture.u16_hir,
        &u16_type) == CM_TYPECK_OK);
    cm_trait_impl_selection_witness_init(&witness);

    query = trait_query(ordinary_trait);
    type_count = cm_typeck_type_count(&typeck);
    result = cm_trait_solver_select_with_witness(&index, &typeck,
        bool_type, &query, &witness);
    assert(result.kind == CM_TRAIT_SOLVER_NEGATIVE
        && result.negative_match_count == 1u
        && result.supported_match_count == 0u
        && result.blocking_match_count == 0u
        && result.proof_origin == CM_TRAIT_PROOF_NONE
        && cm_hir_def_id_is_none(result.impl_definition)
        && result.impl_item == CM_HIR_ITEM_NONE
        && cm_hir_def_id_is_none(result.impl_associated_definition)
        && !cm_trait_impl_selection_witness_is_current(&witness, &typeck)
        && !cm_trait_impl_selection_witness_instantiation(&witness,
            &typeck, &witness_instantiation)
        && cm_typeck_type_count(&typeck) == type_count);
    result = cm_trait_solver_select(&index, &typeck, u16_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_METADATA
        && result.negative_match_count == 0u);
    query = trait_query(foreign_trait);
    result = cm_trait_solver_select(&index, &typeck, bool_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_NEGATIVE
        && result.negative_match_count == 1u
        && result.proof_origin == CM_TRAIT_PROOF_NONE
        && cm_hir_def_id_is_none(result.impl_definition));

    memset(&trait_argument, 0, sizeof(trait_argument));
    trait_argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    trait_argument.data.type = u8_type;
    query = trait_query(argument_negative_trait);
    query.arguments = &trait_argument;
    query.argument_count = 1u;
    result = cm_trait_solver_select(&index, &typeck, bool_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_NEGATIVE
        && result.negative_match_count == 1u);
    assert(cm_typeck_new_variable(&typeck, CM_HIR_INFER_GENERAL,
        test_span(1u, 2u), &variable) == CM_TYPECK_OK);
    trait_argument.data.type = variable;
    type_count = cm_typeck_type_count(&typeck);
    result = cm_trait_solver_select(&index, &typeck, bool_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED
        && result.negative_match_count == 0u
        && result.blocking_match_count == 1u
        && cm_typeck_type_count(&typeck) == type_count
        && cm_typeck_resolve(&typeck, variable, &resolved) == CM_TYPECK_OK
        && resolved == variable);

    query = trait_query(generic_negative_trait);
    result = cm_trait_solver_select(&index, &typeck, bool_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED
        && result.negative_match_count == 0u
        && result.blocking_match_count == 1u);
    query = trait_query(duplicate_negative_trait);
    result = cm_trait_solver_select(&index, &typeck, bool_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_AMBIGUOUS
        && result.negative_match_count == 2u
        && result.supported_match_count == 0u);
    query = trait_query(overlap_trait);
    result = cm_trait_solver_select(&index, &typeck, bool_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_AMBIGUOUS
        && result.negative_match_count == 1u
        && result.supported_match_count == 1u);

    cm_typeck_instantiation_init(&typeck, &owner_instantiation);
    owner_instantiation.parameter_owner = owner;
    owner_instantiation.self_owner = owner;
    owner_instantiation.self_type = bool_type;
    memset(&substitution, 0, sizeof(substitution));
    substitution.exact = &owner_instantiation;
    memset(&goal, 0, sizeof(goal));
    goal.owner = owner;
    goal.self_type = bool_type;
    goal.trait_type = trait_query(ordinary_trait);
    type_count = cm_typeck_type_count(&typeck);
    result = cm_trait_solver_solve_implemented(&index, &environment,
        &typeck, &substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_NEGATIVE
        && result.negative_match_count == 1u
        && result.proof_origin == CM_TRAIT_PROOF_NONE
        && cm_hir_def_id_is_none(result.impl_definition)
        && cm_typeck_type_count(&typeck) == type_count);

    goal.self_type = u16_type;
    goal.trait_type = trait_query(fixture.auto_trait);
    result = cm_trait_solver_solve_implemented(&index, &environment,
        &typeck, &substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_NEGATIVE
        && result.negative_match_count == 1u
        && result.proof_origin == CM_TRAIT_PROOF_NONE
        && cm_hir_def_id_is_none(result.impl_definition));

    memset(&projection, 0, sizeof(projection));
    projection.kind = CM_TYPECK_TYPE_PROJECTION;
    projection.span = test_span(1u, 2u);
    projection.data.projection_type.self_type = bool_type;
    projection.data.projection_type.trait_type.definition =
        projection_trait;
    projection.data.projection_type.associated_type.definition =
        associated_type;
    assert(cm_typeck_add_type(&typeck, &projection, &projection_type)
        == CM_TYPECK_OK);
    memset(&projection_goal, 0, sizeof(projection_goal));
    projection_goal.owner = owner;
    projection_goal.projection_type = projection_type;
    projection_goal.expected_type = u8_type;
    type_count = cm_typeck_type_count(&typeck);
    result = cm_trait_solver_solve_projection_equality(&index,
        &environment, &typeck, &substitution, &projection_goal, NULL);
    assert(result.kind == CM_TRAIT_SOLVER_NEGATIVE
        && result.negative_match_count == 1u
        && result.proof_origin == CM_TRAIT_PROOF_NONE
        && cm_hir_def_id_is_none(result.impl_definition)
        && result.impl_item == CM_HIR_ITEM_NONE
        && cm_hir_def_id_is_none(result.impl_associated_definition)
        && cm_typeck_type_count(&typeck) == type_count);

    cm_trait_impl_selection_witness_destroy(&witness);
    cm_typeck_context_destroy(&typeck);
    cm_trait_impl_index_destroy(&index);
    cm_param_env_destroy(&environment);
    fixture_destroy(&fixture);
}

static void test_exact_positive_auto_selection(void)
{
    TestFixture fixture;
    CmHirDefId explicit_auto;
    CmHirDefId explicit_impl;
    CmHirDefId duplicate_auto;
    CmHirDefId empty_auto;
    CmHirDefId generic_auto;
    CmHirDefId predicate_auto;
    CmHirDefId outlives_auto;
    CmHirDefId child_auto;
    CmHirDefId child_impl;
    CmHirDefId unsafe_auto;
    CmHirDefId unsafe_impl;
    CmHirDefId foreign_auto;
    CmHirDefId child_definition;
    CmHirCrateId foreign_crate;
    CmHirModuleId foreign_root;
    CmHirItem child;
    CmHirItem *item;
    CmTraitImplIndex index;
    CmTraitImplSelectionWitness witness;
    CmTypeckContext typeck;
    CmTypeckInstantiation selected;
    CmTypeckNamedType query;
    CmTraitSelectionResult result;
    CmTypeckTypeId bool_type;
    CmTypeckTypeId u8_type;
    size_t type_count;

    fixture_init(&fixture);
    explicit_auto = add_trait(&fixture.hir, fixture.crate_id,
        fixture.root, "ExplicitPositiveAuto", 1);
    explicit_impl = add_impl(&fixture.hir, fixture.crate_id,
        fixture.root, explicit_auto, fixture.bool_hir, 0);
    duplicate_auto = add_trait(&fixture.hir, fixture.crate_id,
        fixture.root, "DuplicatePositiveAuto", 1);
    (void)add_impl(&fixture.hir, fixture.crate_id, fixture.root,
        duplicate_auto, fixture.bool_hir, 0);
    (void)add_impl(&fixture.hir, fixture.crate_id, fixture.root,
        duplicate_auto, fixture.bool_hir, 0);
    empty_auto = add_trait(&fixture.hir, fixture.crate_id,
        fixture.root, "NoStructuralAuto", 1);
    generic_auto = add_trait(&fixture.hir, fixture.crate_id,
        fixture.root, "GenericPositiveAuto", 1);
    (void)add_blanket_impl(&fixture.hir, fixture.crate_id, fixture.root,
        generic_auto, 0);
    predicate_auto = add_trait(&fixture.hir, fixture.crate_id,
        fixture.root, "PredicatePositiveAuto", 1);
    (void)add_constrained_bool_impl(&fixture, predicate_auto, 1, 0);
    outlives_auto = add_trait(&fixture.hir, fixture.crate_id,
        fixture.root, "OutlivesPositiveAuto", 1);
    (void)add_constrained_bool_impl(&fixture, outlives_auto, 0, 1);
    child_auto = add_trait(&fixture.hir, fixture.crate_id,
        fixture.root, "ChildBearingPositiveAuto", 1);
    child_impl = add_impl(&fixture.hir, fixture.crate_id, fixture.root,
        child_auto, fixture.bool_hir, 0);
    assert(cm_hir_reserve_item_definition_as(&fixture.hir,
        fixture.crate_id, CM_HIR_ITEM_FUNCTION, test_span(1u, 2u),
        &child_definition) == CM_HIR_OK);
    init_item(&child, CM_HIR_ITEM_FUNCTION, child_definition,
        fixture.root, cm_hir_intern(&fixture.hir, "forged"));
    child.parent_definition = child_impl;
    child.data.function_item.signature.return_type = fixture.bool_hir;
    assert(cm_vec_push(&fixture.hir.items, &child));
    unsafe_auto = add_trait(&fixture.hir, fixture.crate_id,
        fixture.root, "UnsafePositiveAuto", 1);
    unsafe_impl = add_impl(&fixture.hir, fixture.crate_id, fixture.root,
        unsafe_auto, fixture.bool_hir, 0);
    item = find_mutable_item(&fixture, unsafe_auto);
    assert(item != NULL);
    item->data.trait_item.safety = CM_HIR_UNSAFE;
    item = find_mutable_item(&fixture, unsafe_impl);
    assert(item != NULL);
    item->data.impl_item.safety = CM_HIR_UNSAFE;
    foreign_auto = add_trait(&fixture.hir, fixture.crate_id,
        fixture.root, "ForeignHeaderPositiveAuto", 1);
    assert(cm_hir_create_crate(&fixture.hir,
        cm_hir_intern(&fixture.hir, "solver_auto_dependency"),
        CM_HIR_EDITION_2024, test_span(0u, 100u), &foreign_crate,
        &foreign_root) == CM_HIR_OK);
    (void)add_impl(&fixture.hir, foreign_crate, foreign_root,
        foreign_auto, fixture.bool_hir, 0);

    memset(&index, 0, sizeof(index));
    assert(cm_trait_impl_index_init(&index, &fixture.hir,
        fixture.crate_id, CM_TRAIT_IMPL_UNIVERSE_OPEN)
        == CM_TRAIT_SOLVER_PROVEN);
    cm_typeck_context_init(&typeck, &fixture.hir);
    assert(cm_typeck_import_hir_type(&typeck, fixture.bool_hir,
        &bool_type) == CM_TYPECK_OK);
    assert(cm_typeck_import_hir_type(&typeck, fixture.u8_hir,
        &u8_type) == CM_TYPECK_OK);
    cm_trait_impl_selection_witness_init(&witness);

    query = trait_query(explicit_auto);
    assert(cm_trait_solver_validate_implemented_goal(&fixture.hir,
        &typeck, bool_type, &query) == CM_TRAIT_SOLVER_UNSUPPORTED);
    result = cm_trait_solver_select_with_witness(&index, &typeck,
        bool_type, &query, &witness);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && result.proof_origin == CM_TRAIT_PROOF_IMPL
        && cm_hir_def_id_equal(result.impl_definition, explicit_impl)
        && result.impl_item != CM_HIR_ITEM_NONE
        && result.supported_match_count == 1u
        && result.negative_match_count == 0u
        && result.blocking_match_count == 0u
        && cm_trait_impl_selection_witness_instantiation(&witness,
            &typeck, &selected)
        && cm_hir_def_id_equal(selected.parameter_owner, explicit_impl)
        && selected.argument_count == 0u);
    type_count = cm_typeck_type_count(&typeck);
    result = cm_trait_solver_select_with_witness(&index, &typeck,
        u8_type, &query, &witness);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED
        && result.supported_match_count == 0u
        && result.negative_match_count == 0u
        && result.blocking_match_count == 0u
        && !cm_trait_impl_selection_witness_is_current(&witness, &typeck)
        && cm_typeck_type_count(&typeck) == type_count);

    query = trait_query(duplicate_auto);
    result = cm_trait_solver_select_with_witness(&index, &typeck,
        bool_type, &query, &witness);
    assert(result.kind == CM_TRAIT_SOLVER_AMBIGUOUS
        && result.supported_match_count == 2u
        && result.negative_match_count == 0u
        && result.blocking_match_count == 0u
        && result.proof_origin == CM_TRAIT_PROOF_NONE
        && cm_hir_def_id_is_none(result.impl_definition)
        && result.impl_item == CM_HIR_ITEM_NONE
        && !cm_trait_impl_selection_witness_is_current(&witness, &typeck));

    query = trait_query(empty_auto);
    result = cm_trait_solver_select(&index, &typeck, bool_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED
        && result.supported_match_count == 0u
        && result.blocking_match_count == 0u);
    query = trait_query(generic_auto);
    result = cm_trait_solver_select(&index, &typeck, bool_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED
        && result.supported_match_count == 0u
        && result.blocking_match_count == 1u);
    query = trait_query(predicate_auto);
    result = cm_trait_solver_select(&index, &typeck, bool_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED
        && result.supported_match_count == 0u
        && result.blocking_match_count == 1u);
    query = trait_query(outlives_auto);
    result = cm_trait_solver_select(&index, &typeck, bool_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED
        && result.supported_match_count == 0u
        && result.blocking_match_count == 1u);
    query = trait_query(child_auto);
    result = cm_trait_solver_select(&index, &typeck, bool_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED
        && result.supported_match_count == 0u
        && result.blocking_match_count == 1u);
    query = trait_query(unsafe_auto);
    result = cm_trait_solver_select(&index, &typeck, bool_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED
        && result.supported_match_count == 0u
        && result.blocking_match_count == 1u);
    query = trait_query(foreign_auto);
    result = cm_trait_solver_select(&index, &typeck, bool_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED
        && result.supported_match_count == 0u
        && result.blocking_match_count == 1u);

    cm_trait_impl_selection_witness_destroy(&witness);
    cm_typeck_context_destroy(&typeck);
    cm_trait_impl_index_destroy(&index);
    fixture_destroy(&fixture);
}

static void test_index_and_selection(void)
{
    TestFixture fixture;
    CmTraitImplIndex complete;
    CmTraitImplIndex incomplete;
    CmTypeckContext typeck;
    CmTypeckTypeId u8_type;
    CmTypeckTypeId u16_type;
    CmTypeckTypeId bool_type;
    CmTypeckTypeId variable;
    CmTypeckSnapshot caller_snapshot;
    CmTypeckGenericArg malformed_argument;
    CmTypeckGenericArg lifetime_argument;
    CmTraitSelectionResult result;
    CmTypeckNamedType query;
    size_t index;
    int saw_auto;
    int saw_negative;

    fixture_init(&fixture);
    memset(&complete, 0, sizeof(complete));
    memset(&incomplete, 0, sizeof(incomplete));
    assert(cm_trait_impl_index_init(&complete, &fixture.hir,
        fixture.crate_id,
        CM_TRAIT_IMPL_UNIVERSE_SINGLE_LOCAL_CRATE_COMPLETE)
        == CM_TRAIT_SOLVER_INVALID);
    assert(cm_trait_impl_index_init(&complete, &fixture.hir,
        fixture.crate_id, CM_TRAIT_IMPL_UNIVERSE_OPEN)
        == CM_TRAIT_SOLVER_PROVEN);
    assert(cm_trait_impl_index_init(&incomplete, &fixture.hir,
        fixture.crate_id, CM_TRAIT_IMPL_UNIVERSE_OPEN)
        == CM_TRAIT_SOLVER_PROVEN);
    assert(cm_trait_impl_index_entry_count(&complete) == 16u);
    for (index = 1u; index < cm_trait_impl_index_entry_count(&complete);
         ++index) {
        const CmTraitImplIndexEntry *prior;
        const CmTraitImplIndexEntry *current;

        prior = cm_trait_impl_index_entry(&complete, index - 1u);
        current = cm_trait_impl_index_entry(&complete, index);
        assert(prior != NULL && current != NULL);
        assert(prior->trait_definition.crate_id
                < current->trait_definition.crate_id
            || (prior->trait_definition.crate_id
                    == current->trait_definition.crate_id
                && prior->trait_definition.index
                    <= current->trait_definition.index));
    }
    saw_auto = 0;
    saw_negative = 0;
    for (index = 0u; index < cm_trait_impl_index_entry_count(&complete);
         ++index) {
        const CmTraitImplIndexEntry *entry;

        entry = cm_trait_impl_index_entry(&complete, index);
        assert(entry != NULL);
        if ((entry->unsupported_flags
                & CM_TRAIT_IMPL_UNSUPPORTED_AUTO_TRAIT) != 0u) {
            saw_auto = 1;
        }
        if ((entry->unsupported_flags
                & CM_TRAIT_IMPL_UNSUPPORTED_NEGATIVE) != 0u) {
            saw_negative = 1;
        }
    }
    assert(saw_auto && saw_negative);

    cm_typeck_context_init(&typeck, &fixture.hir);
    assert(cm_typeck_import_hir_type(&typeck, fixture.u8_hir, &u8_type)
        == CM_TYPECK_OK);
    assert(cm_typeck_import_hir_type(&typeck, fixture.u16_hir, &u16_type)
        == CM_TYPECK_OK);
    assert(cm_typeck_import_hir_type(&typeck, fixture.bool_hir, &bool_type)
        == CM_TYPECK_OK);

    query = trait_query(fixture.exact_trait);
    result = cm_trait_solver_select(&complete, &typeck, u8_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && cm_hir_def_id_equal(result.impl_definition, fixture.exact_impl)
        && result.impl_item != CM_HIR_ITEM_NONE
        && result.supported_match_count == 1u
        && result.blocking_match_count == 0u);
    result = cm_trait_solver_select(&complete, &typeck, bool_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_METADATA);
    result = cm_trait_solver_select(&incomplete, &typeck, bool_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_METADATA);

    query = trait_query(fixture.empty_trait);
    result = cm_trait_solver_select(&complete, &typeck, u8_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_METADATA);
    result = cm_trait_solver_select(&incomplete, &typeck, u8_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_METADATA);

    query = trait_query(fixture.ambiguous_trait);
    index = cm_typeck_type_count(&typeck);
    assert(cm_typeck_snapshot(&typeck, &caller_snapshot) == CM_TYPECK_OK);
    result = cm_trait_solver_select(&complete, &typeck, u16_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_AMBIGUOUS
        && result.supported_match_count == 2u
        && cm_typeck_type_count(&typeck) == index);
    assert(cm_typeck_rollback(&typeck, &caller_snapshot) == CM_TYPECK_OK);

    query = trait_query(fixture.generic_trait);
    result = cm_trait_solver_select(&complete, &typeck, bool_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && result.supported_match_count == 1u
        && result.blocking_match_count == 0u);
    query = trait_query(fixture.auto_trait);
    index = cm_typeck_type_count(&typeck);
    assert(cm_trait_solver_validate_implemented_goal(&fixture.hir,
        &typeck, u16_type, &query) == CM_TRAIT_SOLVER_UNSUPPORTED);
    assert(cm_typeck_snapshot(&typeck, &caller_snapshot) == CM_TYPECK_OK);
    result = cm_trait_solver_select(&complete, &typeck, u8_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_AMBIGUOUS
        && result.supported_match_count == 1u
        && result.blocking_match_count == 0u
        && result.negative_match_count == 1u
        && cm_typeck_type_count(&typeck) == index);
    assert(cm_typeck_rollback(&typeck, &caller_snapshot) == CM_TYPECK_OK);
    result = cm_trait_solver_select(&complete, &typeck, bool_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_AMBIGUOUS
        && result.negative_match_count == 2u
        && result.blocking_match_count == 0u);
    result = cm_trait_solver_select(&complete, &typeck, u16_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_NEGATIVE
        && result.negative_match_count == 1u
        && result.supported_match_count == 0u
        && result.blocking_match_count == 0u
        && result.proof_origin == CM_TRAIT_PROOF_NONE
        && cm_hir_def_id_is_none(result.impl_definition)
        && result.impl_item == CM_HIR_ITEM_NONE);
    query = trait_query(fixture.empty_auto_trait);
    result = cm_trait_solver_select(&complete, &typeck, u8_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED
        && result.supported_match_count == 0u
        && result.blocking_match_count == 0u);

    memset(&malformed_argument, 0, sizeof(malformed_argument));
    malformed_argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    malformed_argument.data.type = u8_type;
    query = trait_query(fixture.exact_trait);
    query.arguments = &malformed_argument;
    query.argument_count = 1u;
    index = cm_typeck_type_count(&typeck);
    result = cm_trait_solver_select(&complete, &typeck, u8_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_INVALID
        && cm_typeck_type_count(&typeck) == index);

    memset(&lifetime_argument, 0, sizeof(lifetime_argument));
    lifetime_argument.kind = CM_HIR_GENERIC_ARG_LIFETIME;
    lifetime_argument.data.lifetime.kind = CM_HIR_REGION_STATIC;
    query = trait_query(fixture.lifetime_trait);
    query.arguments = &lifetime_argument;
    query.argument_count = 1u;
    result = cm_trait_solver_select(&complete, &typeck, u8_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_METADATA);
    lifetime_argument.data.lifetime.kind = CM_HIR_REGION_INFER;
    lifetime_argument.data.lifetime.data.inference_variable = 1u;
    result = cm_trait_solver_select(&complete, &typeck, u8_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_INFERENCE);
    lifetime_argument.data.lifetime.kind = CM_HIR_REGION_EARLY_BOUND;
    lifetime_argument.data.lifetime.data.parameter =
        fixture.lifetime_parameter;
    result = cm_trait_solver_select(&complete, &typeck, u8_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED);

    assert(cm_typeck_new_variable(&typeck, CM_HIR_INFER_GENERAL,
        test_span(1u, 2u), &variable) == CM_TYPECK_OK);
    query = trait_query(fixture.exact_trait);
    index = cm_typeck_type_count(&typeck);
    result = cm_trait_solver_select(&complete, &typeck, variable, &query);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_INFERENCE
        && cm_typeck_type_count(&typeck) == index);

    cm_typeck_context_destroy(&typeck);
    cm_trait_impl_index_destroy(&incomplete);
    cm_trait_impl_index_destroy(&complete);
    fixture_destroy(&fixture);
}

static void test_projection_overflow_and_invalid_query(void)
{
    TestFixture fixture;
    TestFixture other_fixture;
    CmTraitImplIndex index;
    CmTypeckContext typeck;
    CmTypeckContext other_typeck;
    CmTypeckNamedType query;
    CmTypeckType type;
    CmTypeckTypeId current;
    CmTypeckTypeId projection;
    CmTypeckTypeId other_u8;
    CmTypeckTypeId bogus_adt;
    CmTypeckTypeId shared;
    CmTypeckTypeId chain;
    CmTypeckTypeId tuple;
    CmTypeckTypeId tuple_elements[2];
    CmTraitSelectionResult result;
    uint32_t depth;

    fixture_init(&fixture);
    memset(&index, 0, sizeof(index));
    assert(cm_trait_impl_index_init(&index, &fixture.hir,
        fixture.crate_id, CM_TRAIT_IMPL_UNIVERSE_OPEN)
        == CM_TRAIT_SOLVER_PROVEN);
    cm_typeck_context_init(&typeck, &fixture.hir);
    assert(cm_typeck_import_hir_type(&typeck, fixture.u8_hir, &current)
        == CM_TYPECK_OK);
    fixture_init(&other_fixture);
    cm_typeck_context_init(&other_typeck, &other_fixture.hir);
    assert(cm_typeck_import_hir_type(&other_typeck, other_fixture.u8_hir,
        &other_u8) == CM_TYPECK_OK);
    query = trait_query(fixture.exact_trait);
    result = cm_trait_solver_select(&index, &other_typeck, other_u8,
        &query);
    assert(result.kind == CM_TRAIT_SOLVER_INVALID);
    cm_typeck_context_destroy(&other_typeck);
    fixture_destroy(&other_fixture);

    memset(&type, 0, sizeof(type));
    type.kind = CM_TYPECK_TYPE_ADT;
    type.span = test_span(1u, 2u);
    type.data.named_type.definition = fixture.exact_trait;
    assert(cm_typeck_add_type(&typeck, &type, &bogus_adt)
        == CM_TYPECK_OK);
    query = trait_query(fixture.exact_trait);
    result = cm_trait_solver_select(&index, &typeck, bogus_adt, &query);
    assert(result.kind == CM_TRAIT_SOLVER_INVALID);
    tuple_elements[0] = current;
    tuple_elements[1] = bogus_adt;
    memset(&type, 0, sizeof(type));
    type.kind = CM_TYPECK_TYPE_TUPLE;
    type.span = test_span(1u, 2u);
    type.data.tuple_type.elements = tuple_elements;
    type.data.tuple_type.element_count = 2u;
    assert(cm_typeck_add_type(&typeck, &type, &tuple) == CM_TYPECK_OK);
    result = cm_trait_solver_select(&index, &typeck, tuple, &query);
    assert(result.kind == CM_TRAIT_SOLVER_INVALID);

    memset(&type, 0, sizeof(type));
    type.kind = CM_TYPECK_TYPE_REFERENCE;
    type.span = test_span(1u, 2u);
    type.data.reference_type.region.kind = CM_HIR_REGION_EARLY_BOUND;
    type.data.reference_type.region.data.parameter =
        fixture.lifetime_parameter;
    type.data.reference_type.pointee = current;
    type.data.reference_type.mutability = CM_HIR_IMMUTABLE;
    assert(cm_typeck_add_type(&typeck, &type, &shared) == CM_TYPECK_OK);
    result = cm_trait_solver_select(&index, &typeck, shared, &query);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED);

    type.data.reference_type.region.kind = CM_HIR_REGION_STATIC;
    assert(cm_typeck_add_type(&typeck, &type, &shared) == CM_TYPECK_OK);
    chain = shared;
    for (depth = 0u; depth < 254u; ++depth) {
        type.data.reference_type.pointee = chain;
        assert(cm_typeck_add_type(&typeck, &type, &chain) == CM_TYPECK_OK);
    }
    tuple_elements[0] = shared;
    tuple_elements[1] = chain;
    memset(&type, 0, sizeof(type));
    type.kind = CM_TYPECK_TYPE_TUPLE;
    type.span = test_span(1u, 2u);
    type.data.tuple_type.elements = tuple_elements;
    type.data.tuple_type.element_count = 2u;
    assert(cm_typeck_add_type(&typeck, &type, &tuple) == CM_TYPECK_OK);
    result = cm_trait_solver_select(&index, &typeck, tuple, &query);
    assert(result.kind == CM_TRAIT_SOLVER_OVERFLOW);

    tuple_elements[0] = chain;
    tuple_elements[1] = chain;
    assert(cm_typeck_add_type(&typeck, &type, &tuple) == CM_TYPECK_OK);
    result = cm_trait_solver_select(&index, &typeck, tuple, &query);
    assert(result.kind == CM_TRAIT_SOLVER_OVERFLOW);

    memset(&type, 0, sizeof(type));
    type.kind = CM_TYPECK_TYPE_PROJECTION;
    type.span = test_span(1u, 2u);
    type.data.projection_type.self_type = current;
    type.data.projection_type.trait_type.definition = fixture.exact_trait;
    type.data.projection_type.associated_type.definition =
        fixture.empty_trait;
    assert(cm_typeck_add_type(&typeck, &type, &projection)
        == CM_TYPECK_OK);
    query = trait_query(fixture.exact_trait);
    result = cm_trait_solver_select(&index, &typeck, projection, &query);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED);

    for (depth = 0u; depth < 257u; ++depth) {
        memset(&type, 0, sizeof(type));
        type.kind = CM_TYPECK_TYPE_REFERENCE;
        type.span = test_span(1u, 2u);
        type.data.reference_type.region.kind = CM_HIR_REGION_STATIC;
        type.data.reference_type.pointee = current;
        type.data.reference_type.mutability = CM_HIR_IMMUTABLE;
        assert(cm_typeck_add_type(&typeck, &type, &current)
            == CM_TYPECK_OK);
    }
    result = cm_trait_solver_select(&index, &typeck, current, &query);
    assert(result.kind == CM_TRAIT_SOLVER_OVERFLOW);

    query.definition = cm_hir_def_id_none();
    result = cm_trait_solver_select(&index, &typeck, current, &query);
    assert(result.kind == CM_TRAIT_SOLVER_INVALID);
    assert(strcmp(cm_trait_solver_result_name(CM_TRAIT_SOLVER_PROVEN),
        "proven") == 0);
    cm_typeck_context_destroy(&typeck);
    cm_trait_impl_index_destroy(&index);
    fixture_destroy(&fixture);
}

static void test_const_scanning(void)
{
    TestFixture fixture;
    CmTraitImplIndex index;
    CmTypeckContext typeck;
    CmTypeckType type;
    CmTypeckTypeId u8_type;
    CmTypeckTypeId array_type;
    CmTypeckGenericArg argument;
    CmTypeckNamedType query;
    CmTraitSelectionResult result;

    fixture_init(&fixture);
    memset(&index, 0, sizeof(index));
    assert(cm_trait_impl_index_init(&index, &fixture.hir,
        fixture.crate_id, CM_TRAIT_IMPL_UNIVERSE_OPEN)
        == CM_TRAIT_SOLVER_PROVEN);
    cm_typeck_context_init(&typeck, &fixture.hir);
    assert(cm_typeck_import_hir_type(&typeck, fixture.u8_hir, &u8_type)
        == CM_TYPECK_OK);

    memset(&type, 0, sizeof(type));
    type.kind = CM_TYPECK_TYPE_ARRAY;
    type.span = test_span(1u, 2u);
    type.data.array_type.element = u8_type;
    type.data.array_type.length.kind = CM_HIR_CONST_VALUE;
    type.data.array_type.length.type = u8_type;
    type.data.array_type.length.data.value.low_bits = 4u;
    assert(cm_typeck_add_type(&typeck, &type, &array_type) == CM_TYPECK_OK);
    query = trait_query(fixture.exact_trait);
    result = cm_trait_solver_select(&index, &typeck, array_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_METADATA);

    type.data.array_type.length.kind = CM_HIR_CONST_PARAMETER;
    type.data.array_type.length.data.parameter = fixture.const_parameter;
    assert(cm_typeck_add_type(&typeck, &type, &array_type) == CM_TYPECK_OK);
    result = cm_trait_solver_select(&index, &typeck, array_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED);

    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_CONST;
    argument.data.constant.kind = CM_HIR_CONST_VALUE;
    argument.data.constant.type = u8_type;
    argument.data.constant.data.value.low_bits = 4u;
    query = trait_query(fixture.const_trait);
    query.arguments = &argument;
    query.argument_count = 1u;
    result = cm_trait_solver_select(&index, &typeck, u8_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_METADATA);

    argument.data.constant.kind = CM_HIR_CONST_INFER;
    result = cm_trait_solver_select(&index, &typeck, u8_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_INFERENCE);
    argument.data.constant.type = CM_TYPECK_TYPE_NONE;
    result = cm_trait_solver_select(&index, &typeck, u8_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_INVALID);

    argument.data.constant.type = u8_type;
    argument.data.constant.kind = CM_HIR_CONST_PARAMETER;
    argument.data.constant.data.parameter = fixture.const_parameter;
    result = cm_trait_solver_select(&index, &typeck, u8_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED);
    argument.data.constant.data.parameter = fixture.lifetime_parameter;
    result = cm_trait_solver_select(&index, &typeck, u8_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_INVALID);

    argument.data.constant.kind = CM_HIR_CONST_UNEVALUATED;
    result = cm_trait_solver_select(&index, &typeck, u8_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED);
    argument.data.constant.kind = CM_HIR_CONST_ERROR;
    result = cm_trait_solver_select(&index, &typeck, u8_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED);

    cm_typeck_context_destroy(&typeck);
    cm_trait_impl_index_destroy(&index);
    fixture_destroy(&fixture);
}

static void test_stale_index_fingerprints(void)
{
    TestFixture fixture;
    CmTraitImplIndex index;
    CmTypeckContext typeck;
    CmTypeckTypeId self_type;
    CmTypeckNamedType query;
    CmTraitSelectionResult result;
    CmHirContextMark mark;
    CmHirCrateId second_crate;
    CmHirModuleId second_root;

    fixture_init(&fixture);
    memset(&index, 0, sizeof(index));
    assert(cm_trait_impl_index_init(&index, &fixture.hir,
        fixture.crate_id, CM_TRAIT_IMPL_UNIVERSE_OPEN)
        == CM_TRAIT_SOLVER_PROVEN);
    cm_typeck_context_init(&typeck, &fixture.hir);
    assert(cm_typeck_import_hir_type(&typeck, fixture.u8_hir, &self_type)
        == CM_TYPECK_OK);
    (void)add_scalar(&fixture.hir, CM_HIR_TYPE_INTEGER_KIND,
        CM_HIR_INT_U32);
    query = trait_query(fixture.exact_trait);
    result = cm_trait_solver_select(&index, &typeck, self_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_INVALID
        && cm_trait_impl_index_entry_count(&index) == 0u);
    cm_typeck_context_destroy(&typeck);
    cm_trait_impl_index_destroy(&index);
    fixture_destroy(&fixture);

    fixture_init(&fixture);
    assert(cm_hir_context_mark(&fixture.hir, &mark) == CM_HIR_OK);
    memset(&index, 0, sizeof(index));
    assert(cm_trait_impl_index_init(&index, &fixture.hir,
        fixture.crate_id, CM_TRAIT_IMPL_UNIVERSE_OPEN)
        == CM_TRAIT_SOLVER_PROVEN);
    cm_typeck_context_init(&typeck, &fixture.hir);
    assert(cm_typeck_import_hir_type(&typeck, fixture.u8_hir, &self_type)
        == CM_TYPECK_OK);
    assert(cm_hir_context_rewind(&fixture.hir, &mark) == CM_HIR_OK);
    query = trait_query(fixture.exact_trait);
    result = cm_trait_solver_select(&index, &typeck, self_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_INVALID);
    cm_typeck_context_destroy(&typeck);
    cm_trait_impl_index_destroy(&index);
    fixture_destroy(&fixture);

    fixture_init(&fixture);
    assert(cm_hir_create_crate(&fixture.hir,
        cm_hir_intern(&fixture.hir, "second"), CM_HIR_EDITION_2024,
        test_span(0u, 10u), &second_crate, &second_root) == CM_HIR_OK);
    assert(second_crate != fixture.crate_id
        && second_root != CM_HIR_MODULE_NONE);
    memset(&index, 0, sizeof(index));
    assert(cm_trait_impl_index_init(&index, &fixture.hir,
        fixture.crate_id,
        CM_TRAIT_IMPL_UNIVERSE_SINGLE_LOCAL_CRATE_COMPLETE)
        == CM_TRAIT_SOLVER_INVALID);
    assert(cm_trait_impl_index_init(&index, &fixture.hir,
        fixture.crate_id, CM_TRAIT_IMPL_UNIVERSE_OPEN)
        == CM_TRAIT_SOLVER_PROVEN);
    cm_trait_impl_index_destroy(&index);
    fixture_destroy(&fixture);
}

static void test_finalized_complete_index(void)
{
    TestFixture fixture;
    CmHirCrateFinalization finalization;
    CmTraitImplIndex index;
    CmTypeckContext typeck;
    CmTypeckTypeId self_type;
    CmTypeckTypeId negative_type;
    CmTypeckNamedType query;
    CmTraitSelectionResult result;
    CmHirAttribute attribute;
    CmHirContextMark mark;
    uint64_t generation;

    fixture_init(&fixture);
    memset(&finalization, 0, sizeof(finalization));
    memset(&index, 0, sizeof(index));
    assert(cm_hir_crate_finalization_init(&finalization, &fixture.hir,
        fixture.crate_id) == CM_HIR_OK);
    assert(cm_hir_crate_finalization_is_current(&finalization));
    assert(cm_hir_crate_finalization_hir(&finalization) == &fixture.hir);
    assert(cm_hir_crate_finalization_crate(&finalization)
        == fixture.crate_id);
    generation = cm_hir_crate_finalization_generation(&finalization);
    assert(generation == fixture.hir.semantic_generation);
    assert(cm_hir_crate_finalization_init(&finalization, &fixture.hir,
        fixture.crate_id) == CM_HIR_INVALID_ARGUMENT);
    assert(cm_trait_impl_index_init_complete(&index, &finalization)
        == CM_TRAIT_SOLVER_PROVEN);
    assert(cm_trait_impl_index_universe(&index)
        == CM_TRAIT_IMPL_UNIVERSE_SINGLE_LOCAL_CRATE_COMPLETE);
    cm_typeck_context_init(&typeck, &fixture.hir);
    assert(cm_typeck_import_hir_type(&typeck, fixture.u8_hir, &self_type)
        == CM_TYPECK_OK);
    assert(cm_typeck_import_hir_type(&typeck, fixture.u16_hir,
        &negative_type) == CM_TYPECK_OK);
    query = trait_query(fixture.exact_trait);
    result = cm_trait_solver_select(&index, &typeck, self_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && cm_hir_def_id_equal(result.impl_definition,
            fixture.exact_impl));
    query = trait_query(fixture.empty_trait);
    result = cm_trait_solver_select(&index, &typeck, self_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_METADATA);
    query = trait_query(fixture.auto_trait);
    result = cm_trait_solver_select(&index, &typeck, negative_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_NEGATIVE
        && result.negative_match_count == 1u
        && result.proof_origin == CM_TRAIT_PROOF_NONE
        && cm_hir_def_id_is_none(result.impl_definition)
        && result.impl_item == CM_HIR_ITEM_NONE);

    assert(cm_hir_intern(&fixture.hir, "not-semantic")
        != CM_INTERN_ID_NONE);
    assert(cm_hir_crate_finalization_is_current(&finalization));
    assert(cm_trait_impl_index_is_current(&index));
    assert(cm_hir_set_crate_inner_attributes(&fixture.hir,
        fixture.crate_id, NULL, 0u) == CM_HIR_OK);
    assert(cm_hir_crate_finalization_is_current(&finalization));
    memset(&attribute, 0, sizeof(attribute));
    attribute.metadata = cm_hir_intern(&fixture.hir, "sealed");
    attribute.span = test_span(1u, 2u);
    attribute.source_attribute = 1u;
    assert(cm_hir_set_crate_inner_attributes(&fixture.hir,
        fixture.crate_id, &attribute, 1u) == CM_HIR_OK);
    assert(!cm_hir_crate_finalization_is_current(&finalization));
    assert(!cm_trait_impl_index_is_current(&index));
    assert(cm_hir_crate_finalization_generation(&finalization) == 0u);
    cm_typeck_context_destroy(&typeck);
    cm_trait_impl_index_destroy(&index);
    cm_hir_crate_finalization_destroy(&finalization);
    cm_hir_crate_finalization_destroy(&finalization);
    fixture_destroy(&fixture);

    fixture_init(&fixture);
    memset(&finalization, 0, sizeof(finalization));
    assert(cm_hir_crate_finalization_init(&finalization, &fixture.hir,
        fixture.crate_id) == CM_HIR_OK);
    (void)add_scalar(&fixture.hir, CM_HIR_TYPE_INTEGER_KIND,
        CM_HIR_INT_U32);
    assert(!cm_hir_crate_finalization_is_current(&finalization));
    cm_hir_crate_finalization_destroy(&finalization);
    fixture_destroy(&fixture);

    fixture_init(&fixture);
    memset(&finalization, 0, sizeof(finalization));
    assert(cm_hir_context_mark(&fixture.hir, &mark) == CM_HIR_OK);
    assert(cm_hir_crate_finalization_init(&finalization, &fixture.hir,
        fixture.crate_id) == CM_HIR_OK);
    assert(cm_hir_context_rewind(&fixture.hir, &mark) == CM_HIR_OK);
    assert(!cm_hir_crate_finalization_is_current(&finalization));
    memset(&index, 0, sizeof(index));
    assert(cm_trait_impl_index_init_complete(&index, &finalization)
        == CM_TRAIT_SOLVER_INVALID);
    assert(index.state == NULL);
    cm_trait_impl_index_destroy(&index);
    cm_hir_crate_finalization_destroy(&finalization);
    fixture_destroy(&fixture);
}

static void test_finalization_local_completeness(void)
{
    TestFixture fixture;
    CmHirCrateFinalization finalization;
    CmHirCrateId dependency_crate;
    CmHirModuleId dependency_root;
    CmHirDefId reserved;

    fixture_init(&fixture);
    memset(&finalization, 0, sizeof(finalization));
    assert(cm_hir_create_crate(&fixture.hir,
        cm_hir_intern(&fixture.hir, "dependency"), CM_HIR_EDITION_2024,
        test_span(0u, 10u), &dependency_crate, &dependency_root)
        == CM_HIR_OK);
    assert(dependency_root != CM_HIR_MODULE_NONE);
    assert(cm_hir_reserve_item_definition(&fixture.hir, dependency_crate,
        test_span(1u, 2u), &reserved) == CM_HIR_OK);
    assert(cm_hir_crate_finalization_init(&finalization, &fixture.hir,
        fixture.crate_id) == CM_HIR_OK);
    cm_hir_crate_finalization_destroy(&finalization);
    assert(cm_hir_crate_finalization_init(&finalization, &fixture.hir,
        (CmHirCrateId)(dependency_crate + 100u)) == CM_HIR_INVALID_ID);
    assert(finalization.state == NULL);
    assert(cm_hir_reserve_item_definition(&fixture.hir, fixture.crate_id,
        test_span(2u, 3u), &reserved) == CM_HIR_OK);
    assert(cm_hir_crate_finalization_init(&finalization, &fixture.hir,
        fixture.crate_id) == CM_HIR_INVARIANT_VIOLATION);
    assert(finalization.state == NULL);
    cm_hir_crate_finalization_destroy(&finalization);
    fixture_destroy(&fixture);
}

int main(void)
{
    test_index_and_selection();
    test_type_only_generic_selection();
    test_impl_selection_witness();
    test_impl_selection_witness_hir_staleness();
    test_specializable_impl_is_a_solver_blocker();
    test_exact_negative_selection();
    test_exact_positive_auto_selection();
    test_projection_overflow_and_invalid_query();
    test_const_scanning();
    test_stale_index_fingerprints();
    test_finalized_complete_index();
    test_finalization_local_completeness();
    puts("hir trait solver tests passed");
    return 0;
}
