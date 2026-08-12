#include "cm/hir/semantic_body.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct TestFixture {
    CmHirContext hir;
    CmHirCrateId crate_id;
    CmHirModuleId root;
    CmHirTypeId u32_type;
    CmHirTypeId infer_type;
    CmHirDefId present_trait;
    CmHirDefId missing_trait;
    CmHirDefId present_impl;
    CmHirDefId present_callee;
    CmHirDefId missing_callee;
    CmHirBodyId present_callee_body;
    CmHirBodyId missing_callee_body;
    CmHirDefId present_caller;
    CmHirDefId missing_caller;
    CmHirBodyId present_body;
    CmHirBodyId missing_body;
    CmHirExprId present_call;
    CmHirExprId missing_call;
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
    CmHirDefId definition, CmHirModuleId module, const char *name,
    CmHirContext *hir)
{
    memset(item, 0, sizeof(*item));
    item->kind = kind;
    item->definition = definition;
    item->owner_module = module;
    item->parent_definition = cm_hir_def_id_none();
    item->name = name == NULL ? CM_INTERN_ID_NONE
        : cm_hir_intern(hir, name);
    item->visibility.kind = CM_HIR_VIS_PRIVATE;
    item->visibility.restriction = cm_hir_def_id_none();
    item->span = test_span(1u, 240u);
}

static CmHirTypeId add_type(CmHirContext *hir, CmHirTypeKind kind)
{
    CmHirType type;
    CmHirTypeId id;

    memset(&type, 0, sizeof(type));
    type.kind = kind;
    type.span = test_span(2u, 5u);
    if (kind == CM_HIR_TYPE_INTEGER_KIND) {
        type.data.integer_type.kind = CM_HIR_INT_U32;
    } else if (kind == CM_HIR_TYPE_INFER_KIND) {
        type.data.infer_type.kind = CM_HIR_INFER_GENERAL;
        type.data.infer_type.variable = 1u;
    }
    assert(cm_hir_add_type(hir, &type, &id) == CM_HIR_OK);
    return id;
}

static CmHirStatus add_local_expression(CmHirContext *hir,
    CmHirBodyId body, uint32_t local_index, CmHirTypeId type, CmSpan span,
    CmHirExprId *out_expression)
{
    CmHirExpr expression;

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = body;
    expression.type = type;
    expression.span = span;
    expression.data.local.local_index = local_index;
    return cm_hir_add_expr(hir, &expression, out_expression);
}

static CmHirStatus add_call_expression(CmHirContext *hir,
    CmHirBodyId body, CmHirDefId callee,
    const CmHirTypeId *type_substitutions, uint32_t substitution_count,
    const CmHirExprId *arguments, uint32_t argument_count,
    CmHirTypeId result_type, CmSpan span, CmHirExprId *out_expression)
{
    CmHirExpr expression;

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_CALL;
    expression.owner_body = body;
    expression.type = result_type;
    expression.span = span;
    expression.data.call.callee = callee;
    expression.data.call.type_substitutions =
        (CmHirTypeId *)type_substitutions;
    expression.data.call.type_substitution_count = substitution_count;
    expression.data.call.arguments = (CmHirExprId *)arguments;
    expression.data.call.argument_count = argument_count;
    return cm_hir_add_expr(hir, &expression, out_expression);
}

