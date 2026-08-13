#ifndef CMRUSTC_HIR_ADMISSION_AUTHORITY_INTERNAL_H
#define CMRUSTC_HIR_ADMISSION_AUTHORITY_INTERNAL_H

#include "cm/hir/admission.h"

typedef struct CmSemanticAdmissionAuthority CmSemanticAdmissionAuthority;

CmSemanticAdmissionAuthority *cm_semantic_admission_authority_retain(
    const CmSemanticAdmission *admission, int require_regions_whole_local);
void cm_semantic_admission_authority_release(
    CmSemanticAdmissionAuthority *authority);
uint64_t cm_semantic_admission_parent_capability_id(
    const CmSemanticAdmission *admission);

#endif
