#ifndef CMRUSTC_CM_HIR_PARAM_ENV_H
#define CMRUSTC_CM_HIR_PARAM_ENV_H

#include "cm/hir/model.h"
#include "cm/hir/typeck.h"

#include <stddef.h>

typedef struct CmParamEnv {
    void *state;
} CmParamEnv;

typedef enum CmParamEnvStatus {
    CM_PARAM_ENV_READY = 0,
    CM_PARAM_ENV_INVALID,
    CM_PARAM_ENV_STALE,
    CM_PARAM_ENV_UNSUPPORTED,
    CM_PARAM_ENV_OVERFLOW,
    CM_PARAM_ENV_TYPECK_FAILURE
} CmParamEnvStatus;

typedef enum CmParamEnvFactKind {
    CM_PARAM_ENV_FACT_IMPLEMENTED = 0,
    CM_PARAM_ENV_FACT_OUTLIVES
} CmParamEnvFactKind;

typedef enum CmParamEnvFactProvenance {
    CM_PARAM_ENV_PROVENANCE_EXACT_PREDICATE = 0,
    CM_PARAM_ENV_PROVENANCE_ENCLOSING_PREDICATE,
    CM_PARAM_ENV_PROVENANCE_EXACT_OUTLIVES,
    CM_PARAM_ENV_PROVENANCE_ENCLOSING_OUTLIVES,
    CM_PARAM_ENV_PROVENANCE_TRAIT_SELF,
    CM_PARAM_ENV_PROVENANCE_SUPERTRAIT,
    CM_PARAM_ENV_PROVENANCE_POSITIVE_IMPL_HEADER
} CmParamEnvFactProvenance;

typedef enum CmParamEnvPendingKind {
    CM_PARAM_ENV_PENDING_HIGHER_RANKED = 0,
    CM_PARAM_ENV_PENDING_OUTLIVES,
    CM_PARAM_ENV_PENDING_PROJECTION_EQUALITY,
    CM_PARAM_ENV_PENDING_PROJECTION_NORMALIZATION,
    CM_PARAM_ENV_PENDING_RECURSIVE_IMPL_PREDICATE,
    CM_PARAM_ENV_PENDING_UNSUPPORTED_MODIFIER,
    CM_PARAM_ENV_PENDING_MIXED_OWNER_SUBSTITUTION,
    CM_PARAM_ENV_PENDING_FOREIGN_OWNER_SUBSTITUTION
} CmParamEnvPendingKind;

enum {
    CM_PARAM_ENV_BLOCK_NONE = 0u,
    CM_PARAM_ENV_BLOCK_HIGHER_RANKED = 1u << 0,
    CM_PARAM_ENV_BLOCK_PROJECTION = 1u << 1,
    CM_PARAM_ENV_BLOCK_MODIFIER = 1u << 2,
    CM_PARAM_ENV_BLOCK_MIXED_OWNER = 1u << 3,
    CM_PARAM_ENV_BLOCK_FOREIGN_OWNER = 1u << 4,
    CM_PARAM_ENV_BLOCK_OVERFLOW = 1u << 5
};

/*
 * Raw HIR fact. Variable-length members remain HIR-owned; freshness checks
 * reject every append, rewind, destruction/reinitialization, or arena change.
 */
typedef struct CmParamEnvFact {
    CmParamEnvFactKind kind;
    CmParamEnvFactProvenance provenance;
    CmHirDefId source_owner;
    CmHirDefId parameter_owner;
    CmHirDefId self_owner;
    CmSpan span;
    /* Aggregate retained for diagnostics and pending-goal summaries. */
    unsigned int blocker_flags;
    /* Applicability blockers from the implemented subject/trait head only. */
    unsigned int head_blocker_flags;
    union {
        struct {
            /* NONE denotes the enclosing trait/impl Self. */
            CmHirTypeId subject;
            CmHirNamedType trait_type;
            CmHirAssociatedTypeEquality *equalities;
            /*
             * Environment-owned flags for each head-plus-RHS equality,
             * parallel to equalities.
             */
            unsigned int *equality_blocker_flags;
            uint32_t equality_count;
            CmHirPredicateScopeId scope;
            CmHirLifetimeBinder binder;
            CmHirTraitPredicateModifier modifier;
        } implemented;
        CmHirOutlivesPredicate outlives;
    } data;
} CmParamEnvFact;

