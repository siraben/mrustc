#include "cm/hir/admission.h"

#include "cm/alloc.h"

#include <string.h>

typedef struct CmSemanticAdmissionState {
    const CmHirContext *hir;
    CmHirCrateId local_crate;
    uint64_t storage_lifetime_id;
    uint64_t semantic_generation;
    uint64_t rewind_generation;
} CmSemanticAdmissionState;

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
    return hir->storage.lifetime_id == state->storage_lifetime_id
        && hir->semantic_generation == state->semantic_generation
        && hir->rewind_generation == state->rewind_generation
        && cm_hir_get_crate(hir, state->local_crate) != NULL;
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
    result.item_result = cm_semantic_item_check_local_trait_impls(hir,
        local_crate);
    if (result.item_result.status != CM_SEMANTIC_ITEM_OK) {
        result.owner = cm_hir_def_id_is_none(result.item_result.impl_member)
            ? result.item_result.impl_definition
            : result.item_result.impl_member;
        result.status = CM_SEMANTIC_ADMISSION_ITEM_FAILURE;
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
        result.body_result = cm_semantic_body_check_definition(&session,
            result.body);
        cm_semantic_session_destroy(&session);
        if (result.body_result.status != CM_SEMANTIC_BODY_OK) {
            result.status = CM_SEMANTIC_ADMISSION_BODY_FAILURE;
            goto rollback;
        }
    }
    hir_status = cm_hir_context_commit(hir, &mark);
    if (hir_status != CM_HIR_OK) {
        result.status = CM_SEMANTIC_ADMISSION_HIR_FAILURE;
        result.hir_status = hir_status;
        goto rollback;
    }
    mark_active = 0;
    cm_hir_crate_finalization_destroy(&finalization);
    {
        CmSemanticAdmissionState *state;
        state = (CmSemanticAdmissionState *)cm_alloc_zeroed(1u,
            sizeof(*state));
        state->hir = hir;
        state->local_crate = local_crate;
        state->storage_lifetime_id = hir->storage.lifetime_id;
        state->semantic_generation = hir->semantic_generation;
        state->rewind_generation = hir->rewind_generation;
        admission->state = state;
    }
    cm_free(journal);
    result.status = CM_SEMANTIC_ADMISSION_OK;
    result.local_bodies.status = CM_HIR_LOCAL_BODIES_OK;
    result.item_result.status = CM_SEMANTIC_ITEM_OK;
    result.body_result.status = CM_SEMANTIC_BODY_OK;
    result.body_result.solver_kind = CM_TRAIT_SOLVER_PROVEN;
    result.session_status = CM_TRAIT_SOLVER_PROVEN;
    return result;

rollback:
    cm_semantic_session_destroy(&session);
    cm_hir_crate_finalization_destroy(&finalization);
    if (mark_active) {
        hir_status = cm_admission_rollback(hir, &mark, journal, body_count);
        if (hir_status != CM_HIR_OK) {
            result.status = CM_SEMANTIC_ADMISSION_HIR_FAILURE;
            result.hir_status = hir_status;
        }
    }
    cm_free(journal);
    return result;
}

void cm_semantic_admission_destroy(CmSemanticAdmission *admission)
{
    if (admission == NULL) return;
    if (admission->state != NULL) {
        memset(admission->state, 0, sizeof(CmSemanticAdmissionState));
        cm_free(admission->state);
    }
    admission->state = NULL;
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
