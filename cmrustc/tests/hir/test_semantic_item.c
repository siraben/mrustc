#include "cm/hir/semantic_item.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct TestFixture {
    CmHirContext hir;
    CmHirCrateId crate_id;
    CmHirModuleId root;
    CmHirTypeId u32_type;
    CmHirTypeId bool_type;
    CmHirDefId trait_definition;
    CmHirDefId trait_method;
    CmHirDefId trait_type;
    CmHirDefId impl_definition;
    CmHirDefId impl_method;
    CmHirDefId impl_type;
} TestFixture;

static CmSpan test_span(uint32_t start, uint32_t end)
{
    CmSpan span;

    span.source = 1u;
    span.start = start;
    span.end = end;
    return span;
}

static void init_item(TestFixture *fixture, CmHirItem *item,
    CmHirItemKind kind, CmHirDefId definition, const char *name)
{
    memset(item, 0, sizeof(*item));
    item->kind = kind;
    item->definition = definition;
    item->owner_module = fixture->root;
    item->parent_definition = cm_hir_def_id_none();
    item->name = name == NULL ? CM_INTERN_ID_NONE
        : cm_hir_intern(&fixture->hir, name);
    item->visibility.kind = CM_HIR_VIS_PRIVATE;
    item->visibility.restriction = cm_hir_def_id_none();
    item->span = test_span(1u, 200u);
}

static CmHirTypeId add_leaf_type(TestFixture *fixture, CmHirTypeKind kind)
{
    CmHirType type;
    CmHirTypeId id;

    memset(&type, 0, sizeof(type));
    type.kind = kind;
    type.span = test_span(2u, 4u);
    if (kind == CM_HIR_TYPE_INTEGER_KIND) {
        type.data.integer_type.kind = CM_HIR_INT_U32;
    }
    assert(cm_hir_add_type(&fixture->hir, &type, &id) == CM_HIR_OK);
    return id;
}

static CmHirTypeId add_projection_type(TestFixture *fixture)
{
    CmHirType type;
    CmHirTypeId id;

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PROJECTION_KIND;
    type.span = test_span(2u, 4u);
    type.data.projection_type.self_type = fixture->u32_type;
    type.data.projection_type.trait_type.definition =
        fixture->trait_definition;
    type.data.projection_type.associated_type.definition =
        fixture->trait_type;
    assert(cm_hir_add_type(&fixture->hir, &type, &id) == CM_HIR_OK);
    return id;
}

static CmHirDefId reserve_item(TestFixture *fixture, CmHirItemKind kind,
    uint32_t start)
{
    CmHirDefId definition;

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, kind, test_span(start, start + 20u),
        &definition) == CM_HIR_OK);
    return definition;
}

static void add_trait(TestFixture *fixture)
{
    CmHirItem item;
    CmHirItemId item_id;

    fixture->trait_definition = reserve_item(fixture,
        CM_HIR_ITEM_TRAIT, 10u);
    init_item(fixture, &item, CM_HIR_ITEM_TRAIT,
        fixture->trait_definition, "Convert");
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
}

