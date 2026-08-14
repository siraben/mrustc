#include "cm/hir/admission.h"
#include "cm/hir/lower.h"
#include "cm/hir/semantic_mark.h"
#include "cm/hir/semantic_regions.h"
#include "cm/hir/semantic_results.h"
#include "cm/alloc.h"
#include "cm/source.h"

#include "../../src/hir/semantic_results_internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct Fixture {
    CmSourceSet sources;
    CmSourceId source;
    CmCfgSet cfg;
    CmModuleGraph graph;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmHirContext hir;
    CmHirModuleMap modules;
} Fixture;

typedef struct ProjectionFailureProbe {
    CmSemanticResultsBodyStage stage;
    int duplicate_projection;
    int duplicate_callable_projection;
    int tamper_projection_index;
    int tamper_projection_input;
    int reject_checked_body;
} ProjectionFailureProbe;

static CmSemanticBodyWritebackStatus projection_failure_checked(
    void *context, CmSemanticSession *session, CmHirBodyId body,
    const CmSemanticCheckedBodyFacts *facts)
{
    ProjectionFailureProbe *probe;

    probe = (ProjectionFailureProbe *)context;
    if (probe->reject_checked_body) {
        return CM_SEMANTIC_BODY_WRITEBACK_INVALID;
    }
    return cm_semantic_results_stage_checked_body(&probe->stage, session,
        body, facts);
}

static CmSemanticBodyWritebackStatus projection_failure_decision(
    void *context, CmSemanticSession *session, CmHirBodyId body,
    CmHirExprId expression,
    CmSemanticProjectionDecisionKind decision_kind,
    uint32_t decision_index, CmTypeckTypeId input_type,
    CmTypeckTypeId normalized_type,
    const CmProjectionNormalizeTrace *trace)
{
    ProjectionFailureProbe *probe;
    CmSemanticBodyWritebackStatus status;

    probe = (ProjectionFailureProbe *)context;
    if (probe->tamper_projection_input) {
        return cm_semantic_results_stage_projection_decision(&probe->stage,
            session, body, expression, decision_kind, decision_index,
            normalized_type, normalized_type, trace);
    }
    status = cm_semantic_results_stage_projection_decision(&probe->stage,
        session, body, expression, decision_kind, decision_index,
        input_type, normalized_type, trace);
    if (status == CM_SEMANTIC_BODY_WRITEBACK_OK
        && probe->duplicate_projection) {
        status = cm_semantic_results_stage_projection_decision(&probe->stage,
            session, body, expression, decision_kind, decision_index,
            input_type, normalized_type, trace);
    }
    if (status == CM_SEMANTIC_BODY_WRITEBACK_OK
        && probe->tamper_projection_index) {
        status = cm_semantic_results_stage_projection_decision(&probe->stage,
            session, body, expression,
            CM_SEMANTIC_PROJECTION_DECISION_CALLABLE_REQUESTED_SELF_TYPE,
            1u, input_type, normalized_type, trace);
    }
    if (status == CM_SEMANTIC_BODY_WRITEBACK_OK
        && probe->duplicate_callable_projection) {
        status = cm_semantic_results_stage_projection_decision(&probe->stage,
            session, body, expression,
            CM_SEMANTIC_PROJECTION_DECISION_CALLABLE_RETURN_TYPE, 0u,
            input_type, normalized_type, trace);
        if (status == CM_SEMANTIC_BODY_WRITEBACK_OK) {
            status = cm_semantic_results_stage_projection_decision(
                &probe->stage, session, body, expression,
                CM_SEMANTIC_PROJECTION_DECISION_CALLABLE_RETURN_TYPE, 0u,
                input_type, normalized_type, trace);
        }
    }
    return status;
}

static void projection_failure_discard(void *context)
{
    ProjectionFailureProbe *probe;

    probe = (ProjectionFailureProbe *)context;
    cm_semantic_results_body_stage_destroy(&probe->stage);
}

static CmHirDefId find_named_item(const Fixture *fixture,
    const char *name, CmHirDefId parent)
{
    size_t index;

    for (index = 0u; index < fixture->hir.items.len; ++index) {
        const CmHirItem *item;
        const CmInternedString *interned;

        item = (const CmHirItem *)cm_vec_at_const(&fixture->hir.items,
            index);
        interned = item == NULL ? NULL
            : cm_interner_get(&fixture->hir.strings, item->name);
        if (item != NULL && interned != NULL
            && interned->len == strlen(name)
            && memcmp(interned->bytes, name, interned->len) == 0
            && cm_hir_def_id_equal(item->parent_definition, parent)) {
            return item->definition;
        }
    }
    return cm_hir_def_id_none();
}

static CmHirTypeId find_integer_type(const Fixture *fixture,
    CmHirIntType kind)
{
    size_t index;

    for (index = 0u; index < fixture->hir.types.len; ++index) {
        const CmHirType *type;

        type = cm_hir_get_type(&fixture->hir, (CmHirTypeId)(index + 1u));
        if (type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
            && type->data.integer_type.kind == kind) {
            return (CmHirTypeId)(index + 1u);
        }
    }
    return CM_HIR_TYPE_NONE;
}

static const CmHirItem *find_function_item(const Fixture *fixture,
    const char *name)
{
    CmHirDefId definition;
    const CmHirDefinition *record;

    definition = find_named_item(fixture, name, cm_hir_def_id_none());
    record = cm_hir_lookup_definition(&fixture->hir, definition);
    return record == NULL || record->kind != CM_HIR_DEFINITION_ITEM
        ? NULL : cm_hir_get_item(&fixture->hir, record->entity.item_id);
}

static CmHirExprId find_owned_callable(const Fixture *fixture,
    CmHirBodyId body, CmHirExprKind kind)
{
    CmHirExprId found;
    size_t index;

    found = CM_HIR_EXPR_NONE;
    for (index = 0u; index < fixture->hir.expressions.len; ++index) {
        const CmHirExpr *expression;

        expression = cm_hir_get_expr(&fixture->hir,
            (CmHirExprId)(index + 1u));
        if (expression != NULL && expression->owner_body == body
            && expression->kind == kind) {
            assert(found == CM_HIR_EXPR_NONE);
            found = (CmHirExprId)(index + 1u);
        }
    }
    return found;
}

static CmHirTypeId add_projection_type(Fixture *fixture,
    CmHirDefId trait_definition, CmHirDefId associated_definition,
    CmHirTypeId self_type)
{
    CmHirType projection;
    CmHirTypeId type;

    memset(&projection, 0, sizeof(projection));
    projection.kind = CM_HIR_TYPE_PROJECTION_KIND;
    projection.span = (CmSpan){ fixture->source, 0u, 1u };
    projection.data.projection_type.self_type = self_type;
    projection.data.projection_type.trait_type.definition = trait_definition;
    projection.data.projection_type.associated_type.definition =
        associated_definition;
    assert(cm_hir_add_type(&fixture->hir, &projection, &type) == CM_HIR_OK);
    return type;
}

static void assert_no_callable_generic_arguments(
    const CmSemanticResults *results, const CmSemanticAdmission *admission,
    CmHirBodyId body, CmHirExprId expression)
{
    CmSemanticGenericArgumentView argument;
    size_t domain;

    for (domain = 0u; domain < 4u; ++domain) {
        assert(cm_semantic_results_callable_generic_argument(results,
            admission, body, expression,
            (CmSemanticCallableGenericArgumentDomain)domain, 0u, &argument)
                == CM_SEMANTIC_RESULTS_NOT_FOUND);
    }
    assert(cm_semantic_results_callable_generic_argument(results, admission,
        body, expression, (CmSemanticCallableGenericArgumentDomain)4, 0u,
        &argument) == CM_SEMANTIC_RESULTS_INVALID_ARGUMENT
        && cm_semantic_results_callable_generic_argument(results, admission,
            body, expression,
            CM_SEMANTIC_CALLABLE_GENERIC_ARGUMENT_ITEM, 0u, NULL)
                == CM_SEMANTIC_RESULTS_INVALID_ARGUMENT);
}

static void assert_no_instance_callable_generic_arguments(
    const CmSemanticResults *results, const CmSemanticAdmission *admission,
    const CmHirInstanceSpec *caller, CmHirExprId expression)
{
    CmSemanticGenericArgumentView argument;
    size_t domain;

    for (domain = 0u; domain < 4u; ++domain) {
        assert(cm_semantic_results_instance_callable_generic_argument(results,
            admission, caller, expression,
            (CmSemanticCallableGenericArgumentDomain)domain, 0u, &argument)
                == CM_SEMANTIC_RESULTS_NOT_FOUND);
    }
    assert(cm_semantic_results_instance_callable_generic_argument(results,
        admission, caller, expression,
        (CmSemanticCallableGenericArgumentDomain)4, 0u, &argument)
            == CM_SEMANTIC_RESULTS_INVALID_ARGUMENT
        && cm_semantic_results_instance_callable_generic_argument(results,
            admission, caller, expression,
            CM_SEMANTIC_CALLABLE_GENERIC_ARGUMENT_ITEM, 0u, NULL)
                == CM_SEMANTIC_RESULTS_INVALID_ARGUMENT);
}

static void fixture_init(Fixture *fixture, const char *source)
{
    CmModuleGraphOptions graph_options;
    CmImportResult import_result;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;

    memset(fixture, 0, sizeof(*fixture));
    cm_source_set_init(&fixture->sources);
    assert(cm_source_add_memory(&fixture->sources, "results/lib.rs",
        (const unsigned char *)source, strlen(source), &fixture->source)
        == CM_SOURCE_OK);
    cm_cfg_set_init(&fixture->cfg);
    cm_module_graph_init(&fixture->graph);
    cm_module_graph_options_init(&graph_options);
    graph_options.edition = CM_EDITION_2021;
    graph_options.cfg = &fixture->cfg;
    fixture->graph_result = cm_module_graph_build(&fixture->graph,
        &fixture->sources, fixture->source, &graph_options);
    assert(fixture->graph_result.error_count == 0u);
    cm_import_resolver_init(&fixture->imports);
    import_result = cm_import_resolve(&fixture->imports, &fixture->graph,
        fixture->graph_result.revision);
    assert(import_result.error_count == 0u);
    cm_hir_context_init(&fixture->hir);
    cm_hir_module_map_init(&fixture->modules);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "results_test";
    lower_options.edition = CM_HIR_EDITION_2021;
    lower_result = cm_hir_lower_module_graph(&fixture->hir,
        &fixture->graph, fixture->graph_result.revision,
        &fixture->imports, &fixture->modules, &lower_options);
    assert(lower_result.error_count == 0u && fixture->hir.crates.len == 1u);
}

static void fixture_destroy(Fixture *fixture)
{
    cm_hir_module_map_destroy(&fixture->modules);
    cm_hir_context_destroy(&fixture->hir);
    cm_import_resolver_destroy(&fixture->imports);
    cm_module_graph_destroy(&fixture->graph);
    cm_source_set_destroy(&fixture->sources);
}

static CmSemanticAdmissionResult admit(Fixture *fixture,
    CmSemanticAdmission *admission)
{
    return cm_semantic_admit_local_crate(admission, &fixture->hir, 1u,
        &fixture->graph, fixture->graph_result.revision,
        &fixture->imports, &fixture->modules);
}

