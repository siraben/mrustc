#ifndef CMRUSTC_CM_HIR_TRAIT_SOLVER_H
#define CMRUSTC_CM_HIR_TRAIT_SOLVER_H

#include "cm/hir/param_env.h"

typedef enum CmTraitSolverResultKind {
    CM_TRAIT_SOLVER_PROVEN = 0,
    /* Reserved for a future negative-obligation API; select never emits it. */
    CM_TRAIT_SOLVER_NEGATIVE,
    CM_TRAIT_SOLVER_NO_SOLUTION,
    CM_TRAIT_SOLVER_AMBIGUOUS,
    CM_TRAIT_SOLVER_DEFERRED_INFERENCE,
    CM_TRAIT_SOLVER_DEFERRED_METADATA,
    CM_TRAIT_SOLVER_UNSUPPORTED,
    CM_TRAIT_SOLVER_OVERFLOW,
    CM_TRAIT_SOLVER_INVALID,
    CM_TRAIT_SOLVER_TYPECK_FAILURE
} CmTraitSolverResultKind;

typedef enum CmTraitImplUniverse {
    CM_TRAIT_IMPL_UNIVERSE_OPEN = 0,
    /* Reserved until HIR exposes an authenticated finalization capability. */
    CM_TRAIT_IMPL_UNIVERSE_SINGLE_LOCAL_CRATE_COMPLETE
} CmTraitImplUniverse;

typedef enum CmTraitImplHeadKind {
    CM_TRAIT_IMPL_HEAD_WILDCARD = 0,
    CM_TRAIT_IMPL_HEAD_NEVER,
    CM_TRAIT_IMPL_HEAD_UNIT,
    CM_TRAIT_IMPL_HEAD_BOOL,
    CM_TRAIT_IMPL_HEAD_CHAR,
    CM_TRAIT_IMPL_HEAD_STR,
    CM_TRAIT_IMPL_HEAD_INTEGER,
    CM_TRAIT_IMPL_HEAD_FLOAT,
    CM_TRAIT_IMPL_HEAD_REFERENCE,
    CM_TRAIT_IMPL_HEAD_RAW_POINTER,
    CM_TRAIT_IMPL_HEAD_TUPLE,
    CM_TRAIT_IMPL_HEAD_ARRAY,
    CM_TRAIT_IMPL_HEAD_SLICE,
    CM_TRAIT_IMPL_HEAD_FN_POINTER,
    CM_TRAIT_IMPL_HEAD_NAMED
} CmTraitImplHeadKind;

typedef enum CmTraitImplUnsupportedFlag {
    CM_TRAIT_IMPL_UNSUPPORTED_NONE = 0u,
    CM_TRAIT_IMPL_UNSUPPORTED_GENERIC = 1u << 0,
    CM_TRAIT_IMPL_UNSUPPORTED_PREDICATE = 1u << 1,
    CM_TRAIT_IMPL_UNSUPPORTED_OUTLIVES = 1u << 2,
    CM_TRAIT_IMPL_UNSUPPORTED_AUTO_TRAIT = 1u << 3,
    CM_TRAIT_IMPL_UNSUPPORTED_NEGATIVE = 1u << 4,
    CM_TRAIT_IMPL_UNSUPPORTED_PROJECTION = 1u << 5,
    CM_TRAIT_IMPL_UNSUPPORTED_NON_MONOMORPHIC = 1u << 6,
    CM_TRAIT_IMPL_UNSUPPORTED_TYPE = 1u << 7
} CmTraitImplUnsupportedFlag;

typedef struct CmTraitImplIndexEntry {
    CmHirDefId trait_definition;
    CmTraitImplHeadKind self_head;
    CmHirDefId self_head_definition;
    CmHirItemId item;
    CmHirDefId impl_definition;
    unsigned int unsupported_flags;
} CmTraitImplIndexEntry;

/*
 * Immutable snapshot of one HIR context's trait impls. Only OPEN is currently
 * accepted because mutable HIR has no authenticated finalization capability.
 * Consequently absence is always deferred metadata, never NO_SOLUTION.
 */
typedef struct CmTraitImplIndex {
    void *state;
} CmTraitImplIndex;

typedef struct CmTraitSelectionResult {
    CmTraitSolverResultKind kind;
    CmHirDefId impl_definition;
    CmHirItemId impl_item;
    /* Set only by a committed projection-equality proof. */
    CmHirDefId impl_associated_definition;
    size_t supported_match_count;
    size_t negative_match_count;
    size_t blocking_match_count;
    CmTypeckStatus typeck_status;
} CmTraitSelectionResult;