static void add_trait_method(TestFixture *fixture)
{
    CmHirItem item;
    CmHirFunctionParameter parameter;
    CmHirItemId item_id;

    fixture->trait_method = reserve_item(fixture,
        CM_HIR_ITEM_FUNCTION, 35u);
    memset(&parameter, 0, sizeof(parameter));
    parameter.name = cm_hir_intern(&fixture->hir, "value");
    parameter.type = fixture->u32_type;
    parameter.span = test_span(38u, 42u);
    parameter.binding_kind = CM_HIR_BINDING_NAMED;
    init_item(fixture, &item, CM_HIR_ITEM_FUNCTION,
        fixture->trait_method, "convert");
    item.parent_definition = fixture->trait_definition;
    item.data.function_item.signature.parameters = &parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_NONE;
    item.data.function_item.signature.return_type = fixture->u32_type;
    item.data.function_item.signature.abi =
        cm_hir_intern(&fixture->hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = CM_HIR_BODY_NONE;
    item.data.function_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
}

static void add_trait_type(TestFixture *fixture)
{
    CmHirItem item;
    CmHirItemId item_id;

    fixture->trait_type = reserve_item(fixture,
        CM_HIR_ITEM_TYPE_ALIAS, 58u);
    init_item(fixture, &item, CM_HIR_ITEM_TYPE_ALIAS,
        fixture->trait_type, "Output");
    item.parent_definition = fixture->trait_definition;
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    item.data.type_alias_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
}

static void add_impl(TestFixture *fixture)
{
    CmHirItem item;
    CmHirItemId item_id;

    fixture->impl_definition = reserve_item(fixture,
        CM_HIR_ITEM_IMPL, 80u);
    init_item(fixture, &item, CM_HIR_ITEM_IMPL,
        fixture->impl_definition, NULL);
    item.data.impl_item.self_type = fixture->u32_type;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = fixture->trait_definition;
    item.data.impl_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
}

static void add_impl_method(TestFixture *fixture)
{
    CmHirFunctionParameter parameter;
    CmHirLocal local;
    CmHirBody body;
    CmHirBodyId body_id;
    CmHirItem item;
    CmHirItemId item_id;

    fixture->impl_method = reserve_item(fixture,
        CM_HIR_ITEM_FUNCTION, 105u);
    memset(&parameter, 0, sizeof(parameter));
    parameter.name = cm_hir_intern(&fixture->hir, "value");
    parameter.type = fixture->u32_type;
    parameter.span = test_span(108u, 112u);
    parameter.binding_kind = CM_HIR_BINDING_NAMED;
    memset(&local, 0, sizeof(local));
    local.name = parameter.name;
    local.type = parameter.type;
    local.span = parameter.span;
    local.parameter_index = 0u;
    memset(&body, 0, sizeof(body));
    body.owner = fixture->impl_method;
    body.origin = cm_hir_body_origin_item_source(fixture->impl_method);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = fixture->u32_type;
    body.locals = &local;
    body.local_count = 1u;
    body.parameter_count = 1u;
    body.source = 1u;
    body.source_expression_id = 1u;
    body.span = test_span(105u, 125u);
    assert(cm_hir_add_body(&fixture->hir, &body, &body_id) == CM_HIR_OK);
    init_item(fixture, &item, CM_HIR_ITEM_FUNCTION,
        fixture->impl_method, "convert");
    item.span = body.span;
    item.parent_definition = fixture->impl_definition;
    item.data.function_item.signature.parameters = &parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_NONE;
    item.data.function_item.signature.return_type = fixture->u32_type;
    item.data.function_item.signature.abi =
        cm_hir_intern(&fixture->hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = body_id;
    item.data.function_item.trait_item_definition = fixture->trait_method;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
}

static void add_impl_type(TestFixture *fixture)
{
    CmHirItem item;
    CmHirItemId item_id;

    fixture->impl_type = reserve_item(fixture,
        CM_HIR_ITEM_TYPE_ALIAS, 135u);
    init_item(fixture, &item, CM_HIR_ITEM_TYPE_ALIAS,
        fixture->impl_type, "Output");
    item.parent_definition = fixture->impl_definition;
    item.data.type_alias_item.target = fixture->u32_type;
    item.data.type_alias_item.trait_item_definition = fixture->trait_type;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
}

static CmHirItem *mutable_item(TestFixture *fixture,
    CmHirDefId definition);

static void fixture_init(TestFixture *fixture, int method, int assoc_type)
{
    memset(fixture, 0, sizeof(*fixture));
    cm_hir_context_init(&fixture->hir);
    assert(cm_hir_create_crate(&fixture->hir,
        cm_hir_intern(&fixture->hir, "semantic_item"),
        CM_HIR_EDITION_2024, test_span(0u, 200u), &fixture->crate_id,
        &fixture->root) == CM_HIR_OK);
    fixture->u32_type = add_leaf_type(fixture, CM_HIR_TYPE_INTEGER_KIND);
    fixture->bool_type = add_leaf_type(fixture, CM_HIR_TYPE_BOOL_KIND);
    add_trait(fixture);
    add_trait_method(fixture);
    if (assoc_type) add_trait_type(fixture);
    add_impl(fixture);
    if (method) add_impl_method(fixture);
    if (assoc_type) add_impl_type(fixture);
}

static void fixture_init_auto(TestFixture *fixture, CmHirSafety safety)
{
    memset(fixture, 0, sizeof(*fixture));
    cm_hir_context_init(&fixture->hir);
    assert(cm_hir_create_crate(&fixture->hir,
        cm_hir_intern(&fixture->hir, "semantic_item_auto"),
        CM_HIR_EDITION_2024, test_span(0u, 200u), &fixture->crate_id,
        &fixture->root) == CM_HIR_OK);
    fixture->u32_type = add_leaf_type(fixture, CM_HIR_TYPE_INTEGER_KIND);
    fixture->bool_type = add_leaf_type(fixture, CM_HIR_TYPE_BOOL_KIND);
    add_trait(fixture);
    add_impl(fixture);
    mutable_item(fixture, fixture->trait_definition)->data.trait_item
        .is_auto = 1;
    mutable_item(fixture, fixture->trait_definition)->data.trait_item
        .safety = safety;
    mutable_item(fixture, fixture->impl_definition)->data.impl_item.safety =
        safety;
}

static void fixture_destroy(TestFixture *fixture)
{
    cm_hir_context_destroy(&fixture->hir);
}

static CmHirItem *mutable_item(TestFixture *fixture, CmHirDefId definition)
{
    const CmHirDefinition *record;

    record = cm_hir_lookup_definition(&fixture->hir, definition);
    assert(record != NULL && record->kind == CM_HIR_DEFINITION_ITEM);
    return (CmHirItem *)cm_vec_at(&fixture->hir.items,
        (size_t)record->entity.item_id - 1u);
}

static CmHirBody *mutable_body(TestFixture *fixture, CmHirBodyId body_id)
{
    assert(body_id != CM_HIR_BODY_NONE);
    return (CmHirBody *)cm_vec_at(&fixture->hir.bodies,
        (size_t)body_id - 1u);
}

static CmHirGenericParam *mutable_generic_parameter(
    TestFixture *fixture, CmHirGenericParamId parameter_id)
{
    assert(parameter_id != CM_HIR_GENERIC_PARAM_NONE);
    return (CmHirGenericParam *)cm_vec_at(&fixture->hir.generic_parameters,
        (size_t)parameter_id - 1u);
}

static CmHirGenericParamId make_impl_type_generic(TestFixture *fixture,
    CmHirTypeId *out_parameter_type, CmHirTypeId *out_trait_self_type)
{
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_id;
    CmHirType type;
    CmHirItem *impl_item;
    CmHirItem *trait_method;
    CmHirItem *impl_method;
    CmHirBody *body;

    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = fixture->impl_definition;
    parameter.index = 0u;
    parameter.name = cm_hir_intern(&fixture->hir, "T");
    parameter.span = test_span(82u, 83u);
    assert(cm_hir_add_generic_param(&fixture->hir, &parameter,
        &parameter_id) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = parameter.span;
    type.data.parameter_type.parameter = parameter_id;
    assert(cm_hir_add_type(&fixture->hir, &type, out_parameter_type)
        == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_SELF_KIND;
    type.span = test_span(38u, 39u);
    type.data.self_type.owner = fixture->trait_definition;
    assert(cm_hir_add_type(&fixture->hir, &type, out_trait_self_type)
        == CM_HIR_OK);

    impl_item = mutable_item(fixture, fixture->impl_definition);
    impl_item->generic_parameter_start = parameter_id;
    impl_item->generic_parameter_count = 1u;
    impl_item->data.impl_item.self_type = *out_parameter_type;
    trait_method = mutable_item(fixture, fixture->trait_method);
    trait_method->data.function_item.signature.parameters[0].type =
        *out_trait_self_type;
    trait_method->data.function_item.signature.return_type =
        *out_trait_self_type;
    impl_method = mutable_item(fixture, fixture->impl_method);
    impl_method->data.function_item.signature.parameters[0].type =
        *out_parameter_type;
    impl_method->data.function_item.signature.return_type =
        *out_parameter_type;
    body = mutable_body(fixture, impl_method->data.function_item.body);
    assert(body != NULL && body->local_count == 1u);
    body->expected_type = *out_parameter_type;
    body->locals[0].type = *out_parameter_type;
    return parameter_id;
}

static void give_trait_method_default_body(TestFixture *fixture)
{
    CmHirBody body;
    CmHirBodyId body_id;
    CmHirItem *method;

    memset(&body, 0, sizeof(body));
    body.owner = fixture->trait_method;
    body.origin = cm_hir_body_origin_item_source(fixture->trait_method);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = fixture->u32_type;
    body.source = 1u;
    body.source_expression_id = 1u;
    body.span = test_span(35u, 55u);
    assert(cm_hir_add_body(&fixture->hir, &body, &body_id) == CM_HIR_OK);
    method = mutable_item(fixture, fixture->trait_method);
    method->data.function_item.body = body_id;
    method->data.function_item.has_default_body = 1;
}

static void add_duplicate_impl_method(TestFixture *fixture)
{
    const CmHirItem *source;
    CmHirItem duplicate;

    source = mutable_item(fixture, fixture->impl_method);
    duplicate = *source;
    (void)cm_vec_push(&fixture->hir.items, &duplicate);
}

static void test_positive_and_signature_mismatches(void)
{
    TestFixture fixture;
    CmSemanticItemResult result;
    CmHirItem *method;
    CmHirFunctionSignature saved;

    fixture_init(&fixture, 1, 1);
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_OK);
    method = mutable_item(&fixture, fixture.impl_method);
    saved = method->data.function_item.signature;

    method->data.function_item.signature.parameters[0].type =
        fixture.bool_type;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_PARAMETER_TYPE_MISMATCH
        && result.parameter_index == 0u
        && cm_hir_def_id_equal(result.impl_member, fixture.impl_method)
        && cm_hir_def_id_equal(result.trait_member, fixture.trait_method));
    method->data.function_item.signature.parameters[0].type =
        fixture.u32_type;
    method->data.function_item.signature = saved;

    method->data.function_item.signature.return_type = fixture.bool_type;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_RETURN_TYPE_MISMATCH);
    method->data.function_item.signature = saved;

    method->data.function_item.signature.receiver = CM_HIR_RECEIVER_VALUE;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_RECEIVER_MISMATCH);
    method->data.function_item.signature = saved;

    method->data.function_item.signature.safety = CM_HIR_UNSAFE;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_SAFETY_MISMATCH);
    method->data.function_item.signature = saved;

    method->data.function_item.signature.is_const = 1;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_CONST_MISMATCH);
    method->data.function_item.signature = saved;

    method->data.function_item.signature.parameter_count = 0u;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_PARAMETER_COUNT_MISMATCH);
    method->data.function_item.signature = saved;

    method->data.function_item.signature.abi =
        cm_hir_intern(&fixture.hir, "C");
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_ABI_MISMATCH);
    method->data.function_item.signature = saved;

    method->data.function_item.signature.is_async = 1;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_ASYNC_MISMATCH);
    method->data.function_item.signature = saved;

    method->data.function_item.signature.is_variadic = 1;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_VARIADIC_MISMATCH);
    method->data.function_item.signature = saved;
    fixture_destroy(&fixture);
}