static void test_successful_results(void)
{
    Fixture fixture;
    CmSemanticAdmission admission;
    CmSemanticAdmissionResult admission_result;
    const CmSemanticResults *results;
    size_t body_index;
    size_t expression_index;
    size_t queried_expression_count;
    int saw_call;
    int saw_binary;
    int saw_field;

    fixture_init(&fixture,
        "struct Pair { left: u32, right: u32 } "
        "fn add(left: u32, right: u32) -> u32 { left + right } "
        "fn call(value: u32) -> u32 { add(value, 1u32) } "
        "fn read(value: Pair) -> u32 { value.left }");
    memset(&admission, 0, sizeof(admission));
    admission_result = admit(&fixture, &admission);
    assert(admission_result.status == CM_SEMANTIC_ADMISSION_OK);
    results = cm_semantic_admission_results(&admission);
    assert(results != NULL
        && cm_semantic_results_seal_kind(results)
            == CM_SEMANTIC_RESULTS_SEAL_WHOLE_LOCAL
        && cm_semantic_results_is_current(results, &admission)
        && cm_semantic_results_hir(results, &admission) == &fixture.hir
        && cm_semantic_results_crate(results, &admission) == 1u
        && cm_semantic_results_generation(results, &admission)
            == fixture.hir.semantic_generation
        && cm_semantic_results_body_count(results, &admission) == 3u);
    queried_expression_count = 0u;
    saw_call = 0;
    saw_binary = 0;
    saw_field = 0;
    for (body_index = 0u; body_index < 3u; ++body_index) {
        CmSemanticBodyView body_view;
        CmSemanticFunctionSignatureView signature_view;
        const CmHirBody *body;
        const CmHirDefinition *owner_definition;
        const CmHirItem *owner_item;
        uint32_t signature_parameter_index;
        uint32_t body_expressions;
        assert(cm_semantic_results_body_at(results, &admission,
            body_index, &body_view) == CM_SEMANTIC_RESULTS_OK);
        body = cm_hir_get_body(&fixture.hir, body_view.body);
        owner_definition = body == NULL ? NULL
            : cm_hir_lookup_definition(&fixture.hir, body->owner);
        owner_item = owner_definition == NULL
                || owner_definition->kind != CM_HIR_DEFINITION_ITEM
            ? NULL : cm_hir_get_item(&fixture.hir,
                owner_definition->entity.item_id);
        assert(body != NULL && owner_item != NULL
            && owner_item->kind == CM_HIR_ITEM_FUNCTION
            && cm_semantic_results_signature(results, &admission,
                body_view.body, &signature_view) == CM_SEMANTIC_RESULTS_OK
            && cm_hir_def_id_equal(signature_view.definition,
                body_view.owner)
            && signature_view.body == body_view.body
            && signature_view.parameter_count
                == owner_item->data.function_item.signature.parameter_count
            && signature_view.return_type.bytes != NULL
            && signature_view.return_type.size != 0u);
        {
            int matches;

            assert(cm_semantic_type_view_matches_monomorphic_hir(results,
                &admission, &signature_view.return_type,
                owner_item->data.function_item.signature.return_type,
                &matches) == CM_SEMANTIC_RESULTS_OK
                && matches);
        }
        for (signature_parameter_index = 0u;
             signature_parameter_index < signature_view.parameter_count;
             ++signature_parameter_index) {
            CmSemanticTypeView parameter_view;

            assert(cm_semantic_results_signature_parameter(results,
                &admission, body_view.body, signature_parameter_index,
                &parameter_view) == CM_SEMANTIC_RESULTS_OK
                && parameter_view.bytes != NULL
                && parameter_view.size != 0u);
            {
                int matches;

                assert(cm_semantic_type_view_matches_monomorphic_hir(
                    results, &admission, &parameter_view,
                    owner_item->data.function_item.signature.parameters[
                        signature_parameter_index].type,
                    &matches) == CM_SEMANTIC_RESULTS_OK
                    && matches);
            }
        }
        body_expressions = 0u;
        for (expression_index = 0u;
                expression_index < fixture.hir.expressions.len;
                ++expression_index) {
            const CmHirExpr *expression;
            CmSemanticExpressionView expression_view;
            int equal;
            expression = cm_hir_get_expr(&fixture.hir,
                (CmHirExprId)(expression_index + 1u));
            if (expression == NULL || expression->owner_body != body_view.body)
                continue;
            assert(cm_semantic_results_expression(results, &admission,
                body_view.body, (CmHirExprId)(expression_index + 1u),
                &expression_view) == CM_SEMANTIC_RESULTS_OK);
            assert(expression_view.unadjusted_type.bytes
                    == expression_view.adjusted_type.bytes
                && expression_view.adjustment_count == 0u
                && cm_semantic_type_view_equal(
                    &expression_view.unadjusted_type,
                    &expression_view.adjusted_type, &equal)
                    == CM_SEMANTIC_RESULTS_OK
                && equal);
            if (expression->kind == CM_HIR_EXPR_CALL) {
                CmSemanticDirectCallView call_view;
                uint32_t parameter_index;

                assert(expression_view.has_direct_callable
                    && cm_hir_def_id_equal(expression_view.direct_callable,
                        expression->data.call.callee));
                assert(cm_semantic_results_direct_call(results, &admission,
                    body_view.body, (CmHirExprId)(expression_index + 1u),
                    &call_view) == CM_SEMANTIC_RESULTS_OK
                    && call_view.body == body_view.body
                    && call_view.expression
                        == (CmHirExprId)(expression_index + 1u)
                    && cm_hir_def_id_equal(call_view.callee,
                        expression_view.direct_callable)
                    && call_view.parameter_count
                        == expression->data.call.argument_count
                    && cm_semantic_type_view_equal(
                        &call_view.return_type,
                        &expression_view.adjusted_type, &equal)
                        == CM_SEMANTIC_RESULTS_OK
                    && equal);
                for (parameter_index = 0u;
                     parameter_index < call_view.parameter_count;
                     ++parameter_index) {
                    CmSemanticTypeView parameter_view;
                    CmSemanticExpressionView argument_view;

                    assert(cm_semantic_results_direct_call_parameter(
                        results, &admission, body_view.body,
                        call_view.expression, parameter_index,
                        &parameter_view) == CM_SEMANTIC_RESULTS_OK
                        && cm_semantic_results_expression(results,
                            &admission, body_view.body,
                            expression->data.call.arguments[parameter_index],
                            &argument_view) == CM_SEMANTIC_RESULTS_OK
                        && cm_semantic_type_view_equal(&parameter_view,
                            &argument_view.adjusted_type, &equal)
                            == CM_SEMANTIC_RESULTS_OK
                        && equal);
                }
                saw_call = 1;
            }
            if (expression->kind == CM_HIR_EXPR_BINARY) {
                CmSemanticPrimitiveBinaryView binary_view;
                CmSemanticExpressionView left_view;
                CmSemanticExpressionView right_view;

                assert(expression_view.has_primitive_operator
                    && expression_view.primitive_operator
                        == expression->data.binary.operator_kind
                    && cm_semantic_results_primitive_binary(results,
                        &admission, body_view.body,
                        (CmHirExprId)(expression_index + 1u),
                        &binary_view) == CM_SEMANTIC_RESULTS_OK
                    && binary_view.body == body_view.body
                    && binary_view.expression
                        == (CmHirExprId)(expression_index + 1u)
                    && binary_view.operator_kind
                        == expression->data.binary.operator_kind
                    && binary_view.left_expression
                        == expression->data.binary.left
                    && binary_view.right_expression
                        == expression->data.binary.right
                    && cm_semantic_results_expression(results, &admission,
                        body_view.body, binary_view.left_expression,
                        &left_view) == CM_SEMANTIC_RESULTS_OK
                    && cm_semantic_results_expression(results, &admission,
                        body_view.body, binary_view.right_expression,
                        &right_view) == CM_SEMANTIC_RESULTS_OK
                    && cm_semantic_type_view_equal(&binary_view.left_type,
                        &left_view.adjusted_type, &equal)
                        == CM_SEMANTIC_RESULTS_OK && equal
                    && cm_semantic_type_view_equal(&binary_view.right_type,
                        &right_view.adjusted_type, &equal)
                        == CM_SEMANTIC_RESULTS_OK && equal
                    && cm_semantic_type_view_equal(&binary_view.result_type,
                        &expression_view.adjusted_type, &equal)
                        == CM_SEMANTIC_RESULTS_OK && equal
                    && cm_semantic_results_expression_adjustment(results,
                        &admission, body_view.body,
                        (CmHirExprId)(expression_index + 1u), 0u,
                        &(CmSemanticAdjustmentView){0})
                        == CM_SEMANTIC_RESULTS_NOT_FOUND);
                saw_binary = 1;
            }
            if (expression->kind == CM_HIR_EXPR_FIELD) {
                CmSemanticFieldSelectionView field_view;
                CmSemanticExpressionView base_view;

                assert(cm_semantic_results_field_selection(results,
                        &admission, body_view.body,
                        (CmHirExprId)(expression_index + 1u),
                        &field_view) == CM_SEMANTIC_RESULTS_OK
                    && field_view.body == body_view.body
                    && field_view.expression
                        == (CmHirExprId)(expression_index + 1u)
                    && field_view.base_expression
                        == expression->data.field.base
                    && cm_hir_def_id_equal(
                        field_view.aggregate_definition,
                        expression->data.field.definition)
                    && field_view.field_index
                        == expression->data.field.field_index
                    && cm_semantic_results_expression(results, &admission,
                        body_view.body, field_view.base_expression,
                        &base_view) == CM_SEMANTIC_RESULTS_OK
                    && cm_semantic_type_view_equal(&field_view.base_type,
                        &base_view.adjusted_type, &equal)
                        == CM_SEMANTIC_RESULTS_OK && equal
                    && cm_semantic_type_view_equal(&field_view.field_type,
                        &expression_view.adjusted_type, &equal)
                        == CM_SEMANTIC_RESULTS_OK && equal);
                saw_field = 1;
            }
            body_expressions += 1u;
            queried_expression_count += 1u;
        }
        assert(body_expressions == body_view.expression_count);
    }
    assert(queried_expression_count == fixture.hir.expressions.len
        && saw_call && saw_binary && saw_field);
    cm_semantic_admission_destroy(&admission);
    fixture_destroy(&fixture);
}

static void test_stale_and_failure_publish_nothing(void)
{
    Fixture fixture;
    CmSemanticAdmission admission;
    CmSemanticAdmissionResult admission_result;
    const CmSemanticResults *results;
    CmHirType type;
    CmHirTypeId type_id;

    fixture_init(&fixture, "fn okay() -> u32 { 1u32 }");
    memset(&admission, 0, sizeof(admission));
    admission_result = admit(&fixture, &admission);
    assert(admission_result.status == CM_SEMANTIC_ADMISSION_OK);
    results = cm_semantic_admission_results(&admission);
    assert(results != NULL);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_BOOL_KIND;
    type.span = (CmSpan){ fixture.source, 0u, 1u };
    assert(cm_hir_add_type(&fixture.hir, &type, &type_id) == CM_HIR_OK);
    assert(cm_semantic_admission_results(&admission) == NULL
        && !cm_semantic_results_is_current(results, &admission));
    cm_semantic_admission_destroy(&admission);
    fixture_destroy(&fixture);

    fixture_init(&fixture, "fn bad() -> u32 { true }");
    memset(&admission, 0, sizeof(admission));
    admission_result = admit(&fixture, &admission);
    assert(admission_result.status != CM_SEMANTIC_ADMISSION_OK
        && cm_semantic_admission_results(&admission) == NULL);
    fixture_destroy(&fixture);
}

static void test_same_generation_foreign_admission(void)
{
    Fixture fixture;
    CmSemanticAdmission first;
    CmSemanticAdmission second;
    CmSemanticAdmissionResult admission_result;
    const CmSemanticResults *first_results;
    const CmSemanticResults *second_results;
    CmSemanticBodyView body_view;
    CmSemanticFunctionSignatureView signature_view;
    CmSemanticDirectCallView call_view;

    fixture_init(&fixture, "fn okay() -> u32 { 1u32 }");
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    admission_result = admit(&fixture, &first);
    assert(admission_result.status == CM_SEMANTIC_ADMISSION_OK);
    first_results = cm_semantic_admission_results(&first);
    assert(first_results != NULL);
    admission_result = admit(&fixture, &second);
    assert(admission_result.status == CM_SEMANTIC_ADMISSION_OK);
    second_results = cm_semantic_admission_results(&second);
    assert(second_results != NULL && second_results != first_results);
    assert(!cm_semantic_results_is_current(first_results, &second)
        && cm_semantic_results_body_at(first_results, &second, 0u,
            &body_view) == CM_SEMANTIC_RESULTS_FOREIGN
        && cm_semantic_results_signature(first_results, &second, 1u,
            &signature_view) == CM_SEMANTIC_RESULTS_FOREIGN
        && cm_semantic_results_direct_call(first_results, &second, 1u, 1u,
            &call_view) == CM_SEMANTIC_RESULTS_FOREIGN
        && !cm_semantic_results_is_current(second_results, &first)
        && cm_semantic_results_body_at(second_results, &first, 0u,
            &body_view) == CM_SEMANTIC_RESULTS_FOREIGN);
    cm_semantic_admission_destroy(&second);
    cm_semantic_admission_destroy(&first);
    fixture_destroy(&fixture);
}

static void test_generic_parameter_type_is_structural(void)
{
    Fixture fixture;
    CmSemanticAdmission admission;
    CmSemanticAdmissionResult admission_result;
    const CmSemanticResults *results;
    CmSemanticBodyView body_view;
    CmSemanticExpressionView local_view;
    const CmHirBody *body;
    int equal;

    fixture_init(&fixture, "fn identity<T>(value: T) -> T { value }");
    memset(&admission, 0, sizeof(admission));
    admission_result = admit(&fixture, &admission);
    assert(admission_result.status == CM_SEMANTIC_ADMISSION_OK);
    results = cm_semantic_admission_results(&admission);
    assert(results != NULL
        && cm_semantic_results_body_at(results, &admission, 0u,
            &body_view) == CM_SEMANTIC_RESULTS_OK);
    body = cm_hir_get_body(&fixture.hir, body_view.body);
    assert(body != NULL
        && cm_semantic_results_expression(results, &admission,
            body_view.body, body->root_expression, &local_view)
            == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_type_view_equal(&local_view.unadjusted_type,
            &local_view.adjusted_type, &equal) == CM_SEMANTIC_RESULTS_OK
        && equal && local_view.unadjusted_type.size > sizeof(uint32_t));
    cm_semantic_admission_destroy(&admission);
    fixture_destroy(&fixture);
}

static void test_type_view_materializes_existing_hir_read_only(void)
{
    Fixture fixture;
    CmSemanticAdmission admission;
    CmSemanticAdmission foreign_admission;
    CmSemanticAdmissionResult admission_result;
    const CmSemanticResults *results;
    const CmHirItem *u32_item;
    const CmHirItem *u8_item;
    const CmHirItem *generic_item;
    CmSemanticFunctionSignatureView u32_signature;
    CmSemanticFunctionSignatureView u8_signature;
    CmSemanticFunctionSignatureView generic_signature;
    CmSemanticTypeView foreign_view;
    CmSemanticTypeView invalid_view;
    const CmHirType *source_type;
    CmHirType duplicate_type;
    CmHirType stale_type;
    CmHirTypeId u32_type;
    CmHirTypeId u8_type;
    CmHirTypeId duplicate_type_id;
    CmHirTypeId stale_type_id;
    CmHirTypeId materialized;
    unsigned char *foreign_bytes;
    size_t sealed_type_count;

    fixture_init(&fixture,
        "fn u32_value(value: u32) -> u32 { value } "
        "fn u8_value(value: u8) -> u8 { value } "
        "fn generic<T>(value: T) -> T { value }");
    u32_type = find_integer_type(&fixture, CM_HIR_INT_U32);
    u8_type = find_integer_type(&fixture, CM_HIR_INT_U8);
    source_type = cm_hir_get_type(&fixture.hir, u32_type);
    assert(u32_type != CM_HIR_TYPE_NONE && u8_type != CM_HIR_TYPE_NONE
        && source_type != NULL);
    duplicate_type = *source_type;
    assert(cm_hir_add_type(&fixture.hir, &duplicate_type,
            &duplicate_type_id) == CM_HIR_OK
        && duplicate_type_id > u32_type);

    memset(&admission, 0, sizeof(admission));
    admission_result = admit(&fixture, &admission);
    results = cm_semantic_admission_results(&admission);
    u32_item = find_function_item(&fixture, "u32_value");
    u8_item = find_function_item(&fixture, "u8_value");
    generic_item = find_function_item(&fixture, "generic");
    assert(admission_result.status == CM_SEMANTIC_ADMISSION_OK
        && results != NULL && u32_item != NULL && u8_item != NULL
        && generic_item != NULL
        && cm_semantic_results_signature(results, &admission,
            u32_item->data.function_item.body, &u32_signature)
                == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_results_signature(results, &admission,
            u8_item->data.function_item.body, &u8_signature)
                == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_results_signature(results, &admission,
            generic_item->data.function_item.body, &generic_signature)
                == CM_SEMANTIC_RESULTS_OK);
    sealed_type_count = fixture.hir.types.len;

    materialized = CM_HIR_TYPE_NONE;
    assert(cm_semantic_type_view_materialize_existing_hir(results,
            &admission, &u32_signature.return_type, &materialized)
                == CM_SEMANTIC_RESULTS_OK
        && materialized == u32_type
        && fixture.hir.types.len == sealed_type_count);
    materialized = CM_HIR_TYPE_NONE;
    assert(cm_semantic_type_view_materialize_existing_hir(results,
            &admission, &u8_signature.return_type, &materialized)
                == CM_SEMANTIC_RESULTS_OK
        && materialized == u8_type
        && fixture.hir.types.len == sealed_type_count);

    materialized = duplicate_type_id;
    assert(cm_semantic_type_view_materialize_existing_hir(results,
            &admission, &generic_signature.return_type, &materialized)
                == CM_SEMANTIC_RESULTS_NOT_FOUND
        && materialized == CM_HIR_TYPE_NONE
        && fixture.hir.types.len == sealed_type_count);
    materialized = duplicate_type_id;
    assert(cm_semantic_type_view_materialize_existing_hir(results,
            &admission, NULL, &materialized)
                == CM_SEMANTIC_RESULTS_INVALID_ARGUMENT
        && materialized == CM_HIR_TYPE_NONE);
    memset(&invalid_view, 0, sizeof(invalid_view));
    materialized = duplicate_type_id;
    assert(cm_semantic_type_view_materialize_existing_hir(results,
            &admission, &invalid_view, &materialized)
                == CM_SEMANTIC_RESULTS_INVALID_ARGUMENT
        && materialized == CM_HIR_TYPE_NONE
        && cm_semantic_type_view_materialize_existing_hir(results,
            &admission, &u32_signature.return_type, NULL)
                == CM_SEMANTIC_RESULTS_INVALID_ARGUMENT);

    foreign_bytes = (unsigned char *)cm_alloc(u32_signature.return_type.size);
    assert(foreign_bytes != NULL);
    memcpy(foreign_bytes, u32_signature.return_type.bytes,
        u32_signature.return_type.size);
    foreign_view.bytes = foreign_bytes;
    foreign_view.size = u32_signature.return_type.size;
    materialized = duplicate_type_id;
    assert(cm_semantic_type_view_materialize_existing_hir(results,
            &admission, &foreign_view, &materialized)
                == CM_SEMANTIC_RESULTS_FOREIGN
        && materialized == CM_HIR_TYPE_NONE
        && fixture.hir.types.len == sealed_type_count);
    cm_free(foreign_bytes);

    memset(&foreign_admission, 0, sizeof(foreign_admission));
    admission_result = admit(&fixture, &foreign_admission);
    materialized = duplicate_type_id;
    assert(admission_result.status == CM_SEMANTIC_ADMISSION_OK
        && cm_semantic_type_view_materialize_existing_hir(results,
            &foreign_admission, &u32_signature.return_type, &materialized)
                == CM_SEMANTIC_RESULTS_FOREIGN
        && materialized == CM_HIR_TYPE_NONE
        && fixture.hir.types.len == sealed_type_count);
    cm_semantic_admission_destroy(&foreign_admission);

    memset(&stale_type, 0, sizeof(stale_type));
    stale_type.kind = CM_HIR_TYPE_BOOL_KIND;
    stale_type.span = u32_item->span;
    assert(cm_hir_add_type(&fixture.hir, &stale_type, &stale_type_id)
            == CM_HIR_OK);
    materialized = duplicate_type_id;
    assert(cm_semantic_type_view_materialize_existing_hir(results,
            &admission, &u32_signature.return_type, &materialized)
                == CM_SEMANTIC_RESULTS_STALE
        && materialized == CM_HIR_TYPE_NONE);

    cm_semantic_admission_destroy(&admission);
    fixture_destroy(&fixture);
}

