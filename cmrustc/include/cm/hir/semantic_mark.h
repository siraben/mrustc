#ifndef CMRUSTC_CM_HIR_SEMANTIC_MARK_H
#define CMRUSTC_CM_HIR_SEMANTIC_MARK_H

#include "cm/hir/model.h"

struct CmSemanticAdmission;

typedef enum CmSemanticMarkStatus {
    CM_SEMANTIC_MARK_OK = 0,
    CM_SEMANTIC_MARK_INVALID_ARGUMENT,
    CM_SEMANTIC_MARK_INVALID_HIR,
    CM_SEMANTIC_MARK_UNSUPPORTED_EXPRESSION
} CmSemanticMarkStatus;

typedef struct CmSemanticMarkResult {
    CmSemanticMarkStatus status;
    size_t body_index;
    CmHirBodyId body;
    CmHirExprId expression;
} CmSemanticMarkResult;

#define CM_SEMANTIC_MARK_BODY_INDEX_NONE ((size_t)-1)

/*
 * Preflight and then annotate one complete, duplicate-free manifest of typed
 * bodies.  All expression nodes owned by a listed body must be reachable
 * exactly once from its root.  Failure is read-only.  Success records exactly
 * one HIR semantic mutation after all usage and promotion evidence is stored.
 */
CmSemanticMarkResult cm_hir_semantic_mark_bodies(CmHirContext *hir,
    const CmHirBodyId *bodies, size_t body_count);

/* As above, but consume durable semantic call recipes for selected calls. */
CmSemanticMarkResult cm_hir_semantic_mark_admitted_bodies(
    CmHirContext *hir, const CmHirBodyId *bodies, size_t body_count,
    const struct CmSemanticAdmission *admission);

const char *cm_semantic_mark_status_name(CmSemanticMarkStatus status);

#endif
