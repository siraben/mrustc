#ifndef CMRUSTC_SRC_HIR_TRAIT_SOLVER_INTERNAL_H
#define CMRUSTC_SRC_HIR_TRAIT_SOLVER_INTERNAL_H

#include "cm/hir/trait_solver.h"

typedef struct CmProjectionTargetGoal {
    CmHirDefId owner;
    CmTypeckTypeId projection_type;
    /* NONE materializes a raw target; otherwise preserve equality behavior. */
    CmTypeckTypeId expected_type;
} CmProjectionTargetGoal;

typedef struct CmProjectionTargetResult {
    CmTraitSelectionResult selection;
    /* Set only when selection.kind is PROVEN; otherwise always NONE. */
    CmTypeckTypeId target;
} CmProjectionTargetResult;

/*
 * Shared bounds-first projection policy.  The selected target and all proof
 * bindings are committed together; every non-proof leaves typeck unchanged.
 */
CmProjectionTargetResult cm_trait_solver_select_projection_target(
    const CmTraitImplIndex *index, const CmParamEnv *environment,
    CmTypeckContext *typeck, const CmParamEnvSubstitution *substitution,
    const CmProjectionTargetGoal *goal,
    const CmTraitGoalEvaluator *evaluator);

/* Shared authentication for arbitrary typeck semantic passes. */
CmTraitSolverResultKind cm_trait_solver_validate_session(
    const CmTraitImplIndex *index, const CmParamEnv *environment,
    CmTypeckContext *typeck, const CmParamEnvSubstitution *substitution,
    CmHirDefId owner);

CmTraitSelectionResult
cm_trait_solver_solve_implemented_with_evaluator_and_witness(
    const CmTraitImplIndex *index, const CmParamEnv *environment,
    CmTypeckContext *typeck, const CmParamEnvSubstitution *substitution,
    const CmImplementedTraitGoal *goal,
    const CmTraitGoalEvaluator *evaluator,
    CmTraitImplSelectionWitness *witness);

#endif