typedef struct CmParamEnvPendingGoal {
    CmParamEnvPendingKind kind;
    CmHirDefId source_owner;
    size_t fact_index;
    CmSpan span;
} CmParamEnvPendingGoal;

typedef struct CmParamEnvSubstitution {
    const CmTypeckInstantiation *exact;
    const CmTypeckInstantiation *enclosing;
} CmParamEnvSubstitution;

/* Session-owned instantiation of one exact associated-type equality fact. */
typedef struct CmParamEnvEqualityInstance {
    size_t fact_index;
    uint32_t equality_index;
    CmParamEnvFactProvenance provenance;
    CmHirDefId source_owner;
    CmTypeckTypeId subject;
    CmTypeckNamedType trait_type;
    CmHirDefId associated_type;
    CmTypeckTypeId value;
    CmSpan span;
} CmParamEnvEqualityInstance;

CmParamEnvStatus cm_param_env_init(CmParamEnv *environment,
    const CmHirContext *hir, CmHirDefId exact_owner);
void cm_param_env_destroy(CmParamEnv *environment);

int cm_param_env_is_current(const CmParamEnv *environment);
const CmHirContext *cm_param_env_hir(const CmParamEnv *environment);
CmHirDefId cm_param_env_exact_owner(const CmParamEnv *environment);
CmHirDefId cm_param_env_enclosing_owner(const CmParamEnv *environment);

size_t cm_param_env_fact_count(const CmParamEnv *environment);
const CmParamEnvFact *cm_param_env_fact(const CmParamEnv *environment,
    size_t fact_index);
size_t cm_param_env_pending_count(const CmParamEnv *environment);
const CmParamEnvPendingGoal *cm_param_env_pending(
    const CmParamEnv *environment, size_t pending_index);

/*
 * Transactionally instantiate one non-HRTB implemented fact. `out_trait`
 * borrows typeck-session or substitution storage. Failure appends no terms.
 */
CmParamEnvStatus cm_param_env_instantiate_implemented(
    const CmParamEnv *environment, size_t fact_index,
    CmTypeckContext *typeck, const CmParamEnvSubstitution *substitution,
    CmTypeckTypeId *out_subject, CmTypeckNamedType *out_trait,
    CmTypeckStatus *out_typeck_status);

/*
 * Transactionally instantiate one associated equality retained by an
 * implemented fact. The returned terms borrow `typeck`; provenance and
 * definition identities remain stable while the environment is current.
 */
CmParamEnvStatus cm_param_env_instantiate_equality(
    const CmParamEnv *environment, size_t fact_index,
    uint32_t equality_index, CmTypeckContext *typeck,
    const CmParamEnvSubstitution *substitution,
    CmParamEnvEqualityInstance *out_equality,
    CmTypeckStatus *out_typeck_status);

/*
 * As above, but permits a projection blocker isolated to the equality RHS so
 * a recursive normalizer can consume the instantiated target. Every head or
 * non-projection RHS blocker still rejects transactionally.
 */
CmParamEnvStatus cm_param_env_instantiate_equality_target(
    const CmParamEnv *environment, size_t fact_index,
    uint32_t equality_index, CmTypeckContext *typeck,
    const CmParamEnvSubstitution *substitution,
    CmParamEnvEqualityInstance *out_equality,
    CmTypeckStatus *out_typeck_status);

const char *cm_param_env_status_name(CmParamEnvStatus status);
const char *cm_param_env_pending_name(CmParamEnvPendingKind kind);

#endif
