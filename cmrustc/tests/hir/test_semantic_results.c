#include "cm/hir/admission.h"
#include "cm/hir/lower.h"
#include "cm/hir/semantic_results.h"
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
        && facts->adjustment_count == 0u
        && facts->adjustments == NULL);
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
        && cm_semantic_results_commit_checked_body(draft, &session,
            &body_result, &stage) == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_results_commit_checked_body(draft, &session,
            &body_result, &stage) == CM_SEMANTIC_RESULTS_INVALID_ARGUMENT
        && cm_semantic_results_seal(draft)
            == CM_SEMANTIC_RESULTS_INVALID_HIR);
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

int main(void)
{
    test_successful_results();
    test_stale_and_failure_publish_nothing();
    test_same_generation_foreign_admission();
    test_generic_parameter_type_is_structural();
    test_malformed_recipe_facts_publish_no_stage();
    test_partial_checked_draft_does_not_seal();
    test_instance_commit_requires_producer_session();
    test_writeback_distinguishes_unsolved_terms();
    puts("semantic results tests passed");
    return 0;
}
