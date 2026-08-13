#include "cm/hir/admission.h"

#include "semantic_results_internal.h"

#include "cm/alloc.h"
#include "cm/hir/instance.h"

#include <stdlib.h>
#include <string.h>

typedef struct CmSemanticAdmissionState {
    const CmHirContext *hir;
    CmHirCrateId local_crate;
    uint64_t storage_lifetime_id;
    uint64_t semantic_generation;
    uint64_t rewind_generation;
    uint64_t capability_id;
    CmSemanticResults *results;
} CmSemanticAdmissionState;

static uint64_t cm_admission_capability_counter;

static uint64_t cm_admission_new_capability_id(void)
{
    if (cm_admission_capability_counter == UINT64_MAX) abort();
    cm_admission_capability_counter += 1u;
    return cm_admission_capability_counter;
}

static CmSemanticAdmissionResult cm_admission_result(
    CmSemanticAdmissionStatus status)
{
    CmSemanticAdmissionResult result;
    memset(&result, 0, sizeof(result));
    result.status = status;
    result.item = CM_HIR_ITEM_NONE;
    result.owner = cm_hir_def_id_none();
    result.body = CM_HIR_BODY_NONE;
    result.local_bodies.status = CM_HIR_LOCAL_BODIES_INVALID_ARGUMENT;
    result.local_bodies.body_result.status = CM_HIR_BODY_LOWER_INVALID_ARGUMENT;
    result.item_result.status = CM_SEMANTIC_ITEM_INVALID;
    result.item_result.impl_definition = cm_hir_def_id_none();
    result.item_result.trait_definition = cm_hir_def_id_none();
    result.item_result.impl_member = cm_hir_def_id_none();
    result.item_result.trait_member = cm_hir_def_id_none();
    result.item_result.parameter_index = CM_SEMANTIC_ITEM_PARAMETER_NONE;
    result.body_result.status = CM_SEMANTIC_BODY_INVALID;
    result.body_result.expression = CM_HIR_EXPR_NONE;
    result.body_result.callee = cm_hir_def_id_none();
    result.body_result.predicate_index = CM_SEMANTIC_BODY_PREDICATE_NONE;
    result.body_result.solver_kind = CM_TRAIT_SOLVER_INVALID;
    result.session_status = CM_TRAIT_SOLVER_INVALID;
    result.hir_status = CM_HIR_OK;
    return result;
}

static int cm_admission_state_current(const CmSemanticAdmissionState *state)
{
    const CmHirContext *hir;
    if (state == NULL || state->hir == NULL
        || state->local_crate == CM_HIR_CRATE_NONE) return 0;
    hir = state->hir;
    return state->capability_id != UINT64_C(0)
        && hir->storage.lifetime_id == state->storage_lifetime_id
        && hir->semantic_generation == state->semantic_generation
        && hir->rewind_generation == state->rewind_generation
        && cm_hir_get_crate(hir, state->local_crate) != NULL;
}

static const CmHirItem *cm_admission_item(const CmHirContext *hir,
    CmHirDefId definition)
{
    const CmHirDefinition *record;
    const CmHirItem *item;

    record = hir == NULL ? NULL : cm_hir_lookup_definition(hir, definition);
    item = record == NULL || record->kind != CM_HIR_DEFINITION_ITEM
            || record->state != CM_HIR_DEFINITION_BOUND
        ? NULL : cm_hir_get_item(hir, record->entity.item_id);
    return item != NULL && cm_hir_def_id_equal(item->definition, definition)
        ? item : NULL;
}

static int cm_admission_method_trait_in_scope(const CmHirExpr *expression,
    CmHirDefId trait_definition)
{
    uint32_t index;

    if (expression == NULL || expression->kind != CM_HIR_EXPR_METHOD_CALL) {
        return 0;
    }
    for (index = 0u;
         index < expression->data.method_call.in_scope_trait_count; ++index) {
        if (cm_hir_def_id_equal(
                expression->data.method_call.in_scope_traits[index],
                trait_definition)) return 1;
    }
    return 0;
}

static int cm_admission_instance_callable_supported(
    const CmHirContext *hir, CmHirCrateId local_crate,
    const CmHirInstanceSpec *spec, const CmHirItem *item, int leaf_only)
{
    const CmHirItem *impl_item;

    if (hir == NULL || local_crate == CM_HIR_CRATE_NONE || spec == NULL
        || item == NULL || item->kind != CM_HIR_ITEM_FUNCTION
        || item->definition.crate_id != local_crate
        || item->predicate_scope_count != 0u
        || (leaf_only && item->predicate_count != 0u)
        || item->outlives_predicate_count != 0u) return 0;
    if (cm_hir_def_id_is_none(item->parent_definition)) {
        return cm_hir_def_id_is_none(
            item->data.function_item.trait_item_definition);
    }
    impl_item = cm_admission_item(hir, item->parent_definition);
    return item->generic_parameter_count == 0u
        && item->predicate_count == 0u
        && impl_item != NULL && impl_item->kind == CM_HIR_ITEM_IMPL
        && impl_item->definition.crate_id == local_crate
        && impl_item->generic_parameter_count == 0u
        && impl_item->predicate_scope_count == 0u
        && impl_item->predicate_count == 0u
        && impl_item->outlives_predicate_count == 0u
        && impl_item->data.impl_item.has_trait
        && !impl_item->data.impl_item.is_negative
        && impl_item->data.impl_item.trait_type.argument_count == 0u
        && impl_item->data.impl_item.trait_type.arguments == NULL
        && !cm_hir_def_id_is_none(
            item->data.function_item.trait_item_definition)
        && cm_hir_def_id_equal(spec->enclosing_impl,
            impl_item->definition)
        && cm_hir_def_id_equal(spec->implemented_trait,
            impl_item->data.impl_item.trait_type.definition)
        && cm_hir_def_id_equal(spec->declared_trait_callable,
            item->data.function_item.trait_item_definition)
        && cm_hir_def_id_equal(spec->self_owner, impl_item->definition)
        && spec->item_argument_count == 0u
        && spec->item_arguments == NULL
        && spec->method_argument_count == 0u
        && spec->method_arguments == NULL
        && spec->enclosing_impl_argument_count == 0u
        && spec->enclosing_impl_arguments == NULL
        && spec->implemented_trait_argument_count == 0u
        && spec->implemented_trait_arguments == NULL;
}

