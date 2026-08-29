#ifndef CMRUSTC_CM_HIR_TYCK_H
#define CMRUSTC_CM_HIR_TYCK_H
#include "cm/hir/ty.h"
#include "cm/hir/ubody.h"
#include <stddef.h>
#include <stdint.h>

/*
 * Lenient inference typeck over untyped bodies (M9-04).  Every expression,
 * pattern, and local of every `ubody` receives an arena type; the pass
 * never rejects a body, it counts what it could not decide.  Method,
 * field, index, and associated-item resolution is retried through a
 * pending worklist until it stops making progress, then integer and float
 * variables default.  Mirrors mrustc's typeck in spirit, not in rigor.
 */

typedef enum CmTyckBodyStatus {
    CM_TYCK_BODY_TYPED = 0,     /* every node has a concrete type */
    CM_TYCK_BODY_PARTIAL,       /* some nodes remain unresolved */
    CM_TYCK_BODY_SKIPPED        /* no ubody available */
} CmTyckBodyStatus;

typedef struct CmTyckBody {
    CmHirBodyId body;
    CmTyckBodyStatus status;
    CmTyId *expr_types;   /* indexed by expression id (1-based) */
    CmTyId *pat_types;
    CmTyId *local_types;
    /* Chosen method definition per METHOD_CALL expression (none when
     * unresolved); MIR emission renders the callee symbol from it. */
    CmHirDefId *method_targets;
    /* `&T -> &dyn Trait` coercion recorded per coerced expression: the
     * target reference type (none elsewhere); MIR emission builds the
     * [data, vtable] pair there. */
    CmTyId *unsize_targets;
    /* The written Self of a qualified path (`<T>::C`, `<T as Tr>::C`)
     * whose target is an associated item: emission substitutes it per
     * instance to reach the impl's item (none elsewhere). */
    CmTyId *path_self_types;
    CmTyId return_type;
    uint32_t unresolved_nodes;
    uint32_t error_nodes;
    const char *first_error;
} CmTyckBody;

#define CM_TYCK_ERROR_CLASSES 32u

typedef struct CmTyckSet {
    CmTyArena arena;
    CmVec bodies;  /* CmTyckBody, index = body id - 1 */
    CmVec storage; /* CmTyId blocks */
} CmTyckSet;

typedef struct CmTyckResult {
    size_t bodies;
    size_t typed;
    size_t partial;
    size_t skipped;
    size_t expressions;
    size_t unresolved_nodes;
    size_t error_nodes;
    struct {
        const char *reason;
        size_t count;
    } error_classes[CM_TYCK_ERROR_CLASSES];
    size_t error_class_count;
} CmTyckResult;

void cm_tyck_set_init(CmTyckSet *set);
void cm_tyck_set_destroy(CmTyckSet *set);
const CmTyckBody *cm_tyck_get(const CmTyckSet *set, CmHirBodyId body);

CmTyckResult cm_tyck_all(CmTyckSet *set, const CmHirContext *hir,
    const CmUBodySet *bodies, const CmModuleGraph *graph,
    CmModuleGraphRevision revision, const CmImportResolver *imports,
    const CmHirModuleMap *modules,
    const CmUBodyDependency *dependencies, size_t dependency_count);

#endif