static void test_finalized_projection_signature(void)
{
    TestFixture fixture;
    CmHirCrateFinalization finalization;
    CmProjectionNormalizeLimits limits;
    CmSemanticItemResult result;
    CmHirItem *trait_method;
    CmHirTypeId projection;

    fixture_init(&fixture, 1, 1);
    projection = add_projection_type(&fixture);
    trait_method = mutable_item(&fixture, fixture.trait_method);
    trait_method->data.function_item.signature.parameters[0].type =
        projection;
    trait_method->data.function_item.signature.return_type = projection;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_PENDING_PROJECTION);

    memset(&finalization, 0, sizeof(finalization));
    assert(cm_hir_crate_finalization_init(&finalization, &fixture.hir,
        fixture.crate_id) == CM_HIR_OK);
    limits.max_nodes = 4096u;
    limits.max_projection_steps = 256u;
    result = cm_semantic_item_check_finalized_local_trait_impls(
        &finalization, limits);
    assert(result.status == CM_SEMANTIC_ITEM_OK
        && result.solver_kind == CM_TRAIT_SOLVER_PROVEN);

    mutable_item(&fixture, fixture.impl_method)
        ->data.function_item.signature.return_type = fixture.bool_type;
    result = cm_semantic_item_check_finalized_local_trait_impls(
        &finalization, limits);
    assert(result.status == CM_SEMANTIC_ITEM_RETURN_TYPE_MISMATCH
        && cm_hir_def_id_equal(result.impl_member, fixture.impl_method)
        && cm_hir_def_id_equal(result.trait_member, fixture.trait_method));
    mutable_item(&fixture, fixture.impl_method)
        ->data.function_item.signature.return_type = fixture.u32_type;

    limits.max_nodes = 0u;
    result = cm_semantic_item_check_finalized_local_trait_impls(
        &finalization, limits);
    assert(result.status == CM_SEMANTIC_ITEM_INVALID);
    limits.max_nodes = 4096u;
    cm_hir_crate_finalization_destroy(&finalization);
    assert(cm_hir_crate_finalization_init(&finalization, &fixture.hir,
        fixture.crate_id) == CM_HIR_OK);
    (void)add_leaf_type(&fixture, CM_HIR_TYPE_UNIT_KIND);
    result = cm_semantic_item_check_finalized_local_trait_impls(
        &finalization, limits);
    assert(result.status == CM_SEMANTIC_ITEM_INVALID);
    cm_hir_crate_finalization_destroy(&finalization);
    fixture_destroy(&fixture);
}

