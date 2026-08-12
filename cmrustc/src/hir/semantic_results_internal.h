#ifndef CMRUSTC_HIR_SEMANTIC_RESULTS_INTERNAL_H
#define CMRUSTC_HIR_SEMANTIC_RESULTS_INTERNAL_H

#include "cm/hir/semantic_results.h"
#include "cm/hir/semantic_body.h"

CmSemanticResultsStatus cm_semantic_results_begin(
    const CmHirContext *hir, CmHirCrateId local_crate,
    CmSemanticResults **out_results);
CmSemanticResultsStatus cm_semantic_results_add_checked_body(
    CmSemanticResults *results, CmSemanticSession *session,
    const CmSemanticBodyResult *check);
CmSemanticResultsStatus cm_semantic_results_seal(CmSemanticResults *results);
void cm_semantic_results_destroy(CmSemanticResults *results);

#endif
