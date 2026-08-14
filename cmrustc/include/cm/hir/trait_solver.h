#ifndef CMRUSTC_CM_HIR_TRAIT_SOLVER_H
#define CMRUSTC_CM_HIR_TRAIT_SOLVER_H

#include "cm/hir/finalization.h"
#include "cm/hir/param_env.h"

typedef enum CmTraitSolverResultKind {
    CM_TRAIT_SOLVER_PROVEN = 0,
    /* One exact authenticated local negative impl refutes the goal. */
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
    /* Opt-in only through an authenticated current local-crate finalization. */
    CM_TRAIT_IMPL_UNIVERSE_SINGLE_LOCAL_CRATE_COMPLETE
} CmTraitImplUniverse;

/* Authenticated source of one committed proof. */
typedef enum CmTraitProofOrigin {
    CM_TRAIT_PROOF_NONE = 0,
    CM_TRAIT_PROOF_PARAM_ENV,
    CM_TRAIT_PROOF_IMPL
} CmTraitProofOrigin;

#define CM_TRAIT_PROOF_FACT_NONE ((size_t)-1)
#define CM_TRAIT_PROOF_EQUALITY_NONE UINT32_MAX

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
 * Immutable snapshot of one HIR context's trait impls. The legacy initializer
 * accepts only OPEN; the finalization initializer opts into COMPLETE.  This
 * increment deliberately preserves conservative absence semantics in both
 * universes: no matching ordinary impl is deferred metadata, not NO_SOLUTION.
 */
typedef struct CmTraitImplIndex {
    void *state;
} CmTraitImplIndex;

/*
 * Caller-owned evidence from the unique selected impl replay. Scratch generic
 * arguments remain tied to the exact typeck lifetime and state revision that
 * committed them; goal-table result records never own or cache this object.
 */
typedef struct CmTraitImplSelectionWitness {
    void *state;
} CmTraitImplSelectionWitness;

typedef struct CmTraitSelectionResult {
    CmTraitSolverResultKind kind;
    /* Non-PROVEN results always report NONE and no fact/impl identity. */
    CmTraitProofOrigin proof_origin;
    /* Set only when proof_origin is PARAM_ENV. */
    size_t param_env_fact_index;
    /* Set only for a PARAM_ENV projection-equality proof; implemented-trait
     * environment proofs retain CM_TRAIT_PROOF_EQUALITY_NONE. */
    uint32_t param_env_equality_index;
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
/* Opt in to a closed local-crate universe using current authenticated proof. */
CmTraitSolverResultKind cm_trait_impl_index_init_complete(
    CmTraitImplIndex *index,
    const CmHirCrateFinalization *finalization);
void cm_trait_impl_index_destroy(CmTraitImplIndex *index);

void cm_trait_impl_selection_witness_init(
    CmTraitImplSelectionWitness *witness);
void cm_trait_impl_selection_witness_destroy(
    CmTraitImplSelectionWitness *witness);
void cm_trait_impl_selection_witness_clear(
    CmTraitImplSelectionWitness *witness);
int cm_trait_impl_selection_witness_is_current(
    const CmTraitImplSelectionWitness *witness,
    const CmTypeckContext *typeck);
/*
 * Authenticated declaration-ordered impl substitution view. The returned
 * argument slice is borrowed from `witness` and remains valid only until that
 * witness is cleared, destroyed, or passed to another witness-producing API.
 * Consumers must copy any durable payload before mutating the witness.
 */
int cm_trait_impl_selection_witness_instantiation(
    const CmTraitImplSelectionWitness *witness,
    const CmTypeckContext *typeck, CmTypeckInstantiation *out_view);

int cm_trait_impl_index_is_current(const CmTraitImplIndex *index);
const CmHirContext *cm_trait_impl_index_hir(const CmTraitImplIndex *index);
CmTraitImplUniverse cm_trait_impl_index_universe(
    const CmTraitImplIndex *index);
CmHirCrateId cm_trait_impl_index_local_crate(
    const CmTraitImplIndex *index);
size_t cm_trait_impl_index_entry_count(const CmTraitImplIndex *index);
const CmTraitImplIndexEntry *cm_trait_impl_index_entry(
    const CmTraitImplIndex *index, size_t entry_index);

/* Public ordinary-goal authentication. Auto-trait selection has one narrower
 * internal path used only to discover exact local negative evidence. */
CmTraitSolverResultKind cm_trait_solver_validate_implemented_goal(
    const CmHirContext *hir, CmTypeckContext *typeck,
    CmTypeckTypeId self_type, const CmTypeckNamedType *trait_type);

/*
 * Select one positive ordinary impl or refute the goal with one exact local,
 * nongeneric, predicate-free negative impl. Type-only positive impl generics
 * are instantiated with fresh inference variables; every broader negative,
 * generic, predicate, auto-positive, or overlapping shape is an explicit
 * blocker or ambiguity. Every candidate probe is rolled back. Bindings from
 * the unique positive winner are recreated and committed; every non-PROVEN
 * result leaves the typeck session unchanged and publishes no provider.
 */
CmTraitSelectionResult cm_trait_solver_select(
    const CmTraitImplIndex *index, CmTypeckContext *typeck,
    CmTypeckTypeId self_type, const CmTypeckNamedType *trait_type);
CmTraitSelectionResult cm_trait_solver_select_with_witness(
    const CmTraitImplIndex *index, CmTypeckContext *typeck,
    CmTypeckTypeId self_type, const CmTypeckNamedType *trait_type,
    CmTraitImplSelectionWitness *witness);

/*
 * Search authenticated environment assumptions first, then the immutable impl
 * index. Environment candidate probes and the unique replay are transactional.
 */
CmTraitSelectionResult cm_trait_solver_solve_implemented(
    const CmTraitImplIndex *index, const CmParamEnv *environment,
    CmTypeckContext *typeck, const CmParamEnvSubstitution *substitution,
    const CmImplementedTraitGoal *goal);
CmTraitSelectionResult cm_trait_solver_solve_implemented_with_witness(
    const CmTraitImplIndex *index, const CmParamEnv *environment,
    CmTypeckContext *typeck, const CmParamEnvSubstitution *substitution,
    const CmImplementedTraitGoal *goal,
    CmTraitImplSelectionWitness *witness);

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