static CmHirStatus cm_admission_rollback(CmHirContext *hir,
    CmHirContextMark *mark, const CmHirBody *journal, size_t body_count)
{
    if (hir == NULL || mark == NULL || hir->bodies.len < body_count
        || (body_count != 0u && journal == NULL))
        return CM_HIR_INVARIANT_VIOLATION;
    if (body_count != 0u)
        memcpy(hir->bodies.data, journal, body_count * sizeof(*journal));
    return cm_hir_context_rewind(hir, mark);
}

CmSemanticAdmissionResult cm_semantic_admit_local_crate(
    CmSemanticAdmission *admission, CmHirContext *hir,
    CmHirCrateId local_crate, const CmModuleGraph *graph,
    CmModuleGraphRevision revision, const CmImportResolver *imports,
    const CmHirModuleMap *modules)
{
    CmSemanticAdmissionResult result;
    CmHirContextMark mark;
    CmHirBody *journal;
    size_t body_count, body_bytes, item_index;
    CmHirCrateFinalization finalization;
    CmSemanticSession session;
    CmHirStatus hir_status;
    CmSemanticResultsStatus results_status;
    CmSemanticResults *semantic_results;
    CmSemanticResultsBodyStage body_stage;
    CmSemanticAdmissionState *state;
    int mark_active;

    result = cm_admission_result(CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT);
    if (admission == NULL || admission->state != NULL || hir == NULL
        || local_crate == CM_HIR_CRATE_NONE || graph == NULL
        || revision == CM_MODULE_GRAPH_REVISION_NONE || imports == NULL
        || modules == NULL || cm_hir_get_crate(hir, local_crate) == NULL)
        return result;
    body_count = hir->bodies.len;
    if (!cm_size_mul(body_count, sizeof(*journal), &body_bytes)) {
        result.status = CM_SEMANTIC_ADMISSION_HIR_FAILURE;
        result.hir_status = CM_HIR_ID_EXHAUSTED;
        return result;
    }
    journal = body_count == 0u ? NULL
        : (CmHirBody *)cm_alloc_zeroed(body_count, sizeof(*journal));
    if (body_count != 0u) memcpy(journal, hir->bodies.data, body_bytes);
    memset(&mark, 0, sizeof(mark));
    hir_status = cm_hir_context_mark(hir, &mark);
    if (hir_status != CM_HIR_OK) {
        result.status = CM_SEMANTIC_ADMISSION_HIR_FAILURE;
        result.hir_status = hir_status;
        cm_free(journal);
        return result;
    }
    mark_active = 1;
    memset(&finalization, 0, sizeof(finalization));
    memset(&session, 0, sizeof(session));
    semantic_results = NULL;
    cm_semantic_results_body_stage_init(&body_stage);
    state = NULL;

    result.local_bodies = cm_hir_lower_local_bodies(hir, local_crate,
        graph, revision, imports, modules);
    if (result.local_bodies.status != CM_HIR_LOCAL_BODIES_OK) {
        result.item = result.local_bodies.item;
        result.owner = result.local_bodies.owner;
        result.body = result.local_bodies.body;
        result.status = CM_SEMANTIC_ADMISSION_LOCAL_BODIES_FAILURE;
        goto rollback;
    }
    hir_status = cm_hir_crate_finalization_init(&finalization, hir,
        local_crate);
    if (hir_status != CM_HIR_OK) {
        result.status = CM_SEMANTIC_ADMISSION_FINALIZATION_FAILURE;
        result.hir_status = hir_status;
        goto rollback;
    }
    {
        CmProjectionNormalizeLimits normalize_limits;

        normalize_limits.max_nodes = 4096u;
        normalize_limits.max_projection_steps = 256u;
        result.item_result =
            cm_semantic_item_check_finalized_local_trait_impls(
                &finalization, normalize_limits);
    }
    if (result.item_result.status != CM_SEMANTIC_ITEM_OK) {
        result.owner = cm_hir_def_id_is_none(result.item_result.impl_member)
            ? result.item_result.impl_definition
            : result.item_result.impl_member;
        result.status = CM_SEMANTIC_ADMISSION_ITEM_FAILURE;
        goto rollback;
    }
    results_status = cm_semantic_results_begin(hir, local_crate,
        &semantic_results);
    if (results_status != CM_SEMANTIC_RESULTS_OK) {
        result.status = CM_SEMANTIC_ADMISSION_HIR_FAILURE;
        result.hir_status = results_status == CM_SEMANTIC_RESULTS_OVERFLOW
            ? CM_HIR_ID_EXHAUSTED : CM_HIR_INVARIANT_VIOLATION;
        goto rollback;
    }
    for (item_index = 0u; item_index < hir->items.len; ++item_index) {
        const CmHirItem *item;
        CmSemanticSessionOptions options;
        item = (const CmHirItem *)cm_vec_at_const(&hir->items, item_index);
        if (item == NULL) {
            result.status = CM_SEMANTIC_ADMISSION_HIR_FAILURE;
            result.hir_status = CM_HIR_INVARIANT_VIOLATION;
            goto rollback;
        }
        if (item->definition.crate_id != local_crate
            || item->kind != CM_HIR_ITEM_FUNCTION
            || item->data.function_item.body == CM_HIR_BODY_NONE) continue;
        result.item = (CmHirItemId)(item_index + 1u);
        result.owner = item->definition;
        result.body = item->data.function_item.body;
        cm_semantic_session_options_init(&options);
        options.local_crate = local_crate;
        options.exact_owner = item->definition;
        options.universe = CM_TRAIT_IMPL_UNIVERSE_SINGLE_LOCAL_CRATE_COMPLETE;
        options.finalization = &finalization;
        result.session_status = cm_semantic_session_init(&session, hir,
            &options);
        if (result.session_status != CM_TRAIT_SOLVER_PROVEN) {
            result.status = CM_SEMANTIC_ADMISSION_SESSION_FAILURE;
            goto rollback;
        }
        {
            CmSemanticBodyEvidenceWriteback writeback;

            memset(&writeback, 0, sizeof(writeback));
            writeback.context = &body_stage;
            writeback.checked_body = cm_semantic_results_stage_checked_body;
            writeback.projection_decision =
                cm_semantic_results_stage_projection_decision;
            writeback.discard = cm_semantic_results_discard_body_stage;
            result.body_result =
                cm_semantic_body_check_definition_with_evidence(&session,
                    result.body, &writeback);
        }
        if (result.body_result.status != CM_SEMANTIC_BODY_OK) {
            result.status = CM_SEMANTIC_ADMISSION_BODY_FAILURE;
            goto rollback;
        }
        results_status = cm_semantic_results_commit_checked_body(
            semantic_results, &session, &result.body_result, &body_stage);
        cm_semantic_session_destroy(&session);
        if (results_status != CM_SEMANTIC_RESULTS_OK) {
            result.status = CM_SEMANTIC_ADMISSION_HIR_FAILURE;
            result.hir_status = results_status == CM_SEMANTIC_RESULTS_OVERFLOW
                ? CM_HIR_ID_EXHAUSTED : CM_HIR_INVARIANT_VIOLATION;
            goto rollback;
        }
    }
    state = (CmSemanticAdmissionState *)cm_alloc_zeroed(1u,
        sizeof(*state));
    results_status = cm_semantic_results_seal(semantic_results);
    if (results_status != CM_SEMANTIC_RESULTS_OK) {
        result.status = CM_SEMANTIC_ADMISSION_HIR_FAILURE;
        result.hir_status = results_status == CM_SEMANTIC_RESULTS_OVERFLOW
            ? CM_HIR_ID_EXHAUSTED : CM_HIR_INVARIANT_VIOLATION;
        goto rollback;
    }
    hir_status = cm_hir_context_commit(hir, &mark);
    if (hir_status != CM_HIR_OK) {
        result.status = CM_SEMANTIC_ADMISSION_HIR_FAILURE;
        result.hir_status = hir_status;
        goto rollback;
    }
    mark_active = 0;
    cm_hir_crate_finalization_destroy(&finalization);
    state->hir = hir;
    state->local_crate = local_crate;
    state->storage_lifetime_id = hir->storage.lifetime_id;
    state->semantic_generation = hir->semantic_generation;
    state->rewind_generation = hir->rewind_generation;
    state->capability_id = cm_admission_new_capability_id();
    state->results = semantic_results;
    admission->state = state;
    cm_free(journal);
    result.status = CM_SEMANTIC_ADMISSION_OK;
    result.local_bodies.status = CM_HIR_LOCAL_BODIES_OK;
    result.item_result.status = CM_SEMANTIC_ITEM_OK;
    result.body_result.status = CM_SEMANTIC_BODY_OK;
    result.body_result.solver_kind = CM_TRAIT_SOLVER_PROVEN;
    result.session_status = CM_TRAIT_SOLVER_PROVEN;
    return result;

rollback:
    cm_semantic_results_body_stage_destroy(&body_stage);
    cm_semantic_session_destroy(&session);
    cm_hir_crate_finalization_destroy(&finalization);
    if (mark_active) {
        hir_status = cm_admission_rollback(hir, &mark, journal, body_count);
        if (hir_status != CM_HIR_OK) {
            result.status = CM_SEMANTIC_ADMISSION_HIR_FAILURE;
            result.hir_status = hir_status;
        }
    }
    cm_semantic_results_destroy(semantic_results);
    cm_free(state);
    cm_free(journal);
    return result;
}