static void test_default_method_rules(void)
{
    TestFixture fixture;
    CmSemanticItemResult result;
    CmHirItem *method;

    fixture_init(&fixture, 0, 0);
    method = mutable_item(&fixture, fixture.trait_method);
    method->data.function_item.has_default_body = 1;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_OK
        && method->data.function_item.body == CM_HIR_BODY_NONE);
    fixture_destroy(&fixture);

    fixture_init(&fixture, 0, 0);
    give_trait_method_default_body(&fixture);
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_OK);
    fixture_destroy(&fixture);

    fixture_init(&fixture, 1, 0);
    give_trait_method_default_body(&fixture);
    method = mutable_item(&fixture, fixture.impl_method);
    method->data.function_item.signature.parameters[0].type =
        fixture.bool_type;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_PARAMETER_TYPE_MISMATCH);
    fixture_destroy(&fixture);
}

static void test_trait_argument_and_self_instantiation(void)
{
    TestFixture fixture;
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_id;
    CmHirType type;
    CmHirTypeId parameter_type;
    CmHirTypeId self_type;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirGenericArg argument;
    CmHirItem *method;
    CmHirItem *impl_item;
    CmHirDefId foreign_owner;
    CmHirGenericParamId foreign_parameter;
    CmHirTypeId foreign_parameter_type;
    CmSemanticItemResult result;

    memset(&fixture, 0, sizeof(fixture));
    cm_hir_context_init(&fixture.hir);
    assert(cm_hir_create_crate(&fixture.hir,
        cm_hir_intern(&fixture.hir, "semantic_item_generic"),
        CM_HIR_EDITION_2024, test_span(0u, 200u), &fixture.crate_id,
        &fixture.root) == CM_HIR_OK);
    fixture.u32_type = add_leaf_type(&fixture, CM_HIR_TYPE_INTEGER_KIND);
    fixture.bool_type = add_leaf_type(&fixture, CM_HIR_TYPE_BOOL_KIND);

    fixture.trait_definition = reserve_item(&fixture,
        CM_HIR_ITEM_TRAIT, 10u);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = fixture.trait_definition;
    parameter.name = cm_hir_intern(&fixture.hir, "T");
    parameter.span = test_span(20u, 21u);
    assert(cm_hir_add_generic_param(&fixture.hir, &parameter,
        &parameter_id) == CM_HIR_OK);
    init_item(&fixture, &item, CM_HIR_ITEM_TRAIT,
        fixture.trait_definition, "Convert");
    item.generic_parameter_start = parameter_id;
    item.generic_parameter_count = 1u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture.hir, &item, &item_id) == CM_HIR_OK);

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = test_span(22u, 23u);
    type.data.parameter_type.parameter = parameter_id;
    assert(cm_hir_add_type(&fixture.hir, &type, &parameter_type)
        == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_SELF_KIND;
    type.span = test_span(23u, 24u);
    type.data.self_type.owner = fixture.trait_definition;
    assert(cm_hir_add_type(&fixture.hir, &type, &self_type) == CM_HIR_OK);

    add_trait_method(&fixture);
    method = mutable_item(&fixture, fixture.trait_method);
    method->data.function_item.signature.parameters[0].type = parameter_type;
    method->data.function_item.signature.return_type = self_type;

    fixture.impl_definition = reserve_item(&fixture,
        CM_HIR_ITEM_IMPL, 80u);
    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    argument.data.type = fixture.u32_type;
    init_item(&fixture, &item, CM_HIR_ITEM_IMPL,
        fixture.impl_definition, NULL);
    item.data.impl_item.self_type = fixture.u32_type;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = fixture.trait_definition;
    item.data.impl_item.trait_type.arguments = &argument;
    item.data.impl_item.trait_type.argument_count = 1u;
    item.data.impl_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture.hir, &item, &item_id) == CM_HIR_OK);
    add_impl_method(&fixture);

    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_OK);
    method = mutable_item(&fixture, fixture.impl_method);
    method->data.function_item.signature.return_type = fixture.bool_type;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_RETURN_TYPE_MISMATCH);
    method->data.function_item.signature.return_type = fixture.u32_type;

    foreign_owner = reserve_item(&fixture, CM_HIR_ITEM_FUNCTION, 165u);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = foreign_owner;
    parameter.name = cm_hir_intern(&fixture.hir, "Foreign");
    parameter.span = test_span(168u, 169u);
    assert(cm_hir_add_generic_param(&fixture.hir, &parameter,
        &foreign_parameter) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = test_span(169u, 170u);
    type.data.parameter_type.parameter = foreign_parameter;
    assert(cm_hir_add_type(&fixture.hir, &type, &foreign_parameter_type)
        == CM_HIR_OK);
    impl_item = mutable_item(&fixture, fixture.impl_definition);
    impl_item->data.impl_item.trait_type.arguments[0].data.type =
        foreign_parameter_type;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_PENDING_GENERIC);
    fixture_destroy(&fixture);
}

