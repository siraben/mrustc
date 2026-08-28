#ifndef CMRUSTC_CM_MIR_ULOWER_H
#define CMRUSTC_CM_MIR_ULOWER_H

#include "cm/hir/tyck.h"

/*
 * M9-05 ubody->MIR lowering, grown by census.  The v1 walker executes the
 * lowering's control-flow skeleton over every fully typed ubody and
 * classifies each body: lowered (every construct in the v1 vocabulary) or
 * first-blocked by a named construct class.  Real MIR construction lands
 * class-by-class behind the same walk.
 */

#define CM_MIR_ULOWER_CLASSES 40u

typedef struct CmMirULowerResult {
    size_t bodies;      /* mir-ready bodies attempted */
    size_t lowered;     /* full walk succeeded */
    size_t blocked;     /* walk hit an unsupported construct */
    size_t statements;  /* v1 construction: assigns the build would emit */
    size_t blocks;      /* v1 construction: extra basic blocks */
    struct {
        const char *reason;
        size_t count;
    } classes[CM_MIR_ULOWER_CLASSES];
    size_t class_count;
} CmMirULowerResult;

CmMirULowerResult cm_mir_ulower_all(const CmHirContext *hir,
    const CmUBodySet *bodies, const CmTyckSet *tyck);

#endif
