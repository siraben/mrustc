#ifndef CMRUSTC_CM_MIR_LOWER_H
#define CMRUSTC_CM_MIR_LOWER_H

#include "cm/mir/model.h"

typedef enum CmMirLowerErrorKind {
    CM_MIR_LOWER_INVALID_ARGUMENT = 0,
    CM_MIR_LOWER_INVALID_HIR,
    CM_MIR_LOWER_UNSUPPORTED_BODY_STATE,
    CM_MIR_LOWER_UNSUPPORTED_TYPE,
    CM_MIR_LOWER_UNSUPPORTED_EXPRESSION,
    CM_MIR_LOWER_CONSTANT_OUT_OF_RANGE,
    CM_MIR_LOWER_MODEL_FAILURE
} CmMirLowerErrorKind;

typedef struct CmMirLowerError {
    CmMirLowerErrorKind kind;
    CmHirBodyId hir_body;
    CmHirExprId hir_expression;
    CmMirStatus mir_status;
    char message[160];
} CmMirLowerError;

typedef struct CmMirLowerResult {
    CmMirBodyId body;
    size_t lowered_body_count;
    size_t error_count;
    CmMirLowerError first_error;
} CmMirLowerResult;

/*
 * Lower one fully typed HIR body. The initial slice admits a possibly nested
 * block whose tail is one i32 integer expression. It produces `_0 = const`
 * followed by `return`. Validation and construction precede the sole context
 * insertion, so every failure leaves the MIR context unchanged.
 */
CmMirLowerResult cm_mir_lower_body(CmMirContext *context,
    const CmHirContext *hir, CmHirBodyId body);

/*
 * Lower one exact reachable instance. Direct callees must already be
 * published in the MIR context, making reachability order explicit. Calls
 * may pass one or two exact u32 or checked same-crate named-aggregate values;
 * aggregate arguments require a monomorphic callee and every call result
 * remains exact u32.
 */
CmMirLowerResult cm_mir_lower_instance(CmMirContext *context,
    const CmHirContext *hir, CmHirBodyId body,
    const CmHirTypeId *substitutions, uint32_t substitution_count);

const char *cm_mir_lower_error_kind_name(CmMirLowerErrorKind kind);

#endif