static void test_type_generic_impl_method_conformance(void)
{
    TestFixture fixture;
    CmHirGenericParamId impl_parameter;
    CmHirTypeId parameter_type;
    CmHirTypeId trait_self_type;
    CmHirItem *impl_method;
    CmHirItem *trait_item;
    CmHirGenericParam *parameter;
    CmHirGenericParam method_parameter;
    CmHirGenericParamId method_parameter_id;
    CmHirGenericParam trait_parameter;
    CmHirGenericParamId trait_parameter_id;
    CmHirType trait_parameter_hir_type;
    CmHirTypeId trait_parameter_type;
    CmHirGenericArg trait_argument;
    CmHirTraitPredicate impl_predicate;
    CmHirCrateFinalization finalization;
    CmProjectionNormalizeLimits limits;
    CmSemanticItemResult result;

    fixture_init(&fixture, 1, 0);
    impl_parameter = make_impl_type_generic(&fixture, &parameter_type,
        &trait_self_type);
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_OK);

    memset(&impl_predicate, 0, sizeof(impl_predicate));
    impl_predicate.subject = parameter_type;
    impl_predicate.trait_type.definition = fixture.trait_definition;
    impl_predicate.modifier = CM_HIR_PREDICATE_REQUIRED;
    mutable_item(&fixture, fixture.impl_definition)->predicates =
        &impl_predicate;
    mutable_item(&fixture, fixture.impl_definition)->predicate_count = 1u;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_OK);
    mutable_item(&fixture, fixture.impl_definition)->predicates = NULL;
    mutable_item(&fixture, fixture.impl_definition)->predicate_count = 0u;

    memset(&finalization, 0, sizeof(finalization));
    assert(cm_hir_crate_finalization_init(&finalization, &fixture.hir,
        fixture.crate_id) == CM_HIR_OK);
    limits.max_nodes = 4096u;
    limits.max_projection_steps = 256u;
    result = cm_semantic_item_check_finalized_local_trait_impls(
        &finalization, limits);
    assert(result.status == CM_SEMANTIC_ITEM_OK
        && result.solver_kind == CM_TRAIT_SOLVER_PROVEN);

    impl_method = mutable_item(&fixture, fixture.impl_method);
    impl_method->data.function_item.signature.parameters[0].type =
        fixture.u32_type;
    result = cm_semantic_item_check_finalized_local_trait_impls(
        &finalization, limits);
    assert(result.status == CM_SEMANTIC_ITEM_PARAMETER_TYPE_MISMATCH
        && result.parameter_index == 0u);
    impl_method->data.function_item.signature.parameters[0].type =
        parameter_type;
    impl_method->data.function_item.signature.return_type = fixture.u32_type;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_RETURN_TYPE_MISMATCH);
    impl_method->data.function_item.signature.return_type = parameter_type;
    cm_hir_crate_finalization_destroy(&finalization);

    parameter = mutable_generic_parameter(&fixture, impl_parameter);
    parameter->kind = CM_HIR_GENERIC_LIFETIME;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_PENDING_GENERIC);
    parameter->kind = CM_HIR_GENERIC_CONST;
    parameter->declared_type = fixture.u32_type;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_PENDING_GENERIC);
    parameter->kind = CM_HIR_GENERIC_TYPE;
    parameter->declared_type = CM_HIR_TYPE_NONE;
    parameter->has_default = 1;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_PENDING_GENERIC);
    parameter->has_default = 0;
    parameter->is_relaxed_sized = 1;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_PENDING_GENERIC);
    parameter->is_relaxed_sized = 0;

    memset(&method_parameter, 0, sizeof(method_parameter));
    method_parameter.kind = CM_HIR_GENERIC_TYPE;
    method_parameter.owner = fixture.impl_method;
    method_parameter.index = 0u;
    method_parameter.name = cm_hir_intern(&fixture.hir, "U");
    method_parameter.span = test_span(106u, 107u);
    assert(cm_hir_add_generic_param(&fixture.hir, &method_parameter,
        &method_parameter_id) == CM_HIR_OK);
    impl_method->generic_parameter_start = method_parameter_id;
    impl_method->generic_parameter_count = 1u;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_PENDING_GENERIC);
    mutable_generic_parameter(&fixture, method_parameter_id)->has_default = 1;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_PENDING_GENERIC);
    mutable_generic_parameter(&fixture, method_parameter_id)->has_default = 0;
    impl_method->generic_parameter_start = CM_HIR_GENERIC_PARAM_NONE;
    impl_method->generic_parameter_count = 0u;

    memset(&trait_parameter, 0, sizeof(trait_parameter));
    trait_parameter.kind = CM_HIR_GENERIC_TYPE;
    trait_parameter.owner = fixture.trait_definition;
    trait_parameter.index = 0u;
    trait_parameter.name = cm_hir_intern(&fixture.hir, "A");
    trait_parameter.span = test_span(12u, 13u);
    assert(cm_hir_add_generic_param(&fixture.hir, &trait_parameter,
        &trait_parameter_id) == CM_HIR_OK);
    memset(&trait_parameter_hir_type, 0, sizeof(trait_parameter_hir_type));
    trait_parameter_hir_type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    trait_parameter_hir_type.span = test_span(13u, 14u);
    trait_parameter_hir_type.data.parameter_type.parameter =
        trait_parameter_id;
    assert(cm_hir_add_type(&fixture.hir, &trait_parameter_hir_type,
        &trait_parameter_type) == CM_HIR_OK);
    memset(&trait_argument, 0, sizeof(trait_argument));
    trait_argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    trait_argument.data.type = parameter_type;
    trait_item = mutable_item(&fixture, fixture.trait_definition);
    trait_item->generic_parameter_start = trait_parameter_id;
    trait_item->generic_parameter_count = 1u;
    trait_item = mutable_item(&fixture, fixture.trait_method);
    trait_item->data.function_item.signature.parameters[0].type =
        trait_parameter_type;
    trait_item->data.function_item.signature.return_type =
        trait_parameter_type;
    mutable_item(&fixture, fixture.impl_definition)
        ->data.impl_item.trait_type.arguments = &trait_argument;
    mutable_item(&fixture, fixture.impl_definition)
        ->data.impl_item.trait_type.argument_count = 1u;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_OK);
    fixture_destroy(&fixture);

    fixture_init(&fixture, 1, 1);
    (void)make_impl_type_generic(&fixture, &parameter_type,
        &trait_self_type);
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_PENDING_GENERIC
        && cm_hir_def_id_equal(result.trait_member, fixture.trait_type));
    fixture_destroy(&fixture);
}

