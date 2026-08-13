#ifndef CMRUSTC_CM_HIR_SEMANTIC_RESULTS_H
#define CMRUSTC_CM_HIR_SEMANTIC_RESULTS_H

#include "cm/hir/projection_normalizer.h"

struct CmSemanticAdmission;
struct CmHirInstanceSpec;
typedef struct CmSemanticResults CmSemanticResults;

typedef enum CmSemanticResultsStatus {
    CM_SEMANTIC_RESULTS_OK = 0,
    CM_SEMANTIC_RESULTS_INVALID_ARGUMENT,
    CM_SEMANTIC_RESULTS_STALE,
    CM_SEMANTIC_RESULTS_FOREIGN,
    CM_SEMANTIC_RESULTS_NOT_FOUND,
    CM_SEMANTIC_RESULTS_INVALID_HIR,
    CM_SEMANTIC_RESULTS_DEFERRED_INFERENCE,
    CM_SEMANTIC_RESULTS_PENDING_PROJECTION,
    CM_SEMANTIC_RESULTS_UNSUPPORTED_TYPE,
    CM_SEMANTIC_RESULTS_OVERFLOW
} CmSemanticResultsStatus;

/*
 * Canonical authenticated solved structural bytes, independent of
 * generation-local CmHirTypeId and session-local CmTypeckTypeId. Generic
 * parameters retain their definition identity; concrete impl Self is
 * substituted before publication. Bytes may be compared across current
 * results produced for the same HIR context; storage is borrowed from the
 * producing results.
 */
typedef struct CmSemanticTypeView {
    const unsigned char *bytes;
    size_t size;
} CmSemanticTypeView;

typedef struct CmSemanticBodyView {
    CmHirBodyId body;
    CmHirDefId owner;
    uint32_t expression_count;
    uint32_t projection_trace_count;
    uint32_t projection_step_count;
} CmSemanticBodyView;

typedef struct CmSemanticExpressionView {
    CmHirExprId expression;
    CmHirBodyId body;
    CmSemanticTypeView unadjusted_type;
    CmSemanticTypeView adjusted_type;
    uint32_t adjustment_count;
    int has_direct_callable;
    CmHirDefId direct_callable;
    int has_primitive_operator;
    CmHirBinaryOperator primitive_operator;
} CmSemanticExpressionView;

/*
 * One ordered, already-proven conversion applied to an expression.  A
 * published adjustment sequence is contiguous: the first source type is the
 * expression's unadjusted type, every target is the next source, and the last
 * target is the expression's adjusted type.  MIR must replay this sequence;
 * it must not rediscover coercions from the endpoint types.
 */
typedef enum CmSemanticAdjustmentKind {
    CM_SEMANTIC_ADJUSTMENT_DEREFERENCE_BUILTIN = 0,
    CM_SEMANTIC_ADJUSTMENT_DEREFERENCE_TRAIT,
    CM_SEMANTIC_ADJUSTMENT_BORROW_SHARED,
    CM_SEMANTIC_ADJUSTMENT_BORROW_MUTABLE,
    CM_SEMANTIC_ADJUSTMENT_UNSIZE,
    CM_SEMANTIC_ADJUSTMENT_REIFY_FUNCTION,
    CM_SEMANTIC_ADJUSTMENT_MUTABLE_POINTER_TO_CONST,
    CM_SEMANTIC_ADJUSTMENT_NEVER_TO_ANY
} CmSemanticAdjustmentKind;

typedef struct CmSemanticAdjustmentView {
    CmHirBodyId body;
    CmHirExprId expression;
    uint32_t index;
    CmSemanticAdjustmentKind kind;
    CmSemanticTypeView source_type;
    CmSemanticTypeView target_type;
    /* Set only for CM_SEMANTIC_ADJUSTMENT_DEREFERENCE_TRAIT. */
    int has_selected_trait;
    CmHirDefId selected_trait;
    CmHirDefId selected_method;
    CmHirDefId selected_impl;
} CmSemanticAdjustmentView;