typedef struct MalformedFactsProbe {
    CmHirBodyId body;
    size_t invocation_count;
} MalformedFactsProbe;

static void assert_stage_rejects(CmSemanticSession *session,
    CmHirBodyId body, const CmSemanticCheckedBodyFacts *facts)
{
    CmSemanticResultsBodyStage stage;

    cm_semantic_results_body_stage_init(&stage);
    assert(cm_semantic_results_stage_checked_body(&stage, session, body,
            facts) == CM_SEMANTIC_BODY_WRITEBACK_INVALID
        && stage.state == NULL);
    cm_semantic_results_body_stage_destroy(&stage);
}

typedef struct ReceiverAdjustmentProbe {
    CmHirBodyId body;
    CmSemanticAdjustmentKind expected_kind;
    size_t invocation_count;
} ReceiverAdjustmentProbe;

static CmSemanticBodyWritebackStatus receiver_adjustment_probe_writeback(
    void *context, CmSemanticSession *session, CmHirBodyId body,
    const CmSemanticCheckedBodyFacts *facts)
{
    ReceiverAdjustmentProbe *probe;
    CmSemanticCheckedBodyFacts mutated;
    CmSemanticCheckedAdjustmentFacts adjustments[2];

    probe = (ReceiverAdjustmentProbe *)context;
    assert(probe != NULL && session != NULL && facts != NULL
        && body == probe->body
        && facts->callable_count == 1u
        && facts->callables != NULL
        && facts->adjustment_count == 1u
        && facts->adjustments != NULL
        && facts->adjustments[0].kind == probe->expected_kind);
    probe->invocation_count += 1u;

    mutated = *facts;
    mutated.adjustment_count = 0u;
    assert_stage_rejects(session, body, &mutated);

    adjustments[0] = facts->adjustments[0];
    adjustments[0].kind = probe->expected_kind
            == CM_SEMANTIC_ADJUSTMENT_BORROW_SHARED
        ? CM_SEMANTIC_ADJUSTMENT_BORROW_MUTABLE
        : CM_SEMANTIC_ADJUSTMENT_BORROW_SHARED;
    mutated = *facts;
    mutated.adjustments = adjustments;
    assert_stage_rejects(session, body, &mutated);

    adjustments[0] = facts->adjustments[0];
    adjustments[0].source_type = adjustments[0].target_type;
    mutated = *facts;
    mutated.adjustments = adjustments;
    assert_stage_rejects(session, body, &mutated);

    adjustments[0] = facts->adjustments[0];
    adjustments[0].has_selected_trait = 1;
    adjustments[0].selected_trait = facts->callables[0].requested_trait;
    adjustments[0].selected_method =
        facts->callables[0].declared_trait_callable;
    adjustments[0].selected_impl = facts->callables[0].selected_impl;
    mutated = *facts;
    mutated.adjustments = adjustments;
    assert_stage_rejects(session, body, &mutated);

    adjustments[0] = facts->adjustments[0];
    adjustments[1] = facts->adjustments[0];
    adjustments[1].source_type = adjustments[0].target_type;
    mutated = *facts;
    mutated.adjustments = adjustments;
    mutated.adjustment_count = 2u;
    assert_stage_rejects(session, body, &mutated);

    return CM_SEMANTIC_BODY_WRITEBACK_OK;
}

static CmSemanticBodyWritebackStatus malformed_facts_probe_writeback(
    void *context, CmSemanticSession *session, CmHirBodyId body,
    const CmSemanticCheckedBodyFacts *facts)
{
    MalformedFactsProbe *probe;
    CmSemanticCheckedBodyFacts mutated;
    CmSemanticCheckedPrimitiveBinaryFacts primitives[2];
    CmSemanticCheckedFieldSelectionFacts field;
    CmSemanticCheckedAdjustmentFacts adjustments[3];

    probe = (MalformedFactsProbe *)context;
    assert(probe != NULL && session != NULL && facts != NULL
        && body == probe->body
        && facts->primitive_binary_count == 1u
        && facts->primitive_binaries != NULL
        && facts->field_selection_count == 2u
        && facts->field_selections != NULL
        && facts->adjustment_count == 0u);
    probe->invocation_count += 1u;

    mutated = *facts;
    primitives[0] = facts->primitive_binaries[0];
    primitives[0].operator_kind = primitives[0].operator_kind
            == CM_HIR_BINARY_ADD
        ? CM_HIR_BINARY_SUBTRACT : CM_HIR_BINARY_ADD;
    mutated.primitive_binaries = primitives;
    assert_stage_rejects(session, body, &mutated);

    mutated = *facts;
    primitives[0] = facts->primitive_binaries[0];
    primitives[1] = facts->primitive_binaries[0];
    mutated.primitive_binaries = primitives;
    mutated.primitive_binary_count = 2u;
    assert_stage_rejects(session, body, &mutated);

    mutated = *facts;
    field = facts->field_selections[0];
    field.field_index = field.field_index == 0u ? 1u : 0u;
    mutated.field_selections = &field;
    mutated.field_selection_count = 1u;
    assert_stage_rejects(session, body, &mutated);

    mutated = *facts;
    field = facts->field_selections[0];
    field.aggregate_definition = cm_hir_def_id_none();
    mutated.field_selections = &field;
    mutated.field_selection_count = 1u;
    assert_stage_rejects(session, body, &mutated);

    memset(adjustments, 0, sizeof(adjustments));
    adjustments[0].expression = facts->primitive_binaries[0].expression;
    adjustments[0].kind = CM_SEMANTIC_ADJUSTMENT_DEREFERENCE_BUILTIN;
    adjustments[0].source_type = facts->primitive_binaries[0].result_type;
    adjustments[0].target_type = facts->primitive_binaries[0].result_type;
    adjustments[0].has_selected_trait = 1;
    mutated = *facts;
    mutated.adjustments = adjustments;
    mutated.adjustment_count = 1u;
    assert_stage_rejects(session, body, &mutated);

    memset(adjustments, 0, sizeof(adjustments));
    adjustments[0].expression =
        facts->primitive_binaries[0].left_expression;
    adjustments[0].kind = CM_SEMANTIC_ADJUSTMENT_NEVER_TO_ANY;
    adjustments[0].source_type = facts->primitive_binaries[0].left_type;
    adjustments[0].target_type = facts->primitive_binaries[0].left_type;
    adjustments[1].expression =
        facts->primitive_binaries[0].right_expression;
    adjustments[1].kind = CM_SEMANTIC_ADJUSTMENT_NEVER_TO_ANY;
    adjustments[1].source_type = facts->primitive_binaries[0].right_type;
    adjustments[1].target_type = facts->primitive_binaries[0].right_type;
    adjustments[2] = adjustments[0];
    mutated = *facts;
    mutated.adjustments = adjustments;
    mutated.adjustment_count = 3u;
    assert_stage_rejects(session, body, &mutated);

    return CM_SEMANTIC_BODY_WRITEBACK_OK;
}

static void test_malformed_recipe_facts_publish_no_stage(void)
{
    Fixture fixture;
    CmSemanticAdmission admission;
    CmSemanticAdmissionResult admission_result;
    CmHirCrateFinalization finalization;
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmSemanticBodyResult body_result;
    MalformedFactsProbe probe;
    const CmHirBody *body;

    fixture_init(&fixture,
        "struct Pair { left: u32, right: u32 } "
        "fn sum(value: Pair) -> u32 { value.left + value.right }");
    memset(&admission, 0, sizeof(admission));
    admission_result = admit(&fixture, &admission);
    assert(admission_result.status == CM_SEMANTIC_ADMISSION_OK);
    body = cm_hir_get_body(&fixture.hir, 1u);
    assert(body != NULL);
    memset(&finalization, 0, sizeof(finalization));
    assert(cm_hir_crate_finalization_init(&finalization, &fixture.hir, 1u)
        == CM_HIR_OK);
    memset(&session, 0, sizeof(session));
    cm_semantic_session_options_init(&options);
    options.local_crate = 1u;
    options.exact_owner = body->owner;
    options.universe = CM_TRAIT_IMPL_UNIVERSE_SINGLE_LOCAL_CRATE_COMPLETE;
    options.finalization = &finalization;
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    memset(&probe, 0, sizeof(probe));
    probe.body = 1u;
    body_result = cm_semantic_body_check_definition_with_writeback(&session,
        1u, malformed_facts_probe_writeback, &probe);
    assert(body_result.status == CM_SEMANTIC_BODY_OK
        && probe.invocation_count == 1u);
    cm_semantic_session_destroy(&session);
    cm_hir_crate_finalization_destroy(&finalization);
    cm_semantic_admission_destroy(&admission);
    fixture_destroy(&fixture);
}

static void test_partial_checked_draft_does_not_seal(void)
{
    Fixture fixture;
    CmSemanticAdmission admission;
    CmSemanticAdmissionResult admission_result;
    CmHirCrateFinalization finalization;
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmSemanticBodyResult body_result;
    CmSemanticResults *draft;
    CmSemanticResultsBodyStage stage;
    const CmHirBody *body;

    fixture_init(&fixture,
        "fn first() -> u32 { 1u32 } fn second() -> u32 { 2u32 }");
    memset(&admission, 0, sizeof(admission));
    admission_result = admit(&fixture, &admission);
    assert(admission_result.status == CM_SEMANTIC_ADMISSION_OK);
    body = cm_hir_get_body(&fixture.hir, 1u);
    assert(body != NULL);
    memset(&finalization, 0, sizeof(finalization));
    assert(cm_hir_crate_finalization_init(&finalization, &fixture.hir, 1u)
        == CM_HIR_OK);
    memset(&session, 0, sizeof(session));
    cm_semantic_session_options_init(&options);
    options.local_crate = 1u;
    options.exact_owner = body->owner;
    options.universe = CM_TRAIT_IMPL_UNIVERSE_SINGLE_LOCAL_CRATE_COMPLETE;
    options.finalization = &finalization;
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    cm_semantic_results_body_stage_init(&stage);
    body_result = cm_semantic_body_check_definition_with_writeback(&session,
        1u, cm_semantic_results_stage_checked_body, &stage);
    assert(body_result.status == CM_SEMANTIC_BODY_OK);
    draft = NULL;
    assert(cm_semantic_results_begin(&fixture.hir, 1u, &draft)
            == CM_SEMANTIC_RESULTS_OK
        && draft != NULL
        && cm_semantic_results_seal_kind(draft)
            == CM_SEMANTIC_RESULTS_SEAL_UNSEALED
        && cm_semantic_results_commit_checked_body(draft, &session,
            &body_result, &stage) == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_results_commit_checked_body(draft, &session,
            &body_result, &stage) == CM_SEMANTIC_RESULTS_INVALID_ARGUMENT
        && cm_semantic_results_seal(draft)
            == CM_SEMANTIC_RESULTS_INVALID_HIR
        && cm_semantic_results_seal_kind(draft)
            == CM_SEMANTIC_RESULTS_SEAL_UNSEALED);
    cm_semantic_results_body_stage_destroy(&stage);
    cm_semantic_results_destroy(draft);
    cm_semantic_session_destroy(&session);
    cm_hir_crate_finalization_destroy(&finalization);
    cm_semantic_admission_destroy(&admission);
    fixture_destroy(&fixture);
}

static void test_instance_commit_requires_producer_session(void)
{
    Fixture fixture;
    CmSemanticAdmission admission;
    CmSemanticAdmissionResult admission_result;
    CmHirCrateFinalization finalization;
    CmSemanticSession producer;
    CmSemanticSession other;
    CmSemanticSessionOptions options;
    CmSemanticBodyResult body_result;
    CmSemanticResults *draft;
    CmSemanticResultsBodyStage stage;
    CmHirInstanceSpec spec;
    CmHirCanonicalInstance identity;
    const CmHirBody *body;

    fixture_init(&fixture, "fn value() -> u32 { 1u32 }");
    memset(&admission, 0, sizeof(admission));
    admission_result = admit(&fixture, &admission);
    assert(admission_result.status == CM_SEMANTIC_ADMISSION_OK);
    body = cm_hir_get_body(&fixture.hir, 1u);
    assert(body != NULL);
    memset(&finalization, 0, sizeof(finalization));
    assert(cm_hir_crate_finalization_init(&finalization, &fixture.hir, 1u)
        == CM_HIR_OK);
    cm_semantic_session_options_init(&options);
    options.local_crate = 1u;
    options.exact_owner = body->owner;
    options.universe = CM_TRAIT_IMPL_UNIVERSE_SINGLE_LOCAL_CRATE_COMPLETE;
    options.finalization = &finalization;
    memset(&producer, 0, sizeof(producer));
    memset(&other, 0, sizeof(other));
    assert(cm_semantic_session_init(&producer, &fixture.hir, &options)
            == CM_TRAIT_SOLVER_PROVEN
        && cm_semantic_session_init(&other, &fixture.hir, &options)
            == CM_TRAIT_SOLVER_PROVEN
        && cm_semantic_session_is_current(&producer)
        && cm_semantic_session_is_current(&other));

    cm_semantic_results_body_stage_init(&stage);
    body_result = cm_semantic_body_check_definition_with_writeback(&producer,
        1u, cm_semantic_results_stage_checked_body, &stage);
    assert(body_result.status == CM_SEMANTIC_BODY_OK
        && stage.state != NULL);
    cm_hir_instance_spec_init(&spec);
    spec.selected_callable = body->owner;
    spec.body_definition = body->owner;
    cm_hir_canonical_instance_init(&identity);
    assert(cm_hir_canonical_instance_encode(&fixture.hir, 1u, &spec,
            &identity) == CM_HIR_INSTANCE_OK
        && identity.body == 1u);
    draft = NULL;
    assert(cm_semantic_results_begin(&fixture.hir, 1u, &draft)
            == CM_SEMANTIC_RESULTS_OK
        && draft != NULL
        && cm_semantic_results_commit_checked_instance(draft, &other,
            &identity, &body_result, &stage, NULL, 0u)
            == CM_SEMANTIC_RESULTS_STALE
        && stage.state != NULL);

    cm_semantic_results_body_stage_destroy(&stage);
    assert(stage.state == NULL);
    cm_semantic_results_destroy(draft);
    cm_hir_canonical_instance_destroy(&identity);
    cm_semantic_session_destroy(&other);
    cm_semantic_session_destroy(&producer);
    cm_hir_crate_finalization_destroy(&finalization);
    cm_semantic_admission_destroy(&admission);
    fixture_destroy(&fixture);
}

