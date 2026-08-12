#ifndef CMRUSTC_HIR_SEMANTIC_RESULTS_INTERNAL_H
#define CMRUSTC_HIR_SEMANTIC_RESULTS_INTERNAL_H

#include "cm/hir/semantic_results.h"
#include "instance_internal.h"
#include "semantic_body_internal.h"

typedef struct CmSemanticResultsBodyStage {
    void *state;
} CmSemanticResultsBodyStage;

typedef struct CmSemanticCanonicalCallInput {
    CmHirExprId expression;
    const CmHirCanonicalInstance *callee;
} CmSemanticCanonicalCallInput;

CmSemanticResultsStatus cm_semantic_results_begin(
    const CmHirContext *hir, CmHirCrateId local_crate,
    CmSemanticResults **out_results);
void cm_semantic_results_body_stage_init(CmSemanticResultsBodyStage *stage);
CmSemanticBodyWritebackStatus cm_semantic_results_stage_checked_body(
    void *context, CmSemanticSession *session, CmHirBodyId body,
    const CmSemanticCheckedBodyFacts *facts);
CmSemanticResultsStatus cm_semantic_results_commit_checked_body(
    CmSemanticResults *results, CmSemanticSession *session,
    const CmSemanticBodyResult *check, CmSemanticResultsBodyStage *stage);
CmSemanticResultsStatus cm_semantic_results_commit_checked_instance(
    CmSemanticResults *results, CmSemanticSession *session,
    const CmHirCanonicalInstance *instance,
    const CmSemanticBodyResult *check, CmSemanticResultsBodyStage *stage,
    const CmSemanticCanonicalCallInput *calls, size_t call_count);
void cm_semantic_results_body_stage_destroy(
    CmSemanticResultsBodyStage *stage);
CmSemanticResultsStatus cm_semantic_results_seal(CmSemanticResults *results);
CmSemanticResultsStatus cm_semantic_results_seal_reachable(
    CmSemanticResults *results, const CmHirBodyId *bodies,
    size_t body_count);
CmSemanticResultsStatus cm_semantic_results_seal_leaf_instances(
    CmSemanticResults *results, size_t instance_count);
CmSemanticResultsStatus cm_semantic_results_seal_instance_closure(
    CmSemanticResults *results, size_t instance_count);
void cm_semantic_results_destroy(CmSemanticResults *results);

#endif
