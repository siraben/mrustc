#ifndef CMRUSTC_CM_HIR_SEMANTIC_REGIONS_H
#define CMRUSTC_CM_HIR_SEMANTIC_REGIONS_H

#include "cm/hir/model.h"

typedef enum CmSemanticRegionsStatus {
    CM_SEMANTIC_REGIONS_OK = 0,
    CM_SEMANTIC_REGIONS_INVALID_ARGUMENT,
    CM_SEMANTIC_REGIONS_INVALID_HIR,
    CM_SEMANTIC_REGIONS_UNRESOLVED_REGION,
    CM_SEMANTIC_REGIONS_UNSUPPORTED_EXPRESSION
} CmSemanticRegionsStatus;

typedef struct CmSemanticRegionsResult {
    CmSemanticRegionsStatus status;
    size_t body_index;
    CmHirBodyId body;
    CmHirExprId expression;
    CmHirTypeId type;
    int has_region;
    CmHirRegionKind region_kind;
    CmHirGenericParamId generic_parameter;
} CmSemanticRegionsResult;

#define CM_SEMANTIC_REGIONS_BODY_INDEX_NONE ((size_t)-1)

/*
 * Read-only proof that one duplicate-free typed body manifest has no region,
 * type, or const inference variables in its represented declaration roots,
 * locals, expression types, or direct-call substitutions. Static and erased
 * regions are closed; early-bound regions must name an exact generic frame
 * owned by the body item or its enclosing trait/impl. Recursive type and
 * expression nesting is conservatively bounded. This is structural closure,
 * not lifetime inference, equality, outlives, promotion eligibility, or
 * borrow checking.
 *
 * Every expression owned by a manifest body must be reachable exactly once
 * and retain the bounded marker's recomputed builtin-Copy usage plus its
 * NOT_PROMOTED sentinel. The body array selects what to check; it does not
 * prove crate completeness. Predicates, supertraits, associated bounds, ADT
 * fields, enum discriminants, and type-position expressions are not roots of
 * this checker. A manifest body owner or enclosing trait/impl with any
 * predicate or outlives constraint is rejected fail-closed. As throughout the
 * HIR model, callers must not bypass public mutators with raw writes.
 */
CmSemanticRegionsResult cm_hir_semantic_check_regions(
    const CmHirContext *hir, const CmHirBodyId *bodies,
    size_t body_count);

const char *cm_semantic_regions_status_name(
    CmSemanticRegionsStatus status);

#endif
