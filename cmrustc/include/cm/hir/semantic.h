#ifndef CMRUSTC_CM_HIR_SEMANTIC_H
#define CMRUSTC_CM_HIR_SEMANTIC_H

#include "cm/hir/goal_table.h"
#include "cm/hir/projection_normalizer.h"

/*
 * One semantic-pass session over one observed HIR generation.  The session
 * does not seal HIR: every append, rewind, destruction, or reinitialization
 * makes the derived index, environment, and table stale.
 */
typedef struct CmSemanticSession {
    void *state;
} CmSemanticSession;

typedef struct CmSemanticSessionOptions {
    CmHirCrateId local_crate;
    CmHirDefId exact_owner;
    CmTraitImplUniverse universe;
    const CmHirCrateFinalization *finalization;
    CmTraitGoalTableLimits goal_limits;
} CmSemanticSessionOptions;

/* Fill deterministic OPEN-universe defaults; crate and owner remain NONE. */
void cm_semantic_session_options_init(CmSemanticSessionOptions *options);

/*
 * `session` must point to zero-initialized storage.  Construction is atomic:
 * on every failure session->state remains NULL and destroy is still valid.
 * Only explicitly bounded, nonzero table limits are accepted by this
 * coordinating layer.
 */
CmTraitSolverResultKind cm_semantic_session_init(
    CmSemanticSession *session, const CmHirContext *hir,
    const CmSemanticSessionOptions *options);
void cm_semantic_session_destroy(CmSemanticSession *session);

int cm_semantic_session_is_current(const CmSemanticSession *session);
const CmHirContext *cm_semantic_session_hir(
    const CmSemanticSession *session);
CmHirCrateId cm_semantic_session_local_crate(
    const CmSemanticSession *session);
CmHirDefId cm_semantic_session_exact_owner(
    const CmSemanticSession *session);
CmHirDefId cm_semantic_session_enclosing_owner(
    const CmSemanticSession *session);
CmTraitImplUniverse cm_semantic_session_universe(
    const CmSemanticSession *session);

/*
 * Session-owned scratch storage used to construct substitutions and goals.
 * The pointer is a capability and becomes invalid when the session is stale
 * or destroyed; callers do not destroy it separately.
 */
CmTypeckContext *cm_semantic_session_typeck(CmSemanticSession *session);

/*
 * Dispatch an authenticated implemented-trait or projection-equality goal to
 * the session's canonical table.  term_owner must be the exact pointer
 * returned by cm_semantic_session_typeck, authenticating all scratch IDs in
 * the substitution and goal before delegation to the table.
 */
CmTraitSelectionResult cm_semantic_session_solve_goal(
    CmSemanticSession *session, const CmTypeckContext *term_owner,
    const CmParamEnvSubstitution *substitution, const CmTraitGoal *goal);

/* Implemented-goal root solve retaining the selected impl substitutions. */
CmTraitSelectionResult cm_semantic_session_solve_goal_with_impl_witness(
    CmSemanticSession *session, const CmTypeckContext *term_owner,
    const CmParamEnvSubstitution *substitution, const CmTraitGoal *goal,
    CmTraitImplSelectionWitness *witness);

/* Compatibility entry point restricted to implemented-trait goals. */
CmTraitSelectionResult cm_semantic_session_solve_implemented(
    CmSemanticSession *session, const CmTypeckContext *term_owner,
    const CmParamEnvSubstitution *substitution, const CmTraitGoal *goal);

/*
 * Normalize one session-owned term using the session's authenticated index,
 * environment, and canonical recursive goal table. Every non-proof is atomic.
 */
CmProjectionNormalizeResult cm_semantic_session_normalize_type(
    CmSemanticSession *session, const CmTypeckContext *term_owner,
    const CmParamEnvSubstitution *substitution, CmTypeckTypeId type,
    CmProjectionNormalizeLimits limits);

/*
 * Traced form of cm_semantic_session_normalize_type. The trace is cleared
 * before session authentication and receives steps only after the complete
 * normalization transaction commits.
 */
CmProjectionNormalizeResult cm_semantic_session_normalize_type_traced(
    CmSemanticSession *session, const CmTypeckContext *term_owner,
    const CmParamEnvSubstitution *substitution, CmTypeckTypeId type,
    CmProjectionNormalizeLimits limits,
    CmProjectionNormalizeTrace *trace);

#endif
