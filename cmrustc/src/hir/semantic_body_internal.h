#ifndef CMRUSTC_HIR_SEMANTIC_BODY_INTERNAL_H
#define CMRUSTC_HIR_SEMANTIC_BODY_INTERNAL_H

#include "cm/hir/semantic_body.h"

typedef enum CmSemanticBodyWritebackStatus {
    CM_SEMANTIC_BODY_WRITEBACK_OK = 0,
    CM_SEMANTIC_BODY_WRITEBACK_INVALID,
    CM_SEMANTIC_BODY_WRITEBACK_UNSUPPORTED,
    CM_SEMANTIC_BODY_WRITEBACK_OVERFLOW
} CmSemanticBodyWritebackStatus;

typedef CmSemanticBodyWritebackStatus (*CmSemanticBodyWritebackFn)(
    void *context, CmSemanticSession *session, CmHirBodyId body,
    const CmTypeckTypeId *expression_terms, size_t expression_term_count);

CmSemanticBodyResult cm_semantic_body_check_definition_with_writeback(
    CmSemanticSession *session, CmHirBodyId body,
    CmSemanticBodyWritebackFn writeback, void *writeback_context);

#endif
