#include "cm/hir/admission.h"

#include "admission_internal.h"
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

typedef struct CmAdmissionCanonicalCallInput {
    const CmHirCanonicalInstance *caller;
    CmHirExprId expression;
    const CmHirCanonicalInstance *callee;
} CmAdmissionCanonicalCallInput;

static int cm_admission_canonical_member(
    const CmHirCanonicalInstance *instances, size_t instance_count,
    const CmHirCanonicalInstance *identity)
{
    size_t index;
    int matches;

    matches = 0;
    for (index = 0u; index < instance_count; ++index) {
        int equal;

        equal = 0;
        if (cm_hir_canonical_instance_equal(&instances[index], identity,
                &equal) != CM_HIR_INSTANCE_OK) return -1;
        matches += equal;
    }
    return matches;
}

static int cm_admission_canonical_call_supported(
    const CmHirContext *hir, CmHirCrateId local_crate,
    const CmHirDecodedCanonicalInstance *caller,
    const CmHirExpr *expression,
    const CmHirDecodedCanonicalInstance *callee,
    const CmHirCanonicalInstance *callee_identity)
{
    const CmHirItem *declared;
    const CmHirItem *selected;
    CmHirCanonicalInstance expected;
    int equal;

    if (hir == NULL || caller == NULL || expression == NULL
        || callee == NULL || callee_identity == NULL) return 0;
    cm_hir_canonical_instance_init(&expected);
    equal = 0;
    if (expression->kind == CM_HIR_EXPR_CALL) {
        if (!cm_hir_def_id_equal(expression->data.call.callee,
                callee->parts.selected_callable)
            || cm_hir_canonical_instance_encode_direct_call_parts(hir,
                local_crate, &caller->parts, expression, &expected)
                    != CM_HIR_INSTANCE_OK) {
            cm_hir_canonical_instance_destroy(&expected);
            return 0;
        }
    } else if (expression->kind == CM_HIR_EXPR_QUALIFIED_CALL) {
        if (callee->parts.selected_callable.crate_id != local_crate
            || !cm_hir_def_id_equal(callee->parts.declared_trait_callable,
                expression->data.qualified_call.declared_trait_callable)
            || !cm_hir_def_id_equal(callee->parts.implemented_trait,
                expression->data.qualified_call.requested_trait)) {
            return 0;
        }
        /* The complete key is authenticated against staged callable evidence
         * before results are committed.  Source syntax only constrains the
         * declaration and requested trait at this earlier boundary. */
        return 1;
    } else if (expression->kind == CM_HIR_EXPR_METHOD_CALL) {
        declared = cm_admission_item(hir,
            callee->parts.declared_trait_callable);
        selected = cm_admission_item(hir,
            callee->parts.selected_callable);
        if (expression->data.method_call.syntax != CM_HIR_CALLABLE_DOT_METHOD
            || expression->data.method_call.receiver == CM_HIR_EXPR_NONE
            || expression->data.method_call.argument_count > 1u
            || (expression->data.method_call.argument_count != 0u
                && expression->data.method_call.arguments == NULL)
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
                callee->parts.implemented_trait)
            || !cm_admission_method_trait_in_scope(expression,
                callee->parts.implemented_trait)
            || !cm_hir_def_id_equal(
                selected->data.function_item.trait_item_definition,
                declared->definition)) return 0;
        return 1;
    } else {
        return 0;
    }
    if (cm_hir_canonical_instance_equal(&expected, callee_identity,
            &equal) != CM_HIR_INSTANCE_OK) equal = 0;
    cm_hir_canonical_instance_destroy(&expected);
    return equal;
}

