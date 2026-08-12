#include "cm/hir/semantic.h"

#include "cm/alloc.h"

#include <stdint.h>
#include <string.h>

#define CM_SEMANTIC_DEFAULT_GOAL_DEPTH ((size_t)64u)
#define CM_SEMANTIC_DEFAULT_CANONICAL_NODES ((size_t)1024u)
#define CM_SEMANTIC_DEFAULT_TABLE_ENTRIES ((size_t)256u)

typedef struct CmSemanticSessionState {
    const CmHirContext *hir;
    CmHirCrateId local_crate;
    CmHirDefId exact_owner;
    CmHirDefId enclosing_owner;
    CmTraitImplUniverse universe;
    CmTraitImplIndex index;
    CmParamEnv environment;
    CmTraitGoalTable table;
    CmTypeckContext typeck;
} CmSemanticSessionState;

static CmSemanticSessionState *cm_semantic_state(CmSemanticSession *session)
{
    return session == NULL ? NULL
        : (CmSemanticSessionState *)session->state;
}

static const CmSemanticSessionState *cm_semantic_state_const(
    const CmSemanticSession *session)
{
    return session == NULL ? NULL
        : (const CmSemanticSessionState *)session->state;
}

static CmTraitSelectionResult cm_semantic_result(
    CmTraitSolverResultKind kind)
{
    CmTraitSelectionResult result;

    memset(&result, 0, sizeof(result));
    result.kind = kind;
    result.param_env_fact_index = CM_TRAIT_PROOF_FACT_NONE;
    result.impl_definition = cm_hir_def_id_none();
    result.impl_item = CM_HIR_ITEM_NONE;
    result.impl_associated_definition = cm_hir_def_id_none();
    result.typeck_status = CM_TYPECK_OK;
    return result;
}

static int cm_semantic_limits_valid(CmTraitGoalTableLimits limits)
{
    return limits.max_goal_depth != 0u
        && limits.max_canonical_nodes != 0u
        && limits.max_table_entries != 0u
        && limits.max_goal_depth < (size_t)UINT32_MAX
        && limits.max_canonical_nodes < (size_t)UINT32_MAX
        && limits.max_table_entries < (size_t)UINT32_MAX;
}

static void cm_semantic_state_destroy(CmSemanticSessionState *state)
{
    if (state == NULL) return;
    cm_trait_goal_table_destroy(&state->table);
    cm_typeck_context_destroy(&state->typeck);
    cm_param_env_destroy(&state->environment);
    cm_trait_impl_index_destroy(&state->index);
    memset(state, 0, sizeof(*state));
    cm_free(state);
}

static CmTraitSolverResultKind cm_semantic_param_status(
    CmParamEnvStatus status)
{
    switch (status) {
    case CM_PARAM_ENV_READY: return CM_TRAIT_SOLVER_PROVEN;
    case CM_PARAM_ENV_UNSUPPORTED: return CM_TRAIT_SOLVER_UNSUPPORTED;
    case CM_PARAM_ENV_OVERFLOW: return CM_TRAIT_SOLVER_OVERFLOW;
    case CM_PARAM_ENV_TYPECK_FAILURE:
        return CM_TRAIT_SOLVER_TYPECK_FAILURE;
    case CM_PARAM_ENV_INVALID:
    case CM_PARAM_ENV_STALE:
        return CM_TRAIT_SOLVER_INVALID;
    }
    return CM_TRAIT_SOLVER_INVALID;
}

static int cm_semantic_state_is_current(
    const CmSemanticSessionState *state)
{
    return state != NULL && state->hir != NULL
        && cm_trait_impl_index_is_current(&state->index)
        && cm_trait_impl_index_hir(&state->index) == state->hir
        && cm_trait_impl_index_local_crate(&state->index)
            == state->local_crate
        && cm_trait_impl_index_universe(&state->index) == state->universe
        && cm_param_env_is_current(&state->environment)
        && cm_param_env_hir(&state->environment) == state->hir
        && cm_hir_def_id_equal(cm_param_env_exact_owner(
                &state->environment), state->exact_owner)
        && cm_hir_def_id_equal(cm_param_env_enclosing_owner(
                &state->environment), state->enclosing_owner)
        && cm_trait_goal_table_is_current(&state->table)
        && cm_typeck_hir_context(&state->typeck) == state->hir;
}

void cm_semantic_session_options_init(CmSemanticSessionOptions *options)
{
    if (options == NULL) return;
    memset(options, 0, sizeof(*options));
    options->local_crate = CM_HIR_CRATE_NONE;
    options->exact_owner = cm_hir_def_id_none();
    options->universe = CM_TRAIT_IMPL_UNIVERSE_OPEN;
    options->goal_limits.max_goal_depth =
        CM_SEMANTIC_DEFAULT_GOAL_DEPTH;
    options->goal_limits.max_canonical_nodes =
        CM_SEMANTIC_DEFAULT_CANONICAL_NODES;
    options->goal_limits.max_table_entries =
        CM_SEMANTIC_DEFAULT_TABLE_ENTRIES;
}