CmSemanticAdmissionResult cm_semantic_admit_typed_reachable_bodies(
    CmSemanticAdmission *admission, CmHirContext *hir,
    CmHirCrateId local_crate, const CmSemanticReachableBody *bodies,
    size_t body_count)
{
    CmSemanticAdmissionResult result;
    CmHirCrateFinalization finalization;
    CmSemanticSession session;
    CmSemanticResults *semantic_results;
    CmSemanticResultsBodyStage body_stage;
    CmSemanticAdmissionState *state;
    CmHirBodyId *body_ids;
    CmSemanticResultsStatus results_status;
    size_t index;

    result = cm_admission_result(CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT);
    if (admission == NULL || admission->state != NULL || hir == NULL
        || local_crate == CM_HIR_CRATE_NONE || bodies == NULL
        || body_count == 0u || cm_hir_get_crate(hir, local_crate) == NULL
        || body_count > (size_t)-1 / sizeof(*body_ids)) return result;
    memset(&finalization, 0, sizeof(finalization));
    memset(&session, 0, sizeof(session));
    semantic_results = NULL;
    state = NULL;
    body_ids = (CmHirBodyId *)cm_alloc(body_count * sizeof(*body_ids));
    cm_semantic_results_body_stage_init(&body_stage);
    for (index = 0u; index < body_count; ++index) {
        const CmHirDefinition *definition;
        const CmHirItem *item;
        const CmHirBody *body;
        size_t previous;

        result.owner = bodies[index].owner;
        result.body = bodies[index].body;
        definition = cm_hir_lookup_definition(hir, bodies[index].owner);
        result.item = definition == NULL
            || definition->kind != CM_HIR_DEFINITION_ITEM
            || definition->state != CM_HIR_DEFINITION_BOUND
            ? CM_HIR_ITEM_NONE : definition->entity.item_id;
        item = definition == NULL
                || definition->kind != CM_HIR_DEFINITION_ITEM
                || definition->state != CM_HIR_DEFINITION_BOUND
            ? NULL : cm_hir_get_item(hir, definition->entity.item_id);
        body = cm_hir_get_body(hir, bodies[index].body);
        if (item == NULL || item->kind != CM_HIR_ITEM_FUNCTION
            || item->definition.crate_id != local_crate
            || !cm_hir_def_id_equal(item->definition,
                bodies[index].owner)
            || !cm_hir_def_id_is_none(item->parent_definition)
            || item->generic_parameter_count != 0u
            || item->data.function_item.body != bodies[index].body
            || body == NULL || body->state != CM_HIR_BODY_TYPED
            || body->root_expression == CM_HIR_EXPR_NONE
            || !cm_hir_def_id_equal(body->owner, item->definition)) {
            goto cleanup;
        }
        for (previous = 0u; previous < index; ++previous) {
            if (body_ids[previous] == bodies[index].body
                || cm_hir_def_id_equal(bodies[previous].owner,
                    bodies[index].owner)) goto cleanup;
        }
        body_ids[index] = bodies[index].body;
    }
    result.hir_status = cm_hir_crate_finalization_init(&finalization, hir,
        local_crate);
    if (result.hir_status != CM_HIR_OK) {
        result.status = CM_SEMANTIC_ADMISSION_FINALIZATION_FAILURE;
        goto cleanup;
    }
    {
        CmProjectionNormalizeLimits limits;

        limits.max_nodes = 4096u;
        limits.max_projection_steps = 256u;
        result.item_result =
            cm_semantic_item_check_finalized_local_trait_impls(
                &finalization, limits);
    }
    if (result.item_result.status != CM_SEMANTIC_ITEM_OK) {
        result.owner = cm_hir_def_id_is_none(result.item_result.impl_member)
            ? result.item_result.impl_definition
            : result.item_result.impl_member;
        result.status = CM_SEMANTIC_ADMISSION_ITEM_FAILURE;
        goto cleanup;
    }
    results_status = cm_semantic_results_begin(hir, local_crate,
        &semantic_results);
    if (results_status != CM_SEMANTIC_RESULTS_OK) {
        result.status = CM_SEMANTIC_ADMISSION_HIR_FAILURE;
        result.hir_status = results_status == CM_SEMANTIC_RESULTS_OVERFLOW
            ? CM_HIR_ID_EXHAUSTED : CM_HIR_INVARIANT_VIOLATION;
        goto cleanup;
    }
    for (index = 0u; index < body_count; ++index) {
        CmSemanticSessionOptions options;
        const CmHirDefinition *definition;

        result.owner = bodies[index].owner;
        result.body = bodies[index].body;
        definition = cm_hir_lookup_definition(hir, bodies[index].owner);
        result.item = definition == NULL ? CM_HIR_ITEM_NONE
            : definition->entity.item_id;
        cm_semantic_session_options_init(&options);
        options.local_crate = local_crate;
        options.exact_owner = bodies[index].owner;
        options.universe = CM_TRAIT_IMPL_UNIVERSE_SINGLE_LOCAL_CRATE_COMPLETE;
        options.finalization = &finalization;
        result.session_status = cm_semantic_session_init(&session, hir,
            &options);
        if (result.session_status != CM_TRAIT_SOLVER_PROVEN) {
            result.status = CM_SEMANTIC_ADMISSION_SESSION_FAILURE;
            goto cleanup;
        }
        {
            CmSemanticBodyEvidenceWriteback writeback;

            memset(&writeback, 0, sizeof(writeback));
            writeback.context = &body_stage;
            writeback.checked_body = cm_semantic_results_stage_checked_body;
            writeback.projection_decision =
                cm_semantic_results_stage_projection_decision;
            writeback.discard = cm_semantic_results_discard_body_stage;
            result.body_result =
                cm_semantic_body_check_definition_with_evidence(&session,
                    bodies[index].body, &writeback);
        }
        if (result.body_result.status != CM_SEMANTIC_BODY_OK) {
            result.status = CM_SEMANTIC_ADMISSION_BODY_FAILURE;
            goto cleanup;
        }
        results_status = cm_semantic_results_commit_checked_body(
            semantic_results, &session, &result.body_result, &body_stage);
        cm_semantic_session_destroy(&session);
        if (results_status != CM_SEMANTIC_RESULTS_OK) {
            result.status = CM_SEMANTIC_ADMISSION_HIR_FAILURE;
            result.hir_status = results_status == CM_SEMANTIC_RESULTS_OVERFLOW
                ? CM_HIR_ID_EXHAUSTED : CM_HIR_INVARIANT_VIOLATION;
            goto cleanup;
        }
    }
    results_status = cm_semantic_results_seal_reachable(semantic_results,
        body_ids, body_count);
    if (results_status != CM_SEMANTIC_RESULTS_OK) {
        result.status = CM_SEMANTIC_ADMISSION_HIR_FAILURE;
        result.hir_status = results_status == CM_SEMANTIC_RESULTS_OVERFLOW
            ? CM_HIR_ID_EXHAUSTED : CM_HIR_INVARIANT_VIOLATION;
        goto cleanup;
    }
    state = (CmSemanticAdmissionState *)cm_alloc_zeroed(1u,
        sizeof(*state));
    state->hir = hir;
    state->local_crate = local_crate;
    state->storage_lifetime_id = hir->storage.lifetime_id;
    state->semantic_generation = hir->semantic_generation;
    state->rewind_generation = hir->rewind_generation;
    state->capability_id = cm_admission_new_capability_id();
    state->results = semantic_results;
    admission->state = state;
    semantic_results = NULL;
    result.status = CM_SEMANTIC_ADMISSION_OK;
    result.item_result.status = CM_SEMANTIC_ITEM_OK;
    result.body_result.status = CM_SEMANTIC_BODY_OK;
    result.body_result.solver_kind = CM_TRAIT_SOLVER_PROVEN;
    result.session_status = CM_TRAIT_SOLVER_PROVEN;

cleanup:
    cm_semantic_results_body_stage_destroy(&body_stage);
    cm_semantic_session_destroy(&session);
    cm_hir_crate_finalization_destroy(&finalization);
    cm_semantic_results_destroy(semantic_results);
    cm_free(body_ids);
    return result;
}

