#ifndef CMRUSTC_CM_HIR_SEMANTIC_ITEM_H
#define CMRUSTC_CM_HIR_SEMANTIC_ITEM_H

#include "cm/hir/semantic.h"

typedef enum CmSemanticItemStatus {
    CM_SEMANTIC_ITEM_OK = 0,
    CM_SEMANTIC_ITEM_PENDING_CROSS_CRATE,
    CM_SEMANTIC_ITEM_PENDING_GENERIC,
    CM_SEMANTIC_ITEM_PENDING_HIGHER_RANKED,
    CM_SEMANTIC_ITEM_PENDING_OUTLIVES,
    CM_SEMANTIC_ITEM_PENDING_PREDICATE,
    CM_SEMANTIC_ITEM_PENDING_PROJECTION,
    CM_SEMANTIC_ITEM_PENDING_DEFAULT,
    CM_SEMANTIC_ITEM_PENDING_SPECIALIZATION,
    CM_SEMANTIC_ITEM_PENDING_NEGATIVE,
    CM_SEMANTIC_ITEM_MISSING_ASSOCIATED_TYPE,
    CM_SEMANTIC_ITEM_MISSING_REQUIRED_METHOD,
    CM_SEMANTIC_ITEM_DUPLICATE_ASSOCIATED_ITEM,
    CM_SEMANTIC_ITEM_WRONG_ASSOCIATION,
    CM_SEMANTIC_ITEM_RECEIVER_MISMATCH,
    CM_SEMANTIC_ITEM_PARAMETER_COUNT_MISMATCH,
    CM_SEMANTIC_ITEM_PARAMETER_TYPE_MISMATCH,
    CM_SEMANTIC_ITEM_RETURN_TYPE_MISMATCH,
    CM_SEMANTIC_ITEM_ABI_MISMATCH,
    CM_SEMANTIC_ITEM_SAFETY_MISMATCH,
    CM_SEMANTIC_ITEM_CONST_MISMATCH,
    CM_SEMANTIC_ITEM_ASYNC_MISMATCH,
    CM_SEMANTIC_ITEM_VARIADIC_MISMATCH,
    CM_SEMANTIC_ITEM_UNSUPPORTED,
    CM_SEMANTIC_ITEM_OVERFLOW,
    CM_SEMANTIC_ITEM_TYPECK_FAILURE,
    CM_SEMANTIC_ITEM_INVALID
} CmSemanticItemStatus;

typedef struct CmSemanticItemResult {
    CmSemanticItemStatus status;
    CmHirDefId impl_definition;
    CmHirDefId trait_definition;
    CmHirDefId impl_member;
    CmHirDefId trait_member;
    uint32_t parameter_index;
    CmTypeckStatus typeck_status;
    CmTraitSolverResultKind solver_kind;
} CmSemanticItemResult;

#define CM_SEMANTIC_ITEM_PARAMETER_NONE ((uint32_t)UINT32_MAX)

/*
 * Validate the supported local positive trait-impl slice without mutating HIR.
 * Every non-OK result is a hard barrier before body semantics or MIR.
 */
CmSemanticItemResult cm_semantic_item_check_local_trait_impls(
    const CmHirContext *hir, CmHirCrateId local_crate);

/*
 * COMPLETE-universe item validation. Projection-bearing method signatures
 * are normalized through one exact-member semantic session before comparison.
 */
CmSemanticItemResult cm_semantic_item_check_finalized_local_trait_impls(
    const CmHirCrateFinalization *finalization,
    CmProjectionNormalizeLimits normalize_limits);

const char *cm_semantic_item_status_name(CmSemanticItemStatus status);

#endif
