#ifndef CMRUSTC_HIR_SEMANTIC_RESULTS_INTERNAL_H
#define CMRUSTC_HIR_SEMANTIC_RESULTS_INTERNAL_H

#include "cm/hir/semantic_results.h"

CmSemanticResultsStatus cm_semantic_results_create(
    const CmHirContext *hir, CmHirCrateId local_crate,
    CmSemanticResults **out_results);
void cm_semantic_results_destroy(CmSemanticResults *results);

#endif
