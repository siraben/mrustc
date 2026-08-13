#ifndef CMRUSTC_CM_HIR_ADMISSION_H
#define CMRUSTC_CM_HIR_ADMISSION_H

#include "cm/hir/body.h"
#include "cm/hir/finalization.h"
#include "cm/hir/semantic_body.h"
#include "cm/hir/semantic_item.h"

typedef enum CmSemanticAdmissionStatus {
    CM_SEMANTIC_ADMISSION_OK = 0,
    CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT,
    CM_SEMANTIC_ADMISSION_LOCAL_BODIES_FAILURE,
    CM_SEMANTIC_ADMISSION_FINALIZATION_FAILURE,
    CM_SEMANTIC_ADMISSION_ITEM_FAILURE,
    CM_SEMANTIC_ADMISSION_SESSION_FAILURE,
    CM_SEMANTIC_ADMISSION_BODY_FAILURE,
    CM_SEMANTIC_ADMISSION_HIR_FAILURE
} CmSemanticAdmissionStatus;

typedef struct CmSemanticAdmissionResult {
    CmSemanticAdmissionStatus status;
    CmHirItemId item;
    CmHirDefId owner;
    CmHirBodyId body;
    CmHirLocalBodiesResult local_bodies;
    CmSemanticItemResult item_result;
    CmSemanticBodyResult body_result;
    CmTraitSolverResultKind session_status;
    CmHirStatus hir_status;
} CmSemanticAdmissionResult;

/* Process-local evidence for one authenticated slice of an exact HIR generation. */
typedef struct CmSemanticAdmission { void *state; } CmSemanticAdmission;

typedef struct CmSemanticReachableBody {
    CmHirDefId owner;
    CmHirBodyId body;
} CmSemanticReachableBody;

struct CmHirInstanceSpec;
typedef struct CmSemanticReachableInstance {
    CmHirBodyId body;
    const struct CmHirInstanceSpec *spec;
} CmSemanticReachableInstance;

typedef struct CmSemanticReachableInstanceCall {
    const struct CmHirInstanceSpec *caller;
    CmHirExprId expression;
    const struct CmHirInstanceSpec *callee;
} CmSemanticReachableInstanceCall;

/*
 * Transactionally lower and admit every supported local free-function or
 * trait-impl method body definition. Generic definitions retain symbolic
 * owner parameters here; executable instances require exact admission below.
 */
CmSemanticAdmissionResult cm_semantic_admit_local_crate(
    CmSemanticAdmission *admission, CmHirContext *hir,
    CmHirCrateId local_crate, const CmModuleGraph *graph,
    CmModuleGraphRevision revision, const CmImportResolver *imports,
    const CmHirModuleMap *modules);

/*
 * Admit one closed set of already-typed, monomorphic local function bodies.
 * Generic functions and associated functions are not accepted. Unlisted
 * bodies remain outside the capability and may remain unlowered. Local trait
 * impl signatures are still finalized and checked for the complete crate.
 */
CmSemanticAdmissionResult cm_semantic_admit_typed_reachable_bodies(
    CmSemanticAdmission *admission, CmHirContext *hir,
    CmHirCrateId local_crate, const CmSemanticReachableBody *bodies,
    size_t body_count);

/*
 * Admit exact already-typed free-function instances. This first exact slice
 * is deliberately leaf-only and predicate-free: bodies containing calls or
 * owner predicates are rejected until exact call/obligation proof records
 * carry canonical instance identity.
 */
CmSemanticAdmissionResult cm_semantic_admit_typed_leaf_instances(
    CmSemanticAdmission *admission, CmHirContext *hir,
    CmHirCrateId local_crate, const CmSemanticReachableInstance *instances,
    size_t instance_count);
CmSemanticAdmissionResult cm_semantic_admit_typed_instance_closure(
    CmSemanticAdmission *admission, CmHirContext *hir,
    CmHirCrateId local_crate, const CmSemanticReachableInstance *instances,
    size_t instance_count, const CmSemanticReachableInstanceCall *calls,
    size_t call_count);
void cm_semantic_admission_destroy(CmSemanticAdmission *admission);
int cm_semantic_admission_is_current(const CmSemanticAdmission *admission);
const CmHirContext *cm_semantic_admission_hir(
    const CmSemanticAdmission *admission);
CmHirCrateId cm_semantic_admission_crate(
    const CmSemanticAdmission *admission);
uint64_t cm_semantic_admission_generation(
    const CmSemanticAdmission *admission);
/* Process-unique identity of this live capability; zero if empty or stale. */
uint64_t cm_semantic_admission_capability_id(
    const CmSemanticAdmission *admission);
const char *cm_semantic_admission_status_name(
    CmSemanticAdmissionStatus status);

#endif
