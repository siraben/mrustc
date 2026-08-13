#ifndef CMRUSTC_HIR_SEMANTIC_BODY_INTERNAL_H
#define CMRUSTC_HIR_SEMANTIC_BODY_INTERNAL_H

#include "cm/hir/semantic_body.h"
#include "cm/hir/semantic_results.h"

typedef enum CmSemanticBodyWritebackStatus {
    CM_SEMANTIC_BODY_WRITEBACK_OK = 0,
    CM_SEMANTIC_BODY_WRITEBACK_INVALID,
    CM_SEMANTIC_BODY_WRITEBACK_DEFERRED_INFERENCE,
    CM_SEMANTIC_BODY_WRITEBACK_PENDING_PROJECTION,
    CM_SEMANTIC_BODY_WRITEBACK_UNSUPPORTED,
    CM_SEMANTIC_BODY_WRITEBACK_OVERFLOW
} CmSemanticBodyWritebackStatus;

typedef struct CmSemanticCheckedCallFacts {
    CmHirExprId expression;
    CmHirDefId callee;
    CmTypeckTypeId return_type;
    const CmTypeckTypeId *parameter_types;
    uint32_t parameter_count;
} CmSemanticCheckedCallFacts;

/* One expression-indexed step in source-to-target application order. */
typedef struct CmSemanticCheckedAdjustmentFacts {
    CmHirExprId expression;
    CmSemanticAdjustmentKind kind;
    CmTypeckTypeId source_type;
    CmTypeckTypeId target_type;
    /* Present only for a proven overloaded dereference. */
    int has_selected_trait;
    CmHirDefId selected_trait;
    CmHirDefId selected_method;
    CmHirDefId selected_impl;
} CmSemanticCheckedAdjustmentFacts;

typedef struct CmSemanticCheckedPrimitiveBinaryFacts {
    CmHirExprId expression;
    CmHirBinaryOperator operator_kind;
    CmHirExprId left_expression;
    CmHirExprId right_expression;
    CmTypeckTypeId left_type;
    CmTypeckTypeId right_type;
    CmTypeckTypeId result_type;
} CmSemanticCheckedPrimitiveBinaryFacts;

typedef struct CmSemanticCheckedFieldSelectionFacts {
    CmHirExprId expression;
    CmHirExprId base_expression;
    CmHirDefId aggregate_definition;
    uint32_t field_index;
    CmTypeckTypeId base_type;
    CmTypeckTypeId field_type;
} CmSemanticCheckedFieldSelectionFacts;

typedef struct CmSemanticCheckedBodyFacts {
    const CmTypeckTypeId *expression_terms;
    size_t expression_term_count;
    CmTypeckTypeId signature_return_type;
    const CmTypeckTypeId *signature_parameter_types;
    uint32_t signature_parameter_count;
    const CmSemanticCheckedCallFacts *calls;
    size_t call_count;
    const CmSemanticCheckedAdjustmentFacts *adjustments;
    size_t adjustment_count;
    const CmSemanticCheckedPrimitiveBinaryFacts *primitive_binaries;
    size_t primitive_binary_count;
    const CmSemanticCheckedFieldSelectionFacts *field_selections;
    size_t field_selection_count;
} CmSemanticCheckedBodyFacts;

typedef CmSemanticBodyWritebackStatus (*CmSemanticBodyWritebackFn)(
    void *context, CmSemanticSession *session, CmHirBodyId body,
    const CmSemanticCheckedBodyFacts *facts);

CmSemanticBodyResult cm_semantic_body_check_definition_with_writeback(
    CmSemanticSession *session, CmHirBodyId body,
    CmSemanticBodyWritebackFn writeback, void *writeback_context);
CmSemanticBodyResult cm_semantic_body_check_instance_with_writeback(
    CmSemanticSession *session, CmHirBodyId body,
    const CmHirTypeId *owner_type_substitutions,
    uint32_t owner_type_substitution_count,
    CmSemanticBodyWritebackFn writeback, void *writeback_context);

#endif