/* An exact primitive operation recipe, separate from overloaded dispatch. */
typedef struct CmSemanticPrimitiveBinaryView {
    CmHirBodyId body;
    CmHirExprId expression;
    CmHirBinaryOperator operator_kind;
    CmHirExprId left_expression;
    CmHirExprId right_expression;
    CmSemanticTypeView left_type;
    CmSemanticTypeView right_type;
    CmSemanticTypeView result_type;
} CmSemanticPrimitiveBinaryView;

/* An authenticated direct field projection; no field lookup is repeated. */
typedef struct CmSemanticFieldSelectionView {
    CmHirBodyId body;
    CmHirExprId expression;
    CmHirExprId base_expression;
    CmHirDefId aggregate_definition;
    uint32_t field_index;
    CmSemanticTypeView base_type;
    CmSemanticTypeView field_type;
} CmSemanticFieldSelectionView;

typedef struct CmSemanticFunctionSignatureView {
    CmHirDefId definition;
    CmHirBodyId body;
    uint32_t parameter_count;
    CmSemanticTypeView return_type;
} CmSemanticFunctionSignatureView;

typedef struct CmSemanticDirectCallView {
    CmHirBodyId body;
    CmHirExprId expression;
    CmHirDefId callee;
    uint32_t parameter_count;
    CmSemanticTypeView return_type;
} CmSemanticDirectCallView;

typedef enum CmSemanticProjectionDecisionKind {
    CM_SEMANTIC_PROJECTION_DECISION_EXPRESSION_TYPE = 0
} CmSemanticProjectionDecisionKind;

/* One durable normalization decision made by the production semantic pass. */
typedef struct CmSemanticProjectionTraceView {
    CmHirBodyId body;
    CmHirExprId expression;
    CmSemanticProjectionDecisionKind decision_kind;
    uint32_t decision_index;
    uint32_t trace_index;
    CmSemanticTypeView input_type;
    CmSemanticTypeView normalized_type;
    uint32_t step_count;
} CmSemanticProjectionTraceView;

/* One durable, authenticated projection reduction within a trace. */
typedef struct CmSemanticProjectionStepView {
    CmHirBodyId body;
    uint32_t trace_index;
    uint32_t index;
    CmSemanticTypeView projection;
    CmSemanticTypeView target;
    CmSemanticTypeView normalized_target;
    CmTraitProofOrigin proof_origin;
    size_t param_env_fact_index;
    uint32_t param_env_equality_index;
    CmHirDefId impl_definition;
    CmHirDefId impl_associated_definition;
} CmSemanticProjectionStepView;

CmSemanticResultsStatus cm_semantic_results_instance_body(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission,
    const struct CmHirInstanceSpec *spec, CmSemanticBodyView *out_view);
CmSemanticResultsStatus cm_semantic_results_instance_expression(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission,
    const struct CmHirInstanceSpec *spec, CmHirExprId expression,
    CmSemanticExpressionView *out_view);
CmSemanticResultsStatus cm_semantic_results_instance_expression_adjustment(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission,
    const struct CmHirInstanceSpec *spec, CmHirExprId expression,
    uint32_t adjustment, CmSemanticAdjustmentView *out_view);
CmSemanticResultsStatus cm_semantic_results_instance_primitive_binary(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission,
    const struct CmHirInstanceSpec *spec, CmHirExprId expression,
    CmSemanticPrimitiveBinaryView *out_view);
CmSemanticResultsStatus cm_semantic_results_instance_field_selection(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission,
    const struct CmHirInstanceSpec *spec, CmHirExprId expression,
    CmSemanticFieldSelectionView *out_view);
CmSemanticResultsStatus cm_semantic_results_instance_signature(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission,
    const struct CmHirInstanceSpec *spec,
    CmSemanticFunctionSignatureView *out_view);
CmSemanticResultsStatus cm_semantic_results_instance_signature_parameter(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission,
    const struct CmHirInstanceSpec *spec, uint32_t parameter,
    CmSemanticTypeView *out_view);
