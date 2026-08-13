#ifndef CMRUSTC_CM_HIR_TYPE_ALIAS_H
#define CMRUSTC_CM_HIR_TYPE_ALIAS_H

#include "cm/hir/model.h"

typedef enum CmHirTypeAliasStatus {
    CM_HIR_TYPE_ALIAS_OK = 0,
    CM_HIR_TYPE_ALIAS_INVALID_ARGUMENT,
    CM_HIR_TYPE_ALIAS_INVALID_TYPE,
    CM_HIR_TYPE_ALIAS_INVALID_ALIAS,
    CM_HIR_TYPE_ALIAS_ARGUMENT_COUNT,
    CM_HIR_TYPE_ALIAS_ARGUMENT_KIND,
    CM_HIR_TYPE_ALIAS_CYCLE,
    CM_HIR_TYPE_ALIAS_UNSUPPORTED_CONST,
    CM_HIR_TYPE_ALIAS_UNSUPPORTED_DYN_TRAIT,
    CM_HIR_TYPE_ALIAS_UNSUPPORTED_OPAQUE,
    CM_HIR_TYPE_ALIAS_RECURSION_LIMIT,
    CM_HIR_TYPE_ALIAS_HIR_FAILURE
} CmHirTypeAliasStatus;

typedef struct CmHirTypeAliasResult {
    CmHirTypeAliasStatus status;
    CmHirTypeId type;
    CmHirTypeId source_type;
    CmHirDefId alias_definition;
    CmHirGenericParamId parameter;
    CmHirStatus hir_status;
    size_t allocated_type_count;
} CmHirTypeAliasResult;

/*
 * Recursively remove transient CM_HIR_TYPE_ALIAS_APPLICATION_KIND values from
 * one type root. Lifetime and type parameters owned by each alias definition
 * are substituted through references, pointers, tuples, arrays, slices,
 * function pointers, and nominal generic arguments. Nominal definition IDs
 * are never rewritten.
 *
 * The returned type is either the unchanged input ID or an append-only
 * structural replacement. Existing types are never modified. Unresolved
 * projections are preserved while their self type and trait/associated type
 * arguments are normalized structurally; this pass never selects an impl. On
 * failure, all types and arena storage allocated by this call are rewound.
 * Target-typed const-parameter arguments on nominal ADTs are preserved when
 * their source parameter has the same authenticated scalar type. Const
 * substitution inside an active alias frame, opaque types,
 * excessive-recursion, malformed-alias, and active alias-cycle cases are
 * explicit errors.
 */
CmHirTypeAliasResult cm_hir_normalize_type_aliases(CmHirContext *context,
    CmHirTypeId root);

/*
 * Instantiate one type root with the exact generic arguments for a bound item
 * definition. The owner's generic parameters must form its declared
 * contiguous range, and argument_count must equal that range's count.
 * Arguments must have matching kinds; type arguments must already be
 * alias-normalized. Const parameters are not supported.
 *
 * The owner arguments form an outer substitution frame while the existing
 * alias normalizer expands aliases and substitutes parameters recursively.
 * A root that is exactly one substituted type parameter returns the argument's
 * existing type ID without adding a type. Structural changes are append-only.
 * On ordinary failure, every type and arena allocation made by this call is
 * rewound. Existing types and the caller-owned argument array are never
 * modified.
 */
CmHirTypeAliasResult cm_hir_instantiate_type(CmHirContext *context,
    CmHirTypeId root, CmHirDefId owner_definition,
    const CmHirGenericArg *arguments, uint32_t argument_count);

const char *cm_hir_type_alias_status_name(CmHirTypeAliasStatus status);

#endif