static void test_foreign_parameter_terms_are_pending(void)
{
    TestFixture fixture;
    CmHirDefId foreign_owner;
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_id;
    CmHirType type;
    CmHirTypeId parameter_type;
    CmHirItem *impl_item;
    CmHirItem *impl_method;
    CmHirItem *impl_type;
    CmSemanticItemResult result;

    fixture_init(&fixture, 1, 1);
    foreign_owner = reserve_item(&fixture, CM_HIR_ITEM_FUNCTION, 165u);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = foreign_owner;
    parameter.name = cm_hir_intern(&fixture.hir, "Foreign");
    parameter.span = test_span(168u, 169u);
    assert(cm_hir_add_generic_param(&fixture.hir, &parameter,
        &parameter_id) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = test_span(169u, 170u);
    type.data.parameter_type.parameter = parameter_id;
    assert(cm_hir_add_type(&fixture.hir, &type, &parameter_type)
        == CM_HIR_OK);

    impl_type = mutable_item(&fixture, fixture.impl_type);
    impl_type->data.type_alias_item.target = parameter_type;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_PENDING_GENERIC);
    impl_type->data.type_alias_item.target = fixture.u32_type;

    impl_method = mutable_item(&fixture, fixture.impl_method);
    impl_method->data.function_item.signature.parameters[0].type =
        parameter_type;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_PENDING_GENERIC);
    impl_method->data.function_item.signature.parameters[0].type =
        fixture.u32_type;

    impl_item = mutable_item(&fixture, fixture.impl_definition);
    impl_item->data.impl_item.self_type = parameter_type;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_PENDING_GENERIC);
    fixture_destroy(&fixture);
}

static void test_missing_required_members(void)
{
    TestFixture fixture;
    CmSemanticItemResult result;

    fixture_init(&fixture, 0, 0);
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_MISSING_REQUIRED_METHOD);
    fixture_destroy(&fixture);

    fixture_init(&fixture, 1, 1);
    mutable_item(&fixture, fixture.impl_type)->data.type_alias_item
        .trait_item_definition = cm_hir_def_id_none();
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_MISSING_ASSOCIATED_TYPE);
    fixture_destroy(&fixture);
}

static void test_association_failures(void)
{
    TestFixture fixture;
    CmSemanticItemResult result;
    CmHirItem *impl_type;

    fixture_init(&fixture, 1, 0);
    add_duplicate_impl_method(&fixture);
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_DUPLICATE_ASSOCIATED_ITEM
        && cm_hir_def_id_equal(result.trait_member, fixture.trait_method));
    fixture_destroy(&fixture);

    fixture_init(&fixture, 1, 1);
    impl_type = mutable_item(&fixture, fixture.impl_type);
    impl_type->data.type_alias_item.trait_item_definition =
        fixture.trait_method;
    mutable_item(&fixture, fixture.impl_method)->data.function_item
        .trait_item_definition = fixture.trait_type;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_WRONG_ASSOCIATION
        && cm_hir_def_id_equal(result.trait_member, fixture.trait_method));
    fixture_destroy(&fixture);
}

static void test_associated_type_default_is_pending(void)
{
    TestFixture fixture;
    CmSemanticItemResult result;

    fixture_init(&fixture, 1, 1);
    mutable_item(&fixture, fixture.trait_type)->data.type_alias_item.target =
        fixture.u32_type;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_PENDING_DEFAULT
        && cm_hir_def_id_equal(result.trait_member, fixture.trait_type));
    fixture_destroy(&fixture);
}