static int cm_admission_canonical_instance_supported(
    const CmHirContext *hir, CmHirCrateId local_crate,
    const CmHirDecodedCanonicalInstance *decoded, const CmHirBody *body,
    int leaf_only)
{
    const CmHirItem *selected;
    const CmHirItem *enclosing;

    if (hir == NULL || local_crate == CM_HIR_CRATE_NONE || decoded == NULL
        || body == NULL || body->state != CM_HIR_BODY_TYPED
        || body->root_expression == CM_HIR_EXPR_NONE) return 0;
    selected = cm_admission_item(hir, decoded->parts.selected_callable);
    if (selected == NULL || selected->kind != CM_HIR_ITEM_FUNCTION
        || selected->definition.crate_id != local_crate
        || selected->predicate_scope_count != 0u
        || (leaf_only && selected->predicate_count != 0u)
        || selected->outlives_predicate_count != 0u
        || selected->data.function_item.body == CM_HIR_BODY_NONE
        || !cm_hir_def_id_equal(body->owner, selected->definition)) {
        return 0;
    }
    if (cm_hir_def_id_is_none(selected->parent_definition)) {
        return cm_hir_def_id_is_none(
                selected->data.function_item.trait_item_definition)
            && decoded->parts.item_argument_count
                == selected->generic_parameter_count;
    }
    enclosing = cm_admission_item(hir, decoded->parts.enclosing_impl);
    return selected->generic_parameter_count
            == decoded->parts.method_argument_count
        && selected->predicate_count == 0u
        && enclosing != NULL && enclosing->kind == CM_HIR_ITEM_IMPL
        && enclosing->definition.crate_id == local_crate
        && enclosing->predicate_scope_count == 0u
        && enclosing->predicate_count == 0u
        && enclosing->outlives_predicate_count == 0u
        && enclosing->data.impl_item.has_trait
        && !enclosing->data.impl_item.is_negative
        && cm_hir_def_id_equal(selected->parent_definition,
            enclosing->definition)
        && cm_hir_def_id_equal(decoded->parts.self_owner,
            enclosing->definition);
}