typedef struct CmAdmissionCanonicalCall {
    CmHirCanonicalInstance caller;
    CmHirExprId expression;
    CmHirCanonicalInstance callee;
} CmAdmissionCanonicalCall;

static CmSemanticAdmissionResult cm_admit_typed_instances(
    CmSemanticAdmission *admission, CmHirContext *hir,
    CmHirCrateId local_crate, const CmSemanticReachableInstance *instances,
    size_t instance_count, const CmSemanticReachableInstanceCall *calls,
    size_t call_count, int leaf_only)
{
    CmSemanticAdmissionResult result;
    CmHirCrateFinalization finalization;
    CmSemanticSession session;
    CmSemanticResults *semantic_results;
    CmSemanticResultsBodyStage stage;
    CmSemanticAdmissionState *state;
    CmHirCanonicalInstance *identities;
    CmAdmissionCanonicalCall *canonical_calls;
    size_t call_bytes;
    size_t index;

    result = cm_admission_result(CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT);
    if (admission == NULL || admission->state != NULL || hir == NULL
        || local_crate == CM_HIR_CRATE_NONE || instances == NULL
        || instance_count == 0u
        || (call_count == 0u) != (calls == NULL)
        || (leaf_only && call_count != 0u)
        || cm_hir_get_crate(hir, local_crate) == NULL
        || !cm_size_mul(instance_count, sizeof(*identities), NULL)
        || !cm_size_mul(call_count, sizeof(*canonical_calls),
            &call_bytes)) return result;
    memset(&finalization, 0, sizeof(finalization));
    memset(&session, 0, sizeof(session));
    cm_semantic_results_body_stage_init(&stage);
    semantic_results = NULL;
    state = NULL;
    identities = (CmHirCanonicalInstance *)cm_alloc_zeroed(instance_count,
        sizeof(*identities));
    canonical_calls = call_count == 0u ? NULL
        : (CmAdmissionCanonicalCall *)cm_alloc_zeroed(1u, call_bytes);
    for (index = 0u; index < call_count; ++index) {
        cm_hir_canonical_instance_init(&canonical_calls[index].caller);
        cm_hir_canonical_instance_init(&canonical_calls[index].callee);
    }
    for (index = 0u; index < instance_count; ++index) {
        const CmHirItem *item;
        const CmHirBody *body;
        const CmHirInstanceSpec *spec;
        CmHirInstanceStatus instance_status;
        size_t previous;

        spec = instances[index].spec;
        if (spec == NULL) goto cleanup_instances;
        result.owner = spec->selected_callable;
        result.body = instances[index].body;
        item = cm_admission_item(hir, spec->selected_callable);
        body = cm_hir_get_body(hir, instances[index].body);
        if (!cm_admission_instance_callable_supported(hir, local_crate,
                spec, item, leaf_only)
            || item->data.function_item.body != instances[index].body
            || body == NULL || body->state != CM_HIR_BODY_TYPED
            || body->root_expression == CM_HIR_EXPR_NONE
            || !cm_hir_def_id_equal(body->owner, item->definition)) {
            goto cleanup_instances;
        }
        instance_status = cm_hir_canonical_instance_encode(hir, local_crate,
            spec, &identities[index]);
        if (instance_status != CM_HIR_INSTANCE_OK
            || identities[index].body != instances[index].body) {
            goto cleanup_instances;
        }
        for (previous = 0u; previous < index; ++previous) {
            int equal;

            if (cm_hir_canonical_instance_equal(&identities[previous],
                    &identities[index], &equal) != CM_HIR_INSTANCE_OK
                || equal) goto cleanup_instances;
        }
    }
    for (index = 0u; index < call_count; ++index) {
        const CmHirExpr *expression;
        CmHirCanonicalInstance checked_callee;
        size_t instance_index;
        size_t previous;
        int caller_member;
        int callee_member;
        int equal;

        cm_hir_canonical_instance_init(&checked_callee);
        canonical_calls[index].expression = calls[index].expression;
        if (calls[index].caller == NULL || calls[index].callee == NULL
            || cm_hir_canonical_instance_encode(hir, local_crate,
                calls[index].caller, &canonical_calls[index].caller)
                != CM_HIR_INSTANCE_OK
            || cm_hir_canonical_instance_encode(hir, local_crate,
                calls[index].callee, &canonical_calls[index].callee)
                != CM_HIR_INSTANCE_OK) {
            goto cleanup_instances;
        }
        expression = cm_hir_get_expr(hir, calls[index].expression);
        if (expression == NULL
            || expression->owner_body
                != canonical_calls[index].caller.body) {
            cm_hir_canonical_instance_destroy(&checked_callee);
            goto cleanup_instances;
        }
        if (expression->kind == CM_HIR_EXPR_CALL) {
            if (!cm_hir_def_id_equal(expression->data.call.callee,
                    canonical_calls[index].callee.definition)
                || cm_hir_canonical_instance_encode_direct_call(hir,
                    local_crate, calls[index].caller, expression,
                    &checked_callee) != CM_HIR_INSTANCE_OK
                || cm_hir_canonical_instance_equal(&checked_callee,
                    &canonical_calls[index].callee, &equal)
                    != CM_HIR_INSTANCE_OK || !equal) {
                cm_hir_canonical_instance_destroy(&checked_callee);
                goto cleanup_instances;
            }
        } else if (expression->kind == CM_HIR_EXPR_QUALIFIED_CALL) {
            CmHirInstanceSpec requested_spec;

            requested_spec = *calls[index].callee;
            requested_spec.self_type =
                expression->data.qualified_call.requested_self_type;
            if (calls[index].callee->selected_callable.crate_id
                    != local_crate
                || !cm_hir_def_id_equal(
                    calls[index].callee->selected_callable,
                    canonical_calls[index].callee.definition)
                || !cm_hir_def_id_equal(
                    calls[index].callee->declared_trait_callable,
                    expression->data.qualified_call
                        .declared_trait_callable)
                || !cm_hir_def_id_equal(
                    calls[index].callee->implemented_trait,
                    expression->data.qualified_call.requested_trait)
                || cm_hir_canonical_instance_encode(hir, local_crate,
                    &requested_spec, &checked_callee) != CM_HIR_INSTANCE_OK
                || cm_hir_canonical_instance_equal(&checked_callee,
                    &canonical_calls[index].callee, &equal)
                    != CM_HIR_INSTANCE_OK || !equal) {
                cm_hir_canonical_instance_destroy(&checked_callee);
                goto cleanup_instances;
            }
        } else if (expression->kind == CM_HIR_EXPR_METHOD_CALL) {
            const CmHirExpr *receiver;
            const CmHirItem *declared;
            const CmHirItem *selected;
            CmHirInstanceSpec requested_spec;

            receiver = cm_hir_get_expr(hir,
                expression->data.method_call.receiver);
            declared = cm_admission_item(hir,
                calls[index].callee->declared_trait_callable);
            selected = cm_admission_item(hir,
                calls[index].callee->selected_callable);
            requested_spec = *calls[index].callee;
            requested_spec.self_type = receiver == NULL
                ? CM_HIR_TYPE_NONE : receiver->type;
            if (expression->data.method_call.syntax
                    != CM_HIR_CALLABLE_DOT_METHOD
                || expression->data.method_call.receiver == CM_HIR_EXPR_NONE
                || expression->data.method_call.argument_count > 1u
                || (expression->data.method_call.argument_count != 0u
                    && expression->data.method_call.arguments == NULL)
                || receiver == NULL
                || receiver->owner_body != expression->owner_body
                || calls[index].callee->selected_callable.crate_id
                    != local_crate
                || !cm_hir_def_id_equal(
                    calls[index].callee->selected_callable,
                    canonical_calls[index].callee.definition)
                || selected == NULL || selected->kind != CM_HIR_ITEM_FUNCTION
                || selected->data.function_item.signature.receiver
                    != CM_HIR_RECEIVER_VALUE
                || selected->data.function_item.signature.parameter_count
                    != expression->data.method_call.argument_count + 1u
                || declared == NULL || declared->kind != CM_HIR_ITEM_FUNCTION
                || declared->name != expression->data.method_call.method_name
                || declared->data.function_item.signature.receiver
                    != CM_HIR_RECEIVER_VALUE
                || !cm_hir_def_id_equal(declared->parent_definition,
                    calls[index].callee->implemented_trait)
                || !cm_admission_method_trait_in_scope(expression,
                    calls[index].callee->implemented_trait)
                || !cm_hir_def_id_equal(
                    selected->data.function_item.trait_item_definition,
                    declared->definition)
                || cm_hir_canonical_instance_encode(hir, local_crate,
                    &requested_spec, &checked_callee) != CM_HIR_INSTANCE_OK
                || cm_hir_canonical_instance_equal(&checked_callee,
                    &canonical_calls[index].callee, &equal)
                    != CM_HIR_INSTANCE_OK || !equal) {
                cm_hir_canonical_instance_destroy(&checked_callee);
                goto cleanup_instances;
            }
        } else {
            cm_hir_canonical_instance_destroy(&checked_callee);
            goto cleanup_instances;
        }
        cm_hir_canonical_instance_destroy(&checked_callee);
        caller_member = 0;
        callee_member = 0;
        for (instance_index = 0u; instance_index < instance_count;
             ++instance_index) {
            equal = 0;
            if (cm_hir_canonical_instance_equal(
                    &canonical_calls[index].caller,
                    &identities[instance_index], &equal)
                    != CM_HIR_INSTANCE_OK) goto cleanup_instances;
            caller_member += equal;
            equal = 0;
            if (cm_hir_canonical_instance_equal(
                    &canonical_calls[index].callee,
                    &identities[instance_index], &equal)
                    != CM_HIR_INSTANCE_OK) goto cleanup_instances;
            callee_member += equal;
        }
        if (caller_member != 1 || callee_member != 1) {
            goto cleanup_instances;
        }
        for (previous = 0u; previous < index; ++previous) {
            equal = 0;
            if (canonical_calls[previous].expression
                    == canonical_calls[index].expression
                && cm_hir_canonical_instance_equal(
                    &canonical_calls[previous].caller,
                    &canonical_calls[index].caller, &equal)
                    == CM_HIR_INSTANCE_OK && equal) {
                goto cleanup_instances;
            }
        }
    }
    result.hir_status = cm_hir_crate_finalization_init(&finalization, hir,
        local_crate);
    if (result.hir_status != CM_HIR_OK) {
        result.status = CM_SEMANTIC_ADMISSION_FINALIZATION_FAILURE;
        goto cleanup_instances;
    }
    {
        CmProjectionNormalizeLimits limits;

        limits.max_nodes = 4096u;
        limits.max_projection_steps = 256u;
        result.item_result =
            cm_semantic_item_check_finalized_local_trait_impls(
                &finalization, limits);
    }
    if (result.item_result.status != CM_SEMANTIC_ITEM_OK) {
        result.status = CM_SEMANTIC_ADMISSION_ITEM_FAILURE;
        goto cleanup_instances;
    }
    if (cm_semantic_results_begin(hir, local_crate, &semantic_results)
            != CM_SEMANTIC_RESULTS_OK) {
        result.status = CM_SEMANTIC_ADMISSION_HIR_FAILURE;
        result.hir_status = CM_HIR_ID_EXHAUSTED;
        goto cleanup_instances;
    }
    for (index = 0u; index < instance_count; ++index) {
        const CmHirInstanceSpec *spec;
        CmSemanticSessionOptions options;
        CmSemanticResultsStatus results_status;
        const CmHirTypeId *substitutions;
        uint32_t substitution_count;
        uint32_t argument;
        CmSemanticCanonicalCallInput *instance_calls;
        size_t instance_call_count;
        size_t call_index;

        spec = instances[index].spec;
        result.owner = spec->selected_callable;
        result.body = instances[index].body;
        substitution_count = cm_hir_def_id_is_none(spec->enclosing_impl)
            ? spec->item_argument_count : spec->method_argument_count;
        substitutions = substitution_count == 0u ? NULL
            : (const CmHirTypeId *)cm_alloc(
                substitution_count * sizeof(*substitutions));
        for (argument = 0u; argument < substitution_count; ++argument) {
            const CmHirGenericArg *arguments;

            arguments = cm_hir_def_id_is_none(spec->enclosing_impl)
                ? spec->item_arguments : spec->method_arguments;
            if (arguments[argument].kind
                    != CM_HIR_GENERIC_ARG_TYPE) {
                cm_free((void *)substitutions);
                goto cleanup_instances;
            }
            ((CmHirTypeId *)substitutions)[argument] =
                arguments[argument].data.type;
        }
        cm_semantic_session_options_init(&options);
        options.local_crate = local_crate;
        options.exact_owner = spec->selected_callable;
        options.universe = CM_TRAIT_IMPL_UNIVERSE_SINGLE_LOCAL_CRATE_COMPLETE;
        options.finalization = &finalization;
        result.session_status = cm_semantic_session_init(&session, hir,
            &options);
        if (result.session_status != CM_TRAIT_SOLVER_PROVEN) {
            cm_free((void *)substitutions);
            result.status = CM_SEMANTIC_ADMISSION_SESSION_FAILURE;
            goto cleanup_instances;
        }
        {
            CmSemanticBodyEvidenceWriteback writeback;

            memset(&writeback, 0, sizeof(writeback));
            writeback.context = &stage;
            writeback.checked_body = cm_semantic_results_stage_checked_body;
            writeback.projection_decision =
                cm_semantic_results_stage_projection_decision;
            writeback.discard = cm_semantic_results_discard_body_stage;
            result.body_result = cm_semantic_body_check_instance_with_evidence(
                &session, instances[index].body, substitutions,
                substitution_count, &writeback);
        }
        cm_free((void *)substitutions);
        if (result.body_result.status != CM_SEMANTIC_BODY_OK) {
            result.status = CM_SEMANTIC_ADMISSION_BODY_FAILURE;
            goto cleanup_instances;
        }
        instance_call_count = 0u;
        for (call_index = 0u; call_index < call_count; ++call_index) {
            int equal;

            equal = 0;
            if (cm_hir_canonical_instance_equal(&identities[index],
                    &canonical_calls[call_index].caller, &equal)
                    == CM_HIR_INSTANCE_OK && equal) {
                ++instance_call_count;
            }
        }
        if (!cm_size_mul(instance_call_count, sizeof(*instance_calls),
                &call_bytes)) goto cleanup_instances;
        instance_calls = instance_call_count == 0u ? NULL
            : (CmSemanticCanonicalCallInput *)cm_alloc_zeroed(1u,
                call_bytes);
        instance_call_count = 0u;
        for (call_index = 0u; call_index < call_count; ++call_index) {
            int equal;

            equal = 0;
            if (cm_hir_canonical_instance_equal(&identities[index],
                    &canonical_calls[call_index].caller, &equal)
                    == CM_HIR_INSTANCE_OK && equal) {
                instance_calls[instance_call_count].expression =
                    canonical_calls[call_index].expression;
                instance_calls[instance_call_count].callee =
                    &canonical_calls[call_index].callee;
                ++instance_call_count;
            }
        }
        results_status = cm_semantic_results_commit_checked_instance(
            semantic_results, &session, &identities[index],
            &result.body_result, &stage, instance_calls,
            instance_call_count);
        cm_free(instance_calls);
        cm_semantic_session_destroy(&session);
        if (results_status != CM_SEMANTIC_RESULTS_OK) {
            result.status = CM_SEMANTIC_ADMISSION_HIR_FAILURE;
            result.hir_status = results_status == CM_SEMANTIC_RESULTS_OVERFLOW
                ? CM_HIR_ID_EXHAUSTED : CM_HIR_INVARIANT_VIOLATION;
            goto cleanup_instances;
        }
    }
    if ((leaf_only
            ? cm_semantic_results_seal_leaf_instances(semantic_results,
                instance_count)
            : cm_semantic_results_seal_instance_closure(semantic_results,
                instance_count)) != CM_SEMANTIC_RESULTS_OK) {
        result.status = CM_SEMANTIC_ADMISSION_HIR_FAILURE;
        result.hir_status = CM_HIR_INVARIANT_VIOLATION;
        goto cleanup_instances;
    }
    state = (CmSemanticAdmissionState *)cm_alloc_zeroed(1u, sizeof(*state));
    state->hir = hir;
    state->local_crate = local_crate;
    state->storage_lifetime_id = hir->storage.lifetime_id;
    state->semantic_generation = hir->semantic_generation;
    state->rewind_generation = hir->rewind_generation;
    state->capability_id = cm_admission_new_capability_id();
    state->results = semantic_results;
    semantic_results = NULL;
    admission->state = state;
    result.status = CM_SEMANTIC_ADMISSION_OK;
    result.item_result.status = CM_SEMANTIC_ITEM_OK;
    result.body_result.status = CM_SEMANTIC_BODY_OK;
    result.body_result.solver_kind = CM_TRAIT_SOLVER_PROVEN;
    result.session_status = CM_TRAIT_SOLVER_PROVEN;

cleanup_instances:
    cm_semantic_results_body_stage_destroy(&stage);
    cm_semantic_session_destroy(&session);
    cm_hir_crate_finalization_destroy(&finalization);
    cm_semantic_results_destroy(semantic_results);
    for (index = 0u; index < instance_count; ++index) {
        cm_hir_canonical_instance_destroy(&identities[index]);
    }
    for (index = 0u; index < call_count; ++index) {
        cm_hir_canonical_instance_destroy(&canonical_calls[index].caller);
        cm_hir_canonical_instance_destroy(&canonical_calls[index].callee);
    }
    cm_free(canonical_calls);
    cm_free(identities);
    return result;
}