static CmHirDefId add_trait(TestFixture *fixture, const char *name)
{
    CmHirDefId definition;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_TRAIT, test_span(1u, 20u),
        &definition) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_TRAIT, definition, fixture->root, name,
        &fixture->hir);
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_impl(TestFixture *fixture, CmHirDefId trait_definition)
{
    CmHirDefId definition;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_IMPL, test_span(20u, 35u),
        &definition) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_IMPL, definition, fixture->root, NULL,
        &fixture->hir);
    item.data.impl_item.self_type = fixture->u32_type;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = trait_definition;
    item.data.impl_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_bounded_identity(TestFixture *fixture,
    const char *name, CmHirDefId trait_definition, uint32_t base,
    CmHirBodyId *out_body)
{
    CmHirDefId definition;
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_id;
    CmHirType parameter_type_value;
    CmHirTypeId parameter_type;
    CmHirTraitPredicate predicate;
    CmHirFunctionParameter function_parameter;
    CmHirLocal local;
    CmHirBody body;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirExprId root;

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_FUNCTION,
        test_span(base, base + 39u), &definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = definition;
    parameter.index = 0u;
    parameter.name = cm_hir_intern(&fixture->hir, "T");
    parameter.span = test_span(base + 1u, base + 2u);
    assert(cm_hir_add_generic_param(&fixture->hir, &parameter,
        &parameter_id) == CM_HIR_OK);
    memset(&parameter_type_value, 0, sizeof(parameter_type_value));
    parameter_type_value.kind = CM_HIR_TYPE_PARAMETER_KIND;
    parameter_type_value.span = parameter.span;
    parameter_type_value.data.parameter_type.parameter = parameter_id;
    assert(cm_hir_add_type(&fixture->hir, &parameter_type_value,
        &parameter_type) == CM_HIR_OK);
    memset(&predicate, 0, sizeof(predicate));
    predicate.subject = parameter_type;
    predicate.trait_type.definition = trait_definition;
    predicate.span = test_span(base + 3u, base + 8u);
    predicate.modifier = CM_HIR_PREDICATE_REQUIRED;
    memset(&function_parameter, 0, sizeof(function_parameter));
    function_parameter.name = cm_hir_intern(&fixture->hir, "value");
    function_parameter.type = parameter_type;
    function_parameter.span = test_span(base + 10u, base + 15u);
    function_parameter.binding_kind = CM_HIR_BINDING_NAMED;
    memset(&local, 0, sizeof(local));
    local.name = function_parameter.name;
    local.type = parameter_type;
    local.span = function_parameter.span;
    local.parameter_index = 0u;
    memset(&body, 0, sizeof(body));
    body.owner = definition;
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = parameter_type;
    body.locals = &local;
    body.local_count = 1u;
    body.parameter_count = 1u;
    body.source = 1u;
    body.source_expression_id = base;
    body.span = test_span(base, base + 39u);
    assert(cm_hir_add_body(&fixture->hir, &body, out_body) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_FUNCTION, definition, fixture->root, name,
        &fixture->hir);
    item.span = body.span;
    item.generic_parameter_start = parameter_id;
    item.generic_parameter_count = 1u;
    item.predicates = &predicate;
    item.predicate_count = 1u;
    item.data.function_item.signature.parameters = &function_parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.return_type = parameter_type;
    item.data.function_item.signature.abi =
        cm_hir_intern(&fixture->hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = *out_body;
    item.data.function_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    assert(add_local_expression(&fixture->hir, *out_body, 0u,
        parameter_type, test_span(base + 20u, base + 25u), &root)
        == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(&fixture->hir, *out_body, root)
        == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_caller(TestFixture *fixture, const char *name,
    CmHirDefId callee, uint32_t base, CmHirBodyId *out_body,
    CmHirExprId *out_call)
{
    CmHirDefId definition;
    CmHirFunctionParameter parameter;
    CmHirLocal local;
    CmHirBody body;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirExprId argument;

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_FUNCTION,
        test_span(base, base + 39u), &definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.name = cm_hir_intern(&fixture->hir, "input");
    parameter.type = fixture->u32_type;
    parameter.span = test_span(base + 2u, base + 7u);
    parameter.binding_kind = CM_HIR_BINDING_NAMED;
    memset(&local, 0, sizeof(local));
    local.name = parameter.name;
    local.type = fixture->u32_type;
    local.span = parameter.span;
    local.parameter_index = 0u;
    memset(&body, 0, sizeof(body));
    body.owner = definition;
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = fixture->u32_type;
    body.locals = &local;
    body.local_count = 1u;
    body.parameter_count = 1u;
    body.source = 1u;
    body.source_expression_id = base;
    body.span = test_span(base, base + 39u);
    assert(cm_hir_add_body(&fixture->hir, &body, out_body) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_FUNCTION, definition, fixture->root, name,
        &fixture->hir);
    item.span = body.span;
    item.data.function_item.signature.parameters = &parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.return_type = fixture->u32_type;
    item.data.function_item.signature.abi =
        cm_hir_intern(&fixture->hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = *out_body;
    item.data.function_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    assert(add_local_expression(&fixture->hir, *out_body, 0u,
        fixture->u32_type, test_span(base + 15u, base + 20u), &argument)
        == CM_HIR_OK);
    assert(add_call_expression(&fixture->hir, *out_body,
        callee, &fixture->u32_type, 1u, &argument, 1u,
        fixture->u32_type, test_span(base + 10u, base + 25u), out_call)
        == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(&fixture->hir, *out_body,
        *out_call) == CM_HIR_OK);
    return definition;
}

static void fixture_init(TestFixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    cm_hir_context_init(&fixture->hir);
    assert(cm_hir_create_crate(&fixture->hir,
        cm_hir_intern(&fixture->hir, "semantic_body"),
        CM_HIR_EDITION_2024, test_span(0u, 240u), &fixture->crate_id,
        &fixture->root) == CM_HIR_OK);
    fixture->u32_type = add_type(&fixture->hir,
        CM_HIR_TYPE_INTEGER_KIND);
    fixture->infer_type = add_type(&fixture->hir, CM_HIR_TYPE_INFER_KIND);
    fixture->present_trait = add_trait(fixture, "Present");
    fixture->missing_trait = add_trait(fixture, "Missing");
    fixture->present_impl = add_impl(fixture, fixture->present_trait);
    fixture->present_callee = add_bounded_identity(fixture, "present_id",
        fixture->present_trait, 40u, &fixture->present_callee_body);
    fixture->missing_callee = add_bounded_identity(fixture, "missing_id",
        fixture->missing_trait, 80u, &fixture->missing_callee_body);
    fixture->present_caller = add_caller(fixture, "present_caller",
        fixture->present_callee, 120u, &fixture->present_body,
        &fixture->present_call);
    fixture->missing_caller = add_caller(fixture, "missing_caller",
        fixture->missing_callee, 170u, &fixture->missing_body,
        &fixture->missing_call);
}

static void fixture_destroy(TestFixture *fixture)
{
    cm_hir_context_destroy(&fixture->hir);
}

static CmSemanticSessionOptions session_options(const TestFixture *fixture,
    CmHirDefId owner)
{
    CmSemanticSessionOptions options;

    cm_semantic_session_options_init(&options);
    options.local_crate = fixture->crate_id;
    options.exact_owner = owner;
    return options;
}

static CmHirItem *mutable_item(TestFixture *fixture, CmHirDefId definition)
{
    const CmHirDefinition *record;

    record = cm_hir_lookup_definition(&fixture->hir, definition);
    assert(record != NULL && record->kind == CM_HIR_DEFINITION_ITEM);
    return (CmHirItem *)cm_vec_at(&fixture->hir.items,
        (size_t)record->entity.item_id - 1u);
}

static void test_positive_impl_and_missing_metadata(void)
{
    TestFixture fixture;
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmSemanticBodyResult result;
    CmTypeckContext *typeck;
    size_t type_count;

    fixture_init(&fixture);
    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, fixture.present_caller);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    result = cm_semantic_body_check_calls(&session, fixture.present_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_OK
        && result.solver_kind == CM_TRAIT_SOLVER_PROVEN);
    cm_semantic_session_destroy(&session);

    options = session_options(&fixture, fixture.missing_caller);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    typeck = cm_semantic_session_typeck(&session);
    type_count = cm_typeck_type_count(typeck);
    result = cm_semantic_body_check_calls(&session, fixture.missing_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_DEFERRED_METADATA
        && result.expression == fixture.missing_call
        && cm_hir_def_id_equal(result.callee, fixture.missing_callee)
        && result.predicate_index == 0u
        && result.solver_kind == CM_TRAIT_SOLVER_DEFERRED_METADATA
        && cm_typeck_type_count(typeck) == type_count);
    result = cm_semantic_body_check_calls(&session, fixture.missing_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_DEFERRED_METADATA
        && cm_typeck_type_count(typeck) == type_count);
    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);
}

static void test_callee_environment_cannot_self_prove(void)
{
    TestFixture fixture;
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmSemanticBodyResult result;
    CmHirTypeId substitution;
    size_t type_count;
    CmTypeckContext *typeck;

    fixture_init(&fixture);
    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, fixture.missing_callee);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    typeck = cm_semantic_session_typeck(&session);
    type_count = cm_typeck_type_count(typeck);
    result = cm_semantic_body_check_calls(&session, fixture.missing_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_INVALID
        && cm_typeck_type_count(typeck) == type_count);
    substitution = fixture.u32_type;
    result = cm_semantic_body_check_calls(&session,
        fixture.missing_callee_body, &substitution, 1u);
    assert(result.status == CM_SEMANTIC_BODY_OK);
    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);
}

