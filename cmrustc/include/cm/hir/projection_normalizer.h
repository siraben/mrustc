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

/* One authenticated projection reduction in causal traversal order. */
typedef struct CmProjectionNormalizeStep {
    CmTypeckTypeId projection;
    /* Raw target selected by the authenticated proof. */
    CmTypeckTypeId target;
    /* Same target after all recursively required normalization succeeds. */
    CmTypeckTypeId normalized_target;
    CmTraitProofOrigin proof_origin;
    size_t param_env_fact_index;
    uint32_t param_env_equality_index;
    CmHirDefId impl_definition;
    CmHirDefId impl_associated_definition;
} CmProjectionNormalizeStep;

/* Caller-owned output. A non-PROVEN normalization always leaves it empty. */
typedef struct CmProjectionNormalizeTrace {
    void *state;
} CmProjectionNormalizeTrace;

void cm_projection_normalize_trace_init(CmProjectionNormalizeTrace *trace);
void cm_projection_normalize_trace_destroy(CmProjectionNormalizeTrace *trace);
void cm_projection_normalize_trace_clear(CmProjectionNormalizeTrace *trace);
size_t cm_projection_normalize_trace_count(
    const CmProjectionNormalizeTrace *trace);
const CmProjectionNormalizeStep *cm_projection_normalize_trace_step(
    const CmProjectionNormalizeTrace *trace, size_t index);

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

/*
 * Traced form of cm_projection_normalize_type. Steps are published only after
 * the complete typeck transaction commits. The legacy entry point above is
 * exactly this operation with no trace output.
 */
CmProjectionNormalizeResult cm_projection_normalize_type_traced(
    const CmTraitImplIndex *index, const CmParamEnv *environment,
    CmTypeckContext *typeck, const CmParamEnvSubstitution *substitution,
    CmHirDefId owner, CmTypeckTypeId type,
    const CmTraitGoalEvaluator *evaluator,
    CmProjectionNormalizeLimits limits,
    CmProjectionNormalizeTrace *trace);

#endif
