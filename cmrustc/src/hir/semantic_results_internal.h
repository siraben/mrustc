#ifndef CMRUSTC_HIR_SEMANTIC_RESULTS_INTERNAL_H
#define CMRUSTC_HIR_SEMANTIC_RESULTS_INTERNAL_H

#include "cm/hir/semantic_results.h"
#include "semantic_body_internal.h"

typedef struct CmSemanticResultsBodyStage {
    void *state;
} CmSemanticResultsBodyStage;

CmSemanticResultsStatus cm_semantic_results_begin(
    const CmHirContext *hir, CmHirCrateId local_crate,
    CmSemanticResults **out_results);
void cm_semantic_results_body_stage_init(CmSemanticResultsBodyStage *stage);
CmSemanticBodyWritebackStatus cm_semantic_results_stage_checked_body(
    void *context, CmSemanticSession *session, CmHirBodyId body,
    const CmTypeckTypeId *expression_terms, size_t expression_term_count);
CmSemanticResultsStatus cm_semantic_results_commit_checked_body(
    CmSemanticResults *results, CmSemanticSession *session,
    const CmSemanticBodyResult *check, CmSemanticResultsBodyStage *stage);
void cm_semantic_results_body_stage_destroy(
    CmSemanticResultsBodyStage *stage);
CmSemanticResultsStatus cm_semantic_results_seal(CmSemanticResults *results);
void cm_semantic_results_destroy(CmSemanticResults *results);

#endif