static void test_associated_type_bounds_are_pending(void)
{
    TestFixture fixture;
    CmSemanticItemResult result;
    CmHirItem *trait_type;
    CmHirItem *impl_type;
    CmHirAssociatedTypeBound bound;
    CmHirTraitPredicate predicate;
    CmHirOutlivesPredicate outlives;

    fixture_init(&fixture, 1, 1);
    memset(&bound, 0, sizeof(bound));
    trait_type = mutable_item(&fixture, fixture.trait_type);
    trait_type->data.type_alias_item.bounds = &bound;
    trait_type->data.type_alias_item.bound_count = 1u;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_PENDING_PREDICATE);
    bound.equality_count = 1u;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_PENDING_PROJECTION);
    trait_type->data.type_alias_item.bound_count = 0u;
    trait_type->data.type_alias_item.bounds = NULL;
    trait_type->generic_parameter_count = 1u;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_PENDING_GENERIC);
    trait_type->generic_parameter_count = 0u;

    memset(&predicate, 0, sizeof(predicate));
    trait_type->predicates = &predicate;
    trait_type->predicate_count = 1u;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_PENDING_PREDICATE);
    trait_type->predicates = NULL;
    trait_type->predicate_count = 0u;

    memset(&outlives, 0, sizeof(outlives));
    trait_type->outlives_predicates = &outlives;
    trait_type->outlives_predicate_count = 1u;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_PENDING_OUTLIVES);
    trait_type->outlives_predicates = NULL;
    trait_type->outlives_predicate_count = 0u;

    impl_type = mutable_item(&fixture, fixture.impl_type);
    impl_type->predicates = &predicate;
    impl_type->predicate_count = 1u;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_PENDING_PREDICATE);
    fixture_destroy(&fixture);
}

static void test_pending_and_invalid(void)
{
    TestFixture fixture;
    CmHirCrateFinalization finalization;
    CmProjectionNormalizeLimits limits;
    CmSemanticItemResult result;
    CmHirItem *impl_item;
    CmHirItem *method;
    CmHirTraitPredicate predicate;
    CmHirOutlivesPredicate outlives;
    CmHirPredicateScope scope;
    CmHirType *return_type;

    fixture_init(&fixture, 0, 0);
    impl_item = mutable_item(&fixture, fixture.impl_definition);
    impl_item->data.impl_item.is_negative = 1;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_OK);
    memset(&finalization, 0, sizeof(finalization));
    assert(cm_hir_crate_finalization_init(&finalization, &fixture.hir,
        fixture.crate_id) == CM_HIR_OK);
    limits.max_nodes = 4096u;
    limits.max_projection_steps = 256u;
    result = cm_semantic_item_check_finalized_local_trait_impls(
        &finalization, limits);
    assert(result.status == CM_SEMANTIC_ITEM_OK
        && result.solver_kind == CM_TRAIT_SOLVER_PROVEN);
    cm_hir_crate_finalization_destroy(&finalization);

    impl_item->data.impl_item.safety = CM_HIR_UNSAFE;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_SAFETY_MISMATCH);
    impl_item->data.impl_item.safety = CM_HIR_SAFE;
    impl_item->data.impl_item.is_negative = 0;
    impl_item->data.impl_item.trait_type.definition.crate_id += 10u;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_PENDING_CROSS_CRATE);
    fixture_destroy(&fixture);

    fixture_init(&fixture, 1, 0);
    impl_item = mutable_item(&fixture, fixture.impl_definition);
    impl_item->data.impl_item.is_negative = 1;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_WRONG_ASSOCIATION
        && cm_hir_def_id_equal(result.impl_member,
            fixture.impl_method));
    fixture_destroy(&fixture);

    fixture_init(&fixture, 1, 0);
    mutable_item(&fixture, fixture.trait_definition)->data.trait_item.is_auto
        = 1;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_WRONG_ASSOCIATION
        && cm_hir_def_id_equal(result.trait_member,
            fixture.trait_method));
    fixture_destroy(&fixture);

    fixture_init(&fixture, 1, 0);
    impl_item = mutable_item(&fixture, fixture.impl_definition);
    impl_item->generic_parameter_count = 1u;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_PENDING_GENERIC);
    impl_item->generic_parameter_count = 0u;

    memset(&scope, 0, sizeof(scope));
    impl_item->predicate_scopes = &scope;
    impl_item->predicate_scope_count = 1u;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_PENDING_HIGHER_RANKED);
    impl_item->predicate_scopes = NULL;
    impl_item->predicate_scope_count = 0u;

    memset(&outlives, 0, sizeof(outlives));
    impl_item->outlives_predicates = &outlives;
    impl_item->outlives_predicate_count = 1u;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_PENDING_OUTLIVES);
    impl_item->outlives_predicates = NULL;
    impl_item->outlives_predicate_count = 0u;

    memset(&predicate, 0, sizeof(predicate));
    predicate.subject = fixture.u32_type;
    predicate.trait_type.definition = fixture.trait_definition;
    predicate.modifier = CM_HIR_PREDICATE_REQUIRED;
    impl_item->predicates = &predicate;
    impl_item->predicate_count = 1u;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_OK);
    predicate.equality_count = 1u;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_PENDING_PROJECTION);
    impl_item->predicates = NULL;
    impl_item->predicate_count = 0u;

    method = mutable_item(&fixture, fixture.impl_method);
    return_type = (CmHirType *)cm_vec_at(&fixture.hir.types,
        (size_t)fixture.bool_type - 1u);
    assert(return_type != NULL);
    return_type->kind = CM_HIR_TYPE_PROJECTION_KIND;
    method->data.function_item.signature.return_type = fixture.bool_type;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_INVALID);
    fixture_destroy(&fixture);

    result = cm_semantic_item_check_local_trait_impls(NULL,
        CM_HIR_CRATE_NONE);
    assert(result.status == CM_SEMANTIC_ITEM_INVALID);
    assert(strcmp(cm_semantic_item_status_name(
        CM_SEMANTIC_ITEM_PENDING_SPECIALIZATION),
        "pending-specialization") == 0);
    assert(strcmp(cm_semantic_item_status_name(
        CM_SEMANTIC_ITEM_PARAMETER_TYPE_MISMATCH),
        "parameter-type-mismatch") == 0);
}

