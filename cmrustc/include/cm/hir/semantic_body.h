#ifndef CMRUSTC_CM_HIR_SEMANTIC_BODY_H
#define CMRUSTC_CM_HIR_SEMANTIC_BODY_H

#include "cm/hir/semantic.h"

/*
 * A non-OK result is never permission to continue to MIR.  The pending
 * variants deliberately preserve which semantic capability is missing.
 */
typedef enum CmSemanticBodyStatus {
    CM_SEMANTIC_BODY_OK = 0,
    CM_SEMANTIC_BODY_PENDING_HIGHER_RANKED,
    CM_SEMANTIC_BODY_PENDING_OUTLIVES,
    CM_SEMANTIC_BODY_PENDING_PROJECTION,
    CM_SEMANTIC_BODY_PENDING_MODIFIER,
    CM_SEMANTIC_BODY_PENDING_SUBSTITUTION,
    CM_SEMANTIC_BODY_DEFERRED_INFERENCE,
    CM_SEMANTIC_BODY_DEFERRED_METADATA,
    CM_SEMANTIC_BODY_AMBIGUOUS,
    CM_SEMANTIC_BODY_NO_SOLUTION,
    CM_SEMANTIC_BODY_NEGATIVE,
    CM_SEMANTIC_BODY_UNSUPPORTED,
    CM_SEMANTIC_BODY_OVERFLOW,
    CM_SEMANTIC_BODY_TYPECK_FAILURE,
    CM_SEMANTIC_BODY_STALE,
    CM_SEMANTIC_BODY_INVALID
} CmSemanticBodyStatus;

typedef struct CmSemanticBodyResult {
    CmSemanticBodyStatus status;
    CmHirBodyId body;
    CmHirExprId expression;
    CmHirDefId callee;
    uint32_t predicate_index;
    CmTraitSolverResultKind solver_kind;
    CmTypeckStatus typeck_status;
} CmSemanticBodyResult;

#define CM_SEMANTIC_BODY_PREDICATE_NONE ((uint32_t)UINT32_MAX)

/*
 * Check every CALL expression owned by one already-typed body.  `session`
 * must be owned by that body's caller definition.  The caller instance is
 * exact and admits declaration-ordered type-only substitutions.
 *
 * Callee predicates are instantiated from each call, but goals and the
 * parameter environment are always caller-owned.  In particular, this API
 * never constructs a callee environment that could prove its own precondition.
 * Every non-OK result restores the session type graph to its entry state.
 */
CmSemanticBodyResult cm_semantic_body_check_calls(
    CmSemanticSession *session, CmHirBodyId body,
    const CmHirTypeId *owner_type_substitutions,
    uint32_t owner_type_substitution_count);

/*
 * Check a body with rigid definition-mode parameters. Concrete trait-impl
 * methods authenticate the session's enclosing impl and expose its imported
 * Self type through exact-method and enclosing-impl instantiations.
 */
CmSemanticBodyResult cm_semantic_body_check_definition(
    CmSemanticSession *session, CmHirBodyId body);

const char *cm_semantic_body_status_name(CmSemanticBodyStatus status);

#endif
