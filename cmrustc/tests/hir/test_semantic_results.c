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

    fixture_init(&fixture,
        "fn add(left: u32, right: u32) -> u32 { left + right } "
        "fn call(value: u32) -> u32 { add(value, 1u32) }");
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
        && cm_semantic_results_body_count(results, &admission) == 2u);
    queried_expression_count = 0u;
    saw_call = 0;
    saw_binary = 0;
    for (body_index = 0u; body_index < 2u; ++body_index) {
        CmSemanticBodyView body_view;
        uint32_t body_expressions;
        assert(cm_semantic_results_body_at(results, &admission,
            body_index, &body_view) == CM_SEMANTIC_RESULTS_OK);
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
                assert(expression_view.has_direct_callable
                    && cm_hir_def_id_equal(expression_view.direct_callable,
                        expression->data.call.callee));
                saw_call = 1;
            }
            if (expression->kind == CM_HIR_EXPR_BINARY) {
                assert(expression_view.has_primitive_operator
                    && expression_view.primitive_operator
                        == expression->data.binary.operator_kind);
                saw_binary = 1;
            }
            body_expressions += 1u;
            queried_expression_count += 1u;
        }
        assert(body_expressions == body_view.expression_count);
    }
    assert(queried_expression_count == fixture.hir.expressions.len
        && saw_call && saw_binary);
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
    body_result = cm_semantic_body_check_definition(&session, 1u);
    assert(body_result.status == CM_SEMANTIC_BODY_OK);
    draft = NULL;
    assert(cm_semantic_results_begin(&fixture.hir, 1u, &draft)
            == CM_SEMANTIC_RESULTS_OK
        && draft != NULL
        && cm_semantic_results_add_checked_body(draft, &session,
            &body_result) == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_results_add_checked_body(draft, &session,
            &body_result) == CM_SEMANTIC_RESULTS_INVALID_HIR
        && cm_semantic_results_seal(draft)
            == CM_SEMANTIC_RESULTS_INVALID_HIR);
    cm_semantic_results_destroy(draft);
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
    test_partial_checked_draft_does_not_seal();
    puts("semantic results tests passed");
    return 0;
}
