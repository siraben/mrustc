#ifndef CMRUSTC_CM_HIR_IDS_H
#define CMRUSTC_CM_HIR_IDS_H

#include "cm/config.h"

/*
 * Every arena ID is one-based.  Zero is reserved for an explicitly absent
 * optional relation; it is never a valid entity and never denotes a default
 * value.  IDs remain valid until the owning CmHirContext is destroyed.
 */
typedef uint32_t CmHirCrateId;
typedef uint32_t CmHirModuleId;
typedef uint32_t CmHirItemId;
typedef uint32_t CmHirBodyId;
typedef uint32_t CmHirClosureId;
typedef uint32_t CmHirExprId;
typedef uint32_t CmHirTypeId;
typedef uint32_t CmHirGenericParamId;

#define CM_HIR_CRATE_NONE ((CmHirCrateId)0u)
#define CM_HIR_MODULE_NONE ((CmHirModuleId)0u)
#define CM_HIR_ITEM_NONE ((CmHirItemId)0u)
#define CM_HIR_BODY_NONE ((CmHirBodyId)0u)
#define CM_HIR_CLOSURE_NONE ((CmHirClosureId)0u)
#define CM_HIR_EXPR_NONE ((CmHirExprId)0u)
#define CM_HIR_TYPE_NONE ((CmHirTypeId)0u)
#define CM_HIR_GENERIC_PARAM_NONE ((CmHirGenericParamId)0u)

/* A definition index is local to its crate, like Rust's DefId. */
typedef struct CmHirDefId {
    CmHirCrateId crate_id;
    uint32_t index;
} CmHirDefId;

#define CM_HIR_DEF_INDEX_NONE ((uint32_t)0u)

CmHirDefId cm_hir_def_id_none(void);
int cm_hir_def_id_is_none(CmHirDefId id);
int cm_hir_def_id_equal(CmHirDefId left, CmHirDefId right);

#endif