static void test_pending_shapes_and_atomicity(void)
{
    TestFixture fixture;
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmSemanticBodyResult result;
    CmTypeckContext *typeck;
    CmHirItem *callee;
    CmHirExpr *call;
    CmInternId binder_name;
    CmHirAssociatedTypeEquality equality;
    CmHirOutlivesPredicate outlives;
    size_t type_count;

    fixture_init(&fixture);
    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, fixture.missing_caller);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    typeck = cm_semantic_session_typeck(&session);
    type_count = cm_typeck_type_count(typeck);
    callee = mutable_item(&fixture, fixture.missing_callee);
    call = (CmHirExpr *)cm_vec_at(&fixture.hir.expressions,
        (size_t)fixture.missing_call - 1u);
    assert(callee != NULL && call != NULL);

    callee->predicates[0].modifier = CM_HIR_PREDICATE_CONST;
    result = cm_semantic_body_check_calls(&session, fixture.missing_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_PENDING_MODIFIER);
    callee->predicates[0].modifier = CM_HIR_PREDICATE_REQUIRED;

    binder_name = cm_hir_intern(&fixture.hir, "late");
    /* The append above makes the old session stale; rebuild before checking. */
    cm_semantic_session_destroy(&session);
    options = session_options(&fixture, fixture.missing_caller);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    typeck = cm_semantic_session_typeck(&session);
    type_count = cm_typeck_type_count(typeck);
    callee->predicates[0].binder.lifetimes = &binder_name;
    callee->predicates[0].binder.lifetime_count = 1u;
    result = cm_semantic_body_check_calls(&session, fixture.missing_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_PENDING_HIGHER_RANKED);
    callee->predicates[0].binder.lifetimes = NULL;
    callee->predicates[0].binder.lifetime_count = 0u;

    memset(&equality, 0, sizeof(equality));
    equality.associated_type = fixture.missing_trait;
    equality.value = fixture.u32_type;
    equality.span = test_span(1u, 2u);
    callee->predicates[0].equalities = &equality;
    callee->predicates[0].equality_count = 1u;
    result = cm_semantic_body_check_calls(&session, fixture.missing_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_PENDING_PROJECTION);
    callee->predicates[0].equalities = NULL;
    callee->predicates[0].equality_count = 0u;

    memset(&outlives, 0, sizeof(outlives));
    outlives.subject_kind = CM_HIR_OUTLIVES_TYPE;
    outlives.subject.type = fixture.u32_type;
    outlives.bound.kind = CM_HIR_REGION_STATIC;
    callee->outlives_predicates = &outlives;
    callee->outlives_predicate_count = 1u;
    result = cm_semantic_body_check_calls(&session, fixture.missing_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_PENDING_OUTLIVES);
    callee->outlives_predicates = NULL;
    callee->outlives_predicate_count = 0u;

    call->data.call.type_substitutions[0] = fixture.infer_type;
    result = cm_semantic_body_check_calls(&session, fixture.missing_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_DEFERRED_INFERENCE);
    call->data.call.type_substitutions[0] = fixture.u32_type;
    assert(cm_typeck_type_count(typeck) == type_count);
    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);
}

