#ifndef CMRUSTC_CM_HIR_GOAL_TABLE_H
#define CMRUSTC_CM_HIR_GOAL_TABLE_H

#include "cm/hir/trait_solver.h"

typedef enum CmTraitGoalKind {
    CM_TRAIT_GOAL_IMPLEMENTED = 0,
    CM_TRAIT_GOAL_PROJECTION_EQUALITY
} CmTraitGoalKind;

/*
 * Names are deliberately absent. Bound regions are keyed by de Bruijn depth
 * and binder index, while placeholders from different universes stay apart.
 */
typedef struct CmTraitGoalBinder {
    uint32_t universe;
    uint32_t debruijn_depth;
    uint32_t lifetime_count;
} CmTraitGoalBinder;

typedef struct CmTraitGoal {
    CmTraitGoalKind kind;
    CmTraitGoalBinder binder;
    union {
        CmImplementedTraitGoal implemented;
        CmProjectionEqualityGoal projection_equality;
    } data;
} CmTraitGoal;

typedef struct CmTraitGoalTableLimits {
    size_t max_goal_depth;
    size_t max_canonical_nodes;
    size_t max_table_entries;
} CmTraitGoalTableLimits;

/*
 * Derived, single-threaded table state tied to one immutable impl index and
 * one immutable parameter environment. Complete entries never change.
 */
typedef struct CmTraitGoalTable {
    void *state;
} CmTraitGoalTable;

CmTraitSolverResultKind cm_trait_goal_table_init(CmTraitGoalTable *table,
    const CmTraitImplIndex *index, const CmParamEnv *environment,
    CmTraitGoalTableLimits limits);
void cm_trait_goal_table_destroy(CmTraitGoalTable *table);

int cm_trait_goal_table_is_current(const CmTraitGoalTable *table);
size_t cm_trait_goal_table_entry_count(const CmTraitGoalTable *table);
size_t cm_trait_goal_table_cache_hit_count(const CmTraitGoalTable *table);

/*
 * Canonical keys own no typeck-session IDs or pointers. This first slice
 * caches deterministic non-PROVEN outcomes only; a proof is always replayed
 * through the transactional solver before it can affect the caller session.
 */
CmTraitSelectionResult cm_trait_goal_table_solve(CmTraitGoalTable *table,
    CmTypeckContext *typeck,
    const CmParamEnvSubstitution *substitution,
    const CmTraitGoal *goal);

/* Root implemented-goal proof with optional unique-impl replay evidence. */
CmTraitSelectionResult cm_trait_goal_table_solve_with_impl_witness(
    CmTraitGoalTable *table, CmTypeckContext *typeck,
    const CmParamEnvSubstitution *substitution,
    const CmTraitGoal *goal, CmTraitImplSelectionWitness *witness);

#endif