static CmSemanticAdmissionResult cm_admit_typed_canonical_instances(
    CmSemanticAdmission *admission, CmHirContext *hir,
    CmHirCrateId local_crate,
    const CmSemanticCanonicalReachableInstance *instances,
    size_t instance_count, const CmAdmissionCanonicalCallInput *calls,
    size_t call_count, int leaf_only)
{
    CmSemanticAdmissionResult result;
    CmHirCrateFinalization finalization;
    CmSemanticSession session;
    CmSemanticResults *semantic_results;
    CmSemanticResultsBodyStage stage;
    CmSemanticAdmissionState *state;
    CmHirCanonicalInstance *identities;
    CmHirDecodedCanonicalInstance *decoded;
    size_t allocation_size;
    size_t index;

    result = cm_admission_result(CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT);
    if (admission == NULL || admission->state != NULL || hir == NULL
        || local_crate == CM_HIR_CRATE_NONE || instances == NULL
        || instance_count == 0u
        || (call_count == 0u) != (calls == NULL)
        || (leaf_only && call_count != 0u)
        || cm_hir_get_crate(hir, local_crate) == NULL
        || !cm_size_mul(instance_count, sizeof(*identities), NULL)
        || !cm_size_mul(instance_count, sizeof(*decoded),
            &allocation_size)) return result;
    memset(&finalization, 0, sizeof(finalization));
    memset(&session, 0, sizeof(session));
    cm_semantic_results_body_stage_init(&stage);
    semantic_results = NULL;
    state = NULL;
    identities = (CmHirCanonicalInstance *)cm_alloc_zeroed(instance_count,
        sizeof(*identities));
    decoded = (CmHirDecodedCanonicalInstance *)cm_alloc_zeroed(1u,
        allocation_size);
    for (index = 0u; index < instance_count; ++index) {
        const CmHirBody *body;
        size_t previous;

        cm_hir_canonical_instance_init(&identities[index]);
        cm_hir_decoded_canonical_instance_init(&decoded[index]);
        if (instances[index].identity == NULL
            || cm_hir_canonical_instance_clone(&identities[index],
                instances[index].identity) != CM_HIR_INSTANCE_OK
            || cm_hir_canonical_instance_decode(hir, local_crate,
                &identities[index], &decoded[index]) != CM_HIR_INSTANCE_OK) {
            goto cleanup_instances;
        }
        result.owner = identities[index].definition;
        result.body = identities[index].body;
        body = cm_hir_get_body(hir, identities[index].body);
        if (!cm_admission_canonical_instance_supported(hir, local_crate,
                &decoded[index], body, leaf_only)) goto cleanup_instances;
        for (previous = 0u; previous < index; ++previous) {
            int equal;

            equal = 0;
            if (cm_hir_canonical_instance_equal(&identities[previous],
                    &identities[index], &equal) != CM_HIR_INSTANCE_OK
                || equal) goto cleanup_instances;
        }
    }
    for (index = 0u; index < call_count; ++index) {
        const CmHirExpr *expression;
        size_t caller_index;
        size_t callee_index;
        size_t previous;
        int equal;

        if (calls[index].caller == NULL || calls[index].callee == NULL
            || cm_hir_canonical_instance_validate(hir, local_crate,
                calls[index].caller) != CM_HIR_INSTANCE_OK
            || cm_hir_canonical_instance_validate(hir, local_crate,
                calls[index].callee) != CM_HIR_INSTANCE_OK
            || cm_admission_canonical_member(identities, instance_count,
                calls[index].caller) != 1
            || cm_admission_canonical_member(identities, instance_count,
                calls[index].callee) != 1) goto cleanup_instances;
        expression = cm_hir_get_expr(hir, calls[index].expression);
        if (expression == NULL
            || expression->owner_body != calls[index].caller->body) {
            goto cleanup_instances;
        }
        caller_index = 0u;
        while (caller_index < instance_count) {
            equal = 0;
            if (cm_hir_canonical_instance_equal(&identities[caller_index],
                    calls[index].caller, &equal) != CM_HIR_INSTANCE_OK) {
                goto cleanup_instances;
            }
            if (equal) break;
            ++caller_index;
        }
        callee_index = 0u;
        while (callee_index < instance_count) {
            equal = 0;
            if (cm_hir_canonical_instance_equal(&identities[callee_index],
                    calls[index].callee, &equal) != CM_HIR_INSTANCE_OK) {
                goto cleanup_instances;
            }
            if (equal) break;
            ++callee_index;
        }
        if (caller_index == instance_count || callee_index == instance_count
            || !cm_admission_canonical_call_supported(hir, local_crate,
                &decoded[caller_index], expression, &decoded[callee_index],
                calls[index].callee)) goto cleanup_instances;
        for (previous = 0u; previous < index; ++previous) {
            equal = 0;
            if (calls[previous].expression == calls[index].expression
                && cm_hir_canonical_instance_equal(calls[previous].caller,
                    calls[index].caller, &equal) == CM_HIR_INSTANCE_OK
                && equal) goto cleanup_instances;
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
        CmSemanticSessionOptions options;
        CmSemanticResultsStatus results_status;
        CmSemanticCanonicalCallInput *instance_calls;
        size_t instance_call_count;
        size_t call_index;

        result.owner = identities[index].definition;
        result.body = identities[index].body;
        cm_semantic_session_options_init(&options);
        options.local_crate = local_crate;
        options.exact_owner = identities[index].definition;
        options.universe = CM_TRAIT_IMPL_UNIVERSE_SINGLE_LOCAL_CRATE_COMPLETE;
        options.finalization = &finalization;
        result.session_status = cm_semantic_session_init(&session, hir,
            &options);
        if (result.session_status != CM_TRAIT_SOLVER_PROVEN) {
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
            result.body_result =
                cm_semantic_body_check_instance_parts_with_evidence(
                    &session, identities[index].body,
                    &decoded[index].parts, &writeback);
        }
        if (result.body_result.status != CM_SEMANTIC_BODY_OK) {
            result.status = CM_SEMANTIC_ADMISSION_BODY_FAILURE;
            goto cleanup_instances;
        }
        instance_call_count = 0u;
        for (call_index = 0u; call_index < call_count; ++call_index) {
            int equal;

            equal = 0;
            if (cm_hir_canonical_instance_equal(&identities[index],
                    calls[call_index].caller, &equal)
                    == CM_HIR_INSTANCE_OK && equal) {
                ++instance_call_count;
            }
        }
        if (!cm_size_mul(instance_call_count, sizeof(*instance_calls),
                &allocation_size)) goto cleanup_instances;
        instance_calls = instance_call_count == 0u ? NULL
            : (CmSemanticCanonicalCallInput *)cm_alloc_zeroed(1u,
                allocation_size);
        instance_call_count = 0u;
        for (call_index = 0u; call_index < call_count; ++call_index) {
            int equal;

            equal = 0;
            if (cm_hir_canonical_instance_equal(&identities[index],
                    calls[call_index].caller, &equal)
                    == CM_HIR_INSTANCE_OK && equal) {
                const CmHirExpr *expression;

                expression = cm_hir_get_expr(hir,
                    calls[call_index].expression);
                if (expression != NULL
                    && (expression->kind == CM_HIR_EXPR_QUALIFIED_CALL
                        || expression->kind == CM_HIR_EXPR_METHOD_CALL)) {
                    CmHirCanonicalInstance expected;

                    cm_hir_canonical_instance_init(&expected);
                    equal = 0;
                    if (cm_semantic_results_stage_callable_callee_identity(
                            &stage, calls[call_index].expression, &expected)
                            != CM_SEMANTIC_RESULTS_OK
                        || cm_hir_canonical_instance_equal(&expected,
                            calls[call_index].callee, &equal)
                            != CM_HIR_INSTANCE_OK || !equal) {
                        cm_hir_canonical_instance_destroy(&expected);
                        cm_free(instance_calls);
                        result.status = CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT;
                        goto cleanup_instances;
                    }
                    cm_hir_canonical_instance_destroy(&expected);
                }
                instance_calls[instance_call_count].expression =
                    calls[call_index].expression;
                instance_calls[instance_call_count].callee =
                    calls[call_index].callee;
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
        cm_hir_decoded_canonical_instance_destroy(&decoded[index]);
        cm_hir_canonical_instance_destroy(&identities[index]);
    }
    cm_free(decoded);
    cm_free(identities);
    return result;
}

CmSemanticAdmissionResult cm_semantic_admit_typed_leaf_instances(
    CmSemanticAdmission *admission, CmHirContext *hir,
    CmHirCrateId local_crate, const CmSemanticReachableInstance *instances,
    size_t instance_count)
{
    CmSemanticCanonicalReachableInstance *canonical;
    CmHirCanonicalInstance *identities;
    CmSemanticAdmissionResult result;
    size_t allocation_size;
    size_t index;

    result = cm_admission_result(CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT);
    if (instances == NULL || instance_count == 0u
        || !cm_size_mul(instance_count, sizeof(*canonical),
            &allocation_size)
        || !cm_size_mul(instance_count, sizeof(*identities), NULL)) {
        return result;
    }
    canonical = (CmSemanticCanonicalReachableInstance *)cm_alloc_zeroed(1u,
        allocation_size);
    identities = (CmHirCanonicalInstance *)cm_alloc_zeroed(instance_count,
        sizeof(*identities));
    for (index = 0u; index < instance_count; ++index) {
        cm_hir_canonical_instance_init(&identities[index]);
        if (instances[index].spec == NULL
            || cm_hir_canonical_instance_encode(hir, local_crate,
                instances[index].spec, &identities[index])
                    != CM_HIR_INSTANCE_OK
            || identities[index].body != instances[index].body) goto cleanup;
        canonical[index].identity = &identities[index];
    }
    result = cm_admit_typed_canonical_instances(admission, hir, local_crate,
        canonical, instance_count, NULL, 0u, 1);

cleanup:
    for (index = 0u; index < instance_count; ++index) {
        cm_hir_canonical_instance_destroy(&identities[index]);
    }
    cm_free(identities);
    cm_free(canonical);
    return result;
}

CmSemanticAdmissionResult cm_semantic_admit_typed_instance_closure(
    CmSemanticAdmission *admission, CmHirContext *hir,
    CmHirCrateId local_crate, const CmSemanticReachableInstance *instances,
    size_t instance_count, const CmSemanticReachableInstanceCall *calls,
    size_t call_count)
{
    CmSemanticCanonicalReachableInstance *canonical;
    CmHirCanonicalInstance *identities;
    CmAdmissionCanonicalCallInput *canonical_calls;
    CmSemanticAdmissionResult result;
    size_t allocation_size;
    size_t index;

    result = cm_admission_result(CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT);
    if (instances == NULL || instance_count == 0u
        || (call_count == 0u) != (calls == NULL)
        || !cm_size_mul(instance_count, sizeof(*canonical),
            &allocation_size)
        || !cm_size_mul(instance_count, sizeof(*identities), NULL)) {
        return result;
    }
    canonical = (CmSemanticCanonicalReachableInstance *)cm_alloc_zeroed(1u,
        allocation_size);
    identities = (CmHirCanonicalInstance *)cm_alloc_zeroed(instance_count,
        sizeof(*identities));
    canonical_calls = call_count == 0u ? NULL
        : (CmAdmissionCanonicalCallInput *)cm_alloc_zeroed(call_count,
            sizeof(*canonical_calls));
    for (index = 0u; index < instance_count; ++index) {
        cm_hir_canonical_instance_init(&identities[index]);
        if (instances[index].spec == NULL
            || cm_hir_canonical_instance_encode(hir, local_crate,
                instances[index].spec, &identities[index])
                    != CM_HIR_INSTANCE_OK
            || identities[index].body != instances[index].body) goto cleanup;
        canonical[index].identity = &identities[index];
    }
    for (index = 0u; index < call_count; ++index) {
        size_t caller_index;
        size_t callee_index;

        caller_index = instance_count;
        callee_index = instance_count;
        if (calls[index].caller == NULL || calls[index].callee == NULL) {
            goto cleanup;
        }
        for (allocation_size = 0u; allocation_size < instance_count;
             ++allocation_size) {
            CmHirCanonicalInstance temporary;
            int equal;

            cm_hir_canonical_instance_init(&temporary);
            equal = 0;
            if (cm_hir_canonical_instance_encode(hir, local_crate,
                    calls[index].caller, &temporary) != CM_HIR_INSTANCE_OK
                || cm_hir_canonical_instance_equal(&temporary,
                    &identities[allocation_size], &equal)
                    != CM_HIR_INSTANCE_OK) {
                cm_hir_canonical_instance_destroy(&temporary);
                goto cleanup;
            }
            cm_hir_canonical_instance_destroy(&temporary);
            if (equal) caller_index = allocation_size;
            cm_hir_canonical_instance_init(&temporary);
            equal = 0;
            if (cm_hir_canonical_instance_encode(hir, local_crate,
                    calls[index].callee, &temporary) != CM_HIR_INSTANCE_OK
                || cm_hir_canonical_instance_equal(&temporary,
                    &identities[allocation_size], &equal)
                    != CM_HIR_INSTANCE_OK) {
                cm_hir_canonical_instance_destroy(&temporary);
                goto cleanup;
            }
            cm_hir_canonical_instance_destroy(&temporary);
            if (equal) callee_index = allocation_size;
        }
        if (caller_index == instance_count || callee_index == instance_count) {
            goto cleanup;
        }
        canonical_calls[index].caller = &identities[caller_index];
        canonical_calls[index].expression = calls[index].expression;
        canonical_calls[index].callee = &identities[callee_index];
    }
    result = cm_admit_typed_canonical_instances(admission, hir, local_crate,
        canonical, instance_count, canonical_calls, call_count, 0);

cleanup:
    for (index = 0u; index < instance_count; ++index) {
        cm_hir_canonical_instance_destroy(&identities[index]);
    }
    cm_free(canonical_calls);
    cm_free(identities);
    cm_free(canonical);
    return result;
}

CmSemanticAdmissionResult
cm_semantic_admit_typed_canonical_instance_closure(
    CmSemanticAdmission *admission, CmHirContext *hir,
    CmHirCrateId local_crate,
    const CmSemanticCanonicalReachableInstance *instances,
    size_t instance_count,
    const CmSemanticCanonicalReachableInstanceCall *calls,
    size_t call_count)
{
    CmAdmissionCanonicalCallInput *inputs;
    CmSemanticAdmissionResult result;
    size_t bytes;
    size_t index;

    result = cm_admission_result(CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT);
    if ((call_count == 0u) != (calls == NULL)
        || !cm_size_mul(call_count, sizeof(*inputs), &bytes)) return result;
    inputs = call_count == 0u ? NULL
        : (CmAdmissionCanonicalCallInput *)cm_alloc_zeroed(1u, bytes);
    for (index = 0u; index < call_count; ++index) {
        inputs[index].caller = calls[index].caller;
        inputs[index].expression = calls[index].expression;
        inputs[index].callee = calls[index].callee;
    }
    result = cm_admit_typed_canonical_instances(admission, hir, local_crate,
        instances, instance_count, inputs, call_count, 0);
    cm_free(inputs);
    return result;
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