static void test_malformed_foreign_and_stale(void)
{
    TestFixture fixture;
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmSemanticBodyResult result;
    CmTypeckContext *typeck;
    CmHirExpr *call;
    CmHirItem *callee;
    CmHirDefId original_callee;
    CmHirTypeId extra;
    size_t type_count;

    fixture_init(&fixture);
    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, fixture.present_caller);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    typeck = cm_semantic_session_typeck(&session);
    result = cm_semantic_body_check_calls(&session, fixture.present_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_OK);
    type_count = cm_typeck_type_count(typeck);
    call = (CmHirExpr *)cm_vec_at(&fixture.hir.expressions,
        (size_t)fixture.present_call - 1u);
    callee = mutable_item(&fixture, fixture.present_callee);
    assert(call != NULL && callee != NULL);

    original_callee = call->data.call.callee;
    call->data.call.callee.crate_id = fixture.crate_id + 100u;
    call->data.call.callee.index = 1u;
    result = cm_semantic_body_check_calls(&session, fixture.present_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_INVALID
        && cm_typeck_type_count(typeck) == type_count);
    call->data.call.callee = original_callee;

    call->data.call.type_substitution_count = 0u;
    result = cm_semantic_body_check_calls(&session, fixture.present_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_PENDING_SUBSTITUTION
        && cm_typeck_type_count(typeck) == type_count);
    call->data.call.type_substitution_count = 1u;

    callee->predicates[0].binder.lifetimes = (CmInternId *)&extra;
    result = cm_semantic_body_check_calls(&session, fixture.present_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_INVALID
        && cm_typeck_type_count(typeck) == type_count);
    callee->predicates[0].binder.lifetimes = NULL;

    extra = add_type(&fixture.hir, CM_HIR_TYPE_BOOL_KIND);
    assert(!cm_semantic_session_is_current(&session));
    result = cm_semantic_body_check_calls(&session, fixture.present_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_STALE);
    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);
}