static void test_writeback_distinguishes_unsolved_terms(void)
{
    Fixture fixture;
    CmSemanticAdmission admission;
    CmSemanticAdmissionResult admission_result;
    CmHirCrateFinalization finalization;
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmSemanticResultsBodyStage stage;
    CmTypeckContext *typeck;
    const CmHirBody *body;
    CmTypeckType projection;
    CmTypeckTypeId terms[8];
    CmTypeckTypeId variable;
    CmTypeckTypeId projection_type;
    CmSemanticCheckedBodyFacts facts;
    size_t expression_index;

    fixture_init(&fixture, "fn value() -> u32 { 1u32 }");
    assert(fixture.hir.expressions.len <= 8u);
    memset(&admission, 0, sizeof(admission));
    admission_result = admit(&fixture, &admission);
    assert(admission_result.status == CM_SEMANTIC_ADMISSION_OK);
    body = cm_hir_get_body(&fixture.hir, 1u);
    assert(body != NULL);
    memset(&finalization, 0, sizeof(finalization));
    assert(cm_hir_crate_finalization_init(&finalization, &fixture.hir, 1u)
        == CM_HIR_OK);
    memset(&session, 0, sizeof(session));
    cm_semantic_session_options_init(&options);
    options.local_crate = 1u;
    options.exact_owner = body->owner;
    options.universe = CM_TRAIT_IMPL_UNIVERSE_SINGLE_LOCAL_CRATE_COMPLETE;
    options.finalization = &finalization;
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    typeck = cm_semantic_session_typeck(&session);
    assert(typeck != NULL);
    memset(&facts, 0, sizeof(facts));
    facts.expression_terms = terms;
    facts.expression_term_count = fixture.hir.expressions.len;

    cm_semantic_results_body_stage_init(&stage);
    assert(cm_typeck_new_variable(typeck, CM_HIR_INFER_GENERAL,
        (CmSpan){ fixture.source, 0u, 1u }, &variable) == CM_TYPECK_OK);
    for (expression_index = 0u;
            expression_index < fixture.hir.expressions.len;
            ++expression_index) {
        const CmHirExpr *expression;

        expression = cm_hir_get_expr(&fixture.hir,
            (CmHirExprId)(expression_index + 1u));
        assert(expression != NULL);
        terms[expression_index] = expression->owner_body == 1u
            ? variable : CM_TYPECK_TYPE_NONE;
    }
    facts.signature_return_type = variable;
    assert(cm_semantic_results_stage_checked_body(&stage, &session, 1u,
        &facts)
            == CM_SEMANTIC_BODY_WRITEBACK_DEFERRED_INFERENCE
        && stage.state == NULL);

    memset(&projection, 0, sizeof(projection));
    projection.kind = CM_TYPECK_TYPE_PROJECTION;
    projection.span = (CmSpan){ fixture.source, 0u, 1u };
    projection.data.projection_type.self_type = variable;
    projection.data.projection_type.trait_type.definition = body->owner;
    projection.data.projection_type.associated_type.definition = body->owner;
    assert(cm_typeck_add_type(typeck, &projection, &projection_type)
        == CM_TYPECK_OK);
    for (expression_index = 0u;
            expression_index < fixture.hir.expressions.len;
            ++expression_index) {
        if (terms[expression_index] != CM_TYPECK_TYPE_NONE) {
            terms[expression_index] = projection_type;
        }
    }
    facts.signature_return_type = projection_type;
    assert(cm_semantic_results_stage_checked_body(&stage, &session, 1u,
        &facts)
            == CM_SEMANTIC_BODY_WRITEBACK_PENDING_PROJECTION
        && stage.state == NULL);

    cm_semantic_results_body_stage_destroy(&stage);
    cm_semantic_session_destroy(&session);
    cm_hir_crate_finalization_destroy(&finalization);
    cm_semantic_admission_destroy(&admission);
    fixture_destroy(&fixture);
}

static void test_durable_projection_trace_definition(void)
{
    Fixture fixture;
    CmSemanticAdmission admission;
    CmSemanticAdmissionResult result;
    const CmSemanticResults *results;
    CmSemanticBodyView body_view;
    CmSemanticProjectionTraceView trace_view;
    CmSemanticProjectionStepView step_view;
    CmSemanticExpressionView expression_view;
    CmSemanticAdmission instance_admission;
    CmSemanticAdmissionResult instance_result;
    CmSemanticReachableInstance reachable;
    CmHirInstanceSpec spec;
    CmHirCanonicalInstance identity;
    unsigned char *tampered_identity;
    unsigned char *trailing_identity;
    int equal;
    const CmHirBody *body;
    CmHirBody *mutable_body;
    CmHirExpr *root;
    CmHirItem *owner;
    CmHirDefId trait_definition;
    CmHirDefId associated_definition;
    CmHirTypeId bool_type;
    CmHirTypeId projection;
    size_t index;
    CmHirDefId value_definition;
    const CmHirDefinition *value_record;
    CmHirBodyLowerResult lower_result;

    fixture_init(&fixture,
        "trait Bound { type Output; } "
        "impl Bound for bool { type Output = u32; } "
        "fn value() -> u32 { 1u32 }");
    trait_definition = find_named_item(&fixture, "Bound",
        cm_hir_def_id_none());
    associated_definition = find_named_item(&fixture, "Output",
        trait_definition);
    bool_type = CM_HIR_TYPE_NONE;
    for (index = 0u; index < fixture.hir.types.len; ++index) {
        const CmHirType *type;
        type = cm_hir_get_type(&fixture.hir, (CmHirTypeId)(index + 1u));
        if (type != NULL && type->kind == CM_HIR_TYPE_BOOL_KIND) {
            bool_type = (CmHirTypeId)(index + 1u);
            break;
        }
    }
    value_definition = find_named_item(&fixture, "value",
        cm_hir_def_id_none());
    value_record = cm_hir_lookup_definition(&fixture.hir, value_definition);
    owner = value_record == NULL ? NULL : (CmHirItem *)cm_hir_get_item(
        &fixture.hir, value_record->entity.item_id);
    assert(owner != NULL);
    lower_result = cm_hir_lower_body(&fixture.hir,
        owner->data.function_item.body, &fixture.graph,
        fixture.graph_result.revision, &fixture.imports, &fixture.modules);
    assert(lower_result.status == CM_HIR_BODY_LOWER_OK);
    owner = (CmHirItem *)cm_hir_get_item(&fixture.hir,
        value_record->entity.item_id);
    body = owner == NULL ? NULL : cm_hir_get_body(&fixture.hir,
        owner->data.function_item.body);
    mutable_body = (CmHirBody *)body;
    root = body == NULL ? NULL : (CmHirExpr *)cm_hir_get_expr(&fixture.hir,
        body->root_expression);
    assert(body != NULL && root != NULL && owner != NULL
        && bool_type != CM_HIR_TYPE_NONE
        && !cm_hir_def_id_is_none(trait_definition)
        && !cm_hir_def_id_is_none(associated_definition));
    projection = add_projection_type(&fixture, trait_definition,
        associated_definition, bool_type);
    owner->data.function_item.signature.return_type = projection;
    mutable_body->expected_type = projection;
    root->type = projection;
    memset(&admission, 0, sizeof(admission));
    result = admit(&fixture, &admission);
    assert(result.status == CM_SEMANTIC_ADMISSION_OK);
    results = cm_semantic_admission_results(&admission);
    body = cm_hir_get_body(&fixture.hir, owner->data.function_item.body);
    assert(results != NULL && body != NULL
        && cm_semantic_results_body(results, &admission,
            owner->data.function_item.body, &body_view)
            == CM_SEMANTIC_RESULTS_OK
        && body_view.projection_trace_count == 1u
        && body_view.projection_step_count == 1u
        && cm_semantic_results_projection_trace(results, &admission,
            owner->data.function_item.body,
            body->root_expression,
            CM_SEMANTIC_PROJECTION_DECISION_EXPRESSION_TYPE, 0u,
            &trace_view) == CM_SEMANTIC_RESULTS_OK
        && trace_view.expression == body->root_expression
        && trace_view.step_count == 1u
        && cm_semantic_results_projection_trace_step(results, &admission,
            owner->data.function_item.body, 0u, 0u, &step_view)
                == CM_SEMANTIC_RESULTS_OK
        && step_view.proof_origin == CM_TRAIT_PROOF_IMPL
        && cm_semantic_results_expression(results, &admission,
            owner->data.function_item.body, body->root_expression,
            &expression_view) == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_type_view_equal(&trace_view.normalized_type,
            &expression_view.unadjusted_type, &equal)
            == CM_SEMANTIC_RESULTS_OK
        && equal
        && cm_semantic_results_projection_trace(results, &admission,
            owner->data.function_item.body, CM_HIR_EXPR_NONE,
            CM_SEMANTIC_PROJECTION_DECISION_EXPRESSION_TYPE, 0u,
            &trace_view) == CM_SEMANTIC_RESULTS_NOT_FOUND
        && cm_semantic_results_projection_trace(results, &admission,
            owner->data.function_item.body, body->root_expression,
            CM_SEMANTIC_PROJECTION_DECISION_EXPRESSION_TYPE, 1u,
            &trace_view) == CM_SEMANTIC_RESULTS_NOT_FOUND);
    cm_semantic_admission_destroy(&admission);

    cm_hir_instance_spec_init(&spec);
    spec.selected_callable = owner->definition;
    spec.body_definition = owner->definition;
    reachable.body = owner->data.function_item.body;
    reachable.spec = &spec;
    memset(&instance_admission, 0, sizeof(instance_admission));
    instance_result = cm_semantic_admit_typed_leaf_instances(
        &instance_admission, &fixture.hir, 1u, &reachable, 1u);
    assert(instance_result.status == CM_SEMANTIC_ADMISSION_OK);
    results = cm_semantic_admission_results(&instance_admission);
    cm_hir_canonical_instance_init(&identity);
    assert(cm_hir_canonical_instance_encode(&fixture.hir, 1u, &spec,
            &identity) == CM_HIR_INSTANCE_OK && identity.size != 0u);
    tampered_identity = (unsigned char *)cm_alloc(identity.size);
    memcpy(tampered_identity, identity.bytes, identity.size);
    tampered_identity[identity.size - 1u] ^= 1u;
    trailing_identity = (unsigned char *)cm_alloc(identity.size + 1u);
    memcpy(trailing_identity, identity.bytes, identity.size);
    trailing_identity[identity.size] = 0u;
    assert(results != NULL
        && cm_semantic_results_instance_body(results, &instance_admission,
            &spec, &body_view) == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_results_canonical_instance_body(results,
            &instance_admission, identity.definition,
            identity.body_definition, identity.body,
            identity.bytes, identity.size, &body_view)
                == CM_SEMANTIC_RESULTS_OK
        && body_view.projection_trace_count == 1u
        && body_view.projection_step_count == 1u
        && cm_semantic_results_canonical_instance_body(results,
            &instance_admission, identity.definition,
            identity.body_definition, identity.body,
            tampered_identity, identity.size, &body_view)
                == CM_SEMANTIC_RESULTS_INVALID_ARGUMENT
        && cm_semantic_results_canonical_instance_body(results,
            &instance_admission, identity.definition,
            identity.body_definition, identity.body,
            identity.bytes, identity.size - 1u, &body_view)
                == CM_SEMANTIC_RESULTS_INVALID_ARGUMENT
        && cm_semantic_results_canonical_instance_body(results,
            &instance_admission, identity.definition,
            identity.body_definition, identity.body,
            trailing_identity, identity.size + 1u, &body_view)
                == CM_SEMANTIC_RESULTS_INVALID_ARGUMENT
        && cm_semantic_results_canonical_instance_expression(results,
            &instance_admission, &identity, body->root_expression,
            &expression_view) == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_results_canonical_instance_body(results,
            &instance_admission, identity.definition,
            identity.body_definition, identity.body,
            NULL, identity.size, &body_view)
                == CM_SEMANTIC_RESULTS_INVALID_ARGUMENT
        && cm_semantic_results_instance_projection_trace(results,
            &instance_admission, &spec, body->root_expression,
            CM_SEMANTIC_PROJECTION_DECISION_EXPRESSION_TYPE, 0u,
            &trace_view) == CM_SEMANTIC_RESULTS_OK
        && trace_view.expression == body->root_expression
        && trace_view.step_count == 1u
        && cm_semantic_results_instance_projection_trace_step(results,
            &instance_admission, &spec, 0u, 0u, &step_view)
            == CM_SEMANTIC_RESULTS_OK
        && step_view.proof_origin == CM_TRAIT_PROOF_IMPL
        && cm_semantic_results_instance_projection_trace_step(results,
            &instance_admission, &spec, 0u, 1u, &step_view)
            == CM_SEMANTIC_RESULTS_NOT_FOUND);
    cm_free(trailing_identity);
    cm_free(tampered_identity);
    cm_hir_canonical_instance_destroy(&identity);
    cm_semantic_admission_destroy(&instance_admission);
    fixture_destroy(&fixture);
}

static void assert_exact_callable_instance_recipe(Fixture *fixture,
    CmHirDefId caller_definition, CmHirBodyId caller_body,
    CmHirExprId expression, CmHirTypeId self_type,
    const CmSemanticCallableSelectionView *expected)
{
    CmSemanticAdmission admission;
    CmSemanticAdmissionResult result;
    CmSemanticReachableInstance reachable[2];
    CmSemanticReachableInstanceCall edge;
    CmHirInstanceSpec caller;
    CmHirInstanceSpec callee;
    CmHirInstanceSpec wrong_callee;
    const CmHirDefinition *callee_record;
    const CmHirItem *callee_item;
    const CmSemanticResults *results;
    CmSemanticCallableSelectionView selection;
    CmSemanticTypeView parameter;
    CmHirCanonicalInstance retained_identity;
    CmHirCanonicalInstance expected_identity;
    int identity_equal;
    uint32_t parameter_index;

    assert(fixture != NULL && expected != NULL
        && expression != CM_HIR_EXPR_NONE
        && self_type != CM_HIR_TYPE_NONE);
    callee_record = cm_hir_lookup_definition(&fixture->hir,
        expected->selected_callable);
    callee_item = callee_record == NULL ? NULL : cm_hir_get_item(
        &fixture->hir, callee_record->entity.item_id);
    assert(callee_item != NULL && callee_item->kind == CM_HIR_ITEM_FUNCTION
        && callee_item->data.function_item.body != CM_HIR_BODY_NONE);

    cm_hir_instance_spec_init(&caller);
    caller.selected_callable = caller_definition;
    caller.body_definition = caller_definition;
    cm_hir_instance_spec_init(&callee);
    callee.selected_callable = expected->selected_callable;
    callee.body_definition = expected->selected_callable;
    callee.declared_trait_callable = expected->declared_trait_callable;
    callee.enclosing_impl = expected->enclosing_impl;
    callee.implemented_trait = expected->implemented_trait;
    callee.self_owner = expected->self_owner;
    callee.self_type = self_type;
    cm_hir_instance_spec_init(&wrong_callee);
    wrong_callee.selected_callable = caller_definition;
    wrong_callee.body_definition = caller_definition;
    reachable[0].body = caller_body;
    reachable[0].spec = &caller;
    reachable[1].body = callee_item->data.function_item.body;
    reachable[1].spec = &callee;
    edge.caller = &caller;
    edge.expression = expression;
    edge.callee = &callee;

    memset(&admission, 0, sizeof(admission));
    result = cm_semantic_admit_typed_instance_closure(&admission,
        &fixture->hir, 1u, reachable, 2u, &edge, 1u);
    results = cm_semantic_admission_results(&admission);
    assert(result.status == CM_SEMANTIC_ADMISSION_OK && results != NULL
        && cm_semantic_results_instance_callable_selection_for_callee(
            results, &admission, &caller, expression, &callee, &selection)
            == CM_SEMANTIC_RESULTS_OK
        && selection.syntax == expected->syntax
        && cm_hir_def_id_equal(selection.selected_callable,
            expected->selected_callable)
        && cm_hir_def_id_equal(selection.declared_trait_callable,
            expected->declared_trait_callable)
        && cm_hir_def_id_equal(selection.enclosing_impl,
            expected->enclosing_impl)
        && cm_hir_def_id_equal(selection.implemented_trait,
            expected->implemented_trait)
        && cm_hir_def_id_equal(selection.self_owner,
            expected->self_owner)
        && selection.item_argument_count == 0u
        && selection.method_argument_count == 0u
        && selection.enclosing_impl_argument_count == 0u
        && selection.implemented_trait_argument_count == 0u
        && cm_semantic_results_instance_callable_selection_for_callee(
            results, &admission, &caller, expression, &wrong_callee,
            &selection) == CM_SEMANTIC_RESULTS_NOT_FOUND);
    cm_hir_canonical_instance_init(&retained_identity);
    cm_hir_canonical_instance_init(&expected_identity);
    assert(cm_semantic_results_instance_callable_callee_identity(results,
            &admission, &caller, expression, &retained_identity)
            == CM_SEMANTIC_RESULTS_OK
        && cm_hir_canonical_instance_encode(&fixture->hir, 1u, &callee,
            &expected_identity) == CM_HIR_INSTANCE_OK
        && cm_hir_canonical_instance_equal(&retained_identity,
            &expected_identity, &identity_equal) == CM_HIR_INSTANCE_OK
        && identity_equal);
    cm_hir_canonical_instance_destroy(&expected_identity);
    cm_hir_canonical_instance_destroy(&retained_identity);
    assert_no_instance_callable_generic_arguments(results, &admission,
        &caller, expression);
    for (parameter_index = 0u;
         parameter_index < expected->argument_count; ++parameter_index) {
        assert(cm_semantic_results_instance_callable_parameter_for_callee(
            results, &admission, &caller, expression, &callee,
            parameter_index, &parameter) == CM_SEMANTIC_RESULTS_OK);
    }
    assert(cm_semantic_results_instance_callable_parameter_for_callee(
        results, &admission, &caller, expression, &callee,
        expected->argument_count, &parameter)
            == CM_SEMANTIC_RESULTS_NOT_FOUND
        && cm_semantic_results_instance_callable_parameter_for_callee(
            results, &admission, &caller, expression, &wrong_callee, 0u,
            &parameter) == CM_SEMANTIC_RESULTS_NOT_FOUND);
    cm_semantic_admission_destroy(&admission);
}

