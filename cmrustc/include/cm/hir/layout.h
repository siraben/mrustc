#ifndef CMRUSTC_CM_HIR_LAYOUT_H
#define CMRUSTC_CM_HIR_LAYOUT_H

#include "cm/hir/model.h"

typedef enum CmHirLayoutStatus {
    CM_HIR_LAYOUT_OK = 0,
    CM_HIR_LAYOUT_INVALID_ARGUMENT,
    CM_HIR_LAYOUT_INVALID_DEFINITION,
    CM_HIR_LAYOUT_UNSUPPORTED_TYPE,
    CM_HIR_LAYOUT_RECURSIVE_TYPE,
    CM_HIR_LAYOUT_INSUFFICIENT_CAPACITY,
    CM_HIR_LAYOUT_OVERFLOW
} CmHirLayoutStatus;

typedef struct CmHirFieldLayout {
    CmHirTypeId type;
    size_t offset;
    size_t size;
    size_t alignment;
} CmHirFieldLayout;

typedef struct CmHirNamedStructLayout {
    CmHirDefId definition;
    size_t size;
    size_t alignment;
    uint32_t field_count;
} CmHirNamedStructLayout;

/*
 * Compute declaration-order layout for one local nongeneric named struct.
 * The initial exact leaf set is i32/u32 and recursively nested structs from
 * the same crate. Empty, generic, repr-modified, recursive, tuple, union, and
 * cross-crate shapes reject. `pointer_bits` is 32 or 64 and bounds every
 * offset and size even when the host running cmrustc is wider.
 *
 * Results are atomic: `out_layout` and `out_fields` remain unchanged on
 * failure. The caller owns all output storage; no layout cache is retained.
 */
CmHirLayoutStatus cm_hir_layout_named_struct(const CmHirContext *context,
    unsigned int pointer_bits, CmHirDefId definition,
    CmHirNamedStructLayout *out_layout, CmHirFieldLayout *out_fields,
    uint32_t field_capacity);

const char *cm_hir_layout_status_name(CmHirLayoutStatus status);

#endif