CmSemanticAdmissionResult cm_semantic_admit_typed_leaf_instances(
    CmSemanticAdmission *admission, CmHirContext *hir,
    CmHirCrateId local_crate, const CmSemanticReachableInstance *instances,
    size_t instance_count)
{
    return cm_admit_typed_instances(admission, hir, local_crate, instances,
        instance_count, NULL, 0u, 1);
}

CmSemanticAdmissionResult cm_semantic_admit_typed_instance_closure(
    CmSemanticAdmission *admission, CmHirContext *hir,
    CmHirCrateId local_crate, const CmSemanticReachableInstance *instances,
    size_t instance_count, const CmSemanticReachableInstanceCall *calls,
    size_t call_count)
{
    return cm_admit_typed_instances(admission, hir, local_crate, instances,
        instance_count, calls, call_count, 0);
}

void cm_semantic_admission_destroy(CmSemanticAdmission *admission)
{
    if (admission == NULL) return;
    if (admission->state != NULL) {
        CmSemanticAdmissionState *state;
        state = (CmSemanticAdmissionState *)admission->state;
        cm_semantic_results_destroy(state->results);
        memset(admission->state, 0, sizeof(CmSemanticAdmissionState));
        cm_free(admission->state);
    }
    admission->state = NULL;
}

const CmSemanticResults *cm_semantic_admission_results(
    const CmSemanticAdmission *admission)
{
    const CmSemanticAdmissionState *state = admission == NULL ? NULL
        : (const CmSemanticAdmissionState *)admission->state;
    return cm_admission_state_current(state) ? state->results : NULL;
}

