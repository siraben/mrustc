#ifndef CMRUSTC_CM_HIR_FINALIZATION_H
#define CMRUSTC_CM_HIR_FINALIZATION_H

#include "cm/hir/model.h"

/*
 * Process-local evidence that one local crate's impl universe was
 * structurally complete at one exact HIR semantic generation.  It does not
 * claim that bodies are typed or that later semantic passes have run.  The
 * token owns no HIR storage.
 */
typedef struct CmHirCrateFinalization {
    void *state;
} CmHirCrateFinalization;

/*
 * `finalization` must point to zero-initialized storage.  The caller is the
 * trusted complete-graph/HIR barrier: structural validation prevents a false
 * seal for reserved or cross-owned local definitions, but cannot discover
 * source items that the caller never lowered.
 */
CmHirStatus cm_hir_crate_finalization_init(
    CmHirCrateFinalization *finalization, const CmHirContext *hir,
    CmHirCrateId local_crate);
void cm_hir_crate_finalization_destroy(
    CmHirCrateFinalization *finalization);

int cm_hir_crate_finalization_is_current(
    const CmHirCrateFinalization *finalization);
const CmHirContext *cm_hir_crate_finalization_hir(
    const CmHirCrateFinalization *finalization);
CmHirCrateId cm_hir_crate_finalization_crate(
    const CmHirCrateFinalization *finalization);
uint64_t cm_hir_crate_finalization_generation(
    const CmHirCrateFinalization *finalization);

#endif