static void test_canonical_parts_function_pointer_abi_parity(void)
{
    Fixture fixture;
    CmHirLocalBodiesResult lower_result;
    CmHirItem *owner;
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_id;
    CmHirType function_pointer;
    CmHirTypeId function_pointer_id;
    CmHirTypeId u32_type;
    CmHirTypeId parameters[1];
    CmHirGenericArg argument;
    CmHirInstanceSpec spec;
    CmHirCanonicalArgumentPart part;
    CmHirCanonicalInstanceParts parts;
    CmHirCanonicalInstance manual;
    CmHirCanonicalInstance structural;
    unsigned char payload[] = {
        (unsigned char)CM_HIR_TYPE_FN_POINTER_KIND,
        1u, 0u, 0u, 0u,
        (unsigned char)CM_HIR_TYPE_INTEGER_KIND,
        (unsigned char)CM_HIR_INT_U32,
        (unsigned char)CM_HIR_TYPE_INTEGER_KIND,
        (unsigned char)CM_HIR_INT_U32,
        1u, 0u, 0u, 0u, (unsigned char)'C',
        (unsigned char)CM_HIR_SAFE, 0u
    };
    int equal;

    fixture_init(&fixture, "fn callable() -> u32 { 0u32 }");
    lower_result = cm_hir_lower_local_bodies(&fixture.hir, 1u,
        &fixture.graph, fixture.graph_result.revision, &fixture.imports,
        &fixture.modules);
    owner = (CmHirItem *)find_function_item(&fixture, "callable");
    u32_type = find_integer_type(&fixture, CM_HIR_INT_U32);
    assert(lower_result.status == CM_HIR_LOCAL_BODIES_OK
        && owner != NULL && u32_type != CM_HIR_TYPE_NONE);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = owner->definition;
    parameter.name = cm_hir_intern(&fixture.hir, "F");
    parameter.span = owner->span;
    assert(cm_hir_add_generic_param(&fixture.hir, &parameter,
        &parameter_id) == CM_HIR_OK);
    owner->generic_parameter_start = parameter_id;
    owner->generic_parameter_count = 1u;
    parameters[0] = u32_type;
    memset(&function_pointer, 0, sizeof(function_pointer));
    function_pointer.kind = CM_HIR_TYPE_FN_POINTER_KIND;
    function_pointer.span = owner->span;
    function_pointer.data.fn_pointer_type.parameters = parameters;
    function_pointer.data.fn_pointer_type.parameter_count = 1u;
    function_pointer.data.fn_pointer_type.return_type = u32_type;
    function_pointer.data.fn_pointer_type.abi =
        cm_hir_intern(&fixture.hir, "C");
    function_pointer.data.fn_pointer_type.safety = CM_HIR_SAFE;
    assert(cm_hir_add_type(&fixture.hir, &function_pointer,
        &function_pointer_id) == CM_HIR_OK);
    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    argument.data.type = function_pointer_id;
    cm_hir_instance_spec_init(&spec);
    spec.selected_callable = owner->definition;
    spec.body_definition = owner->definition;
    spec.item_arguments = &argument;
    spec.item_argument_count = 1u;
    memset(&parts, 0, sizeof(parts));
    parts.selected_callable = owner->definition;
    parts.body_definition = owner->definition;
    part.kind = CM_HIR_GENERIC_ARG_TYPE;
    part.bytes = payload;
    part.size = sizeof(payload);
    parts.item_arguments = &part;
    parts.item_argument_count = 1u;
    cm_hir_canonical_instance_init(&manual);
    cm_hir_canonical_instance_init(&structural);
    assert(cm_hir_canonical_instance_encode(&fixture.hir, 1u, &spec,
            &manual) == CM_HIR_INSTANCE_OK
        && cm_hir_canonical_instance_encode_parts(&fixture.hir, 1u,
            &parts, &structural) == CM_HIR_INSTANCE_OK
        && cm_hir_canonical_instance_equal(&manual, &structural, &equal)
            == CM_HIR_INSTANCE_OK && equal);
    cm_hir_canonical_instance_destroy(&structural);
    cm_hir_canonical_instance_destroy(&manual);
    fixture_destroy(&fixture);
}

static void test_durable_qualified_callable_recipe(void)
{
    Fixture fixture;
    CmSemanticAdmission admission;
    CmSemanticAdmissionResult result;
    const CmSemanticResults *results;
    CmSemanticCallableSelectionView selection;
    CmSemanticBodyView body_view;
    CmSemanticExpressionView argument_view;
    CmSemanticTypeView parameter_type;
    CmHirDefId wrapper_definition;
    const CmHirDefinition *wrapper_record;
    const CmHirItem *wrapper;
    const CmHirBody *body;
    const CmHirExpr *source_call;
    CmHirExprId call_expression;
    CmHirExprId argument;
    size_t index;
    int equal;

    fixture_init(&fixture,
        "trait Convert { fn convert(value: u32) -> u32; } "
        "impl Convert for u32 { fn convert(value: u32) -> u32 { value } } "
        "fn wrapper(value: u32) -> u32 { <u32 as Convert>::convert(value) }");
    memset(&admission, 0, sizeof(admission));
    result = admit(&fixture, &admission);
    assert(result.status == CM_SEMANTIC_ADMISSION_OK);
    results = cm_semantic_admission_results(&admission);
    wrapper_definition = find_named_item(&fixture, "wrapper",
        cm_hir_def_id_none());
    wrapper_record = cm_hir_lookup_definition(&fixture.hir,
        wrapper_definition);
    wrapper = wrapper_record == NULL ? NULL : cm_hir_get_item(&fixture.hir,
        wrapper_record->entity.item_id);
    body = wrapper == NULL ? NULL : cm_hir_get_body(&fixture.hir,
        wrapper->data.function_item.body);
    call_expression = CM_HIR_EXPR_NONE;
    for (index = 0u; index < fixture.hir.expressions.len; ++index) {
        const CmHirExpr *expression;

        expression = cm_hir_get_expr(&fixture.hir,
            (CmHirExprId)(index + 1u));
        if (body != NULL && expression != NULL
            && expression->owner_body == wrapper->data.function_item.body
            && expression->kind == CM_HIR_EXPR_QUALIFIED_CALL) {
            call_expression = (CmHirExprId)(index + 1u);
        }
    }
    source_call = cm_hir_get_expr(&fixture.hir, call_expression);
    assert(results != NULL && body != NULL
        && source_call != NULL
        && call_expression != CM_HIR_EXPR_NONE
        && cm_semantic_results_body(results, &admission,
            wrapper->data.function_item.body, &body_view)
            == CM_SEMANTIC_RESULTS_OK
        && body_view.callable_count == 1u
        && cm_semantic_results_callable_selection(results, &admission,
            wrapper->data.function_item.body, call_expression, &selection)
            == CM_SEMANTIC_RESULTS_OK
        && selection.syntax == CM_HIR_CALLABLE_QUALIFIED_TRAIT_METHOD
        && selection.argument_count == 1u
        && selection.receiver_argument == CM_HIR_CALLABLE_RECEIVER_NONE
        && selection.receiver_expression == CM_HIR_EXPR_NONE
        && cm_hir_def_id_equal(selection.enclosing_impl,
            selection.selected_impl)
        && cm_hir_def_id_equal(selection.implemented_trait,
            selection.requested_trait)
        && cm_hir_def_id_equal(selection.self_owner,
            selection.selected_impl)
        && selection.item_argument_count == 0u
        && selection.method_argument_count == 0u
        && selection.enclosing_impl_argument_count == 0u
        && selection.implemented_trait_argument_count == 0u
        && cm_semantic_results_callable_argument(results, &admission,
            wrapper->data.function_item.body, call_expression, 0u,
            &argument) == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_results_expression(results, &admission,
            wrapper->data.function_item.body, argument, &argument_view)
            == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_results_callable_parameter(results, &admission,
            wrapper->data.function_item.body, call_expression, 0u,
            &parameter_type) == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_type_view_equal(&parameter_type,
            &argument_view.adjusted_type, &equal) == CM_SEMANTIC_RESULTS_OK
        && equal
        && cm_semantic_results_callable_argument(results, &admission,
            wrapper->data.function_item.body, call_expression, 1u,
            &argument) == CM_SEMANTIC_RESULTS_NOT_FOUND
        && cm_semantic_results_callable_parameter(results, &admission,
            wrapper->data.function_item.body, call_expression, 1u,
            &parameter_type) == CM_SEMANTIC_RESULTS_NOT_FOUND);
    assert_no_callable_generic_arguments(results, &admission,
        wrapper->data.function_item.body, call_expression);
    assert_exact_callable_instance_recipe(&fixture, wrapper_definition,
        wrapper->data.function_item.body, call_expression,
        source_call->data.qualified_call.requested_self_type, &selection);
    cm_semantic_admission_destroy(&admission);
    fixture_destroy(&fixture);
}