int cm_semantic_admission_is_current(const CmSemanticAdmission *admission)
{ return admission != NULL && cm_admission_state_current(admission->state); }

const CmHirContext *cm_semantic_admission_hir(
    const CmSemanticAdmission *admission)
{
    const CmSemanticAdmissionState *state = admission == NULL ? NULL
        : (const CmSemanticAdmissionState *)admission->state;
    return cm_admission_state_current(state) ? state->hir : NULL;
}

CmHirCrateId cm_semantic_admission_crate(
    const CmSemanticAdmission *admission)
{
    const CmSemanticAdmissionState *state = admission == NULL ? NULL
        : (const CmSemanticAdmissionState *)admission->state;
    return cm_admission_state_current(state)
        ? state->local_crate : CM_HIR_CRATE_NONE;
}

uint64_t cm_semantic_admission_generation(
    const CmSemanticAdmission *admission)
{
    const CmSemanticAdmissionState *state = admission == NULL ? NULL
        : (const CmSemanticAdmissionState *)admission->state;
    return cm_admission_state_current(state)
        ? state->semantic_generation : UINT64_C(0);
}

uint64_t cm_semantic_admission_capability_id(
    const CmSemanticAdmission *admission)
{
    const CmSemanticAdmissionState *state = admission == NULL ? NULL
        : (const CmSemanticAdmissionState *)admission->state;
    return cm_admission_state_current(state)
        ? state->capability_id : UINT64_C(0);
}

const char *cm_semantic_admission_status_name(CmSemanticAdmissionStatus status)
{
    switch (status) {
    case CM_SEMANTIC_ADMISSION_OK: return "ok";
    case CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT: return "invalid-argument";
    case CM_SEMANTIC_ADMISSION_LOCAL_BODIES_FAILURE: return "local-bodies-failure";
    case CM_SEMANTIC_ADMISSION_FINALIZATION_FAILURE: return "finalization-failure";
    case CM_SEMANTIC_ADMISSION_ITEM_FAILURE: return "item-failure";
    case CM_SEMANTIC_ADMISSION_SESSION_FAILURE: return "session-failure";
    case CM_SEMANTIC_ADMISSION_BODY_FAILURE: return "body-failure";
    case CM_SEMANTIC_ADMISSION_HIR_FAILURE: return "hir-failure";
    }
    return "unknown";
}