static void test_explicit_auto_trait_impl_headers(void)
{
    TestFixture fixture;
    CmHirCrateFinalization finalization;
    CmProjectionNormalizeLimits limits;
    CmSemanticItemResult result;
    CmHirItem *impl_item;
    CmHirItem *trait_item;
    CmHirItem forged_member;

    fixture_init_auto(&fixture, CM_HIR_UNSAFE);
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_OK);
    memset(&finalization, 0, sizeof(finalization));
    assert(cm_hir_crate_finalization_init(&finalization, &fixture.hir,
        fixture.crate_id) == CM_HIR_OK);
    limits.max_nodes = 4096u;
    limits.max_projection_steps = 256u;
    result = cm_semantic_item_check_finalized_local_trait_impls(
        &finalization, limits);
    assert(result.status == CM_SEMANTIC_ITEM_OK
        && result.solver_kind == CM_TRAIT_SOLVER_PROVEN);
    cm_hir_crate_finalization_destroy(&finalization);

    impl_item = mutable_item(&fixture, fixture.impl_definition);
    impl_item->data.impl_item.safety = CM_HIR_SAFE;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_SAFETY_MISMATCH);
    impl_item->data.impl_item.safety = CM_HIR_UNSAFE;

    fixture.impl_method = reserve_item(&fixture, CM_HIR_ITEM_FUNCTION,
        105u);
    init_item(&fixture, &forged_member, CM_HIR_ITEM_FUNCTION,
        fixture.impl_method, "forged");
    forged_member.parent_definition = fixture.impl_definition;
    assert(cm_vec_push(&fixture.hir.items, &forged_member));
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_WRONG_ASSOCIATION
        && cm_hir_def_id_equal(result.impl_member,
            fixture.impl_method));
    fixture_destroy(&fixture);

    fixture_init_auto(&fixture, CM_HIR_SAFE);
    trait_item = mutable_item(&fixture, fixture.trait_definition);
    trait_item->generic_parameter_count = 1u;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_INVALID);
    fixture_destroy(&fixture);
}

static void test_specialization_is_a_hard_barrier(void)
{
    TestFixture fixture;
    CmHirCrateFinalization finalization;
    CmProjectionNormalizeLimits limits;
    CmSemanticItemResult result;

    fixture_init(&fixture, 1, 1);
    mutable_item(&fixture, fixture.impl_method)->is_specializable = 1;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_PENDING_SPECIALIZATION
        && cm_hir_def_id_equal(result.impl_definition,
            fixture.impl_definition)
        && cm_hir_def_id_equal(result.trait_definition,
            fixture.trait_definition)
        && cm_hir_def_id_equal(result.impl_member, fixture.impl_method)
        && cm_hir_def_id_equal(result.trait_member,
            fixture.trait_method));

    memset(&finalization, 0, sizeof(finalization));
    assert(cm_hir_crate_finalization_init(&finalization, &fixture.hir,
        fixture.crate_id) == CM_HIR_OK);
    limits.max_nodes = 4096u;
    limits.max_projection_steps = 256u;
    result = cm_semantic_item_check_finalized_local_trait_impls(
        &finalization, limits);
    assert(result.status == CM_SEMANTIC_ITEM_PENDING_SPECIALIZATION
        && cm_hir_def_id_equal(result.impl_member, fixture.impl_method));
    cm_hir_crate_finalization_destroy(&finalization);

    mutable_item(&fixture, fixture.impl_method)->is_specializable = 0;
    mutable_item(&fixture, fixture.impl_type)->is_specializable = 1;
    result = cm_semantic_item_check_local_trait_impls(&fixture.hir,
        fixture.crate_id);
    assert(result.status == CM_SEMANTIC_ITEM_PENDING_SPECIALIZATION
        && cm_hir_def_id_equal(result.impl_member, fixture.impl_type)
        && cm_hir_def_id_equal(result.trait_member, fixture.trait_type));
    fixture_destroy(&fixture);
}

int main(void)
{
    test_positive_and_signature_mismatches();
    test_finalized_projection_signature();
    test_default_method_rules();
    test_trait_argument_and_self_instantiation();
    test_type_generic_impl_method_conformance();
    test_foreign_parameter_terms_are_pending();
    test_missing_required_members();
    test_association_failures();
    test_associated_type_default_is_pending();
    test_associated_type_bounds_are_pending();
    test_pending_and_invalid();
    test_explicit_auto_trait_impl_headers();
    test_specialization_is_a_hard_barrier();
    puts("hir semantic item tests passed");
    return 0;
}