CmTraitSolverResultKind cm_semantic_session_init(
    CmSemanticSession *session, const CmHirContext *hir,
    const CmSemanticSessionOptions *options)
{
    CmSemanticSessionState *state;
    CmTraitSolverResultKind result;
    CmParamEnvStatus param_status;

    if (session == NULL || session->state != NULL || hir == NULL
        || options == NULL) return CM_TRAIT_SOLVER_INVALID;
    state = (CmSemanticSessionState *)cm_alloc_zeroed(1u,
        sizeof(CmSemanticSessionState));
    state->hir = hir;
    state->local_crate = options->local_crate;
    state->exact_owner = options->exact_owner;
    state->universe = options->universe;

    if (options->universe == CM_TRAIT_IMPL_UNIVERSE_OPEN
        && options->finalization == NULL) {
        result = cm_trait_impl_index_init(&state->index, hir,
            options->local_crate, options->universe);
    } else if (options->universe
            == CM_TRAIT_IMPL_UNIVERSE_SINGLE_LOCAL_CRATE_COMPLETE
        && options->finalization != NULL
        && cm_hir_crate_finalization_hir(options->finalization) == hir
        && cm_hir_crate_finalization_crate(options->finalization)
            == options->local_crate) {
        result = cm_trait_impl_index_init_complete(&state->index,
            options->finalization);
    } else {
        result = CM_TRAIT_SOLVER_INVALID;
    }
    if (result != CM_TRAIT_SOLVER_PROVEN) {
        cm_semantic_state_destroy(state);
        return result;
    }
    param_status = cm_param_env_init(&state->environment, hir,
        options->exact_owner);
    if (param_status != CM_PARAM_ENV_READY) {
        result = cm_semantic_param_status(param_status);
        cm_semantic_state_destroy(state);
        return result;
    }
    state->enclosing_owner = cm_param_env_enclosing_owner(
        &state->environment);
    cm_typeck_context_init(&state->typeck, hir);
    cm_typeck_context_track_hir_semantic_generation(&state->typeck);
    if (cm_typeck_hir_context(&state->typeck) != hir
        || !cm_semantic_limits_valid(options->goal_limits)) {
        cm_semantic_state_destroy(state);
        return CM_TRAIT_SOLVER_INVALID;
    }
    result = cm_trait_goal_table_init(&state->table, &state->index,
        &state->environment, options->goal_limits);
    if (result != CM_TRAIT_SOLVER_PROVEN) {
        cm_semantic_state_destroy(state);
        return result;
    }
    if (!cm_semantic_state_is_current(state)) {
        cm_semantic_state_destroy(state);
        return CM_TRAIT_SOLVER_INVALID;
    }
    session->state = state;
    return CM_TRAIT_SOLVER_PROVEN;
}

void cm_semantic_session_destroy(CmSemanticSession *session)
{
    CmSemanticSessionState *state;

    state = cm_semantic_state(session);
    if (state == NULL) return;
    session->state = NULL;
    cm_semantic_state_destroy(state);
}

int cm_semantic_session_is_current(const CmSemanticSession *session)
{
    return cm_semantic_state_is_current(cm_semantic_state_const(session));
}

const CmHirContext *cm_semantic_session_hir(
    const CmSemanticSession *session)
{
    const CmSemanticSessionState *state;

    state = cm_semantic_state_const(session);
    return !cm_semantic_state_is_current(state) ? NULL : state->hir;
}

CmHirCrateId cm_semantic_session_local_crate(
    const CmSemanticSession *session)
{
    const CmSemanticSessionState *state;

    state = cm_semantic_state_const(session);
    return !cm_semantic_state_is_current(state)
        ? CM_HIR_CRATE_NONE : state->local_crate;
}

CmHirDefId cm_semantic_session_exact_owner(
    const CmSemanticSession *session)
{
    const CmSemanticSessionState *state;

    state = cm_semantic_state_const(session);
    return !cm_semantic_state_is_current(state)
        ? cm_hir_def_id_none() : state->exact_owner;
}

CmHirDefId cm_semantic_session_enclosing_owner(
    const CmSemanticSession *session)
{
    const CmSemanticSessionState *state;

    state = cm_semantic_state_const(session);
    return !cm_semantic_state_is_current(state)
        ? cm_hir_def_id_none() : state->enclosing_owner;
}

CmTraitImplUniverse cm_semantic_session_universe(
    const CmSemanticSession *session)
{
    const CmSemanticSessionState *state;

    state = cm_semantic_state_const(session);
    return !cm_semantic_state_is_current(state)
        ? CM_TRAIT_IMPL_UNIVERSE_OPEN : state->universe;
}

CmTypeckContext *cm_semantic_session_typeck(CmSemanticSession *session)
{
    CmSemanticSessionState *state;

    state = cm_semantic_state(session);
    return !cm_semantic_state_is_current(state) ? NULL : &state->typeck;
}

CmTraitSelectionResult cm_semantic_session_solve_goal(
    CmSemanticSession *session, const CmTypeckContext *term_owner,
    const CmParamEnvSubstitution *substitution, const CmTraitGoal *goal)
{
    CmSemanticSessionState *state;

    state = cm_semantic_state(session);
    if (!cm_semantic_state_is_current(state)
        || term_owner != &state->typeck || substitution == NULL
        || goal == NULL
        || (goal->kind != CM_TRAIT_GOAL_IMPLEMENTED
            && goal->kind != CM_TRAIT_GOAL_PROJECTION_EQUALITY)) {
        return cm_semantic_result(CM_TRAIT_SOLVER_INVALID);
    }
    return cm_trait_goal_table_solve(&state->table, &state->typeck,
        substitution, goal);
}

CmTraitSelectionResult cm_semantic_session_solve_implemented(
    CmSemanticSession *session, const CmTypeckContext *term_owner,
    const CmParamEnvSubstitution *substitution, const CmTraitGoal *goal)
{
    if (goal == NULL || goal->kind != CM_TRAIT_GOAL_IMPLEMENTED) {
        return cm_semantic_result(CM_TRAIT_SOLVER_INVALID);
    }
    return cm_semantic_session_solve_goal(session, term_owner,
        substitution, goal);
}