static void test_invalid_api_and_status_names(void)
{
    TestFixture fixture;
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmSemanticBodyResult result;
    unsigned int status;

    fixture_init(&fixture);
    memset(&session, 0, sizeof(session));
    result = cm_semantic_body_check_calls(NULL, fixture.present_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_INVALID);
    options = session_options(&fixture, fixture.present_caller);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    result = cm_semantic_body_check_calls(&session, CM_HIR_BODY_NONE,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_INVALID);
    result = cm_semantic_body_check_calls(&session, fixture.present_body,
        &fixture.u32_type, 1u);
    assert(result.status == CM_SEMANTIC_BODY_INVALID);
    for (status = 0u; status <= (unsigned int)CM_SEMANTIC_BODY_INVALID;
         ++status) {
        assert(strcmp(cm_semantic_body_status_name(
            (CmSemanticBodyStatus)status), "unknown") != 0);
    }
    assert(strcmp(cm_semantic_body_status_name(CM_SEMANTIC_BODY_NEGATIVE),
        "negative") == 0);
    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);
}

int main(void)
{
    test_positive_impl_and_missing_metadata();
    test_callee_environment_cannot_self_prove();
    test_pending_shapes_and_atomicity();
    test_malformed_foreign_and_stale();
    test_invalid_api_and_status_names();
    puts("hir semantic body tests passed");
    return 0;
}