CmSemanticResultsStatus cm_semantic_results_instance_direct_call(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission,
    const struct CmHirInstanceSpec *caller, CmHirExprId expression,
    const struct CmHirInstanceSpec *expected_callee,
    CmSemanticDirectCallView *out_view);
CmSemanticResultsStatus cm_semantic_results_instance_direct_call_parameter(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission,
    const struct CmHirInstanceSpec *caller, CmHirExprId expression,
    const struct CmHirInstanceSpec *expected_callee, uint32_t parameter,
    CmSemanticTypeView *out_view);
CmSemanticResultsStatus cm_semantic_results_instance_projection_trace(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission,
    const struct CmHirInstanceSpec *spec, CmHirExprId expression,
    CmSemanticProjectionDecisionKind decision_kind, uint32_t decision_index,
    CmSemanticProjectionTraceView *out_view);
CmSemanticResultsStatus cm_semantic_results_instance_projection_trace_step(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission,
    const struct CmHirInstanceSpec *spec, uint32_t trace, uint32_t step,
    CmSemanticProjectionStepView *out_view);

/* The returned results object and all views are borrowed from admission. */
const CmSemanticResults *cm_semantic_admission_results(
    const struct CmSemanticAdmission *admission);
int cm_semantic_results_is_current(const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission);
const CmHirContext *cm_semantic_results_hir(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission);
CmHirCrateId cm_semantic_results_crate(const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission);
uint64_t cm_semantic_results_generation(const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission);
size_t cm_semantic_results_body_count(const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission);
CmSemanticResultsStatus cm_semantic_results_body_at(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission, size_t index,
    CmSemanticBodyView *out_view);
CmSemanticResultsStatus cm_semantic_results_body(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission, CmHirBodyId body,
    CmSemanticBodyView *out_view);
CmSemanticResultsStatus cm_semantic_results_expression(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission, CmHirBodyId body,
    CmHirExprId expression,
    CmSemanticExpressionView *out_view);
CmSemanticResultsStatus cm_semantic_results_expression_adjustment(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission, CmHirBodyId body,
    CmHirExprId expression, uint32_t adjustment,
    CmSemanticAdjustmentView *out_view);
CmSemanticResultsStatus cm_semantic_results_primitive_binary(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission, CmHirBodyId body,
    CmHirExprId expression, CmSemanticPrimitiveBinaryView *out_view);
CmSemanticResultsStatus cm_semantic_results_field_selection(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission, CmHirBodyId body,
    CmHirExprId expression, CmSemanticFieldSelectionView *out_view);
CmSemanticResultsStatus cm_semantic_results_signature(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission, CmHirBodyId body,
    CmSemanticFunctionSignatureView *out_view);
CmSemanticResultsStatus cm_semantic_results_signature_parameter(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission, CmHirBodyId body,
    uint32_t parameter, CmSemanticTypeView *out_view);
CmSemanticResultsStatus cm_semantic_results_direct_call(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission, CmHirBodyId body,
    CmHirExprId expression, CmSemanticDirectCallView *out_view);
CmSemanticResultsStatus cm_semantic_results_direct_call_parameter(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission, CmHirBodyId body,
    CmHirExprId expression, uint32_t parameter,
    CmSemanticTypeView *out_view);
CmSemanticResultsStatus cm_semantic_results_projection_trace(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission, CmHirBodyId body,
    CmHirExprId expression,
    CmSemanticProjectionDecisionKind decision_kind, uint32_t decision_index,
    CmSemanticProjectionTraceView *out_view);
CmSemanticResultsStatus cm_semantic_results_projection_trace_step(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission, CmHirBodyId body,
    uint32_t trace, uint32_t step, CmSemanticProjectionStepView *out_view);
CmSemanticResultsStatus cm_semantic_type_view_equal(
    const CmSemanticTypeView *left, const CmSemanticTypeView *right,
    int *out_equal);
CmSemanticResultsStatus cm_semantic_type_view_matches_monomorphic_hir(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission,
    const CmSemanticTypeView *view, CmHirTypeId type, int *out_equal);
const char *cm_semantic_results_status_name(CmSemanticResultsStatus status);

#endif
