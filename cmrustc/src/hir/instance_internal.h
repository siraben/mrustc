#ifndef CMRUSTC_HIR_INSTANCE_INTERNAL_H
#define CMRUSTC_HIR_INSTANCE_INTERNAL_H

#include "cm/hir/instance.h"

/*
 * Admission-independent structural identity for an exact local callable.
 * This value carries no HIR-generation authentication; callers must only
 * publish it from inside a separately authenticated workflow.
 */
typedef struct CmHirCanonicalInstance {
    CmHirDefId definition;
    CmHirBodyId body;
    unsigned char *bytes;
    size_t size;
} CmHirCanonicalInstance;

/*
 * One already-normalized structural generic-argument payload.  `bytes` omits
 * the generic-argument kind tag, matching CmSemanticGenericArgumentView.
 */
typedef struct CmHirCanonicalArgumentPart {
    CmHirGenericArgKind kind;
    const unsigned char *bytes;
    size_t size;
} CmHirCanonicalArgumentPart;

/*
 * Structural counterpart of CmHirInstanceSpec.  This is the boundary used
 * when exact, authenticated types no longer have reusable CmHirTypeIds.
 */
typedef struct CmHirCanonicalInstanceParts {
    CmHirDefId selected_callable;
    CmHirDefId declared_trait_callable;
    const CmHirCanonicalArgumentPart *item_arguments;
    uint32_t item_argument_count;
    const CmHirCanonicalArgumentPart *method_arguments;
    uint32_t method_argument_count;
    CmHirDefId enclosing_impl;
    const CmHirCanonicalArgumentPart *enclosing_impl_arguments;
    uint32_t enclosing_impl_argument_count;
    CmHirDefId implemented_trait;
    const CmHirCanonicalArgumentPart *implemented_trait_arguments;
    uint32_t implemented_trait_argument_count;
    CmHirDefId self_owner;
    const unsigned char *self_type;
    size_t self_type_size;
} CmHirCanonicalInstanceParts;

void cm_hir_canonical_instance_init(CmHirCanonicalInstance *instance);
CmHirInstanceStatus cm_hir_canonical_instance_encode(
    const CmHirContext *hir, CmHirCrateId local_crate,
    const CmHirInstanceSpec *spec, CmHirCanonicalInstance *out_instance);
CmHirInstanceStatus cm_hir_canonical_instance_encode_parts(
    const CmHirContext *hir, CmHirCrateId local_crate,
    const CmHirCanonicalInstanceParts *parts,
    CmHirCanonicalInstance *out_instance);
CmHirInstanceStatus cm_hir_canonical_instance_encode_direct_call(
    const CmHirContext *hir, CmHirCrateId local_crate,
    const CmHirInstanceSpec *caller, const CmHirExpr *call,
    CmHirCanonicalInstance *out_instance);
CmHirInstanceStatus cm_hir_canonical_instance_clone(
    CmHirCanonicalInstance *out_instance,
    const CmHirCanonicalInstance *source);
void cm_hir_canonical_instance_destroy(CmHirCanonicalInstance *instance);
CmHirInstanceStatus cm_hir_canonical_instance_equal(
    const CmHirCanonicalInstance *left,
    const CmHirCanonicalInstance *right, int *out_equal);
CmHirInstanceStatus cm_hir_canonical_instance_compare(
    const CmHirCanonicalInstance *left,
    const CmHirCanonicalInstance *right, int *out_order);

#endif
