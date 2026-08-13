#ifndef CMRUSTC_CM_HIR_INSTANCE_H
#define CMRUSTC_CM_HIR_INSTANCE_H

#include "cm/hir/admission.h"

#include <stdio.h>

typedef enum CmHirInstanceStatus {
    CM_HIR_INSTANCE_OK = 0,
    CM_HIR_INSTANCE_INVALID_ARGUMENT,
    CM_HIR_INSTANCE_STALE_ADMISSION,
    CM_HIR_INSTANCE_FOREIGN_ADMISSION,
    CM_HIR_INSTANCE_INVALID_ID,
    CM_HIR_INSTANCE_INVALID_RELATION,
    CM_HIR_INSTANCE_UNSUPPORTED_TYPE,
    CM_HIR_INSTANCE_UNSUPPORTED_REGION,
    CM_HIR_INSTANCE_UNSUPPORTED_CONST,
    CM_HIR_INSTANCE_OVERFLOW
} CmHirInstanceStatus;

/*
 * Borrowed construction input for one exact callable instance.  The four
 * argument lists stay separate because Rust paths carry method/item,
 * enclosing-impl, and implemented-trait substitutions in distinct binders.
 * Every nonempty list is declaration ordered.
 */
typedef struct CmHirInstanceSpec {
    CmHirDefId selected_callable;
    CmHirDefId body_definition;
    CmHirDefId declared_trait_callable;
    const CmHirGenericArg *item_arguments;
    uint32_t item_argument_count;
    const CmHirGenericArg *method_arguments;
    uint32_t method_argument_count;
    CmHirDefId enclosing_impl;
    const CmHirGenericArg *enclosing_impl_arguments;
    uint32_t enclosing_impl_argument_count;
    CmHirDefId implemented_trait;
    const CmHirGenericArg *implemented_trait_arguments;
    uint32_t implemented_trait_argument_count;
    CmHirDefId self_owner;
    CmHirTypeId self_type;
} CmHirInstanceSpec;

/* One allocation owns the complete structural key and admission latch. */
typedef struct CmHirInstanceKey { void *state; } CmHirInstanceKey;

void cm_hir_instance_spec_init(CmHirInstanceSpec *spec);
CmHirInstanceStatus cm_hir_instance_key_init(CmHirInstanceKey *key,
    const CmSemanticAdmission *admission, const CmHirInstanceSpec *spec);
CmHirInstanceStatus cm_hir_instance_key_clone(CmHirInstanceKey *out_key,
    const CmSemanticAdmission *admission,
    const CmHirInstanceKey *source_key);
void cm_hir_instance_key_destroy(CmHirInstanceKey *key);

/* A stale or foreign admission never validates or compares a key. */
CmHirInstanceStatus cm_hir_instance_key_validate(
    const CmHirInstanceKey *key,
    const CmSemanticAdmission *admission);
CmHirInstanceStatus cm_hir_instance_key_equal(
    const CmSemanticAdmission *admission, const CmHirInstanceKey *left,
    const CmHirInstanceKey *right, int *out_equal);
CmHirInstanceStatus cm_hir_instance_key_compare(
    const CmSemanticAdmission *admission, const CmHirInstanceKey *left,
    const CmHirInstanceKey *right, int *out_order);
CmHirInstanceStatus cm_hir_instance_key_dump(FILE *stream,
    const CmSemanticAdmission *admission, const CmHirInstanceKey *key);

const char *cm_hir_instance_status_name(CmHirInstanceStatus status);

#endif
