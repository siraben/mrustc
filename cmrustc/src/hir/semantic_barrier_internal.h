#ifndef CMRUSTC_HIR_SEMANTIC_BARRIER_INTERNAL_H
#define CMRUSTC_HIR_SEMANTIC_BARRIER_INTERNAL_H

#include "cm/hir/semantic_barrier.h"

typedef struct CmSemanticBarrierState CmSemanticBarrierState;
typedef struct CmSemanticBarrierState CmSemanticBarrierAuthority;

/* Retain the state behind a live wrapper. The retained authority remains
 * addressable after wrapper destruction, but reports stale immediately. */
CmSemanticBarrierAuthority *cm_semantic_barrier_authority_retain(
    const CmSemanticBarrier *barrier);
void cm_semantic_barrier_authority_release(
    CmSemanticBarrierAuthority *authority);
int cm_semantic_barrier_authority_matches(
    const CmSemanticBarrierAuthority *authority,
    uint64_t capability_id, CmSemanticBarrierPhase phase,
    const CmHirContext *hir, CmHirCrateId crate_id,
    uint64_t semantic_generation);

#endif
