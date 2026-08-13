#ifndef CMRUSTC_HIR_ADMISSION_INTERNAL_H
#define CMRUSTC_HIR_ADMISSION_INTERNAL_H

#include "cm/hir/admission.h"
#include "instance_internal.h"

/*
 * Borrowed canonical inputs for a closed exact-instance transaction.  Every
 * identity must outlive the admission call; committed semantic results clone
 * the complete key rather than retaining these pointers.
 */
typedef struct CmSemanticCanonicalReachableInstance {
    const CmHirCanonicalInstance *identity;
} CmSemanticCanonicalReachableInstance;

typedef struct CmSemanticCanonicalReachableInstanceCall {
    const CmHirCanonicalInstance *caller;
    CmHirExprId expression;
    const CmHirCanonicalInstance *callee;
} CmSemanticCanonicalReachableInstanceCall;

CmSemanticAdmissionResult
cm_semantic_admit_typed_canonical_instance_closure(
    CmSemanticAdmission *admission, CmHirContext *hir,
    CmHirCrateId local_crate,
    const CmSemanticCanonicalReachableInstance *instances,
    size_t instance_count,
    const CmSemanticCanonicalReachableInstanceCall *calls,
    size_t call_count);

#endif
