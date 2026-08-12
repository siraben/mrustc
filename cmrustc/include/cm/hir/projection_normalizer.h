#ifndef CMRUSTC_CM_HIR_PROJECTION_NORMALIZER_H
#define CMRUSTC_CM_HIR_PROJECTION_NORMALIZER_H

#include "cm/hir/trait_solver.h"

typedef struct CmProjectionNormalizeLimits {
    size_t max_nodes;
    size_t max_projection_steps;
} CmProjectionNormalizeLimits;

typedef enum CmProjectionNormalizeCause {
    CM_PROJECTION_NORMALIZE_CAUSE_NONE = 0,
    CM_PROJECTION_NORMALIZE_CAUSE_CYCLE,
    CM_PROJECTION_NORMALIZE_CAUSE_NODE_LIMIT,
    CM_PROJECTION_NORMALIZE_CAUSE_PROJECTION_LIMIT
} CmProjectionNormalizeCause;

typedef struct CmProjectionNormalizeResult {
    CmTraitSolverResultKind kind;
    /* Refines normalizer-originated AMBIGUOUS/OVERFLOW outcomes. */
    CmProjectionNormalizeCause cause;
    /* Session-owned and set only for PROVEN results. */
    CmTypeckTypeId type;
    CmTypeckStatus typeck_status;
    size_t visited_node_count;
    size_t projection_step_count;
} CmProjectionNormalizeResult;

/*
 * Recursively normalize one typeck term using bounds-first projection target
 * selection. The complete operation is transactional: every non-proof leaves
 * typeck unchanged and returns type NONE.
 */
CmProjectionNormalizeResult cm_projection_normalize_type(
    const CmTraitImplIndex *index, const CmParamEnv *environment,
    CmTypeckContext *typeck, const CmParamEnvSubstitution *substitution,
    CmHirDefId owner, CmTypeckTypeId type,
    const CmTraitGoalEvaluator *evaluator,
    CmProjectionNormalizeLimits limits);

#endif