static void assert_durable_dot_method_recipe(CmHirReceiverKind receiver_kind)
{
    Fixture fixture;
    CmSemanticAdmission admission;
    CmSemanticAdmissionResult result;
    CmHirLocalBodiesResult lower_result;
    CmSemanticReachableBody reachable;
    const CmSemanticResults *results;
    CmSemanticCallableSelectionView selection;
    CmSemanticBodyView body_view;
    CmSemanticExpressionView call_view;
    CmSemanticExpressionView receiver_view;
    CmSemanticExpressionView argument_view;
    CmSemanticAdjustmentView adjustment_view;
    CmSemanticTypeView parameter_type;
    CmHirDefId wrapper_definition;
    const CmHirDefinition *wrapper_record;
    const CmHirItem *wrapper;
    const CmHirBody *body;
    const CmHirExpr *source_call;
    CmHirExpr *mutable_call;
    CmHirExprId call_expression;
    CmHirExprId argument;
    CmSemanticMarkResult mark_result;
    CmSemanticRegionsResult regions_result;
    CmHirBody *mutable_body;
    const char *source;
    size_t index;
    int borrowed_receiver;
    int mutable_receiver;
    int equal;

    borrowed_receiver = receiver_kind != CM_HIR_RECEIVER_VALUE;
    mutable_receiver = receiver_kind == CM_HIR_RECEIVER_REF_MUTABLE;
    source = mutable_receiver
        ? "trait Value { fn value(&mut self, other: u32) -> u32; } "
          "impl Value for u32 { "
          "fn value(&mut self, other: u32) -> u32 { 1u32 } } "
          "fn wrapper(mut value: u32) -> u32 { value.value(1u32) }"
        : receiver_kind == CM_HIR_RECEIVER_REF_SHARED
        ? "trait Value { fn value(&self, other: u32) -> u32; } "
          "impl Value for u32 { "
          "fn value(&self, other: u32) -> u32 { 1u32 } } "
          "fn wrapper(value: u32) -> u32 { value.value(1u32) }"
        : "trait Value { fn value(self, other: u32) -> u32; } "
          "impl Value for u32 { "
          "fn value(self, other: u32) -> u32 { 1u32 } } "
          "fn wrapper(value: u32) -> u32 { value.value(1u32) }";
    fixture_init(&fixture, source);
    lower_result = cm_hir_lower_local_bodies(&fixture.hir, 1u,
        &fixture.graph, fixture.graph_result.revision, &fixture.imports,
        &fixture.modules);
    assert(lower_result.status == CM_HIR_LOCAL_BODIES_OK);
    wrapper_definition = find_named_item(&fixture, "wrapper",
        cm_hir_def_id_none());
    wrapper_record = cm_hir_lookup_definition(&fixture.hir,
        wrapper_definition);
    wrapper = wrapper_record == NULL ? NULL : cm_hir_get_item(&fixture.hir,
        wrapper_record->entity.item_id);
    body = wrapper == NULL ? NULL : cm_hir_get_body(&fixture.hir,
        wrapper->data.function_item.body);
    assert(body != NULL && body->state == CM_HIR_BODY_TYPED);
    reachable.owner = wrapper_definition;
    reachable.body = wrapper->data.function_item.body;
    memset(&admission, 0, sizeof(admission));
    result = cm_semantic_admit_typed_reachable_bodies(&admission,
        &fixture.hir, 1u, &reachable, 1u);
    assert(result.status == CM_SEMANTIC_ADMISSION_OK);
    results = cm_semantic_admission_results(&admission);
    call_expression = CM_HIR_EXPR_NONE;
    for (index = 0u; index < fixture.hir.expressions.len; ++index) {
        const CmHirExpr *expression;

        expression = cm_hir_get_expr(&fixture.hir,
            (CmHirExprId)(index + 1u));
        if (body != NULL && expression != NULL
            && expression->owner_body == wrapper->data.function_item.body
            && expression->kind == CM_HIR_EXPR_METHOD_CALL) {
            call_expression = (CmHirExprId)(index + 1u);
        }
    }
    source_call = cm_hir_get_expr(&fixture.hir, call_expression);
    assert(results != NULL && body != NULL && source_call != NULL
        && call_expression != CM_HIR_EXPR_NONE
        && cm_semantic_results_body(results, &admission,
            wrapper->data.function_item.body, &body_view)
            == CM_SEMANTIC_RESULTS_OK
        && body_view.callable_count == 1u
        && cm_semantic_results_expression(results, &admission,
            wrapper->data.function_item.body, call_expression, &call_view)
            == CM_SEMANTIC_RESULTS_OK
        && call_view.adjustment_count == 0u
        && cm_semantic_results_callable_selection(results, &admission,
            wrapper->data.function_item.body, call_expression, &selection)
            == CM_SEMANTIC_RESULTS_OK
        && selection.syntax == CM_HIR_CALLABLE_DOT_METHOD
        && selection.argument_count == 2u
        && selection.receiver_argument == 0u
        && selection.receiver_expression
            == source_call->data.method_call.receiver
        && cm_hir_def_id_equal(selection.enclosing_impl,
            selection.selected_impl)
        && cm_hir_def_id_equal(selection.implemented_trait,
            selection.requested_trait)
        && cm_hir_def_id_equal(selection.self_owner,
            selection.selected_impl)
        && selection.item_argument_count == 0u
        && selection.method_argument_count == 0u
        && selection.enclosing_impl_argument_count == 0u
        && selection.implemented_trait_argument_count == 0u
        && cm_semantic_results_callable_argument(results, &admission,
            wrapper->data.function_item.body, call_expression, 0u,
            &argument) == CM_SEMANTIC_RESULTS_OK
        && argument == source_call->data.method_call.receiver
        && cm_semantic_results_expression(results, &admission,
            wrapper->data.function_item.body, argument, &receiver_view)
            == CM_SEMANTIC_RESULTS_OK
        && receiver_view.adjustment_count == (borrowed_receiver ? 1u : 0u)
        && cm_semantic_type_view_equal(&selection.requested_self_type,
            borrowed_receiver ? &receiver_view.unadjusted_type
                : &receiver_view.adjusted_type, &equal)
            == CM_SEMANTIC_RESULTS_OK && equal
        && cm_semantic_results_callable_parameter(results, &admission,
            wrapper->data.function_item.body, call_expression, 0u,
            &parameter_type) == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_type_view_equal(&parameter_type,
            &receiver_view.adjusted_type, &equal)
            == CM_SEMANTIC_RESULTS_OK && equal
        && cm_semantic_results_callable_argument(results, &admission,
            wrapper->data.function_item.body, call_expression, 1u,
            &argument) == CM_SEMANTIC_RESULTS_OK
        && argument == source_call->data.method_call.arguments[0]
        && cm_semantic_results_expression(results, &admission,
            wrapper->data.function_item.body, argument, &argument_view)
            == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_results_callable_parameter(results, &admission,
            wrapper->data.function_item.body, call_expression, 1u,
            &parameter_type) == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_type_view_equal(&parameter_type,
            &argument_view.adjusted_type, &equal)
            == CM_SEMANTIC_RESULTS_OK && equal
        && cm_semantic_type_view_equal(&selection.return_type,
            &call_view.adjusted_type, &equal)
            == CM_SEMANTIC_RESULTS_OK && equal
        && cm_semantic_results_callable_argument(results, &admission,
            wrapper->data.function_item.body, call_expression, 2u,
            &argument) == CM_SEMANTIC_RESULTS_NOT_FOUND);
    memset(&adjustment_view, 0, sizeof(adjustment_view));
    if (borrowed_receiver) {
        assert(cm_semantic_results_expression_adjustment(results, &admission,
                wrapper->data.function_item.body,
                source_call->data.method_call.receiver, 0u,
                &adjustment_view) == CM_SEMANTIC_RESULTS_OK
            && adjustment_view.body
                == wrapper->data.function_item.body
            && adjustment_view.expression
                == source_call->data.method_call.receiver
            && adjustment_view.index == 0u
            && adjustment_view.kind == (mutable_receiver
                    ? CM_SEMANTIC_ADJUSTMENT_BORROW_MUTABLE
                    : CM_SEMANTIC_ADJUSTMENT_BORROW_SHARED)
            && !adjustment_view.has_selected_trait
            && cm_hir_def_id_is_none(adjustment_view.selected_trait)
            && cm_hir_def_id_is_none(adjustment_view.selected_method)
            && cm_hir_def_id_is_none(adjustment_view.selected_impl)
            && cm_semantic_type_view_equal(&adjustment_view.source_type,
                &receiver_view.unadjusted_type, &equal)
                == CM_SEMANTIC_RESULTS_OK && equal
            && cm_semantic_type_view_equal(&adjustment_view.target_type,
                &receiver_view.adjusted_type, &equal)
                == CM_SEMANTIC_RESULTS_OK && equal);
    } else {
        assert(cm_semantic_results_expression_adjustment(results, &admission,
                wrapper->data.function_item.body,
                source_call->data.method_call.receiver, 0u,
                &adjustment_view) == CM_SEMANTIC_RESULTS_NOT_FOUND);
    }
    assert_no_callable_generic_arguments(results, &admission,
        wrapper->data.function_item.body, call_expression);
    assert_exact_callable_instance_recipe(&fixture, wrapper_definition,
        wrapper->data.function_item.body, call_expression,
        cm_hir_get_expr(&fixture.hir,
            source_call->data.method_call.receiver)->type, &selection);

    /* MARKED and REGIONS authenticate the live source slice independently of
     * HIR construction.  A zero count paired with retained storage must not
     * be accepted merely because no element would be dereferenced. */
    mutable_call = (CmHirExpr *)cm_vec_at(&fixture.hir.expressions,
        (size_t)call_expression - 1u);
    mutable_body = (CmHirBody *)cm_vec_at(&fixture.hir.bodies,
        (size_t)wrapper->data.function_item.body - 1u);
    assert(mutable_call != NULL
        && mutable_body != NULL
        && mutable_call->data.method_call.argument_count == 1u
        && mutable_call->data.method_call.arguments != NULL);
    mutable_call->data.method_call.argument_count = 0u;
    mark_result = cm_hir_semantic_mark_admitted_bodies(&fixture.hir,
        &reachable.body, 1u, &admission);
    assert(mark_result.status == CM_SEMANTIC_MARK_INVALID_HIR
        && mark_result.expression == call_expression
        && cm_semantic_admission_is_current(&admission));
    mutable_call->data.method_call.argument_count = 1u;
    if (mutable_receiver) {
        assert(cm_hir_get_expr(&fixture.hir,
                source_call->data.method_call.receiver)->kind
                == CM_HIR_EXPR_LOCAL);
        mutable_body->locals[cm_hir_get_expr(&fixture.hir,
            source_call->data.method_call.receiver)->data.local.local_index]
                .mutability = CM_HIR_IMMUTABLE;
        mark_result = cm_hir_semantic_mark_admitted_bodies(&fixture.hir,
            &reachable.body, 1u, &admission);
        assert(mark_result.status == CM_SEMANTIC_MARK_INVALID_HIR
            && mark_result.expression == call_expression
            && cm_semantic_admission_is_current(&admission));
        mutable_body->locals[cm_hir_get_expr(&fixture.hir,
            source_call->data.method_call.receiver)->data.local.local_index]
                .mutability = CM_HIR_MUTABLE;
    }
    mark_result = cm_hir_semantic_mark_admitted_bodies(&fixture.hir,
        &reachable.body, 1u, &admission);
    assert(mark_result.status == CM_SEMANTIC_MARK_OK
        && cm_hir_get_expr(&fixture.hir,
            source_call->data.method_call.receiver)->usage
            == (borrowed_receiver ? CM_HIR_USAGE_BORROW : CM_HIR_USAGE_MOVE)
        && !cm_semantic_admission_is_current(&admission));
    cm_semantic_admission_destroy(&admission);

    memset(&admission, 0, sizeof(admission));
    result = cm_semantic_admit_typed_reachable_bodies(&admission,
        &fixture.hir, 1u, &reachable, 1u);
    assert(result.status == CM_SEMANTIC_ADMISSION_OK);
    mutable_call->data.method_call.argument_count = 0u;
    regions_result = cm_hir_semantic_check_admitted_regions(&fixture.hir,
        &reachable.body, 1u, &admission);
    assert(regions_result.status == CM_SEMANTIC_REGIONS_INVALID_HIR
        && regions_result.expression == call_expression
        && cm_semantic_admission_is_current(&admission));
    mutable_call->data.method_call.argument_count = 1u;
    if (mutable_receiver) {
        mutable_body->locals[cm_hir_get_expr(&fixture.hir,
            source_call->data.method_call.receiver)->data.local.local_index]
                .mutability = CM_HIR_IMMUTABLE;
        regions_result = cm_hir_semantic_check_admitted_regions(
            &fixture.hir, &reachable.body, 1u, &admission);
        assert(regions_result.status == CM_SEMANTIC_REGIONS_INVALID_HIR
            && regions_result.expression == call_expression
            && cm_semantic_admission_is_current(&admission));
        mutable_body->locals[cm_hir_get_expr(&fixture.hir,
            source_call->data.method_call.receiver)->data.local.local_index]
                .mutability = CM_HIR_MUTABLE;
    }
    regions_result = cm_hir_semantic_check_admitted_regions(&fixture.hir,
        &reachable.body, 1u, &admission);
    assert(regions_result.status == CM_SEMANTIC_REGIONS_OK);
    cm_semantic_admission_destroy(&admission);
    fixture_destroy(&fixture);
}

static void test_durable_dot_method_recipe(void)
{
    assert_durable_dot_method_recipe(CM_HIR_RECEIVER_VALUE);
    assert_durable_dot_method_recipe(CM_HIR_RECEIVER_REF_SHARED);
    assert_durable_dot_method_recipe(CM_HIR_RECEIVER_REF_MUTABLE);
}

static void test_receiver_adjustments_fail_closed(
    CmHirReceiverKind receiver_kind)
{
    Fixture fixture;
    CmHirLocalBodiesResult lower_result;
    CmHirDefId wrapper_definition;
    const CmHirDefinition *wrapper_record;
    const CmHirItem *wrapper;
    CmHirCrateFinalization finalization;
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmSemanticBodyResult body_result;
    ReceiverAdjustmentProbe probe;
    const char *source;

    source = receiver_kind == CM_HIR_RECEIVER_REF_MUTABLE
        ? "trait Value { fn value(&mut self, other: u32) -> u32; } "
          "impl Value for u32 { "
          "fn value(&mut self, other: u32) -> u32 { 1u32 } } "
          "fn wrapper(mut value: u32) -> u32 { value.value(1u32) }"
        : "trait Value { fn value(&self, other: u32) -> u32; } "
          "impl Value for u32 { "
          "fn value(&self, other: u32) -> u32 { 1u32 } } "
          "fn wrapper(value: u32) -> u32 { value.value(1u32) }";
    fixture_init(&fixture, source);
    lower_result = cm_hir_lower_local_bodies(&fixture.hir, 1u,
        &fixture.graph, fixture.graph_result.revision, &fixture.imports,
        &fixture.modules);
    assert(lower_result.status == CM_HIR_LOCAL_BODIES_OK);
    wrapper_definition = find_named_item(&fixture, "wrapper",
        cm_hir_def_id_none());
    wrapper_record = cm_hir_lookup_definition(&fixture.hir,
        wrapper_definition);
    wrapper = wrapper_record == NULL ? NULL : cm_hir_get_item(&fixture.hir,
        wrapper_record->entity.item_id);
    assert(wrapper != NULL && wrapper->kind == CM_HIR_ITEM_FUNCTION);

    memset(&finalization, 0, sizeof(finalization));
    assert(cm_hir_crate_finalization_init(&finalization, &fixture.hir, 1u)
        == CM_HIR_OK);
    memset(&session, 0, sizeof(session));
    cm_semantic_session_options_init(&options);
    options.local_crate = 1u;
    options.exact_owner = wrapper_definition;
    options.universe = CM_TRAIT_IMPL_UNIVERSE_SINGLE_LOCAL_CRATE_COMPLETE;
    options.finalization = &finalization;
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    memset(&probe, 0, sizeof(probe));
    probe.body = wrapper->data.function_item.body;
    probe.expected_kind = receiver_kind == CM_HIR_RECEIVER_REF_MUTABLE
        ? CM_SEMANTIC_ADJUSTMENT_BORROW_MUTABLE
        : CM_SEMANTIC_ADJUSTMENT_BORROW_SHARED;
    body_result = cm_semantic_body_check_definition_with_writeback(&session,
        probe.body, receiver_adjustment_probe_writeback, &probe);
    assert(body_result.status == CM_SEMANTIC_BODY_OK
        && probe.invocation_count == 1u);

    cm_semantic_session_destroy(&session);
    cm_hir_crate_finalization_destroy(&finalization);
    fixture_destroy(&fixture);
}

static void assert_mutable_receiver_shape_rejected(const char *source)
{
    Fixture fixture;
    CmSemanticAdmission admission;
    CmSemanticAdmissionResult result;
    CmSemanticReachableBody reachable;
    CmHirLocalBodiesResult lower_result;
    CmHirDefId wrapper_definition;
    const CmHirDefinition *wrapper_record;
    const CmHirItem *wrapper;

    fixture_init(&fixture, source);
    lower_result = cm_hir_lower_local_bodies(&fixture.hir, 1u,
        &fixture.graph, fixture.graph_result.revision, &fixture.imports,
        &fixture.modules);
    assert(lower_result.status == CM_HIR_LOCAL_BODIES_OK);
    wrapper_definition = find_named_item(&fixture, "wrapper",
        cm_hir_def_id_none());
    wrapper_record = cm_hir_lookup_definition(&fixture.hir,
        wrapper_definition);
    wrapper = wrapper_record == NULL ? NULL : cm_hir_get_item(&fixture.hir,
        wrapper_record->entity.item_id);
    assert(wrapper != NULL && wrapper->kind == CM_HIR_ITEM_FUNCTION);
    reachable.owner = wrapper_definition;
    reachable.body = wrapper->data.function_item.body;
    memset(&admission, 0, sizeof(admission));
    result = cm_semantic_admit_typed_reachable_bodies(&admission,
        &fixture.hir, 1u, &reachable, 1u);
    assert(result.status == CM_SEMANTIC_ADMISSION_BODY_FAILURE
        && result.body_result.status == CM_SEMANTIC_BODY_UNSUPPORTED
        && admission.state == NULL);
    cm_semantic_admission_destroy(&admission);
    fixture_destroy(&fixture);
}

static void test_mutable_receiver_shapes_fail_closed(void)
{
    assert_mutable_receiver_shape_rejected(
        "trait Value { fn value(&mut self) -> u32; } "
        "impl Value for u32 { fn value(&mut self) -> u32 { 1u32 } } "
        "fn wrapper(value: u32) -> u32 { value.value() }");
    assert_mutable_receiver_shape_rejected(
        "trait Value { fn value(&mut self) -> u32; } "
        "impl Value for u32 { fn value(&mut self) -> u32 { 1u32 } } "
        "fn wrapper(mut value: &'static mut u32) -> u32 { "
        "value.value() }");
    assert_mutable_receiver_shape_rejected(
        "struct Holder { value: u32 } "
        "trait Value { fn value(&mut self) -> u32; } "
        "impl Value for u32 { fn value(&mut self) -> u32 { 1u32 } } "
        "fn wrapper(mut holder: Holder) -> u32 { holder.value.value() }");
}

