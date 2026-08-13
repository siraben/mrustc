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
CmSemanticBodyWritebackStatus cm_semantic_results_stage_projection_decision(
    void *context, CmSemanticSession *session,
    CmHirBodyId body, CmHirExprId expression,
    CmSemanticProjectionDecisionKind decision_kind, uint32_t decision_index,
    CmTypeckTypeId input_type, CmTypeckTypeId normalized_type,
    const CmProjectionNormalizeTrace *trace);
void cm_semantic_results_discard_body_stage(void *context);
CmSemanticResultsStatus cm_semantic_results_commit_checked_body(
    CmSemanticResults *results, CmSemanticSession *session,
    const CmSemanticBodyResult *check, CmSemanticResultsBodyStage *stage);
CmSemanticResultsStatus cm_semantic_results_commit_checked_instance(
    CmSemanticResults *results, CmSemanticSession *session,
    const CmHirCanonicalInstance *instance,
    const CmSemanticBodyResult *check, CmSemanticResultsBodyStage *stage,
    const CmSemanticCanonicalCallInput *calls, size_t call_count);
/* Construct an owned exact callee key from authenticated durable evidence. */
CmSemanticResultsStatus cm_semantic_results_callable_callee_identity(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission, CmHirBodyId body,
    CmHirExprId expression, CmHirCanonicalInstance *out_identity);
CmSemanticResultsStatus cm_semantic_results_instance_callable_callee_identity(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission,
    const CmHirInstanceSpec *caller, CmHirExprId expression,
    CmHirCanonicalInstance *out_identity);
/* Clone the retained callee key for one exact caller without reconstructing
 * either side through generation-local HIR type identifiers. */
CmSemanticResultsStatus
cm_semantic_results_canonical_instance_callee_identity(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission,
    const CmHirCanonicalInstance *caller, CmHirExprId expression,
    CmHirCanonicalInstance *out_identity);
/* Replay one exact instance directly from its authenticated structural key.
 * These accessors deliberately avoid rebuilding CmHirInstanceSpec values from
 * generation-local TypeIds at the MIR boundary. */
CmSemanticResultsStatus cm_semantic_results_canonical_instance_expression(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission,
    const CmHirCanonicalInstance *instance, CmHirExprId expression,
    CmSemanticExpressionView *out_view);
CmSemanticResultsStatus
cm_semantic_results_canonical_instance_expression_adjustment(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission,
    const CmHirCanonicalInstance *instance, CmHirExprId expression,
    uint32_t adjustment, CmSemanticAdjustmentView *out_view);
CmSemanticResultsStatus
cm_semantic_results_canonical_instance_primitive_binary(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission,
    const CmHirCanonicalInstance *instance, CmHirExprId expression,
    CmSemanticPrimitiveBinaryView *out_view);
CmSemanticResultsStatus
cm_semantic_results_canonical_instance_field_selection(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission,
    const CmHirCanonicalInstance *instance, CmHirExprId expression,
    CmSemanticFieldSelectionView *out_view);
CmSemanticResultsStatus cm_semantic_results_canonical_instance_signature(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission,
    const CmHirCanonicalInstance *instance,
    CmSemanticFunctionSignatureView *out_view);
CmSemanticResultsStatus
cm_semantic_results_canonical_instance_signature_parameter(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission,
    const CmHirCanonicalInstance *instance, uint32_t parameter,
    CmSemanticTypeView *out_view);
CmSemanticResultsStatus cm_semantic_results_canonical_instance_direct_call(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission,
    const CmHirCanonicalInstance *caller, CmHirExprId expression,
    const CmHirCanonicalInstance *expected_callee,
    CmSemanticDirectCallView *out_view);
CmSemanticResultsStatus
cm_semantic_results_canonical_instance_direct_call_parameter(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission,
    const CmHirCanonicalInstance *caller, CmHirExprId expression,
    const CmHirCanonicalInstance *expected_callee, uint32_t parameter,
    CmSemanticTypeView *out_view);
CmSemanticResultsStatus
cm_semantic_results_canonical_instance_callable_selection(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission,
    const CmHirCanonicalInstance *caller, CmHirExprId expression,
    CmSemanticCallableSelectionView *out_view);
CmSemanticResultsStatus
cm_semantic_results_canonical_instance_callable_selection_for_callee(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission,
    const CmHirCanonicalInstance *caller, CmHirExprId expression,
    const CmHirCanonicalInstance *expected_callee,
    CmSemanticCallableSelectionView *out_view);
CmSemanticResultsStatus
cm_semantic_results_canonical_instance_callable_argument(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission,
    const CmHirCanonicalInstance *caller, CmHirExprId expression,
    uint32_t argument, CmHirExprId *out_expression);
CmSemanticResultsStatus
cm_semantic_results_canonical_instance_callable_parameter(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission,
    const CmHirCanonicalInstance *caller, CmHirExprId expression,
    uint32_t parameter, CmSemanticTypeView *out_view);
CmSemanticResultsStatus
cm_semantic_results_canonical_instance_callable_generic_argument(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission,
    const CmHirCanonicalInstance *caller, CmHirExprId expression,
    CmSemanticCallableGenericArgumentDomain domain, uint32_t argument,
    CmSemanticGenericArgumentView *out_view);
CmSemanticResultsStatus
cm_semantic_results_canonical_instance_callable_parameter_for_callee(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission,
    const CmHirCanonicalInstance *caller, CmHirExprId expression,
    const CmHirCanonicalInstance *expected_callee, uint32_t parameter,
    CmSemanticTypeView *out_view);
CmSemanticResultsStatus
cm_semantic_results_canonical_instance_projection_trace(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission,
    const CmHirCanonicalInstance *instance, CmHirExprId expression,
    CmSemanticProjectionDecisionKind decision_kind, uint32_t decision_index,
    CmSemanticProjectionTraceView *out_view);
CmSemanticResultsStatus
cm_semantic_results_canonical_instance_projection_trace_step(
    const CmSemanticResults *results,
    const struct CmSemanticAdmission *admission,
    const CmHirCanonicalInstance *instance, uint32_t trace, uint32_t step,
    CmSemanticProjectionStepView *out_view);
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
