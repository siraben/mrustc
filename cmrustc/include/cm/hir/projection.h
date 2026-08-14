#ifndef CMRUSTC_CM_HIR_PROJECTION_H
#define CMRUSTC_CM_HIR_PROJECTION_H

#include "cm/hir/model.h"

typedef enum CmHirProjectionStatus {
    CM_HIR_PROJECTION_SELECTED = 0,
    CM_HIR_PROJECTION_DEFERRED_ARGUMENTS,
    CM_HIR_PROJECTION_DEFERRED_SELF,
    CM_HIR_PROJECTION_DEFERRED_CRATE,
    CM_HIR_PROJECTION_NO_IMPL,
    CM_HIR_PROJECTION_AMBIGUOUS,
    CM_HIR_PROJECTION_SUBSTITUTION_FAILURE,
    CM_HIR_PROJECTION_INVALID_ASSOCIATION
} CmHirProjectionStatus;

typedef struct CmHirProjectionMatch {
    CmHirProjectionStatus status;
    /* Set only for CM_HIR_PROJECTION_SELECTED. */
    CmHirTypeId target_template;
    CmHirTypeId query_self;
    CmHirDefId impl_definition;
    CmHirDefId impl_associated_definition;
} CmHirProjectionMatch;

typedef struct CmHirProjectionResult {
    CmHirProjectionStatus status;
    /* Set only for CM_HIR_PROJECTION_SELECTED. */
    CmHirTypeId target;
    CmHirDefId impl_definition;
    CmHirDefId impl_associated_definition;
    CmHirStatus hir_status;
    size_t allocated_type_count;
} CmHirProjectionResult;

typedef struct CmHirProjectionImplTarget {
    CmHirProjectionStatus status;
    /* Set only for CM_HIR_PROJECTION_SELECTED. */
    CmHirTypeId target_template;
    CmHirDefId impl_associated_definition;
} CmHirProjectionImplTarget;

/*
 * Match one local, positive impl for a qualified projection.  The bounded
 * matcher accepts exact scalar/zero-argument local ADT keys and full ordered
 * generic local ADT templates such as `impl<T> Trait for Wrapper<T>`.  It
 * performs no predicate solving, specialization selection, trait-argument
 * matching, or recursive projection selection. An otherwise matching impl
 * that contains any specialization-default member is an explicit
 * DEFERRED_ARGUMENTS blocker and never exposes a target or provider.
 *
 * `local_crate` is the crate whose compilation is requesting selection.  A
 * foreign trait or nominal self type returns DEFERRED_CRATE.  A potentially
 * matching foreign impl also returns DEFERRED_CRATE rather than allowing a
 * local candidate to be selected without cross-crate coherence/metadata.
 *
 * Matching is read-only and allocation-free.  A selected generic match returns
 * the stored associated target template, not an instantiated target.  Every
 * non-selected result has none IDs; ambiguity never exposes an arbitrary
 * candidate.
 */
CmHirProjectionMatch cm_hir_match_projection(
    const CmHirContext *context, CmHirCrateId local_crate,
    CmHirTypeId projection_type);

/*
 * Match and instantiate one projection.  Monomorphic and direct-parameter
 * targets reuse existing IDs without allocation.  Structural generic targets
 * are appended to the HIR context through the shared type-substitution walker;
 * existing types are never modified, and ordinary substitution failures rewind
 * all type/arena additions made by the call.  `local_crate` has the same
 * caller-boundary meaning as in cm_hir_match_projection.
 */
CmHirProjectionResult cm_hir_select_projection(
    CmHirContext *context, CmHirCrateId local_crate,
    CmHirTypeId projection_type);

/*
 * Authenticate the associated target attached to one already-selected impl.
 * A containing impl with any specialization-default member returns
 * DEFERRED_ARGUMENTS. This performs no impl selection and returns no
 * substitution evidence.
 */
CmHirProjectionImplTarget cm_hir_projection_impl_target(
    const CmHirContext *context, CmHirCrateId local_crate,
    CmHirDefId impl_definition, CmHirDefId trait_definition,
    CmHirDefId trait_associated_definition);

const char *cm_hir_projection_status_name(CmHirProjectionStatus status);

#endif