static void assert_inferred_receiver_region_remains_unsupported(
    const char *source)
{
    Fixture fixture;
    CmSemanticAdmission admission;
    CmSemanticAdmissionResult result;
    CmSemanticReachableBody reachable;
    CmHirLocalBodiesResult lower_result;
    CmHirDefId wrapper_definition;
    const CmHirDefinition *wrapper_record;
    const CmHirItem *wrapper;

    fixture_init(&fixture, source);
    lower_result = cm_hir_lower_local_bodies(&fixture.hir, 1u,
        &fixture.graph, fixture.graph_result.revision, &fixture.imports,
        &fixture.modules);
    assert(lower_result.status == CM_HIR_LOCAL_BODIES_OK);
    wrapper_definition = find_named_item(&fixture, "wrapper",
        cm_hir_def_id_none());
    wrapper_record = cm_hir_lookup_definition(&fixture.hir,
        wrapper_definition);
    wrapper = wrapper_record == NULL ? NULL : cm_hir_get_item(&fixture.hir,
        wrapper_record->entity.item_id);
    assert(wrapper != NULL && wrapper->kind == CM_HIR_ITEM_FUNCTION);
    reachable.owner = wrapper_definition;
    reachable.body = wrapper->data.function_item.body;
    memset(&admission, 0, sizeof(admission));
    result = cm_semantic_admit_typed_reachable_bodies(&admission,
        &fixture.hir, 1u, &reachable, 1u);
    assert(result.status == CM_SEMANTIC_ADMISSION_ITEM_FAILURE
        && result.item_result.status == CM_SEMANTIC_ITEM_PENDING_GENERIC
        && result.item_result.parameter_index == 0u
        && admission.state == NULL);
    cm_semantic_admission_destroy(&admission);
    fixture_destroy(&fixture);
}

static void test_inferred_receiver_regions_remain_unsupported(void)
{
    assert_inferred_receiver_region_remains_unsupported(
        "trait Value { fn value(&'_ self) -> u32; } "
        "impl Value for u32 { fn value(&'_ self) -> u32 { 1u32 } } "
        "fn wrapper(value: u32) -> u32 { value.value() }");
    assert_inferred_receiver_region_remains_unsupported(
        "trait Value { fn value(&'_ mut self) -> u32; } "
        "impl Value for u32 { fn value(&'_ mut self) -> u32 { 1u32 } } "
        "fn wrapper(mut value: u32) -> u32 { value.value() }");
}

static CmSemanticGenericArgumentView assert_generic_impl_callable_recipe(
    const Fixture *fixture, const CmSemanticResults *results,
    const CmSemanticAdmission *admission, const CmHirItem *owner,
    CmHirExprKind expression_kind, CmHirTypeId expected_argument_type,
    CmHirCallableSyntax expected_syntax)
{
    CmSemanticCallableSelectionView selection;
    CmSemanticGenericArgumentView argument;
    CmSemanticGenericArgumentView missing;
    CmHirExprId expression;
    int equal;
    int matches;

    assert(fixture != NULL && results != NULL && admission != NULL
        && owner != NULL && owner->kind == CM_HIR_ITEM_FUNCTION);
    expression = find_owned_callable(fixture,
        owner->data.function_item.body, expression_kind);
    assert(expression != CM_HIR_EXPR_NONE
        && cm_semantic_results_callable_selection(results, admission,
            owner->data.function_item.body, expression, &selection)
            == CM_SEMANTIC_RESULTS_OK
        && selection.syntax == expected_syntax
        && selection.item_argument_count == 0u
        && selection.method_argument_count == 0u
        && selection.enclosing_impl_argument_count == 1u
        && selection.implemented_trait_argument_count == 0u
        && cm_hir_def_id_equal(selection.enclosing_impl,
            selection.selected_impl)
        && cm_hir_def_id_equal(selection.implemented_trait,
            selection.requested_trait)
        && cm_hir_def_id_equal(selection.self_owner,
            selection.selected_impl)
        && cm_semantic_results_callable_generic_argument(results, admission,
            owner->data.function_item.body, expression,
            CM_SEMANTIC_CALLABLE_GENERIC_ARGUMENT_ENCLOSING_IMPL, 0u,
            &argument) == CM_SEMANTIC_RESULTS_OK
        && argument.kind == CM_HIR_GENERIC_ARG_TYPE
        && argument.input.bytes != NULL && argument.input.size != 0u
        && argument.normalized.bytes != NULL
        && argument.normalized.size != 0u
        && cm_semantic_type_view_equal(&argument.input,
            &argument.normalized, &equal) == CM_SEMANTIC_RESULTS_OK
        && equal
        && cm_semantic_type_view_matches_monomorphic_hir(results, admission,
            &argument.input, expected_argument_type, &matches)
            == CM_SEMANTIC_RESULTS_OK && matches
        && cm_semantic_type_view_matches_monomorphic_hir(results, admission,
            &argument.normalized, expected_argument_type, &matches)
            == CM_SEMANTIC_RESULTS_OK && matches
        && cm_semantic_results_callable_generic_argument(results, admission,
            owner->data.function_item.body, expression,
            CM_SEMANTIC_CALLABLE_GENERIC_ARGUMENT_ENCLOSING_IMPL, 1u,
            &missing) == CM_SEMANTIC_RESULTS_NOT_FOUND
        && cm_semantic_results_callable_generic_argument(results, admission,
            owner->data.function_item.body, expression,
            CM_SEMANTIC_CALLABLE_GENERIC_ARGUMENT_ITEM, 0u, &missing)
            == CM_SEMANTIC_RESULTS_NOT_FOUND
        && cm_semantic_results_callable_generic_argument(results, admission,
            owner->data.function_item.body, expression,
            CM_SEMANTIC_CALLABLE_GENERIC_ARGUMENT_METHOD, 0u, &missing)
            == CM_SEMANTIC_RESULTS_NOT_FOUND
        && cm_semantic_results_callable_generic_argument(results, admission,
            owner->data.function_item.body, expression,
            CM_SEMANTIC_CALLABLE_GENERIC_ARGUMENT_IMPLEMENTED_TRAIT, 0u,
            &missing) == CM_SEMANTIC_RESULTS_NOT_FOUND);
    return argument;
}

static void test_durable_generic_impl_callable_recipes(void)
{
    Fixture fixture;
    CmHirLocalBodiesResult lower_result;
    CmSemanticReachableBody reachable[2];
    CmSemanticAdmission admission;
    CmSemanticAdmission foreign_admission;
    CmSemanticAdmissionResult result;
    const CmSemanticResults *results;
    const CmHirItem *qualified;
    const CmHirItem *method;
    CmHirItem *impl_item;
    CmHirItem *impl_method;
    CmHirBody *impl_body;
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_id;
    CmHirType parameter_type_value;
    CmHirTypeId parameter_type;
    CmHirDefId trait_definition;
    CmSemanticGenericArgumentView u32_argument;
    CmSemanticGenericArgumentView u8_argument;
    CmSemanticCallableSelectionView qualified_selection;
    CmSemanticCallableSelectionView method_selection;
    CmHirCanonicalInstance qualified_identity;
    CmHirCanonicalInstance method_identity;
    CmHirCanonicalInstance manual_qualified_identity;
    CmHirCanonicalInstance manual_method_identity;
    CmHirCanonicalInstance rejected_identity;
    CmHirInstanceSpec callee_spec;
    CmHirGenericArg enclosing_argument;
    CmHirCanonicalArgumentPart enclosing_part;
    CmHirCanonicalInstanceParts malformed_parts;
    CmHirExprId qualified_expression;
    CmHirExprId method_expression;
    unsigned char unsupported_parameter[] = {
        (unsigned char)CM_HIR_TYPE_PARAMETER_KIND
    };
    unsigned char unsupported_projection[] = {
        (unsigned char)CM_HIR_TYPE_PROJECTION_KIND
    };
    CmHirTypeId u32_type;
    CmHirTypeId u8_type;
    CmHirType stale_type;
    CmHirTypeId stale_type_id;
    size_t index;
    int equal;

    fixture_init(&fixture,
        "trait Echo { fn echo(self, value: Self) -> Self; } "
        "impl Echo for u32 { "
        "fn echo(self, value: u32) -> u32 { value } } "
        "fn qualified(receiver: u32, value: u32) -> u32 { "
        "<u32 as Echo>::echo(receiver, value) } "
        "fn method(receiver: u8, value: u8) -> u8 { "
        "receiver.echo(value) }");
    lower_result = cm_hir_lower_local_bodies(&fixture.hir, 1u,
        &fixture.graph, fixture.graph_result.revision, &fixture.imports,
        &fixture.modules);
    trait_definition = find_named_item(&fixture, "Echo",
        cm_hir_def_id_none());
    impl_item = NULL;
    for (index = 0u; index < fixture.hir.items.len; ++index) {
        CmHirItem *candidate;

        candidate = (CmHirItem *)cm_vec_at(&fixture.hir.items, index);
        if (candidate != NULL && candidate->kind == CM_HIR_ITEM_IMPL
            && candidate->data.impl_item.has_trait
            && cm_hir_def_id_equal(
                candidate->data.impl_item.trait_type.definition,
                trait_definition)) {
            assert(impl_item == NULL);
            impl_item = candidate;
        }
    }
    impl_method = impl_item == NULL ? NULL : (CmHirItem *)cm_hir_get_item(
        &fixture.hir, cm_hir_lookup_definition(&fixture.hir,
            find_named_item(&fixture, "echo", impl_item->definition))
                ->entity.item_id);
    qualified = find_function_item(&fixture, "qualified");
    method = find_function_item(&fixture, "method");
    u32_type = find_integer_type(&fixture, CM_HIR_INT_U32);
    u8_type = find_integer_type(&fixture, CM_HIR_INT_U8);
    assert(lower_result.status == CM_HIR_LOCAL_BODIES_OK
        && impl_item != NULL && impl_method != NULL
        && qualified != NULL && method != NULL
        && u32_type != CM_HIR_TYPE_NONE && u8_type != CM_HIR_TYPE_NONE);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = impl_item->definition;
    parameter.name = cm_hir_intern(&fixture.hir, "T");
    parameter.span = impl_item->span;
    assert(cm_hir_add_generic_param(&fixture.hir, &parameter,
        &parameter_id) == CM_HIR_OK);
    memset(&parameter_type_value, 0, sizeof(parameter_type_value));
    parameter_type_value.kind = CM_HIR_TYPE_PARAMETER_KIND;
    parameter_type_value.span = impl_item->span;
    parameter_type_value.data.parameter_type.parameter = parameter_id;
    assert(cm_hir_add_type(&fixture.hir, &parameter_type_value,
        &parameter_type) == CM_HIR_OK);
    impl_item->generic_parameter_start = parameter_id;
    impl_item->generic_parameter_count = 1u;
    impl_item->data.impl_item.self_type = parameter_type;
    for (index = 0u;
         index < impl_method->data.function_item.signature.parameter_count;
         ++index) {
        impl_method->data.function_item.signature.parameters[index].type =
            parameter_type;
    }
    impl_method->data.function_item.signature.return_type = parameter_type;
    impl_body = (CmHirBody *)cm_hir_get_body(&fixture.hir,
        impl_method->data.function_item.body);
    assert(impl_body != NULL && impl_body->local_count == 2u);
    impl_body->expected_type = parameter_type;
    for (index = 0u; index < impl_body->local_count; ++index) {
        impl_body->locals[index].type = parameter_type;
    }
    for (index = 0u; index < fixture.hir.expressions.len; ++index) {
        CmHirExpr *expression;

        expression = (CmHirExpr *)cm_vec_at(&fixture.hir.expressions,
            index);
        if (expression != NULL
            && expression->owner_body == impl_method->data.function_item.body) {
            expression->type = parameter_type;
        }
    }
    reachable[0].owner = method->definition;
    reachable[0].body = method->data.function_item.body;
    reachable[1].owner = qualified->definition;
    reachable[1].body = qualified->data.function_item.body;
    memset(&admission, 0, sizeof(admission));
    result = cm_semantic_admit_typed_reachable_bodies(&admission,
        &fixture.hir, 1u, reachable, 2u);
    results = cm_semantic_admission_results(&admission);
    assert(result.status == CM_SEMANTIC_ADMISSION_OK && results != NULL);
    u32_argument = assert_generic_impl_callable_recipe(&fixture, results,
        &admission, qualified, CM_HIR_EXPR_QUALIFIED_CALL, u32_type,
        CM_HIR_CALLABLE_QUALIFIED_TRAIT_METHOD);
    u8_argument = assert_generic_impl_callable_recipe(&fixture, results,
        &admission, method, CM_HIR_EXPR_METHOD_CALL, u8_type,
        CM_HIR_CALLABLE_DOT_METHOD);
    assert(u32_argument.kind == CM_HIR_GENERIC_ARG_TYPE
        && u8_argument.kind == CM_HIR_GENERIC_ARG_TYPE
        && cm_semantic_type_view_equal(&u32_argument.normalized,
            &u8_argument.normalized, &equal) == CM_SEMANTIC_RESULTS_OK
        && !equal);

    qualified_expression = find_owned_callable(&fixture,
        qualified->data.function_item.body, CM_HIR_EXPR_QUALIFIED_CALL);
    method_expression = find_owned_callable(&fixture,
        method->data.function_item.body, CM_HIR_EXPR_METHOD_CALL);
    cm_hir_canonical_instance_init(&qualified_identity);
    cm_hir_canonical_instance_init(&method_identity);
    cm_hir_canonical_instance_init(&manual_qualified_identity);
    cm_hir_canonical_instance_init(&manual_method_identity);
    cm_hir_canonical_instance_init(&rejected_identity);
    assert(cm_semantic_results_callable_selection(results, &admission,
            qualified->data.function_item.body, qualified_expression,
            &qualified_selection) == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_results_callable_selection(results, &admission,
            method->data.function_item.body, method_expression,
            &method_selection) == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_results_callable_callee_identity(results, &admission,
            qualified->data.function_item.body, qualified_expression,
            &qualified_identity) == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_results_callable_callee_identity(results, &admission,
            method->data.function_item.body, method_expression,
            &method_identity) == CM_SEMANTIC_RESULTS_OK
        && cm_hir_canonical_instance_equal(&qualified_identity,
            &method_identity, &equal) == CM_HIR_INSTANCE_OK && !equal);

    cm_hir_instance_spec_init(&callee_spec);
    callee_spec.selected_callable = qualified_selection.selected_callable;
    callee_spec.body_definition = qualified_selection.body_definition;
    callee_spec.declared_trait_callable =
        qualified_selection.declared_trait_callable;
    callee_spec.enclosing_impl = qualified_selection.enclosing_impl;
    callee_spec.implemented_trait = qualified_selection.implemented_trait;
    callee_spec.self_owner = qualified_selection.self_owner;
    callee_spec.self_type = u32_type;
    enclosing_argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    enclosing_argument.data.type = u32_type;
    callee_spec.enclosing_impl_arguments = &enclosing_argument;
    callee_spec.enclosing_impl_argument_count = 1u;
    assert(cm_hir_canonical_instance_encode(&fixture.hir, 1u, &callee_spec,
            &manual_qualified_identity) == CM_HIR_INSTANCE_OK
        && cm_hir_canonical_instance_equal(&qualified_identity,
            &manual_qualified_identity, &equal) == CM_HIR_INSTANCE_OK
        && equal);
    callee_spec.self_type = u8_type;
    enclosing_argument.data.type = u8_type;
    assert(cm_hir_canonical_instance_encode(&fixture.hir, 1u, &callee_spec,
            &manual_method_identity) == CM_HIR_INSTANCE_OK
        && cm_hir_canonical_instance_equal(&method_identity,
            &manual_method_identity, &equal) == CM_HIR_INSTANCE_OK
        && equal);

    rejected_identity.definition = qualified_identity.definition;
    assert(cm_semantic_results_callable_callee_identity(results, &admission,
            qualified->data.function_item.body, qualified_expression,
            &rejected_identity) == CM_SEMANTIC_RESULTS_INVALID_ARGUMENT
        && rejected_identity.bytes == NULL && rejected_identity.size == 0u
        && rejected_identity.body == CM_HIR_BODY_NONE);
    cm_hir_canonical_instance_init(&rejected_identity);
    memset(&malformed_parts, 0, sizeof(malformed_parts));
    malformed_parts.selected_callable =
        qualified_selection.selected_callable;
    malformed_parts.body_definition = qualified_selection.body_definition;
    malformed_parts.declared_trait_callable =
        qualified_selection.declared_trait_callable;
    malformed_parts.enclosing_impl = qualified_selection.enclosing_impl;
    malformed_parts.implemented_trait =
        qualified_selection.implemented_trait;
    malformed_parts.self_owner = qualified_selection.self_owner;
    malformed_parts.self_type = qualified_selection.requested_self_type.bytes;
    malformed_parts.self_type_size =
        qualified_selection.requested_self_type.size;
    enclosing_part.kind = CM_HIR_GENERIC_ARG_TYPE;
    enclosing_part.bytes = unsupported_parameter;
    enclosing_part.size = sizeof(unsupported_parameter);
    malformed_parts.enclosing_impl_arguments = &enclosing_part;
    malformed_parts.enclosing_impl_argument_count = 1u;
    assert(cm_hir_canonical_instance_encode_parts(&fixture.hir, 1u,
            &malformed_parts, &rejected_identity)
            == CM_HIR_INSTANCE_UNSUPPORTED_TYPE
        && rejected_identity.bytes == NULL && rejected_identity.size == 0u
        && cm_hir_def_id_is_none(rejected_identity.definition));
    enclosing_part.bytes = unsupported_projection;
    enclosing_part.size = sizeof(unsupported_projection);
    assert(cm_hir_canonical_instance_encode_parts(&fixture.hir, 1u,
            &malformed_parts, &rejected_identity)
            == CM_HIR_INSTANCE_UNSUPPORTED_TYPE
        && rejected_identity.bytes == NULL && rejected_identity.size == 0u
        && cm_hir_def_id_is_none(rejected_identity.definition));
    malformed_parts.self_owner = cm_hir_def_id_none();
    enclosing_part.bytes = u32_argument.normalized.bytes;
    enclosing_part.size = u32_argument.normalized.size;
    assert(cm_hir_canonical_instance_encode_parts(&fixture.hir, 1u,
            &malformed_parts, &rejected_identity)
            == CM_HIR_INSTANCE_INVALID_RELATION
        && rejected_identity.bytes == NULL && rejected_identity.size == 0u
        && cm_hir_def_id_is_none(rejected_identity.definition));
    assert(cm_semantic_results_callable_callee_identity(results, &admission,
            CM_HIR_BODY_NONE, qualified_expression, &rejected_identity)
            == CM_SEMANTIC_RESULTS_NOT_FOUND
        && rejected_identity.bytes == NULL && rejected_identity.size == 0u
        && cm_hir_def_id_is_none(rejected_identity.definition)
        && cm_semantic_results_callable_callee_identity(results, &admission,
            qualified->data.function_item.body, CM_HIR_EXPR_NONE,
            &rejected_identity) == CM_SEMANTIC_RESULTS_NOT_FOUND
        && rejected_identity.bytes == NULL && rejected_identity.size == 0u
        && cm_hir_def_id_is_none(rejected_identity.definition));
    memset(&foreign_admission, 0, sizeof(foreign_admission));
    result = cm_semantic_admit_typed_reachable_bodies(&foreign_admission,
        &fixture.hir, 1u, reachable, 2u);
    assert(result.status == CM_SEMANTIC_ADMISSION_OK
        && cm_semantic_results_callable_callee_identity(results,
            &foreign_admission, qualified->data.function_item.body,
            qualified_expression, &rejected_identity)
            == CM_SEMANTIC_RESULTS_FOREIGN
        && rejected_identity.bytes == NULL && rejected_identity.size == 0u
        && cm_hir_def_id_is_none(rejected_identity.definition));
    cm_semantic_admission_destroy(&foreign_admission);
    memset(&stale_type, 0, sizeof(stale_type));
    stale_type.kind = CM_HIR_TYPE_BOOL_KIND;
    stale_type.span = qualified->span;
    assert(cm_hir_add_type(&fixture.hir, &stale_type, &stale_type_id)
            == CM_HIR_OK
        && cm_semantic_results_callable_callee_identity(results, &admission,
            qualified->data.function_item.body, qualified_expression,
            &rejected_identity) == CM_SEMANTIC_RESULTS_STALE
        && rejected_identity.bytes == NULL && rejected_identity.size == 0u
        && cm_hir_def_id_is_none(rejected_identity.definition));
    cm_hir_canonical_instance_destroy(&rejected_identity);
    cm_hir_canonical_instance_destroy(&manual_method_identity);
    cm_hir_canonical_instance_destroy(&manual_qualified_identity);
    cm_hir_canonical_instance_destroy(&method_identity);
    cm_hir_canonical_instance_destroy(&qualified_identity);
    cm_semantic_admission_destroy(&admission);
    fixture_destroy(&fixture);
}

