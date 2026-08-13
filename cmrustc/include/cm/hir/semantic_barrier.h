#ifndef CMRUSTC_CM_HIR_SEMANTIC_BARRIER_H
#define CMRUSTC_CM_HIR_SEMANTIC_BARRIER_H

#include "cm/hir/body.h"
#include "cm/hir/semantic_regions.h"

typedef enum CmSemanticBarrierPhase {
    CM_SEMANTIC_BARRIER_NONE = 0,
    CM_SEMANTIC_BARRIER_STRUCTURAL,
    CM_SEMANTIC_BARRIER_TYPED,
    /* Usage annotation and static-borrow marking before region inference. */
    CM_SEMANTIC_BARRIER_MARKED,
    CM_SEMANTIC_BARRIER_REGIONS,
    /* Closure/vtable/UFCS/reborrow/erased-type rewrites after regions. */
    CM_SEMANTIC_BARRIER_REWRITTEN,
    CM_SEMANTIC_BARRIER_VALIDATED
} CmSemanticBarrierPhase;

typedef enum CmSemanticAtomKind {
    CM_SEMANTIC_ATOM_NONE = 0,
    CM_SEMANTIC_ATOM_FUNCTION,
    CM_SEMANTIC_ATOM_CONST,
    CM_SEMANTIC_ATOM_STATIC,
    CM_SEMANTIC_ATOM_TYPE_POSITION
} CmSemanticAtomKind;

/*
 * Immutable identity of one cfg-active semantic body atom.  Type-position
 * atoms are published only when HIR retains a stable source body identity;
 * normalized scalar const arguments are not promoted into fictitious atoms.
 */
typedef struct CmSemanticAtomView {
    CmSemanticAtomKind kind;
    CmHirDefId owner;
    CmHirBodyId body;
    CmHirTypeId declared_type;
    CmSourceId source;
    uint32_t source_expression;
} CmSemanticAtomView;

typedef enum CmSemanticBarrierStatus {
    CM_SEMANTIC_BARRIER_OK = 0,
    CM_SEMANTIC_BARRIER_INVALID_ARGUMENT,
    CM_SEMANTIC_BARRIER_SOURCE_MISMATCH,
    CM_SEMANTIC_BARRIER_INVALID_HIR,
    CM_SEMANTIC_BARRIER_UNSUPPORTED_ATOM,
    CM_SEMANTIC_BARRIER_BODY_FAILURE,
    CM_SEMANTIC_BARRIER_HIR_FAILURE,
    CM_SEMANTIC_BARRIER_STALE,
    CM_SEMANTIC_BARRIER_PHASE_ORDER
} CmSemanticBarrierStatus;

typedef struct CmSemanticBarrierResult {
    CmSemanticBarrierStatus status;
    CmSemanticBarrierPhase phase;
    size_t atom_index;
    CmSemanticAtomView atom;
    CmHirLocalBodiesResult local_bodies;
    CmHirStatus hir_status;
    /* First expression rejected by a post-typing manifest phase, if any. */
    CmHirExprId expression;
    /* First type/region rejected by a post-typing manifest phase, if any. */
    CmHirTypeId type;
    int has_region;
    CmHirRegionKind region_kind;
    CmHirGenericParamId generic_parameter;
} CmSemanticBarrierResult;

#define CM_SEMANTIC_ATOM_INDEX_NONE ((size_t)-1)

/* Process-local evidence for one exact HIR generation and phase. */
typedef struct CmSemanticBarrier { void *state; } CmSemanticBarrier;

/*
 * Capture the complete cfg-active local body manifest in stable HIR item
 * order.  The manifest is derived internally and cannot be caller-trimmed.
 * This is a read-only structural operation. `barrier` must be zero-initialized
 * and single-use until destroyed. The HIR, graph, imports, and module map are
 * borrowed snapshots and must remain addressable until barrier destruction.
 * As with crate finalization, the caller is the trusted complete graph/HIR
 * lowering boundary; this API authenticates the complete module map and HIR
 * ownership but cannot recover source items that a different lowerer omitted.
 * Identities are process-local; callers must still keep all borrowed objects
 * addressable until barrier destruction. No counter API is thread-safe yet.
 */
CmSemanticBarrierResult cm_semantic_barrier_init_structural(
    CmSemanticBarrier *barrier, CmHirContext *hir,
    CmHirCrateId local_crate, const CmModuleGraph *graph,
    CmModuleGraphRevision revision, const CmImportResolver *imports,
    const CmHirModuleMap *modules);

/*
 * Atomically lower every manifest body through the real HIR typing pass.
 * Const/static atoms currently fail explicitly until their body checker is
 * implemented. On failure the exact manifest remains current at STRUCTURAL
 * after rollback. Every successful transition or mutating rollback mints a
 * fresh capability identity for the recaptured generation. No later phase
 * can be minted by this API.
 */
CmSemanticBarrierResult cm_semantic_barrier_advance_typed(
    CmSemanticBarrier *barrier, const CmModuleGraph *graph,
    CmModuleGraphRevision revision, const CmImportResolver *imports,
    const CmHirModuleMap *modules);

/*
 * Atomically annotate value usage and conservative static-borrow evidence for
 * the complete typed manifest. Failure leaves HIR, generation, phase, and
 * capability identity unchanged.
 */
CmSemanticBarrierResult cm_semantic_barrier_advance_marked(
    CmSemanticBarrier *barrier);

/*
 * Prove bounded structural region closure for the represented roots of the
 * complete marked body manifest. Success is read-only: HIR semantic/rewind
 * generations are unchanged while a fresh REGIONS capability is minted.
 * Failure preserves phase, generation, and capability exactly.
 */
CmSemanticBarrierResult cm_semantic_barrier_advance_regions(
    CmSemanticBarrier *barrier);

void cm_semantic_barrier_destroy(CmSemanticBarrier *barrier);
int cm_semantic_barrier_is_current(const CmSemanticBarrier *barrier);
CmSemanticBarrierPhase cm_semantic_barrier_phase(
    const CmSemanticBarrier *barrier);
const CmHirContext *cm_semantic_barrier_hir(
    const CmSemanticBarrier *barrier);
CmHirCrateId cm_semantic_barrier_crate(
    const CmSemanticBarrier *barrier);
uint64_t cm_semantic_barrier_generation(
    const CmSemanticBarrier *barrier);
uint64_t cm_semantic_barrier_capability_id(
    const CmSemanticBarrier *barrier);
size_t cm_semantic_barrier_atom_count(
    const CmSemanticBarrier *barrier);
CmSemanticBarrierStatus cm_semantic_barrier_atom_at(
    const CmSemanticBarrier *barrier, size_t index,
    CmSemanticAtomView *out_atom);
int cm_semantic_barrier_contains_body(const CmSemanticBarrier *barrier,
    CmHirBodyId body, CmSemanticAtomView *out_atom);
const char *cm_semantic_barrier_status_name(CmSemanticBarrierStatus status);
const char *cm_semantic_barrier_phase_name(CmSemanticBarrierPhase phase);

#endif