/* Canonical goal shape; new goal kinds require an explicit tagged extension. */
typedef struct CmImplementedTraitGoal {
    CmHirDefId owner;
    CmTypeckTypeId self_type;
    CmTypeckNamedType trait_type;
} CmImplementedTraitGoal;

typedef struct CmProjectionEqualityGoal {
    CmHirDefId owner;
    CmTypeckTypeId projection_type;
    CmTypeckTypeId expected_type;
} CmProjectionEqualityGoal;

/*
 * Dependency-inverted recursive goal evaluator. The trait solver never owns
 * or names the canonical goal table; callers that provide this callback must
 * preserve the same immutable index and parameter-environment context.
 */
typedef CmTraitSelectionResult (*CmTraitGoalEvaluateFn)(void *context,
    CmTypeckContext *typeck, const CmImplementedTraitGoal *goal);
typedef CmTraitSelectionResult (*CmProjectionGoalEvaluateFn)(void *context,
    CmTypeckContext *typeck, const CmProjectionEqualityGoal *goal);

typedef struct CmTraitGoalEvaluator {
    void *context;
    CmTraitGoalEvaluateFn evaluate;
    CmProjectionGoalEvaluateFn evaluate_projection;
} CmTraitGoalEvaluator;

CmTraitSolverResultKind cm_trait_impl_index_init(CmTraitImplIndex *index,
    const CmHirContext *hir, CmHirCrateId local_crate,
    CmTraitImplUniverse universe);
void cm_trait_impl_index_destroy(CmTraitImplIndex *index);

int cm_trait_impl_index_is_current(const CmTraitImplIndex *index);
const CmHirContext *cm_trait_impl_index_hir(const CmTraitImplIndex *index);
CmTraitImplUniverse cm_trait_impl_index_universe(
    const CmTraitImplIndex *index);
CmHirCrateId cm_trait_impl_index_local_crate(
    const CmTraitImplIndex *index);
size_t cm_trait_impl_index_entry_count(const CmTraitImplIndex *index);
const CmTraitImplIndexEntry *cm_trait_impl_index_entry(
    const CmTraitImplIndex *index, size_t entry_index);

/* Shared fail-closed goal authentication used by both environment and index. */
CmTraitSolverResultKind cm_trait_solver_validate_implemented_goal(
    const CmHirContext *hir, CmTypeckContext *typeck,
    CmTypeckTypeId self_type, const CmTypeckNamedType *trait_type);

/*
 * Select one positive ordinary impl. Type-only impl generics are instantiated
 * with fresh inference variables; every other generic or predicate shape is
 * an explicit blocker. Negative evidence remains unsupported and is
 * separately counted. Every candidate probe is rolled back. Bindings from the
 * unique winner are recreated and committed; every non-PROVEN result leaves
 * the typeck session unchanged.
 */
CmTraitSelectionResult cm_trait_solver_select(
    const CmTraitImplIndex *index, CmTypeckContext *typeck,
    CmTypeckTypeId self_type, const CmTypeckNamedType *trait_type);

/*
 * Search authenticated environment assumptions first, then the immutable impl
 * index. Environment candidate probes and the unique replay are transactional.
 */
CmTraitSelectionResult cm_trait_solver_solve_implemented(
    const CmTraitImplIndex *index, const CmParamEnv *environment,
    CmTypeckContext *typeck, const CmParamEnvSubstitution *substitution,
    const CmImplementedTraitGoal *goal);

/* Canonical tables use this entry to discharge supported impl predicates. */
CmTraitSelectionResult cm_trait_solver_solve_implemented_with_evaluator(
    const CmTraitImplIndex *index, const CmParamEnv *environment,
    CmTypeckContext *typeck, const CmParamEnvSubstitution *substitution,
    const CmImplementedTraitGoal *goal,
    const CmTraitGoalEvaluator *evaluator);

CmTraitSelectionResult cm_trait_solver_solve_projection_equality(
    const CmTraitImplIndex *index, const CmParamEnv *environment,
    CmTypeckContext *typeck, const CmParamEnvSubstitution *substitution,
    const CmProjectionEqualityGoal *goal,
    const CmTraitGoalEvaluator *evaluator);

const char *cm_trait_solver_result_name(CmTraitSolverResultKind result);

#endif