static void test_projection_failure_discards_partial_stage(void)
{
    Fixture fixture;
    CmHirCrateFinalization finalization;
    CmSemanticSession session;
    CmSemanticSession foreign_session;
    CmSemanticSessionOptions options;
    CmSemanticBodyEvidenceWriteback writeback;
    CmSemanticBodyResult result;
    ProjectionFailureProbe probe;
    CmHirDefId trait_definition;
    CmHirDefId associated_definition;
    CmHirDefId value_definition;
    const CmHirDefinition *value_record;
    CmHirItem *owner;
    CmHirBody *body;
    CmHirExpr *root;
    CmHirTypeId bool_type;
    CmHirTypeId projection;
    size_t index;
    CmTypeckContext *typeck;
    CmTypeckContext *foreign_typeck;
    CmTypeckInstantiation exact;
    CmTypeckInstantiation foreign_exact;
    CmParamEnvSubstitution substitution;
    CmParamEnvSubstitution foreign_substitution;
    CmTypeckType projection_type;
    CmTypeckTypeId bool_term;
    CmTypeckTypeId foreign_bool_term;
    CmTypeckTypeId projection_term;
    CmTypeckTypeId foreign_projection_term;
    CmTypeckTypeId bump_term;
    CmTypeckTypeId u32_term;
    CmTypeckSnapshot bump_snapshot;
    CmProjectionNormalizeTrace trace;
    CmProjectionNormalizeTrace foreign_trace;
    CmProjectionNormalizeResult normalization;

    fixture_init(&fixture,
        "trait Bound { type Output; } "
        "impl Bound for bool { type Output = u32; } "
        "fn value() -> u32 { 1u32 }");
    trait_definition = find_named_item(&fixture, "Bound",
        cm_hir_def_id_none());
    associated_definition = find_named_item(&fixture, "Output",
        trait_definition);
    value_definition = find_named_item(&fixture, "value",
        cm_hir_def_id_none());
    value_record = cm_hir_lookup_definition(&fixture.hir, value_definition);
    owner = value_record == NULL ? NULL : (CmHirItem *)cm_hir_get_item(
        &fixture.hir, value_record->entity.item_id);
    assert(owner != NULL
        && cm_hir_lower_body(&fixture.hir, owner->data.function_item.body,
            &fixture.graph, fixture.graph_result.revision, &fixture.imports,
            &fixture.modules).status == CM_HIR_BODY_LOWER_OK);
    owner = (CmHirItem *)cm_hir_get_item(&fixture.hir,
        value_record->entity.item_id);
    body = owner == NULL ? NULL : (CmHirBody *)cm_hir_get_body(&fixture.hir,
        owner->data.function_item.body);
    root = body == NULL ? NULL : (CmHirExpr *)cm_hir_get_expr(&fixture.hir,
        body->root_expression);
    bool_type = CM_HIR_TYPE_NONE;
    for (index = 0u; index < fixture.hir.types.len; ++index) {
        const CmHirType *type;

        type = cm_hir_get_type(&fixture.hir, (CmHirTypeId)(index + 1u));
        if (type != NULL && type->kind == CM_HIR_TYPE_BOOL_KIND) {
            bool_type = (CmHirTypeId)(index + 1u);
            break;
        }
    }
    assert(body != NULL && root != NULL && bool_type != CM_HIR_TYPE_NONE);
    projection = add_projection_type(&fixture, trait_definition,
        associated_definition, bool_type);
    owner->data.function_item.signature.return_type = projection;
    body->expected_type = projection;
    root->type = projection;
    memset(&finalization, 0, sizeof(finalization));
    assert(cm_hir_crate_finalization_init(&finalization, &fixture.hir, 1u)
        == CM_HIR_OK);
    memset(&session, 0, sizeof(session));
    cm_semantic_session_options_init(&options);
    options.local_crate = 1u;
    options.exact_owner = body->owner;
    options.universe = CM_TRAIT_IMPL_UNIVERSE_SINGLE_LOCAL_CRATE_COMPLETE;
    options.finalization = &finalization;
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    memset(&foreign_session, 0, sizeof(foreign_session));
    assert(cm_semantic_session_init(&foreign_session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    typeck = cm_semantic_session_typeck(&session);
    foreign_typeck = cm_semantic_session_typeck(&foreign_session);
    assert(typeck != NULL && foreign_typeck != NULL
        && cm_typeck_import_hir_type(typeck, bool_type, &bool_term)
            == CM_TYPECK_OK
        && cm_typeck_import_hir_type(foreign_typeck, bool_type,
            &foreign_bool_term) == CM_TYPECK_OK);
    cm_typeck_instantiation_init(typeck, &exact);
    exact.parameter_owner = body->owner;
    exact.self_owner = body->owner;
    exact.self_type = bool_term;
    memset(&substitution, 0, sizeof(substitution));
    substitution.exact = &exact;
    cm_typeck_instantiation_init(foreign_typeck, &foreign_exact);
    foreign_exact.parameter_owner = body->owner;
    foreign_exact.self_owner = body->owner;
    foreign_exact.self_type = foreign_bool_term;
    memset(&foreign_substitution, 0, sizeof(foreign_substitution));
    foreign_substitution.exact = &foreign_exact;
    memset(&projection_type, 0, sizeof(projection_type));
    projection_type.kind = CM_TYPECK_TYPE_PROJECTION;
    projection_type.span = body->span;
    projection_type.data.projection_type.self_type = bool_term;
    projection_type.data.projection_type.trait_type.definition =
        trait_definition;
    projection_type.data.projection_type.associated_type.definition =
        associated_definition;
    assert(cm_typeck_add_type(typeck, &projection_type, &projection_term)
        == CM_TYPECK_OK);
    projection_type.data.projection_type.self_type = foreign_bool_term;
    assert(cm_typeck_add_type(foreign_typeck, &projection_type,
        &foreign_projection_term) == CM_TYPECK_OK);
    cm_projection_normalize_trace_init(&foreign_trace);
    normalization = cm_semantic_session_normalize_type_traced(
        &foreign_session, foreign_typeck, &foreign_substitution,
        foreign_projection_term, (CmProjectionNormalizeLimits){64u, 2u},
        &foreign_trace);
    memset(&probe, 0, sizeof(probe));
    cm_semantic_results_body_stage_init(&probe.stage);
    assert(normalization.kind == CM_TRAIT_SOLVER_PROVEN
        && cm_semantic_results_stage_projection_decision(&probe.stage,
            &session, owner->data.function_item.body, body->root_expression,
            CM_SEMANTIC_PROJECTION_DECISION_EXPRESSION_TYPE, 0u,
            projection_term, normalization.type, &foreign_trace)
            == CM_SEMANTIC_BODY_WRITEBACK_INVALID
        && probe.stage.state == NULL);
    cm_projection_normalize_trace_destroy(&foreign_trace);

    cm_projection_normalize_trace_init(&trace);
    normalization = cm_semantic_session_normalize_type_traced(&session,
        typeck, &substitution, projection_term,
        (CmProjectionNormalizeLimits){64u, 2u}, &trace);
    assert(normalization.kind == CM_TRAIT_SOLVER_PROVEN
        && cm_typeck_snapshot(typeck, &bump_snapshot) == CM_TYPECK_OK
        && cm_typeck_new_variable(typeck, CM_HIR_INFER_GENERAL,
            body->span, &bump_term) == CM_TYPECK_OK
        && cm_typeck_import_hir_type(typeck,
            owner->data.function_item.signature.return_type, &u32_term)
            == CM_TYPECK_OK
        && cm_typeck_unify(typeck, bump_term, u32_term) == CM_TYPECK_OK
        && cm_typeck_commit(typeck, &bump_snapshot) == CM_TYPECK_OK
        && bump_term != CM_TYPECK_TYPE_NONE
        && cm_semantic_results_stage_projection_decision(&probe.stage,
            &session, owner->data.function_item.body, body->root_expression,
            CM_SEMANTIC_PROJECTION_DECISION_EXPRESSION_TYPE, 0u,
            projection_term, normalization.type, &trace)
            == CM_SEMANTIC_BODY_WRITEBACK_INVALID
        && probe.stage.state == NULL);
    cm_projection_normalize_trace_destroy(&trace);
    memset(&writeback, 0, sizeof(writeback));
    writeback.context = &probe;
    writeback.checked_body = projection_failure_checked;
    writeback.projection_decision = projection_failure_decision;
    writeback.discard = projection_failure_discard;

    probe.duplicate_projection = 1;
    result = cm_semantic_body_check_definition_with_evidence(&session,
        owner->data.function_item.body, &writeback);
    assert(result.status == CM_SEMANTIC_BODY_INVALID
        && probe.stage.state == NULL);
    probe.duplicate_projection = 0;
    probe.duplicate_callable_projection = 1;
    result = cm_semantic_body_check_definition_with_evidence(&session,
        owner->data.function_item.body, &writeback);
    assert(result.status == CM_SEMANTIC_BODY_INVALID
        && probe.stage.state == NULL);
    probe.duplicate_callable_projection = 0;
    probe.tamper_projection_index = 1;
    result = cm_semantic_body_check_definition_with_evidence(&session,
        owner->data.function_item.body, &writeback);
    assert(result.status == CM_SEMANTIC_BODY_INVALID
        && probe.stage.state == NULL);
    probe.tamper_projection_index = 0;
    probe.tamper_projection_input = 1;
    result = cm_semantic_body_check_definition_with_evidence(&session,
        owner->data.function_item.body, &writeback);
    assert(result.status == CM_SEMANTIC_BODY_INVALID
        && probe.stage.state == NULL);
    probe.tamper_projection_input = 0;
    probe.reject_checked_body = 1;
    result = cm_semantic_body_check_definition_with_evidence(&session,
        owner->data.function_item.body, &writeback);
    assert(result.status == CM_SEMANTIC_BODY_INVALID
        && probe.stage.state == NULL);

    cm_semantic_results_body_stage_destroy(&probe.stage);
    cm_semantic_session_destroy(&foreign_session);
    cm_semantic_session_destroy(&session);
    cm_hir_crate_finalization_destroy(&finalization);
    fixture_destroy(&fixture);
}

int main(void)
{
    test_successful_results();
    test_stale_and_failure_publish_nothing();
    test_same_generation_foreign_admission();
    test_generic_parameter_type_is_structural();
    test_type_view_materializes_existing_hir_read_only();
    test_malformed_recipe_facts_publish_no_stage();
    test_partial_checked_draft_does_not_seal();
    test_instance_commit_requires_producer_session();
    test_writeback_distinguishes_unsolved_terms();
    test_durable_projection_trace_definition();
    test_durable_qualified_callable_recipe();
    test_durable_dot_method_recipe();
    test_receiver_adjustments_fail_closed(CM_HIR_RECEIVER_REF_SHARED);
    test_receiver_adjustments_fail_closed(CM_HIR_RECEIVER_REF_MUTABLE);
    test_mutable_receiver_shapes_fail_closed();
    test_inferred_receiver_regions_remain_unsupported();
    test_durable_generic_impl_callable_recipes();
    test_canonical_parts_function_pointer_abi_parity();
    test_projection_failure_discards_partial_stage();
    puts("semantic results tests passed");
    return 0;
}
