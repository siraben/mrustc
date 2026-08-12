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

void cm_hir_canonical_instance_init(CmHirCanonicalInstance *instance);
CmHirInstanceStatus cm_hir_canonical_instance_encode(
    const CmHirContext *hir, CmHirCrateId local_crate,
    const CmHirInstanceSpec *spec, CmHirCanonicalInstance *out_instance);
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
