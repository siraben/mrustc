#include "cm/hir/trait_solver.h"

#include "cm/hir/projection.h"

#include "cm/alloc.h"
#include "cm/vec.h"

#include <stdlib.h>
#include <string.h>

#define CM_TRAIT_SOLVER_MAX_RECURSION 256u

typedef struct CmTraitImplIndexState {
    const CmHirContext *hir;
    CmVec entries;
    uint64_t hir_storage_lifetime_id;
    uint64_t hir_semantic_generation;
    uint64_t hir_rewind_generation;
    size_t hir_item_count;
    size_t hir_type_count;
    size_t hir_generic_parameter_count;
    size_t hir_definition_count;
    size_t hir_crate_count;
    size_t hir_module_count;
    CmHirCrateId local_crate;
    CmTraitImplUniverse universe;
} CmTraitImplIndexState;

typedef enum CmTraitTypeScan {
    CM_TRAIT_TYPE_SCAN_CONCRETE = 0,
    CM_TRAIT_TYPE_SCAN_INFERENCE,
    CM_TRAIT_TYPE_SCAN_PROJECTION,
    CM_TRAIT_TYPE_SCAN_UNSUPPORTED,
    CM_TRAIT_TYPE_SCAN_OVERFLOW,
    CM_TRAIT_TYPE_SCAN_INVALID
} CmTraitTypeScan;

typedef enum CmTraitMatchKind {
    CM_TRAIT_MATCH_NO = 0,
    CM_TRAIT_MATCH_YES,
    CM_TRAIT_MATCH_NONPROVEN,
    CM_TRAIT_MATCH_UNSUPPORTED,
    CM_TRAIT_MATCH_OVERFLOW,
    CM_TRAIT_MATCH_TYPECK_FAILURE
} CmTraitMatchKind;

typedef struct CmTraitMatchResult {
    CmTraitMatchKind kind;
    CmTraitSolverResultKind solver_kind;
    CmTypeckStatus typeck_status;
} CmTraitMatchResult;

typedef struct CmTraitProjectionMatchGoal {
    CmHirDefId associated_definition;
    CmTypeckTypeId expected_type;
} CmTraitProjectionMatchGoal;

static CmTraitSelectionResult cm_trait_result(CmTraitSolverResultKind kind)
{
    CmTraitSelectionResult result;

    memset(&result, 0, sizeof(result));
    result.kind = kind;
    result.impl_definition = cm_hir_def_id_none();
    result.impl_item = CM_HIR_ITEM_NONE;
    result.impl_associated_definition = cm_hir_def_id_none();
    result.typeck_status = CM_TYPECK_OK;
    return result;
}

static CmTraitImplIndexState *cm_trait_index_state(CmTraitImplIndex *index)
{
    return index == NULL ? NULL : (CmTraitImplIndexState *)index->state;
}

static const CmTraitImplIndexState *cm_trait_index_state_const(
    const CmTraitImplIndex *index)
{
    return index == NULL ? NULL
        : (const CmTraitImplIndexState *)index->state;
}

static int cm_trait_index_is_current(const CmTraitImplIndexState *state)
{
    return state != NULL && state->hir != NULL
        && state->hir->storage.lifetime_id
            == state->hir_storage_lifetime_id
        && state->hir->semantic_generation
            == state->hir_semantic_generation
        && state->hir->rewind_generation == state->hir_rewind_generation
        && state->hir->items.len == state->hir_item_count
        && state->hir->types.len == state->hir_type_count
        && state->hir->generic_parameters.len
            == state->hir_generic_parameter_count
        && state->hir->definitions.len == state->hir_definition_count
        && state->hir->crates.len == state->hir_crate_count
        && state->hir->modules.len == state->hir_module_count;
}

static int cm_trait_def_compare(CmHirDefId left, CmHirDefId right)
{
    if (left.crate_id != right.crate_id) {
        return left.crate_id < right.crate_id ? -1 : 1;
    }
    if (left.index != right.index) return left.index < right.index ? -1 : 1;
    return 0;
}

static int cm_trait_entry_compare(const void *left_value,
    const void *right_value)
{
    const CmTraitImplIndexEntry *left;
    const CmTraitImplIndexEntry *right;
    int definition_order;

    left = (const CmTraitImplIndexEntry *)left_value;
    right = (const CmTraitImplIndexEntry *)right_value;
    definition_order = cm_trait_def_compare(left->trait_definition,
        right->trait_definition);
    if (definition_order != 0) return definition_order;
    if (left->self_head != right->self_head) {
        return left->self_head < right->self_head ? -1 : 1;
    }
    definition_order = cm_trait_def_compare(left->self_head_definition,
        right->self_head_definition);
    if (definition_order != 0) return definition_order;
    if (left->item != right->item) return left->item < right->item ? -1 : 1;
    return cm_trait_def_compare(left->impl_definition,
        right->impl_definition);
}

static CmTraitImplHeadKind cm_trait_hir_head(const CmHirContext *hir,
    CmHirTypeId type_id, CmHirDefId *out_definition)
{
    const CmHirType *type;

    *out_definition = cm_hir_def_id_none();
    type = cm_hir_get_type(hir, type_id);
    if (type == NULL) return CM_TRAIT_IMPL_HEAD_WILDCARD;
    switch (type->kind) {
    case CM_HIR_TYPE_NEVER_KIND: return CM_TRAIT_IMPL_HEAD_NEVER;
    case CM_HIR_TYPE_UNIT_KIND: return CM_TRAIT_IMPL_HEAD_UNIT;
    case CM_HIR_TYPE_BOOL_KIND: return CM_TRAIT_IMPL_HEAD_BOOL;
    case CM_HIR_TYPE_CHAR_KIND: return CM_TRAIT_IMPL_HEAD_CHAR;
    case CM_HIR_TYPE_STR_KIND: return CM_TRAIT_IMPL_HEAD_STR;
    case CM_HIR_TYPE_INTEGER_KIND: return CM_TRAIT_IMPL_HEAD_INTEGER;
    case CM_HIR_TYPE_FLOAT_KIND: return CM_TRAIT_IMPL_HEAD_FLOAT;
    case CM_HIR_TYPE_REFERENCE_KIND: return CM_TRAIT_IMPL_HEAD_REFERENCE;
    case CM_HIR_TYPE_RAW_POINTER_KIND: return CM_TRAIT_IMPL_HEAD_RAW_POINTER;
    case CM_HIR_TYPE_TUPLE_KIND: return CM_TRAIT_IMPL_HEAD_TUPLE;
    case CM_HIR_TYPE_ARRAY_KIND: return CM_TRAIT_IMPL_HEAD_ARRAY;
    case CM_HIR_TYPE_SLICE_KIND: return CM_TRAIT_IMPL_HEAD_SLICE;
    case CM_HIR_TYPE_FN_POINTER_KIND: return CM_TRAIT_IMPL_HEAD_FN_POINTER;
    case CM_HIR_TYPE_ADT_KIND:
        *out_definition = type->data.named_type.definition;
        return CM_TRAIT_IMPL_HEAD_NAMED;
    case CM_HIR_TYPE_ERROR_KIND:
    case CM_HIR_TYPE_INFER_KIND:
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
    case CM_HIR_TYPE_SELF_KIND:
    case CM_HIR_TYPE_PARAMETER_KIND:
    case CM_HIR_TYPE_PROJECTION_KIND:
    case CM_HIR_TYPE_DYN_TRAIT_KIND:
    case CM_HIR_TYPE_OPAQUE_KIND:
    case CM_HIR_TYPE_CLOSURE_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
        return CM_TRAIT_IMPL_HEAD_WILDCARD;
    }
    return CM_TRAIT_IMPL_HEAD_WILDCARD;
}

typedef struct CmTraitHirScanState {
    const CmHirContext *hir;
    CmHirDefId allowed_type_parameter_owner;
    CmHirGenericParamId allowed_type_parameter_start;
    uint32_t allowed_type_parameter_count;
    unsigned char *seen_type_parameters;
    unsigned char *visit_state;
    unsigned int *memo;
    size_t *height;
    size_t type_count;
} CmTraitHirScanState;

static unsigned int cm_trait_scan_hir_const(CmTraitHirScanState *scan,
    const CmHirConstArg *constant, size_t depth, size_t *out_height);

static unsigned int cm_trait_scan_hir_type_inner(CmTraitHirScanState *scan,
    CmHirTypeId type_id, size_t depth, size_t *out_height);

static unsigned int cm_trait_scan_hir_args(CmTraitHirScanState *scan,
    const CmHirGenericArg *arguments, uint32_t count, size_t depth,
    size_t *out_height)
{
    unsigned int flags;
    size_t ignored_height;
    uint32_t index;

    *out_height = 0u;
    if ((count == 0u) != (arguments == NULL)) {
        return CM_TRAIT_IMPL_UNSUPPORTED_TYPE;
    }
    flags = CM_TRAIT_IMPL_UNSUPPORTED_NONE;
    for (index = 0u; index < count; ++index) {
        switch (arguments[index].kind) {
        case CM_HIR_GENERIC_ARG_LIFETIME:
            if (arguments[index].data.lifetime.kind
                    != CM_HIR_REGION_STATIC
                && arguments[index].data.lifetime.kind
                    != CM_HIR_REGION_ERASED) {
                flags |= CM_TRAIT_IMPL_UNSUPPORTED_NON_MONOMORPHIC;
            }
            break;
        case CM_HIR_GENERIC_ARG_TYPE:
            flags |= cm_trait_scan_hir_type_inner(scan,
                arguments[index].data.type, depth + 1u,
                &ignored_height);
            if (ignored_height > *out_height) *out_height = ignored_height;
            break;
        case CM_HIR_GENERIC_ARG_CONST:
            flags |= cm_trait_scan_hir_const(scan,
                &arguments[index].data.constant, depth,
                &ignored_height);
            if (ignored_height > *out_height) *out_height = ignored_height;
            break;
        default:
            flags |= CM_TRAIT_IMPL_UNSUPPORTED_TYPE;
            break;
        }
    }
    return flags;
}

static unsigned int cm_trait_scan_hir_const(CmTraitHirScanState *scan,
    const CmHirConstArg *constant, size_t depth, size_t *out_height)
{
    unsigned int flags;

    *out_height = 0u;
    if (depth >= CM_TRAIT_SOLVER_MAX_RECURSION || constant == NULL) {
        return CM_TRAIT_IMPL_UNSUPPORTED_TYPE;
    }
    {
        flags = cm_trait_scan_hir_type_inner(scan, constant->type,
            depth + 1u, out_height);
    }
    if (constant->kind != CM_HIR_CONST_VALUE) {
        flags |= CM_TRAIT_IMPL_UNSUPPORTED_NON_MONOMORPHIC;
    }
    return flags;
}

static unsigned int cm_trait_scan_hir_type_inner(CmTraitHirScanState *scan,
    CmHirTypeId type_id, size_t depth, size_t *out_height)
{
    const CmHirType *type;
    unsigned int flags;
    size_t maximum_child_height;
    uint32_t index;

    *out_height = 0u;
    if (depth >= CM_TRAIT_SOLVER_MAX_RECURSION) {
        return CM_TRAIT_IMPL_UNSUPPORTED_TYPE;
    }
    if (type_id == CM_HIR_TYPE_NONE
        || (size_t)type_id > scan->type_count) {
        return CM_TRAIT_IMPL_UNSUPPORTED_TYPE;
    }
    if (scan->visit_state[(size_t)type_id - 1u] == 2u) {
        *out_height = scan->height[(size_t)type_id - 1u];
        if (*out_height > CM_TRAIT_SOLVER_MAX_RECURSION - depth) {
            return CM_TRAIT_IMPL_UNSUPPORTED_TYPE;
        }
        return scan->memo[(size_t)type_id - 1u];
    }
    if (scan->visit_state[(size_t)type_id - 1u] == 1u) {
        return CM_TRAIT_IMPL_UNSUPPORTED_TYPE;
    }
    type = cm_hir_get_type(scan->hir, type_id);
    if (type == NULL) return CM_TRAIT_IMPL_UNSUPPORTED_TYPE;
    scan->visit_state[(size_t)type_id - 1u] = 1u;
    flags = CM_TRAIT_IMPL_UNSUPPORTED_NONE;
    maximum_child_height = 0u;
    switch (type->kind) {
    case CM_HIR_TYPE_NEVER_KIND:
    case CM_HIR_TYPE_UNIT_KIND:
    case CM_HIR_TYPE_BOOL_KIND:
    case CM_HIR_TYPE_CHAR_KIND:
    case CM_HIR_TYPE_STR_KIND:
    case CM_HIR_TYPE_INTEGER_KIND:
    case CM_HIR_TYPE_FLOAT_KIND:
        break;
    case CM_HIR_TYPE_REFERENCE_KIND:
        if (type->data.reference_type.region.kind != CM_HIR_REGION_STATIC
            && type->data.reference_type.region.kind
                != CM_HIR_REGION_ERASED) {
            flags |= CM_TRAIT_IMPL_UNSUPPORTED_NON_MONOMORPHIC;
        }
        flags |= cm_trait_scan_hir_type_inner(scan,
            type->data.reference_type.pointee, depth + 1u,
            &maximum_child_height);
        break;
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        flags |= cm_trait_scan_hir_type_inner(scan,
            type->data.raw_pointer_type.pointee, depth + 1u,
            &maximum_child_height);
        break;
    case CM_HIR_TYPE_TUPLE_KIND:
        for (index = 0u; index < type->data.tuple_type.element_count;
             ++index) {
            flags |= cm_trait_scan_hir_type_inner(scan,
                type->data.tuple_type.elements[index], depth + 1u,
                out_height);
            if (*out_height > maximum_child_height) {
                maximum_child_height = *out_height;
            }
        }
        break;
    case CM_HIR_TYPE_ARRAY_KIND:
        flags |= cm_trait_scan_hir_type_inner(scan,
                type->data.array_type.element, depth + 1u,
                &maximum_child_height);
        flags |= cm_trait_scan_hir_const(scan,
                &type->data.array_type.length, depth, out_height);
        if (*out_height > maximum_child_height) {
            maximum_child_height = *out_height;
        }
        break;
    case CM_HIR_TYPE_SLICE_KIND:
        flags |= cm_trait_scan_hir_type_inner(scan,
            type->data.slice_type.element, depth + 1u,
            &maximum_child_height);
        break;
    case CM_HIR_TYPE_FN_POINTER_KIND:
        for (index = 0u;
             index < type->data.fn_pointer_type.parameter_count; ++index) {
            flags |= cm_trait_scan_hir_type_inner(scan,
                type->data.fn_pointer_type.parameters[index], depth + 1u,
                out_height);
            if (*out_height > maximum_child_height) {
                maximum_child_height = *out_height;
            }
        }
        flags |= cm_trait_scan_hir_type_inner(scan,
            type->data.fn_pointer_type.return_type, depth + 1u,
            out_height);
        if (*out_height > maximum_child_height) {
            maximum_child_height = *out_height;
        }
        break;
    case CM_HIR_TYPE_ADT_KIND:
        flags |= cm_trait_scan_hir_args(scan,
            type->data.named_type.arguments,
            type->data.named_type.argument_count, depth, out_height);
        if (*out_height > maximum_child_height) {
            maximum_child_height = *out_height;
        }
        break;
    case CM_HIR_TYPE_PROJECTION_KIND:
        flags |= CM_TRAIT_IMPL_UNSUPPORTED_PROJECTION;
        break;
    case CM_HIR_TYPE_PARAMETER_KIND:
    {
        const CmHirGenericParam *parameter;

        parameter = cm_hir_get_generic_param(scan->hir,
            type->data.parameter_type.parameter);
        if (parameter != NULL && parameter->kind == CM_HIR_GENERIC_TYPE
            && !cm_hir_def_id_is_none(
                scan->allowed_type_parameter_owner)
            && cm_hir_def_id_equal(parameter->owner,
                scan->allowed_type_parameter_owner)
            && parameter->index < scan->allowed_type_parameter_count
            && type->data.parameter_type.parameter
                == scan->allowed_type_parameter_start + parameter->index) {
            if (scan->seen_type_parameters != NULL) {
                scan->seen_type_parameters[parameter->index] = 1u;
            }
            break;
        }
        flags |= CM_TRAIT_IMPL_UNSUPPORTED_NON_MONOMORPHIC;
        break;
    }
    case CM_HIR_TYPE_SELF_KIND:
    case CM_HIR_TYPE_INFER_KIND:
        flags |= CM_TRAIT_IMPL_UNSUPPORTED_NON_MONOMORPHIC;
        break;
    case CM_HIR_TYPE_ERROR_KIND:
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
    case CM_HIR_TYPE_DYN_TRAIT_KIND:
    case CM_HIR_TYPE_OPAQUE_KIND:
    case CM_HIR_TYPE_CLOSURE_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
        flags |= CM_TRAIT_IMPL_UNSUPPORTED_TYPE;
        break;
    }
    *out_height = maximum_child_height + 1u;
    if (*out_height > CM_TRAIT_SOLVER_MAX_RECURSION - depth) {
        return flags | CM_TRAIT_IMPL_UNSUPPORTED_TYPE;
    }
    scan->visit_state[(size_t)type_id - 1u] = 2u;
    scan->memo[(size_t)type_id - 1u] = flags;
    scan->height[(size_t)type_id - 1u] = *out_height;
    return flags;
}

static unsigned int cm_trait_scan_hir_type(const CmHirContext *hir,
    CmHirTypeId type_id, CmHirDefId allowed_type_parameter_owner,
    CmHirGenericParamId allowed_type_parameter_start,
    uint32_t allowed_type_parameter_count,
    unsigned char *seen_type_parameters)
{
    CmTraitHirScanState scan;
    unsigned int result;
    size_t height;

    memset(&scan, 0, sizeof(scan));
    scan.hir = hir;
    scan.allowed_type_parameter_owner = allowed_type_parameter_owner;
    scan.allowed_type_parameter_start = allowed_type_parameter_start;
    scan.allowed_type_parameter_count = allowed_type_parameter_count;
    scan.seen_type_parameters = seen_type_parameters;
    scan.type_count = hir->types.len;
    scan.visit_state = (unsigned char *)cm_alloc_zeroed(scan.type_count,
        sizeof(unsigned char));
    scan.memo = (unsigned int *)cm_alloc_zeroed(scan.type_count,
        sizeof(unsigned int));
    scan.height = (size_t *)cm_alloc_zeroed(scan.type_count,
        sizeof(size_t));
    result = cm_trait_scan_hir_type_inner(&scan, type_id, 0u, &height);
    cm_free(scan.height);
    cm_free(scan.memo);
    cm_free(scan.visit_state);
    return result;
}

static unsigned int cm_trait_scan_hir_named_args(const CmHirContext *hir,
    const CmHirGenericArg *arguments, uint32_t argument_count,
    CmHirDefId allowed_type_parameter_owner,
    CmHirGenericParamId allowed_type_parameter_start,
    uint32_t allowed_type_parameter_count,
    unsigned char *seen_type_parameters)
{
    CmTraitHirScanState scan;
    unsigned int result;
    size_t height;

    memset(&scan, 0, sizeof(scan));
    scan.hir = hir;
    scan.allowed_type_parameter_owner = allowed_type_parameter_owner;
    scan.allowed_type_parameter_start = allowed_type_parameter_start;
    scan.allowed_type_parameter_count = allowed_type_parameter_count;
    scan.seen_type_parameters = seen_type_parameters;
    scan.type_count = hir->types.len;
    scan.visit_state = (unsigned char *)cm_alloc_zeroed(scan.type_count,
        sizeof(unsigned char));
    scan.memo = (unsigned int *)cm_alloc_zeroed(scan.type_count,
        sizeof(unsigned int));
    scan.height = (size_t *)cm_alloc_zeroed(scan.type_count,
        sizeof(size_t));
    result = cm_trait_scan_hir_args(&scan, arguments, argument_count, 0u,
        &height);
    cm_free(scan.height);
    cm_free(scan.memo);
    cm_free(scan.visit_state);
    return result;
}

static const CmHirItem *cm_trait_find_definition_item(
    const CmHirContext *hir, CmHirDefId definition)
{
    size_t index;

    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *item;

        item = cm_hir_get_item(hir, (CmHirItemId)(index + 1u));
        if (item != NULL
            && cm_hir_def_id_equal(item->definition, definition)) {
            return item;
        }
    }
    return NULL;
}

static int cm_trait_definition_is_known_foreign(
    const CmTraitImplIndexState *state, CmHirDefId definition,
    CmHirItemKind expected_kind)
{
    const CmHirDefinition *record;

    if (state == NULL || definition.crate_id == CM_HIR_CRATE_NONE
        || definition.index == CM_HIR_DEF_INDEX_NONE) return 0;
    if (definition.crate_id == state->local_crate
        || cm_hir_get_crate(state->hir, definition.crate_id) == NULL) {
        return 0;
    }
    record = cm_hir_lookup_definition(state->hir, definition);
    return record != NULL && record->kind == CM_HIR_DEFINITION_ITEM
        && record->state == CM_HIR_DEFINITION_RESERVED
        && record->has_reserved_item_kind
        && record->reserved_item_kind == expected_kind;
}

static int cm_trait_impl_type_generics_supported(const CmHirContext *hir,
    const CmHirItem *item)
{
    size_t parameter_offset;
    uint32_t index;

    if (item->generic_parameter_count == 0u) return 1;
    if (item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE) {
        return 0;
    }
    parameter_offset = (size_t)item->generic_parameter_start - 1u;
    if (parameter_offset > hir->generic_parameters.len
        || (size_t)item->generic_parameter_count
            > hir->generic_parameters.len - parameter_offset) {
        return 0;
    }
    for (index = 0u; index < item->generic_parameter_count; ++index) {
        const CmHirGenericParam *parameter;

        parameter = cm_hir_get_generic_param(hir,
            item->generic_parameter_start + index);
        if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_TYPE
            || !cm_hir_def_id_equal(parameter->owner, item->definition)
            || parameter->index != index || parameter->has_default) {
            return 0;
        }
    }
    return 1;
}

static int cm_trait_impl_predicates_supported(const CmHirContext *hir,
    const CmHirItem *item)
{
    uint32_t index;

    if (item->predicate_scope_count != 0u) return 0;
    for (index = 0u; index < item->predicate_count; ++index) {
        const CmHirTraitPredicate *predicate;
        const CmHirItem *trait_item;

        predicate = &item->predicates[index];
        if (predicate->scope != CM_HIR_PREDICATE_SCOPE_NONE
            || predicate->binder.lifetime_count != 0u
            || predicate->modifier != CM_HIR_PREDICATE_REQUIRED
            || predicate->equality_count != 0u
            || cm_trait_scan_hir_type(hir, predicate->subject,
                item->definition, item->generic_parameter_start,
                item->generic_parameter_count, NULL)
                != CM_TRAIT_IMPL_UNSUPPORTED_NONE
            || cm_trait_scan_hir_named_args(hir,
                predicate->trait_type.arguments,
                predicate->trait_type.argument_count, item->definition,
                item->generic_parameter_start,
                item->generic_parameter_count, NULL)
                != CM_TRAIT_IMPL_UNSUPPORTED_NONE) {
            return 0;
        }
        trait_item = cm_trait_find_definition_item(hir,
            predicate->trait_type.definition);
        if (trait_item == NULL || trait_item->kind != CM_HIR_ITEM_TRAIT
            || trait_item->data.trait_item.is_auto) return 0;
    }
    return 1;
}

static CmTraitSolverResultKind cm_trait_impl_index_init_internal(
    CmTraitImplIndex *index,
    const CmHirContext *hir, CmHirCrateId local_crate,
    CmTraitImplUniverse universe)
{
    CmTraitImplIndexState *state;
    size_t item_index;

    if (index == NULL || index->state != NULL || hir == NULL
        || cm_hir_get_crate(hir, local_crate) == NULL
        || (universe != CM_TRAIT_IMPL_UNIVERSE_OPEN
            && universe
                != CM_TRAIT_IMPL_UNIVERSE_SINGLE_LOCAL_CRATE_COMPLETE)) {
        return CM_TRAIT_SOLVER_INVALID;
    }
    state = (CmTraitImplIndexState *)cm_alloc_zeroed(1u,
        sizeof(CmTraitImplIndexState));
    state->hir = hir;
    state->hir_storage_lifetime_id = hir->storage.lifetime_id;
    state->hir_semantic_generation = hir->semantic_generation;
    state->hir_rewind_generation = hir->rewind_generation;
    state->hir_item_count = hir->items.len;
    state->hir_type_count = hir->types.len;
    state->hir_generic_parameter_count = hir->generic_parameters.len;
    state->hir_definition_count = hir->definitions.len;
    state->hir_crate_count = hir->crates.len;
    state->hir_module_count = hir->modules.len;
    state->local_crate = local_crate;
    state->universe = universe;
    cm_vec_init(&state->entries, sizeof(CmTraitImplIndexEntry));
    for (item_index = 0u; item_index < hir->items.len; ++item_index) {
        const CmHirItem *item;
        const CmHirItem *trait_item;
        CmTraitImplIndexEntry entry;
        CmHirDefId allowed_type_parameter_owner;
        unsigned char *seen_type_parameters;
        uint32_t generic_index;

        item = cm_hir_get_item(hir, (CmHirItemId)(item_index + 1u));
        if (item == NULL || item->kind != CM_HIR_ITEM_IMPL
            || !item->data.impl_item.has_trait) continue;
        memset(&entry, 0, sizeof(entry));
        entry.trait_definition = item->data.impl_item.trait_type.definition;
        entry.self_head = cm_trait_hir_head(hir,
            item->data.impl_item.self_type, &entry.self_head_definition);
        entry.item = (CmHirItemId)(item_index + 1u);
        entry.impl_definition = item->definition;
        allowed_type_parameter_owner = cm_hir_def_id_none();
        seen_type_parameters = NULL;
        if (!cm_trait_impl_type_generics_supported(hir, item)) {
            entry.unsupported_flags |= CM_TRAIT_IMPL_UNSUPPORTED_GENERIC;
        } else if (item->generic_parameter_count != 0u) {
            allowed_type_parameter_owner = item->definition;
            seen_type_parameters = (unsigned char *)cm_alloc_zeroed(
                item->generic_parameter_count, sizeof(unsigned char));
        }
        if (!cm_trait_impl_predicates_supported(hir, item)) {
            entry.unsupported_flags |= CM_TRAIT_IMPL_UNSUPPORTED_PREDICATE;
        }
        if (item->outlives_predicate_count != 0u) {
            entry.unsupported_flags |= CM_TRAIT_IMPL_UNSUPPORTED_OUTLIVES;
        }
        if (item->data.impl_item.is_negative) {
            entry.unsupported_flags |= CM_TRAIT_IMPL_UNSUPPORTED_NEGATIVE;
        }
        trait_item = cm_trait_find_definition_item(hir,
            entry.trait_definition);
        if (trait_item == NULL || trait_item->kind != CM_HIR_ITEM_TRAIT) {
            entry.unsupported_flags |= CM_TRAIT_IMPL_UNSUPPORTED_TYPE;
        } else if (trait_item->data.trait_item.is_auto) {
            entry.unsupported_flags |= CM_TRAIT_IMPL_UNSUPPORTED_AUTO_TRAIT;
        }
        entry.unsupported_flags |= cm_trait_scan_hir_type(hir,
            item->data.impl_item.self_type,
            allowed_type_parameter_owner, item->generic_parameter_start,
            item->generic_parameter_count, seen_type_parameters);
        entry.unsupported_flags |= cm_trait_scan_hir_named_args(hir,
            item->data.impl_item.trait_type.arguments,
            item->data.impl_item.trait_type.argument_count,
            allowed_type_parameter_owner, item->generic_parameter_start,
            item->generic_parameter_count, seen_type_parameters);
        for (generic_index = 0u;
             generic_index < item->generic_parameter_count;
             ++generic_index) {
            if (seen_type_parameters == NULL
                || seen_type_parameters[generic_index] == 0u) {
                entry.unsupported_flags |=
                    CM_TRAIT_IMPL_UNSUPPORTED_GENERIC;
            }
        }
        cm_free(seen_type_parameters);
        (void)cm_vec_push(&state->entries, &entry);
    }
    if (state->entries.len > 1u) {
        qsort(state->entries.data, state->entries.len,
            sizeof(CmTraitImplIndexEntry), cm_trait_entry_compare);
    }
    index->state = state;
    return CM_TRAIT_SOLVER_PROVEN;
}

CmTraitSolverResultKind cm_trait_impl_index_init(CmTraitImplIndex *index,
    const CmHirContext *hir, CmHirCrateId local_crate,
    CmTraitImplUniverse universe)
{
    if (universe != CM_TRAIT_IMPL_UNIVERSE_OPEN) {
        return CM_TRAIT_SOLVER_INVALID;
    }
    return cm_trait_impl_index_init_internal(index, hir, local_crate,
        universe);
}

CmTraitSolverResultKind cm_trait_impl_index_init_complete(
    CmTraitImplIndex *index,
    const CmHirCrateFinalization *finalization)
{
    const CmHirContext *hir;
    CmHirCrateId local_crate;

    if (!cm_hir_crate_finalization_is_current(finalization)) {
        return CM_TRAIT_SOLVER_INVALID;
    }
    hir = cm_hir_crate_finalization_hir(finalization);
    local_crate = cm_hir_crate_finalization_crate(finalization);
    if (hir == NULL || local_crate == CM_HIR_CRATE_NONE) {
        return CM_TRAIT_SOLVER_INVALID;
    }
    return cm_trait_impl_index_init_internal(index, hir, local_crate,
        CM_TRAIT_IMPL_UNIVERSE_SINGLE_LOCAL_CRATE_COMPLETE);
}

void cm_trait_impl_index_destroy(CmTraitImplIndex *index)
{
    CmTraitImplIndexState *state;

    state = cm_trait_index_state(index);
    if (state == NULL) return;
    cm_vec_destroy(&state->entries);
    memset(state, 0, sizeof(*state));
    cm_free(state);
    index->state = NULL;
}

int cm_trait_impl_index_is_current(const CmTraitImplIndex *index)
{
    return cm_trait_index_is_current(cm_trait_index_state_const(index));
}

const CmHirContext *cm_trait_impl_index_hir(const CmTraitImplIndex *index)
{
    const CmTraitImplIndexState *state;

    state = cm_trait_index_state_const(index);
    return !cm_trait_index_is_current(state) ? NULL : state->hir;
}

CmTraitImplUniverse cm_trait_impl_index_universe(
    const CmTraitImplIndex *index)
{
    const CmTraitImplIndexState *state;

    state = cm_trait_index_state_const(index);
    return !cm_trait_index_is_current(state)
        ? CM_TRAIT_IMPL_UNIVERSE_OPEN : state->universe;
}

CmHirCrateId cm_trait_impl_index_local_crate(
    const CmTraitImplIndex *index)
{
    const CmTraitImplIndexState *state;

    state = cm_trait_index_state_const(index);
    return !cm_trait_index_is_current(state)
        ? CM_HIR_CRATE_NONE : state->local_crate;
}

size_t cm_trait_impl_index_entry_count(const CmTraitImplIndex *index)
{
    const CmTraitImplIndexState *state;

    state = cm_trait_index_state_const(index);
    return !cm_trait_index_is_current(state) ? 0u : state->entries.len;
}

const CmTraitImplIndexEntry *cm_trait_impl_index_entry(
    const CmTraitImplIndex *index, size_t entry_index)
{
    const CmTraitImplIndexState *state;

    state = cm_trait_index_state_const(index);
    if (!cm_trait_index_is_current(state)) return NULL;
    return (const CmTraitImplIndexEntry *)cm_vec_at_const(&state->entries,
        entry_index);
}

typedef struct CmTraitTypeckScanState {
    const CmTypeckContext *typeck;
    const CmHirContext *hir;
    unsigned char *visit_state;
    CmTraitTypeScan *memo;
    size_t *height;
    size_t type_count;
} CmTraitTypeckScanState;

static CmTraitTypeScan cm_trait_scan_merge(CmTraitTypeScan left,
    CmTraitTypeScan right)
{
    return left > right ? left : right;
}

static int cm_trait_adt_item_kind(CmHirItemKind kind)
{
    return kind == CM_HIR_ITEM_STRUCT || kind == CM_HIR_ITEM_UNION
        || kind == CM_HIR_ITEM_ENUM;
}

static CmTraitTypeScan cm_trait_scan_typeck_type_inner(
    CmTraitTypeckScanState *state, CmTypeckTypeId type_id, size_t depth,
    size_t *out_height);

static CmTraitTypeScan cm_trait_scan_typeck_const_inner(
    CmTraitTypeckScanState *state, const CmTypeckConst *constant,
    size_t depth, size_t *out_height)
{
    const CmHirGenericParam *parameter;
    CmTraitTypeScan type_scan;

    *out_height = 0u;
    if (constant == NULL) return CM_TRAIT_TYPE_SCAN_INVALID;
    type_scan = cm_trait_scan_typeck_type_inner(state, constant->type,
        depth + 1u, out_height);
    if (type_scan == CM_TRAIT_TYPE_SCAN_INVALID
        || type_scan == CM_TRAIT_TYPE_SCAN_OVERFLOW) return type_scan;
    switch (constant->kind) {
    case CM_HIR_CONST_VALUE:
        return type_scan;
    case CM_HIR_CONST_INFER:
        return cm_trait_scan_merge(type_scan,
            CM_TRAIT_TYPE_SCAN_INFERENCE);
    case CM_HIR_CONST_PARAMETER:
        parameter = cm_hir_get_generic_param(state->hir,
            constant->data.parameter);
        if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_CONST) {
            return CM_TRAIT_TYPE_SCAN_INVALID;
        }
        return cm_trait_scan_merge(type_scan,
            CM_TRAIT_TYPE_SCAN_UNSUPPORTED);
    case CM_HIR_CONST_UNEVALUATED:
    case CM_HIR_CONST_ERROR:
        return cm_trait_scan_merge(type_scan,
            CM_TRAIT_TYPE_SCAN_UNSUPPORTED);
    }
    return CM_TRAIT_TYPE_SCAN_INVALID;
}

static CmTraitTypeScan cm_trait_scan_typeck_type_inner(
    CmTraitTypeckScanState *state, CmTypeckTypeId type_id, size_t depth,
    size_t *out_height)
{
    const CmTypeckType *type;
    CmTypeckTypeId resolved;
    CmTypeckStatus status;
    CmTraitTypeScan scan;
    size_t maximum_child_height;
    uint32_t index;

    *out_height = 0u;
    scan = CM_TRAIT_TYPE_SCAN_INVALID;
    if (depth >= CM_TRAIT_SOLVER_MAX_RECURSION) {
        return CM_TRAIT_TYPE_SCAN_OVERFLOW;
    }
    status = cm_typeck_resolve(state->typeck, type_id, &resolved);
    if (status == CM_TYPECK_OVERFLOW) return CM_TRAIT_TYPE_SCAN_OVERFLOW;
    if (status != CM_TYPECK_OK) return CM_TRAIT_TYPE_SCAN_INVALID;
    if (resolved == CM_TYPECK_TYPE_NONE
        || (size_t)resolved > state->type_count) {
        return CM_TRAIT_TYPE_SCAN_INVALID;
    }
    if (state->visit_state[(size_t)resolved - 1u] == 2u) {
        *out_height = state->height[(size_t)resolved - 1u];
        if (*out_height > CM_TRAIT_SOLVER_MAX_RECURSION - depth) {
            return CM_TRAIT_TYPE_SCAN_OVERFLOW;
        }
        return state->memo[(size_t)resolved - 1u];
    }
    if (state->visit_state[(size_t)resolved - 1u] == 1u) {
        return CM_TRAIT_TYPE_SCAN_INVALID;
    }
    type = cm_typeck_get_type(state->typeck, resolved);
    if (type == NULL) return CM_TRAIT_TYPE_SCAN_INVALID;
    state->visit_state[(size_t)resolved - 1u] = 1u;
    maximum_child_height = 0u;
    switch (type->kind) {
    case CM_TYPECK_TYPE_VARIABLE:
        scan = CM_TRAIT_TYPE_SCAN_INFERENCE;
        break;
    case CM_TYPECK_TYPE_NEVER:
    case CM_TYPECK_TYPE_UNIT:
    case CM_TYPECK_TYPE_BOOL:
    case CM_TYPECK_TYPE_CHAR:
    case CM_TYPECK_TYPE_STR:
    case CM_TYPECK_TYPE_INTEGER:
    case CM_TYPECK_TYPE_FLOAT:
        scan = CM_TRAIT_TYPE_SCAN_CONCRETE;
        break;
    case CM_TYPECK_TYPE_REFERENCE:
    {
        CmTraitTypeScan child_scan;
        size_t child_height;

        if (type->data.reference_type.region.kind == CM_HIR_REGION_STATIC
            || type->data.reference_type.region.kind
                == CM_HIR_REGION_ERASED) {
            scan = CM_TRAIT_TYPE_SCAN_CONCRETE;
        } else if (type->data.reference_type.region.kind
                == CM_HIR_REGION_INFER) {
            scan = CM_TRAIT_TYPE_SCAN_INFERENCE;
        } else if (type->data.reference_type.region.kind
                == CM_HIR_REGION_EARLY_BOUND
            || type->data.reference_type.region.kind
                == CM_HIR_REGION_LATE_BOUND) {
            scan = CM_TRAIT_TYPE_SCAN_UNSUPPORTED;
        } else {
            scan = CM_TRAIT_TYPE_SCAN_INVALID;
        }
        child_scan = cm_trait_scan_typeck_type_inner(state,
            type->data.reference_type.pointee, depth + 1u, &child_height);
        if (child_scan == CM_TRAIT_TYPE_SCAN_OVERFLOW) return child_scan;
        scan = cm_trait_scan_merge(scan, child_scan);
        maximum_child_height = child_height;
        break;
    }
    case CM_TYPECK_TYPE_RAW_POINTER:
        scan = cm_trait_scan_typeck_type_inner(state,
            type->data.raw_pointer_type.pointee, depth + 1u,
            &maximum_child_height);
        break;
    case CM_TYPECK_TYPE_TUPLE:
        scan = CM_TRAIT_TYPE_SCAN_CONCRETE;
        for (index = 0u; index < type->data.tuple_type.element_count;
             ++index) {
            CmTraitTypeScan element_scan;
            size_t element_height;

            element_scan = cm_trait_scan_typeck_type_inner(state,
                type->data.tuple_type.elements[index], depth + 1u,
                &element_height);
            if (element_scan == CM_TRAIT_TYPE_SCAN_OVERFLOW) {
                return element_scan;
            }
            scan = cm_trait_scan_merge(scan, element_scan);
            if (element_height > maximum_child_height) {
                maximum_child_height = element_height;
            }
        }
        break;
    case CM_TYPECK_TYPE_ARRAY:
    {
        CmTraitTypeScan child_scan;
        size_t child_height;

        scan = cm_trait_scan_typeck_type_inner(state,
            type->data.array_type.element, depth + 1u,
            &maximum_child_height);
        if (scan == CM_TRAIT_TYPE_SCAN_OVERFLOW) return scan;
        child_scan = cm_trait_scan_typeck_const_inner(state,
            &type->data.array_type.length, depth, &child_height);
        if (child_scan == CM_TRAIT_TYPE_SCAN_OVERFLOW) return child_scan;
        scan = cm_trait_scan_merge(scan, child_scan);
        if (child_height > maximum_child_height) {
            maximum_child_height = child_height;
        }
        break;
    }
    case CM_TYPECK_TYPE_SLICE:
        scan = cm_trait_scan_typeck_type_inner(state,
            type->data.slice_type.element, depth + 1u,
            &maximum_child_height);
        break;
    case CM_TYPECK_TYPE_FN_POINTER:
        scan = CM_TRAIT_TYPE_SCAN_CONCRETE;
        for (index = 0u;
             index < type->data.fn_pointer_type.parameter_count; ++index) {
            CmTraitTypeScan parameter_scan;
            size_t parameter_height;

            parameter_scan = cm_trait_scan_typeck_type_inner(state,
                type->data.fn_pointer_type.parameters[index], depth + 1u,
                &parameter_height);
            if (parameter_scan == CM_TRAIT_TYPE_SCAN_OVERFLOW) {
                return parameter_scan;
            }
            scan = cm_trait_scan_merge(scan, parameter_scan);
            if (parameter_height > maximum_child_height) {
                maximum_child_height = parameter_height;
            }
        }
        {
            CmTraitTypeScan return_scan;
            size_t return_height;

            return_scan = cm_trait_scan_typeck_type_inner(state,
                type->data.fn_pointer_type.return_type, depth + 1u,
                &return_height);
            if (return_scan == CM_TRAIT_TYPE_SCAN_OVERFLOW) {
                return return_scan;
            }
            scan = cm_trait_scan_merge(scan, return_scan);
            if (return_height > maximum_child_height) {
                maximum_child_height = return_height;
            }
        }
        break;
    case CM_TYPECK_TYPE_ADT:
    {
        const CmHirItem *item;

        scan = CM_TRAIT_TYPE_SCAN_CONCRETE;
        item = cm_trait_find_definition_item(state->hir,
            type->data.named_type.definition);
        if (item == NULL || !cm_trait_adt_item_kind(item->kind)
            || item->generic_parameter_count
                != type->data.named_type.argument_count) {
            scan = CM_TRAIT_TYPE_SCAN_INVALID;
        }
        for (index = 0u; index < type->data.named_type.argument_count;
             ++index) {
            const CmTypeckGenericArg *argument;
            const CmHirGenericParam *parameter;
            CmTraitTypeScan argument_scan;
            CmHirGenericArgKind expected_kind;
            size_t argument_height;

            argument = &type->data.named_type.arguments[index];
            argument_scan = CM_TRAIT_TYPE_SCAN_CONCRETE;
            argument_height = 0u;
            parameter = item == NULL ? NULL : cm_hir_get_generic_param(
                state->hir, item->generic_parameter_start + index);
            expected_kind = parameter == NULL ? (CmHirGenericArgKind)-1
                : parameter->kind == CM_HIR_GENERIC_LIFETIME
                    ? CM_HIR_GENERIC_ARG_LIFETIME
                    : parameter->kind == CM_HIR_GENERIC_TYPE
                        ? CM_HIR_GENERIC_ARG_TYPE
                        : CM_HIR_GENERIC_ARG_CONST;
            if (parameter == NULL || argument->kind != expected_kind) {
                argument_scan = CM_TRAIT_TYPE_SCAN_INVALID;
            }
            if (argument->kind == CM_HIR_GENERIC_ARG_TYPE) {
                argument_scan = cm_trait_scan_merge(argument_scan,
                    cm_trait_scan_typeck_type_inner(state,
                        argument->data.type, depth + 1u,
                        &argument_height));
            } else if (argument->kind == CM_HIR_GENERIC_ARG_CONST) {
                argument_scan = cm_trait_scan_merge(argument_scan,
                    cm_trait_scan_typeck_const_inner(state,
                        &argument->data.constant, depth,
                        &argument_height));
            } else if (argument->kind == CM_HIR_GENERIC_ARG_LIFETIME) {
                if (argument->data.lifetime.kind == CM_HIR_REGION_INFER) {
                    argument_scan = cm_trait_scan_merge(argument_scan,
                        CM_TRAIT_TYPE_SCAN_INFERENCE);
                } else if (argument->data.lifetime.kind
                        != CM_HIR_REGION_STATIC
                    && argument->data.lifetime.kind
                        != CM_HIR_REGION_ERASED) {
                    argument_scan = cm_trait_scan_merge(argument_scan,
                        CM_TRAIT_TYPE_SCAN_UNSUPPORTED);
                }
            }
            if (argument_scan == CM_TRAIT_TYPE_SCAN_OVERFLOW) {
                return argument_scan;
            }
            scan = cm_trait_scan_merge(scan, argument_scan);
            if (argument_height > maximum_child_height) {
                maximum_child_height = argument_height;
            }
        }
        break;
    }
    case CM_TYPECK_TYPE_PARAMETER:
        scan = CM_TRAIT_TYPE_SCAN_UNSUPPORTED;
        break;
    case CM_TYPECK_TYPE_PROJECTION:
    {
        CmTraitTypeScan self_scan;

        self_scan = cm_trait_scan_typeck_type_inner(state,
            type->data.projection_type.self_type, depth + 1u,
            &maximum_child_height);
        scan = cm_trait_scan_merge(CM_TRAIT_TYPE_SCAN_PROJECTION,
            self_scan);
        break;
    }
    }
    if (scan == CM_TRAIT_TYPE_SCAN_OVERFLOW) return scan;
    *out_height = maximum_child_height + 1u;
    if (*out_height > CM_TRAIT_SOLVER_MAX_RECURSION - depth) {
        return CM_TRAIT_TYPE_SCAN_OVERFLOW;
    }
    state->visit_state[(size_t)resolved - 1u] = 2u;
    state->memo[(size_t)resolved - 1u] = scan;
    state->height[(size_t)resolved - 1u] = *out_height;
    return scan;
}

static CmTraitTypeScan cm_trait_scan_typeck_type(
    const CmTypeckContext *typeck, CmTypeckTypeId type_id)
{
    CmTraitTypeckScanState state;
    CmTraitTypeScan result;
    size_t height;

    memset(&state, 0, sizeof(state));
    state.typeck = typeck;
    state.hir = cm_typeck_hir_context(typeck);
    state.type_count = cm_typeck_type_count(typeck);
    state.visit_state = (unsigned char *)cm_alloc_zeroed(state.type_count,
        sizeof(unsigned char));
    state.memo = (CmTraitTypeScan *)cm_alloc_zeroed(state.type_count,
        sizeof(CmTraitTypeScan));
    state.height = (size_t *)cm_alloc_zeroed(state.type_count,
        sizeof(size_t));
    result = cm_trait_scan_typeck_type_inner(&state, type_id, 0u,
        &height);
    cm_free(state.height);
    cm_free(state.memo);
    cm_free(state.visit_state);
    return result;
}

static CmTraitTypeScan cm_trait_scan_typeck_const(
    const CmTypeckContext *typeck, const CmTypeckConst *constant)
{
    CmTraitTypeckScanState state;
    CmTraitTypeScan result;
    size_t height;

    memset(&state, 0, sizeof(state));
    state.typeck = typeck;
    state.hir = cm_typeck_hir_context(typeck);
    state.type_count = cm_typeck_type_count(typeck);
    state.visit_state = (unsigned char *)cm_alloc_zeroed(state.type_count,
        sizeof(unsigned char));
    state.memo = (CmTraitTypeScan *)cm_alloc_zeroed(state.type_count,
        sizeof(CmTraitTypeScan));
    state.height = (size_t *)cm_alloc_zeroed(state.type_count,
        sizeof(size_t));
    result = cm_trait_scan_typeck_const_inner(&state, constant, 0u,
        &height);
    cm_free(state.height);
    cm_free(state.memo);
    cm_free(state.visit_state);
    return result;
}

static CmTraitImplHeadKind cm_trait_typeck_head(
    const CmTypeckContext *typeck, CmTypeckTypeId type_id,
    CmHirDefId *out_definition)
{
    const CmTypeckType *type;
    CmTypeckTypeId resolved;

    *out_definition = cm_hir_def_id_none();
    if (cm_typeck_resolve(typeck, type_id, &resolved) != CM_TYPECK_OK) {
        return CM_TRAIT_IMPL_HEAD_WILDCARD;
    }
    type = cm_typeck_get_type(typeck, resolved);
    if (type == NULL) return CM_TRAIT_IMPL_HEAD_WILDCARD;
    switch (type->kind) {
    case CM_TYPECK_TYPE_NEVER: return CM_TRAIT_IMPL_HEAD_NEVER;
    case CM_TYPECK_TYPE_UNIT: return CM_TRAIT_IMPL_HEAD_UNIT;
    case CM_TYPECK_TYPE_BOOL: return CM_TRAIT_IMPL_HEAD_BOOL;
    case CM_TYPECK_TYPE_CHAR: return CM_TRAIT_IMPL_HEAD_CHAR;
    case CM_TYPECK_TYPE_STR: return CM_TRAIT_IMPL_HEAD_STR;
    case CM_TYPECK_TYPE_INTEGER: return CM_TRAIT_IMPL_HEAD_INTEGER;
    case CM_TYPECK_TYPE_FLOAT: return CM_TRAIT_IMPL_HEAD_FLOAT;
    case CM_TYPECK_TYPE_REFERENCE: return CM_TRAIT_IMPL_HEAD_REFERENCE;
    case CM_TYPECK_TYPE_RAW_POINTER: return CM_TRAIT_IMPL_HEAD_RAW_POINTER;
    case CM_TYPECK_TYPE_TUPLE: return CM_TRAIT_IMPL_HEAD_TUPLE;
    case CM_TYPECK_TYPE_ARRAY: return CM_TRAIT_IMPL_HEAD_ARRAY;
    case CM_TYPECK_TYPE_SLICE: return CM_TRAIT_IMPL_HEAD_SLICE;
    case CM_TYPECK_TYPE_FN_POINTER: return CM_TRAIT_IMPL_HEAD_FN_POINTER;
    case CM_TYPECK_TYPE_ADT:
        *out_definition = type->data.named_type.definition;
        return CM_TRAIT_IMPL_HEAD_NAMED;
    case CM_TYPECK_TYPE_VARIABLE:
    case CM_TYPECK_TYPE_PARAMETER:
    case CM_TYPECK_TYPE_PROJECTION:
        return CM_TRAIT_IMPL_HEAD_WILDCARD;
    }
    return CM_TRAIT_IMPL_HEAD_WILDCARD;
}

static int cm_trait_heads_may_match(const CmTraitImplIndexEntry *entry,
    CmTraitImplHeadKind query_head, CmHirDefId query_definition)
{
    if (entry->self_head == CM_TRAIT_IMPL_HEAD_WILDCARD
        || query_head == CM_TRAIT_IMPL_HEAD_WILDCARD) return 1;
    if (entry->self_head != query_head) return 0;
    return query_head != CM_TRAIT_IMPL_HEAD_NAMED
        || cm_hir_def_id_equal(entry->self_head_definition,
            query_definition);
}

static CmTraitMatchResult cm_trait_match_result(CmTraitMatchKind kind,
    CmTypeckStatus status)
{
    CmTraitMatchResult result;

    result.kind = kind;
    if (kind == CM_TRAIT_MATCH_YES) {
        result.solver_kind = CM_TRAIT_SOLVER_PROVEN;
    } else if (kind == CM_TRAIT_MATCH_NO) {
        result.solver_kind = CM_TRAIT_SOLVER_NO_SOLUTION;
    } else if (kind == CM_TRAIT_MATCH_UNSUPPORTED) {
        result.solver_kind = CM_TRAIT_SOLVER_UNSUPPORTED;
    } else if (kind == CM_TRAIT_MATCH_OVERFLOW) {
        result.solver_kind = CM_TRAIT_SOLVER_OVERFLOW;
    } else {
        result.solver_kind = CM_TRAIT_SOLVER_TYPECK_FAILURE;
    }
    result.typeck_status = status;
    return result;
}

static CmTraitMatchResult cm_trait_match_nonproof(
    const CmTraitSelectionResult *selection)
{
    CmTraitMatchResult result;

    result.kind = CM_TRAIT_MATCH_NONPROVEN;
    result.solver_kind = selection->kind;
    result.typeck_status = selection->typeck_status;
    return result;
}

static CmTraitMatchResult cm_trait_match_instantiation_status(
    CmTypeckStatus status)
{
    if (status == CM_TYPECK_OVERFLOW) {
        return cm_trait_match_result(CM_TRAIT_MATCH_OVERFLOW, status);
    }
    if (status == CM_TYPECK_UNSUPPORTED_HIR_TYPE
        || status == CM_TYPECK_UNSUPPORTED_CONSTANT) {
        return cm_trait_match_result(CM_TRAIT_MATCH_UNSUPPORTED, status);
    }
    return cm_trait_match_result(CM_TRAIT_MATCH_TYPECK_FAILURE, status);
}

static CmTraitMatchResult cm_trait_match_evaluation(
    const CmTraitSelectionResult *selection)
{
    if (selection->kind == CM_TRAIT_SOLVER_PROVEN) {
        return cm_trait_match_result(CM_TRAIT_MATCH_YES,
            CM_TYPECK_OK);
    }
    if (selection->kind == CM_TRAIT_SOLVER_OVERFLOW) {
        return cm_trait_match_result(CM_TRAIT_MATCH_OVERFLOW,
            selection->typeck_status);
    }
    if (selection->kind == CM_TRAIT_SOLVER_INVALID
        || selection->kind == CM_TRAIT_SOLVER_TYPECK_FAILURE) {
        return cm_trait_match_result(CM_TRAIT_MATCH_TYPECK_FAILURE,
            selection->typeck_status == CM_TYPECK_OK
                ? CM_TYPECK_INVALID_ARGUMENT
                : selection->typeck_status);
    }
    return cm_trait_match_nonproof(selection);
}

static CmTraitMatchResult cm_trait_evaluate_impl_predicates(
    const CmTraitImplIndexState *state, const CmHirItem *item,
    CmTypeckContext *typeck, const CmTypeckInstantiation *instantiation,
    CmHirDefId goal_owner, const CmTraitGoalEvaluator *evaluator)
{
    uint32_t index;

    if (item->predicate_count == 0u) {
        return cm_trait_match_result(CM_TRAIT_MATCH_YES, CM_TYPECK_OK);
    }
    if (!cm_trait_impl_predicates_supported(state->hir, item)
        || evaluator == NULL || evaluator->evaluate == NULL
        || cm_hir_def_id_is_none(goal_owner)) {
        return cm_trait_match_result(CM_TRAIT_MATCH_UNSUPPORTED,
            CM_TYPECK_OK);
    }
    for (index = 0u; index < item->predicate_count; ++index) {
        const CmHirTraitPredicate *predicate;
        CmImplementedTraitGoal nested_goal;
        CmTraitSelectionResult selection;
        CmTypeckStatus status;

        predicate = &item->predicates[index];
        memset(&nested_goal, 0, sizeof(nested_goal));
        nested_goal.owner = goal_owner;
        status = cm_typeck_instantiate_hir_type(typeck,
            predicate->subject, instantiation, &nested_goal.self_type);
        if (status != CM_TYPECK_OK) {
            return cm_trait_match_instantiation_status(status);
        }
        status = cm_typeck_instantiate_hir_named(typeck,
            &predicate->trait_type, instantiation,
            &nested_goal.trait_type);
        if (status != CM_TYPECK_OK) {
            return cm_trait_match_instantiation_status(status);
        }
        selection = evaluator->evaluate(evaluator->context, typeck,
            &nested_goal);
        if (selection.kind != CM_TRAIT_SOLVER_PROVEN) {
            return cm_trait_match_evaluation(&selection);
        }
    }
    return cm_trait_match_result(CM_TRAIT_MATCH_YES, CM_TYPECK_OK);
}

static CmTraitMatchResult cm_trait_match_projection_target(
    const CmTraitImplIndexState *state, const CmHirItem *item,
    CmTypeckContext *typeck, CmTypeckTypeId candidate_self,
    CmTypeckInstantiation *instantiation, CmHirDefId goal_owner,
    const CmTraitProjectionMatchGoal *projection_goal,
    const CmTraitGoalEvaluator *evaluator, int expose_evidence,
    CmHirDefId *out_associated_definition)
{
    CmHirProjectionImplTarget target;
    CmProjectionEqualityGoal nested_goal;
    CmTraitSelectionResult nested;
    CmTraitTypeScan scan;
    const CmTypeckType *resolved_target_type;
    CmTypeckTypeId instantiated_target;
    CmTypeckTypeId resolved_target;
    CmTypeckStatus status;

    if (projection_goal == NULL) {
        return cm_trait_match_result(CM_TRAIT_MATCH_YES, CM_TYPECK_OK);
    }
    target = cm_hir_projection_impl_target(state->hir,
        state->local_crate, item->definition,
        item->data.impl_item.trait_type.definition,
        projection_goal->associated_definition);
    if (target.status == CM_HIR_PROJECTION_DEFERRED_CRATE) {
        nested = cm_trait_result(CM_TRAIT_SOLVER_DEFERRED_METADATA);
        return cm_trait_match_nonproof(&nested);
    }
    if (target.status != CM_HIR_PROJECTION_SELECTED) {
        return cm_trait_match_result(CM_TRAIT_MATCH_UNSUPPORTED,
            CM_TYPECK_OK);
    }
    instantiation->self_owner = item->definition;
    instantiation->self_type = candidate_self;
    if (!cm_typeck_instantiation_is_valid(typeck, instantiation)) {
        return cm_trait_match_result(CM_TRAIT_MATCH_TYPECK_FAILURE,
            CM_TYPECK_INVALID_ARGUMENT);
    }
    status = cm_typeck_instantiate_hir_type(typeck,
        target.target_template, instantiation, &instantiated_target);
    if (status != CM_TYPECK_OK) {
        return cm_trait_match_instantiation_status(status);
    }
    status = cm_typeck_resolve(typeck, instantiated_target,
        &resolved_target);
    if (status == CM_TYPECK_OVERFLOW) {
        return cm_trait_match_result(CM_TRAIT_MATCH_OVERFLOW, status);
    }
    if (status != CM_TYPECK_OK) {
        return cm_trait_match_result(CM_TRAIT_MATCH_TYPECK_FAILURE,
            status);
    }
    resolved_target_type = cm_typeck_get_type(typeck, resolved_target);
    if (resolved_target_type == NULL) {
        return cm_trait_match_result(CM_TRAIT_MATCH_TYPECK_FAILURE,
            CM_TYPECK_INVALID_ID);
    }
    if (resolved_target_type->kind == CM_TYPECK_TYPE_PROJECTION) {
        if (evaluator == NULL || evaluator->evaluate_projection == NULL) {
            return cm_trait_match_result(CM_TRAIT_MATCH_UNSUPPORTED,
                CM_TYPECK_OK);
        }
        memset(&nested_goal, 0, sizeof(nested_goal));
        nested_goal.owner = goal_owner;
        nested_goal.projection_type = resolved_target;
        nested_goal.expected_type = projection_goal->expected_type;
        nested = evaluator->evaluate_projection(evaluator->context, typeck,
            &nested_goal);
        if (nested.kind != CM_TRAIT_SOLVER_PROVEN) {
            return cm_trait_match_evaluation(&nested);
        }
    } else {
        scan = cm_trait_scan_typeck_type(typeck, resolved_target);
        if (scan == CM_TRAIT_TYPE_SCAN_PROJECTION
            || scan == CM_TRAIT_TYPE_SCAN_UNSUPPORTED) {
            return cm_trait_match_result(CM_TRAIT_MATCH_UNSUPPORTED,
                CM_TYPECK_OK);
        }
        if (scan == CM_TRAIT_TYPE_SCAN_OVERFLOW) {
            return cm_trait_match_result(CM_TRAIT_MATCH_OVERFLOW,
                CM_TYPECK_OVERFLOW);
        }
        if (scan == CM_TRAIT_TYPE_SCAN_INVALID) {
            return cm_trait_match_result(CM_TRAIT_MATCH_TYPECK_FAILURE,
                CM_TYPECK_INVALID_ARGUMENT);
        }
        status = cm_typeck_unify(typeck, instantiated_target,
            projection_goal->expected_type);
        if (status == CM_TYPECK_TYPE_MISMATCH
            || status == CM_TYPECK_KIND_CONFLICT
            || status == CM_TYPECK_OCCURS_CHECK) {
            return cm_trait_match_result(CM_TRAIT_MATCH_NO, status);
        }
        if (status == CM_TYPECK_OVERFLOW) {
            return cm_trait_match_result(CM_TRAIT_MATCH_OVERFLOW, status);
        }
        if (status != CM_TYPECK_OK) {
            return cm_trait_match_result(CM_TRAIT_MATCH_TYPECK_FAILURE,
                status);
        }
    }
    if (expose_evidence && out_associated_definition != NULL) {
        *out_associated_definition = target.impl_associated_definition;
    }
    return cm_trait_match_result(CM_TRAIT_MATCH_YES, CM_TYPECK_OK);
}

static CmTraitMatchResult cm_trait_match_candidate(
    const CmTraitImplIndexState *state,
    const CmTraitImplIndexEntry *entry, CmTypeckContext *typeck,
    CmTypeckTypeId query_self, const CmTypeckNamedType *query_trait,
    CmHirDefId goal_owner, const CmTraitGoalEvaluator *evaluator,
    const CmTraitProjectionMatchGoal *projection_goal, int keep_bindings,
    CmHirDefId *out_associated_definition)
{
    const CmHirItem *item;
    CmTypeckGenericArg *impl_arguments;
    CmTypeckInstantiation instantiation;
    CmTypeckNamedType candidate_trait;
    CmTypeckSnapshot snapshot;
    CmTypeckTypeId candidate_self;
    CmHirDefId associated_evidence;
    CmTypeckStatus status;
    CmTraitMatchKind match;
    CmTraitSolverResultKind solver_kind;
    uint32_t index;

    status = cm_typeck_snapshot(typeck, &snapshot);
    if (status != CM_TYPECK_OK) {
        return cm_trait_match_result(CM_TRAIT_MATCH_TYPECK_FAILURE, status);
    }
    item = cm_hir_get_item(state->hir, entry->item);
    impl_arguments = NULL;
    memset(&instantiation, 0, sizeof(instantiation));
    memset(&candidate_trait, 0, sizeof(candidate_trait));
    candidate_self = CM_TYPECK_TYPE_NONE;
    associated_evidence = cm_hir_def_id_none();
    match = CM_TRAIT_MATCH_YES;
    solver_kind = CM_TRAIT_SOLVER_PROVEN;
    status = CM_TYPECK_OK;
    if (item == NULL || item->kind != CM_HIR_ITEM_IMPL
        || !item->data.impl_item.has_trait) {
        status = CM_TYPECK_INVALID_ID;
        match = CM_TRAIT_MATCH_TYPECK_FAILURE;
    } else if (!cm_trait_impl_type_generics_supported(state->hir, item)) {
        match = CM_TRAIT_MATCH_UNSUPPORTED;
    }
    if (match == CM_TRAIT_MATCH_YES
        && item->generic_parameter_count != 0u) {
        impl_arguments = (CmTypeckGenericArg *)cm_alloc_zeroed(
            item->generic_parameter_count, sizeof(CmTypeckGenericArg));
    }
    for (index = 0u; match == CM_TRAIT_MATCH_YES
         && index < item->generic_parameter_count; ++index) {
        const CmHirGenericParam *parameter;

        parameter = cm_hir_get_generic_param(state->hir,
            item->generic_parameter_start + index);
        if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_TYPE
            || parameter->index != index
            || !cm_hir_def_id_equal(parameter->owner,
                item->definition)) {
            match = CM_TRAIT_MATCH_UNSUPPORTED;
            break;
        }
        impl_arguments[index].kind = CM_HIR_GENERIC_ARG_TYPE;
        status = cm_typeck_new_variable(typeck, CM_HIR_INFER_GENERAL,
            parameter->span, &impl_arguments[index].data.type);
        if (status == CM_TYPECK_OVERFLOW) {
            match = CM_TRAIT_MATCH_OVERFLOW;
        } else if (status != CM_TYPECK_OK) {
            match = CM_TRAIT_MATCH_TYPECK_FAILURE;
        }
    }
    if (match == CM_TRAIT_MATCH_YES) {
        instantiation.parameter_owner = item->definition;
        instantiation.arguments = impl_arguments;
        instantiation.argument_count = item->generic_parameter_count;
        instantiation.self_owner = cm_hir_def_id_none();
        instantiation.self_type = CM_TYPECK_TYPE_NONE;
        if (!cm_typeck_instantiation_is_valid(typeck, &instantiation)) {
            status = CM_TYPECK_INVALID_ARGUMENT;
            match = CM_TRAIT_MATCH_TYPECK_FAILURE;
        }
    }
    if (match == CM_TRAIT_MATCH_YES) {
        status = cm_typeck_instantiate_hir_type(typeck,
            item->data.impl_item.self_type, &instantiation,
            &candidate_self);
        if (status == CM_TYPECK_OVERFLOW) {
            match = CM_TRAIT_MATCH_OVERFLOW;
        } else if (status == CM_TYPECK_UNSUPPORTED_HIR_TYPE
                || status == CM_TYPECK_UNSUPPORTED_CONSTANT) {
            match = CM_TRAIT_MATCH_UNSUPPORTED;
        } else if (status != CM_TYPECK_OK) {
            match = CM_TRAIT_MATCH_TYPECK_FAILURE;
        }
    }
    if (match == CM_TRAIT_MATCH_YES) {
        status = cm_typeck_instantiate_hir_named(typeck,
            &item->data.impl_item.trait_type, &instantiation,
            &candidate_trait);
        if (status == CM_TYPECK_OVERFLOW) {
            match = CM_TRAIT_MATCH_OVERFLOW;
        } else if (status == CM_TYPECK_UNSUPPORTED_HIR_TYPE
                || status == CM_TYPECK_UNSUPPORTED_CONSTANT) {
            match = CM_TRAIT_MATCH_UNSUPPORTED;
        } else if (status != CM_TYPECK_OK) {
            match = CM_TRAIT_MATCH_TYPECK_FAILURE;
        }
    }
    if (match == CM_TRAIT_MATCH_YES && status == CM_TYPECK_OK) {
        status = cm_typeck_unify(typeck, query_self, candidate_self);
    }
    if (match == CM_TRAIT_MATCH_YES
        && (status == CM_TYPECK_TYPE_MISMATCH
        || status == CM_TYPECK_KIND_CONFLICT
        || status == CM_TYPECK_OCCURS_CHECK)) {
        match = CM_TRAIT_MATCH_NO;
    } else if (match == CM_TRAIT_MATCH_YES
        && status == CM_TYPECK_OVERFLOW) {
        match = CM_TRAIT_MATCH_OVERFLOW;
    } else if (match == CM_TRAIT_MATCH_YES
        && (status == CM_TYPECK_UNSUPPORTED_HIR_TYPE
        || status == CM_TYPECK_UNSUPPORTED_CONSTANT)) {
        match = CM_TRAIT_MATCH_UNSUPPORTED;
    } else if (match == CM_TRAIT_MATCH_YES && status != CM_TYPECK_OK) {
        match = CM_TRAIT_MATCH_TYPECK_FAILURE;
    }
    if (match == CM_TRAIT_MATCH_YES
        && (!cm_hir_def_id_equal(candidate_trait.definition,
                query_trait->definition)
            || candidate_trait.argument_count
            != query_trait->argument_count)) {
        match = CM_TRAIT_MATCH_NO;
    }
    for (index = 0u; match == CM_TRAIT_MATCH_YES
         && index < query_trait->argument_count; ++index) {
        const CmTypeckGenericArg *candidate;
        const CmTypeckGenericArg *query;

        candidate = &candidate_trait.arguments[index];
        query = &query_trait->arguments[index];
        if (candidate->kind != query->kind) {
            match = CM_TRAIT_MATCH_NO;
            break;
        }
        if (candidate->kind == CM_HIR_GENERIC_ARG_LIFETIME) {
            /* Regions are erased at this selection boundary. */
        } else if (candidate->kind == CM_HIR_GENERIC_ARG_TYPE) {
            status = cm_typeck_unify(typeck, query->data.type,
                candidate->data.type);
            if (status == CM_TYPECK_TYPE_MISMATCH
                || status == CM_TYPECK_KIND_CONFLICT
                || status == CM_TYPECK_OCCURS_CHECK) {
                match = CM_TRAIT_MATCH_NO;
            } else if (status == CM_TYPECK_OVERFLOW) {
                match = CM_TRAIT_MATCH_OVERFLOW;
            } else if (status != CM_TYPECK_OK) {
                match = status == CM_TYPECK_UNSUPPORTED_HIR_TYPE
                        || status == CM_TYPECK_UNSUPPORTED_CONSTANT
                    ? CM_TRAIT_MATCH_UNSUPPORTED
                    : CM_TRAIT_MATCH_TYPECK_FAILURE;
            }
        } else if (candidate->kind == CM_HIR_GENERIC_ARG_CONST) {
            if (candidate->data.constant.kind != CM_HIR_CONST_VALUE
                || query->data.constant.kind != CM_HIR_CONST_VALUE
                || candidate->data.constant.data.value.low_bits
                    != query->data.constant.data.value.low_bits
                || candidate->data.constant.data.value.high_bits
                    != query->data.constant.data.value.high_bits) {
                match = CM_TRAIT_MATCH_NO;
                break;
            }
            status = cm_typeck_unify(typeck,
                query->data.constant.type,
                candidate->data.constant.type);
            if (status == CM_TYPECK_TYPE_MISMATCH
                || status == CM_TYPECK_KIND_CONFLICT
                || status == CM_TYPECK_OCCURS_CHECK) {
                match = CM_TRAIT_MATCH_NO;
            } else if (status == CM_TYPECK_OVERFLOW) {
                match = CM_TRAIT_MATCH_OVERFLOW;
            } else if (status != CM_TYPECK_OK) {
                match = status == CM_TYPECK_UNSUPPORTED_HIR_TYPE
                        || status == CM_TYPECK_UNSUPPORTED_CONSTANT
                    ? CM_TRAIT_MATCH_UNSUPPORTED
                    : CM_TRAIT_MATCH_TYPECK_FAILURE;
            }
        } else {
            match = CM_TRAIT_MATCH_TYPECK_FAILURE;
            status = CM_TYPECK_INVALID_ARGUMENT;
        }
    }
    if (match == CM_TRAIT_MATCH_YES) {
        CmTraitMatchResult predicates;

        predicates = cm_trait_evaluate_impl_predicates(state, item,
            typeck, &instantiation, goal_owner, evaluator);
        match = predicates.kind;
        solver_kind = predicates.solver_kind;
        status = predicates.typeck_status;
    }
    if (match == CM_TRAIT_MATCH_YES && projection_goal != NULL) {
        CmTraitMatchResult projection;

        projection = cm_trait_match_projection_target(state, item, typeck,
            candidate_self, &instantiation, goal_owner, projection_goal,
            evaluator, keep_bindings, &associated_evidence);
        match = projection.kind;
        solver_kind = projection.solver_kind;
        status = projection.typeck_status;
    }
    if (keep_bindings && match == CM_TRAIT_MATCH_YES) {
        if (cm_typeck_commit(typeck, &snapshot) != CM_TYPECK_OK) {
            cm_free(impl_arguments);
            return cm_trait_match_result(CM_TRAIT_MATCH_TYPECK_FAILURE,
                CM_TYPECK_INVALID_SNAPSHOT);
        }
        if (out_associated_definition != NULL) {
            *out_associated_definition = associated_evidence;
        }
    } else if (cm_typeck_rollback(typeck, &snapshot) != CM_TYPECK_OK) {
        cm_free(impl_arguments);
        return cm_trait_match_result(CM_TRAIT_MATCH_TYPECK_FAILURE,
            CM_TYPECK_INVALID_SNAPSHOT);
    }
    cm_free(impl_arguments);
    {
        CmTraitMatchResult result;

        result = cm_trait_match_result(match, status);
        if (match == CM_TRAIT_MATCH_NONPROVEN) {
            result.solver_kind = solver_kind;
        }
        return result;
    }
}

CmTraitSolverResultKind cm_trait_solver_validate_implemented_goal(
    const CmHirContext *hir, CmTypeckContext *typeck,
    CmTypeckTypeId self_type, const CmTypeckNamedType *trait_type)
{
    CmTraitTypeScan scan;
    const CmHirItem *queried_trait;
    uint32_t argument_index;

    if (hir == NULL || typeck == NULL
        || cm_typeck_hir_context(typeck) != hir
        || trait_type == NULL
        || cm_hir_def_id_is_none(trait_type->definition)
        || (trait_type->argument_count == 0u)
            != (trait_type->arguments == NULL)
        || cm_typeck_get_type(typeck, self_type) == NULL) {
        return CM_TRAIT_SOLVER_INVALID;
    }
    queried_trait = cm_trait_find_definition_item(hir,
        trait_type->definition);
    if (queried_trait == NULL
        || queried_trait->kind != CM_HIR_ITEM_TRAIT
        || queried_trait->generic_parameter_count
            != trait_type->argument_count) return CM_TRAIT_SOLVER_INVALID;
    for (argument_index = 0u;
         argument_index < trait_type->argument_count; ++argument_index) {
        const CmHirGenericParam *parameter;
        CmHirGenericArgKind expected_kind;

        parameter = cm_hir_get_generic_param(hir,
            queried_trait->generic_parameter_start + argument_index);
        if (parameter == NULL) return CM_TRAIT_SOLVER_INVALID;
        expected_kind = parameter->kind == CM_HIR_GENERIC_LIFETIME
            ? CM_HIR_GENERIC_ARG_LIFETIME
            : parameter->kind == CM_HIR_GENERIC_TYPE
                ? CM_HIR_GENERIC_ARG_TYPE : CM_HIR_GENERIC_ARG_CONST;
        if (trait_type->arguments[argument_index].kind
                != expected_kind) return CM_TRAIT_SOLVER_INVALID;
    }
    if (queried_trait->data.trait_item.is_auto) {
        return CM_TRAIT_SOLVER_UNSUPPORTED;
    }
    scan = cm_trait_scan_typeck_type(typeck, self_type);
    for (argument_index = 0u;
         argument_index < trait_type->argument_count; ++argument_index) {
        const CmTypeckGenericArg *argument;
        CmTraitTypeScan argument_scan;

        argument = &trait_type->arguments[argument_index];
        argument_scan = CM_TRAIT_TYPE_SCAN_CONCRETE;
        if (argument->kind == CM_HIR_GENERIC_ARG_TYPE) {
            argument_scan = cm_trait_scan_typeck_type(typeck,
                argument->data.type);
        } else if (argument->kind == CM_HIR_GENERIC_ARG_CONST) {
            argument_scan = cm_trait_scan_typeck_const(typeck,
                &argument->data.constant);
        } else if (argument->kind == CM_HIR_GENERIC_ARG_LIFETIME) {
            if (argument->data.lifetime.kind == CM_HIR_REGION_INFER) {
                argument_scan = CM_TRAIT_TYPE_SCAN_INFERENCE;
            } else if (argument->data.lifetime.kind
                    != CM_HIR_REGION_STATIC
                && argument->data.lifetime.kind
                    != CM_HIR_REGION_ERASED) {
                argument_scan = CM_TRAIT_TYPE_SCAN_UNSUPPORTED;
            }
        } else {
            argument_scan = CM_TRAIT_TYPE_SCAN_INVALID;
        }
        scan = cm_trait_scan_merge(scan, argument_scan);
    }
    if (scan == CM_TRAIT_TYPE_SCAN_INFERENCE) {
        return CM_TRAIT_SOLVER_DEFERRED_INFERENCE;
    }
    if (scan == CM_TRAIT_TYPE_SCAN_PROJECTION
        || scan == CM_TRAIT_TYPE_SCAN_UNSUPPORTED) {
        return CM_TRAIT_SOLVER_UNSUPPORTED;
    }
    if (scan == CM_TRAIT_TYPE_SCAN_OVERFLOW) {
        return CM_TRAIT_SOLVER_OVERFLOW;
    }
    return scan == CM_TRAIT_TYPE_SCAN_CONCRETE
        ? CM_TRAIT_SOLVER_PROVEN : CM_TRAIT_SOLVER_INVALID;
}

static int cm_trait_nonproof_rank(CmTraitSolverResultKind kind)
{
    switch (kind) {
    case CM_TRAIT_SOLVER_UNSUPPORTED: return 6;
    case CM_TRAIT_SOLVER_AMBIGUOUS: return 5;
    case CM_TRAIT_SOLVER_DEFERRED_INFERENCE: return 4;
    case CM_TRAIT_SOLVER_DEFERRED_METADATA: return 3;
    case CM_TRAIT_SOLVER_NEGATIVE: return 2;
    case CM_TRAIT_SOLVER_NO_SOLUTION: return 1;
    case CM_TRAIT_SOLVER_PROVEN:
    case CM_TRAIT_SOLVER_OVERFLOW:
    case CM_TRAIT_SOLVER_INVALID:
    case CM_TRAIT_SOLVER_TYPECK_FAILURE:
        return 0;
    }
    return 0;
}

static CmTraitSolverResultKind cm_trait_stronger_nonproof(
    CmTraitSolverResultKind left, CmTraitSolverResultKind right)
{
    return cm_trait_nonproof_rank(right) > cm_trait_nonproof_rank(left)
        ? right : left;
}

static CmTraitSelectionResult cm_trait_solver_select_inner(
    const CmTraitImplIndex *index, CmTypeckContext *typeck,
    CmTypeckTypeId self_type, const CmTypeckNamedType *trait_type,
    CmHirDefId goal_owner, const CmTraitGoalEvaluator *evaluator,
    const CmTraitProjectionMatchGoal *projection_goal)
{
    const CmTraitImplIndexState *state;
    CmTraitSelectionResult result;
    CmTraitImplHeadKind query_head;
    CmHirDefId query_head_definition;
    const CmTraitImplIndexEntry *winner;
    CmTraitSolverResultKind validation;
    CmTraitSolverResultKind strongest_nonproof;
    size_t entry_index;
    size_t unsupported_blockers;
    int saw_nonproof;

    result = cm_trait_result(CM_TRAIT_SOLVER_INVALID);
    state = cm_trait_index_state_const(index);
    if (!cm_trait_index_is_current(state)) return result;
    validation = cm_trait_solver_validate_implemented_goal(state->hir,
        typeck, self_type, trait_type);
    if (validation != CM_TRAIT_SOLVER_PROVEN) {
        result.kind = validation;
        return result;
    }
    query_head = cm_trait_typeck_head(typeck, self_type,
        &query_head_definition);
    winner = NULL;
    strongest_nonproof = CM_TRAIT_SOLVER_NO_SOLUTION;
    unsupported_blockers = 0u;
    saw_nonproof = 0;
    result = cm_trait_result(CM_TRAIT_SOLVER_NO_SOLUTION);
    for (entry_index = 0u; entry_index < state->entries.len;
         ++entry_index) {
        const CmTraitImplIndexEntry *entry;
        CmTraitMatchResult match;

        entry = (const CmTraitImplIndexEntry *)cm_vec_at_const(
            &state->entries, entry_index);
        if (entry == NULL
            || !cm_hir_def_id_equal(entry->trait_definition,
                trait_type->definition)
            || !cm_trait_heads_may_match(entry, query_head,
                query_head_definition)) continue;
        if (entry->unsupported_flags != CM_TRAIT_IMPL_UNSUPPORTED_NONE) {
            unsigned int header_unknown;

            header_unknown = entry->unsupported_flags
                & (CM_TRAIT_IMPL_UNSUPPORTED_NON_MONOMORPHIC
                    | CM_TRAIT_IMPL_UNSUPPORTED_PROJECTION
                    | CM_TRAIT_IMPL_UNSUPPORTED_TYPE);
            if (header_unknown == 0u) {
                match = cm_trait_match_candidate(state, entry, typeck,
                    self_type, trait_type, goal_owner, evaluator,
                    NULL, 0, NULL);
                if (match.kind == CM_TRAIT_MATCH_NO) continue;
                if (match.kind == CM_TRAIT_MATCH_OVERFLOW) {
                    result.kind = CM_TRAIT_SOLVER_OVERFLOW;
                    result.typeck_status = match.typeck_status;
                    return result;
                }
                if (match.kind == CM_TRAIT_MATCH_TYPECK_FAILURE) {
                    result.kind = CM_TRAIT_SOLVER_TYPECK_FAILURE;
                    result.typeck_status = match.typeck_status;
                    return result;
                }
                if (match.kind == CM_TRAIT_MATCH_YES
                    && (entry->unsupported_flags
                        & CM_TRAIT_IMPL_UNSUPPORTED_NEGATIVE) != 0u) {
                    result.negative_match_count += 1u;
                    continue;
                }
            }
            result.blocking_match_count += 1u;
            unsupported_blockers += 1u;
            continue;
        }
        match = cm_trait_match_candidate(state, entry, typeck,
            self_type, trait_type, goal_owner, evaluator, NULL,
            0, NULL);
        if (match.kind == CM_TRAIT_MATCH_YES) {
            result.supported_match_count += 1u;
            if (winner == NULL) winner = entry;
        } else if (match.kind == CM_TRAIT_MATCH_NONPROVEN) {
            result.blocking_match_count += 1u;
            saw_nonproof = 1;
            strongest_nonproof = cm_trait_stronger_nonproof(
                strongest_nonproof, match.solver_kind);
        } else if (match.kind == CM_TRAIT_MATCH_UNSUPPORTED) {
            result.blocking_match_count += 1u;
            unsupported_blockers += 1u;
            strongest_nonproof = cm_trait_stronger_nonproof(
                strongest_nonproof, CM_TRAIT_SOLVER_UNSUPPORTED);
        } else if (match.kind == CM_TRAIT_MATCH_OVERFLOW) {
            result.kind = CM_TRAIT_SOLVER_OVERFLOW;
            result.typeck_status = match.typeck_status;
            return result;
        } else if (match.kind == CM_TRAIT_MATCH_TYPECK_FAILURE) {
            result.kind = CM_TRAIT_SOLVER_TYPECK_FAILURE;
            result.typeck_status = match.typeck_status;
            return result;
        }
    }
    if (unsupported_blockers != 0u) {
        result.kind = CM_TRAIT_SOLVER_UNSUPPORTED;
        return result;
    }
    if (result.negative_match_count != 0u) {
        result.kind = CM_TRAIT_SOLVER_UNSUPPORTED;
        return result;
    }
    if (saw_nonproof) {
        result.kind = strongest_nonproof;
        return result;
    }
    if (result.supported_match_count > 1u) {
        result.kind = CM_TRAIT_SOLVER_AMBIGUOUS;
        return result;
    }
    if (winner != NULL) {
        CmTraitMatchResult replay;

        replay = cm_trait_match_candidate(state, winner, typeck,
            self_type, trait_type, goal_owner, evaluator, projection_goal,
            1, &result.impl_associated_definition);
        if (replay.kind != CM_TRAIT_MATCH_YES) {
            if (replay.kind == CM_TRAIT_MATCH_OVERFLOW) {
                result.kind = CM_TRAIT_SOLVER_OVERFLOW;
            } else if (replay.kind == CM_TRAIT_MATCH_NONPROVEN) {
                result.blocking_match_count += 1u;
                result.kind = replay.solver_kind;
            } else if (replay.kind == CM_TRAIT_MATCH_UNSUPPORTED) {
                result.blocking_match_count += 1u;
                result.kind = CM_TRAIT_SOLVER_UNSUPPORTED;
            } else if (replay.kind == CM_TRAIT_MATCH_NO) {
                result.kind = CM_TRAIT_SOLVER_NO_SOLUTION;
            } else {
                result.kind = CM_TRAIT_SOLVER_TYPECK_FAILURE;
            }
            result.typeck_status = replay.typeck_status;
            return result;
        }
        result.kind = CM_TRAIT_SOLVER_PROVEN;
        result.impl_definition = winner->impl_definition;
        result.impl_item = winner->item;
        return result;
    }
    result.kind = CM_TRAIT_SOLVER_DEFERRED_METADATA;
    return result;
}

CmTraitSelectionResult cm_trait_solver_select(
    const CmTraitImplIndex *index, CmTypeckContext *typeck,
    CmTypeckTypeId self_type, const CmTypeckNamedType *trait_type)
{
    return cm_trait_solver_select_inner(index, typeck, self_type,
        trait_type, cm_hir_def_id_none(), NULL, NULL);
}

static int cm_trait_typeck_region_equal(const CmHirRegion *left,
    const CmHirRegion *right)
{
    if (left->kind != right->kind) return 0;
    switch (left->kind) {
    case CM_HIR_REGION_STATIC:
    case CM_HIR_REGION_ERASED:
        return 1;
    case CM_HIR_REGION_EARLY_BOUND:
        return left->data.parameter == right->data.parameter;
    case CM_HIR_REGION_LATE_BOUND:
        return left->data.binder_index == right->data.binder_index;
    case CM_HIR_REGION_INFER:
        return left->data.inference_variable
            == right->data.inference_variable;
    case CM_HIR_REGION_ERROR:
        return left->data.error_reason == right->data.error_reason;
    }
    return 0;
}

static CmTraitMatchResult cm_trait_match_typeck_named(
    CmTypeckContext *typeck, const CmTypeckNamedType *candidate,
    const CmTypeckNamedType *query)
{
    CmTypeckStatus status;
    uint32_t index;

    if (candidate == NULL || query == NULL
        || !cm_hir_def_id_equal(candidate->definition, query->definition)
        || candidate->argument_count != query->argument_count) {
        return cm_trait_match_result(CM_TRAIT_MATCH_NO, CM_TYPECK_OK);
    }
    for (index = 0u; index < candidate->argument_count; ++index) {
        const CmTypeckGenericArg *left;
        const CmTypeckGenericArg *right;

        left = &candidate->arguments[index];
        right = &query->arguments[index];
        if (left->kind != right->kind) {
            return cm_trait_match_result(CM_TRAIT_MATCH_NO,
                CM_TYPECK_OK);
        }
        if (left->kind == CM_HIR_GENERIC_ARG_TYPE) {
            status = cm_typeck_unify(typeck, left->data.type,
                right->data.type);
            if (status == CM_TYPECK_TYPE_MISMATCH) {
                return cm_trait_match_result(CM_TRAIT_MATCH_NO, status);
            }
            if (status == CM_TYPECK_OVERFLOW) {
                return cm_trait_match_result(CM_TRAIT_MATCH_OVERFLOW,
                    status);
            }
            if (status != CM_TYPECK_OK) {
                return cm_trait_match_result(CM_TRAIT_MATCH_TYPECK_FAILURE,
                    status);
            }
        } else if (left->kind == CM_HIR_GENERIC_ARG_LIFETIME) {
            if (!cm_trait_typeck_region_equal(&left->data.lifetime,
                    &right->data.lifetime)) {
                return cm_trait_match_result(CM_TRAIT_MATCH_NO,
                    CM_TYPECK_OK);
            }
        } else if (left->kind == CM_HIR_GENERIC_ARG_CONST) {
            if (left->data.constant.kind != CM_HIR_CONST_VALUE
                || right->data.constant.kind != CM_HIR_CONST_VALUE
                || left->data.constant.data.value.low_bits
                    != right->data.constant.data.value.low_bits
                || left->data.constant.data.value.high_bits
                    != right->data.constant.data.value.high_bits) {
                return cm_trait_match_result(CM_TRAIT_MATCH_UNSUPPORTED,
                    CM_TYPECK_OK);
            }
            status = cm_typeck_unify(typeck, left->data.constant.type,
                right->data.constant.type);
            if (status == CM_TYPECK_TYPE_MISMATCH) {
                return cm_trait_match_result(CM_TRAIT_MATCH_NO, status);
            }
            if (status == CM_TYPECK_OVERFLOW) {
                return cm_trait_match_result(CM_TRAIT_MATCH_OVERFLOW,
                    status);
            }
            if (status != CM_TYPECK_OK) {
                return cm_trait_match_result(CM_TRAIT_MATCH_TYPECK_FAILURE,
                    status);
            }
        } else {
            return cm_trait_match_result(CM_TRAIT_MATCH_TYPECK_FAILURE,
                CM_TYPECK_INVALID_ARGUMENT);
        }
    }
    return cm_trait_match_result(CM_TRAIT_MATCH_YES, CM_TYPECK_OK);
}

static CmTraitMatchResult cm_trait_match_environment_fact(
    const CmParamEnv *environment, size_t fact_index,
    CmTypeckContext *typeck, const CmParamEnvSubstitution *substitution,
    const CmImplementedTraitGoal *goal, int keep_bindings)
{
    CmTypeckNamedType candidate_trait;
    CmTypeckTypeId candidate_subject;
    CmTypeckSnapshot snapshot;
    CmTypeckStatus typeck_status;
    CmParamEnvStatus env_status;
    CmTraitMatchResult match;

    typeck_status = cm_typeck_snapshot(typeck, &snapshot);
    if (typeck_status != CM_TYPECK_OK) {
        return cm_trait_match_result(CM_TRAIT_MATCH_TYPECK_FAILURE,
            typeck_status);
    }
    env_status = cm_param_env_instantiate_implemented(environment,
        fact_index, typeck, substitution, &candidate_subject,
        &candidate_trait, &typeck_status);
    if (env_status == CM_PARAM_ENV_READY) {
        typeck_status = cm_typeck_unify(typeck, candidate_subject,
            goal->self_type);
        if (typeck_status == CM_TYPECK_OK) {
            match = cm_trait_match_typeck_named(typeck, &candidate_trait,
                &goal->trait_type);
        } else if (typeck_status == CM_TYPECK_TYPE_MISMATCH) {
            match = cm_trait_match_result(CM_TRAIT_MATCH_NO,
                typeck_status);
        } else if (typeck_status == CM_TYPECK_OVERFLOW) {
            match = cm_trait_match_result(CM_TRAIT_MATCH_OVERFLOW,
                typeck_status);
        } else {
            match = cm_trait_match_result(CM_TRAIT_MATCH_TYPECK_FAILURE,
                typeck_status);
        }
    } else if (env_status == CM_PARAM_ENV_UNSUPPORTED) {
        match = cm_trait_match_result(CM_TRAIT_MATCH_UNSUPPORTED,
            typeck_status);
    } else if (env_status == CM_PARAM_ENV_OVERFLOW) {
        match = cm_trait_match_result(CM_TRAIT_MATCH_OVERFLOW,
            typeck_status);
    } else {
        match = cm_trait_match_result(CM_TRAIT_MATCH_TYPECK_FAILURE,
            typeck_status);
    }
    if (keep_bindings && match.kind == CM_TRAIT_MATCH_YES) {
        if (cm_typeck_commit(typeck, &snapshot) != CM_TYPECK_OK) {
            return cm_trait_match_result(CM_TRAIT_MATCH_TYPECK_FAILURE,
                CM_TYPECK_INVALID_SNAPSHOT);
        }
    } else if (cm_typeck_rollback(typeck, &snapshot) != CM_TYPECK_OK) {
        return cm_trait_match_result(CM_TRAIT_MATCH_TYPECK_FAILURE,
            CM_TYPECK_INVALID_SNAPSHOT);
    }
    return match;
}

CmTraitSelectionResult cm_trait_solver_solve_implemented_with_evaluator(
    const CmTraitImplIndex *index, const CmParamEnv *environment,
    CmTypeckContext *typeck, const CmParamEnvSubstitution *substitution,
    const CmImplementedTraitGoal *goal,
    const CmTraitGoalEvaluator *evaluator)
{
    const CmTraitImplIndexState *index_state;
    const CmParamEnvFact *fact;
    CmTraitSelectionResult result;
    CmTraitSolverResultKind validation;
    CmHirDefId enclosing_owner;
    size_t fact_index;
    size_t winner_index;

    result = cm_trait_result(CM_TRAIT_SOLVER_INVALID);
    index_state = cm_trait_index_state_const(index);
    if (!cm_trait_index_is_current(index_state)
        || !cm_param_env_is_current(environment)
        || cm_param_env_hir(environment) != index_state->hir
        || goal == NULL
        || !cm_hir_def_id_equal(goal->owner,
            cm_param_env_exact_owner(environment))) return result;
    if (substitution == NULL || substitution->exact == NULL
        || !cm_typeck_instantiation_is_valid(typeck, substitution->exact)
        || !cm_hir_def_id_equal(substitution->exact->parameter_owner,
            goal->owner)) return result;
    enclosing_owner = cm_param_env_enclosing_owner(environment);
    if (!cm_hir_def_id_is_none(enclosing_owner)) {
        if (substitution->enclosing == NULL
            || !cm_typeck_instantiation_is_valid(typeck,
                substitution->enclosing)
            || !cm_hir_def_id_equal(
                substitution->enclosing->parameter_owner,
                enclosing_owner)) return result;
    } else if (substitution->enclosing != NULL) {
        return result;
    }
    validation = cm_trait_solver_validate_implemented_goal(index_state->hir,
        typeck, goal->self_type, &goal->trait_type);
    if (validation != CM_TRAIT_SOLVER_PROVEN) {
        result.kind = validation;
        return result;
    }
    result = cm_trait_result(CM_TRAIT_SOLVER_NO_SOLUTION);
    winner_index = (size_t)-1;
    for (fact_index = 0u;
         fact_index < cm_param_env_fact_count(environment); ++fact_index) {
        CmTraitMatchResult match;

        fact = cm_param_env_fact(environment, fact_index);
        if (fact == NULL || fact->kind != CM_PARAM_ENV_FACT_IMPLEMENTED
            || !cm_hir_def_id_equal(
                fact->data.implemented.trait_type.definition,
                goal->trait_type.definition)) continue;
        if ((fact->blocker_flags & CM_PARAM_ENV_BLOCK_OVERFLOW) != 0u) {
            result.kind = CM_TRAIT_SOLVER_OVERFLOW;
            return result;
        }
        if (fact->blocker_flags != CM_PARAM_ENV_BLOCK_NONE) {
            result.blocking_match_count += 1u;
            continue;
        }
        match = cm_trait_match_environment_fact(environment, fact_index,
            typeck, substitution, goal, 0);
        if (match.kind == CM_TRAIT_MATCH_YES) {
            result.supported_match_count += 1u;
            if (winner_index == (size_t)-1) winner_index = fact_index;
        } else if (match.kind == CM_TRAIT_MATCH_UNSUPPORTED) {
            result.blocking_match_count += 1u;
        } else if (match.kind == CM_TRAIT_MATCH_OVERFLOW) {
            result.kind = CM_TRAIT_SOLVER_OVERFLOW;
            result.typeck_status = match.typeck_status;
            return result;
        } else if (match.kind == CM_TRAIT_MATCH_TYPECK_FAILURE) {
            result.kind = CM_TRAIT_SOLVER_TYPECK_FAILURE;
            result.typeck_status = match.typeck_status;
            return result;
        }
    }
    if (result.blocking_match_count != 0u) {
        result.kind = CM_TRAIT_SOLVER_UNSUPPORTED;
        return result;
    }
    if (result.supported_match_count != 0u) {
        CmTraitMatchResult replay;

        replay = cm_trait_match_environment_fact(environment, winner_index,
            typeck, substitution, goal, 1);
        if (replay.kind == CM_TRAIT_MATCH_YES) {
            result.kind = CM_TRAIT_SOLVER_PROVEN;
            return result;
        }
        result.kind = replay.kind == CM_TRAIT_MATCH_OVERFLOW
            ? CM_TRAIT_SOLVER_OVERFLOW : CM_TRAIT_SOLVER_TYPECK_FAILURE;
        result.typeck_status = replay.typeck_status;
        return result;
    }
    return cm_trait_solver_select_inner(index, typeck, goal->self_type,
        &goal->trait_type, goal->owner, evaluator, NULL);
}

CmTraitSelectionResult cm_trait_solver_solve_implemented(
    const CmTraitImplIndex *index, const CmParamEnv *environment,
    CmTypeckContext *typeck, const CmParamEnvSubstitution *substitution,
    const CmImplementedTraitGoal *goal)
{
    return cm_trait_solver_solve_implemented_with_evaluator(index,
        environment, typeck, substitution, goal, NULL);
}

CmTraitSelectionResult cm_trait_solver_solve_projection_equality(
    const CmTraitImplIndex *index, const CmParamEnv *environment,
    CmTypeckContext *typeck, const CmParamEnvSubstitution *substitution,
    const CmProjectionEqualityGoal *goal,
    const CmTraitGoalEvaluator *evaluator)
{
    const CmTraitImplIndexState *index_state;
    const CmHirItem *trait_item;
    const CmHirItem *associated_item;
    const CmTypeckType *projection;
    CmTraitProjectionMatchGoal projection_goal;
    CmTraitSelectionResult result;
    CmTraitTypeScan expected_scan;
    CmTypeckNamedType projection_trait;
    CmHirDefId projection_associated_definition;
    CmTypeckTypeId resolved_projection;
    CmTypeckTypeId projection_self;
    CmTypeckStatus typeck_status;
    CmHirDefId enclosing_owner;

    result = cm_trait_result(CM_TRAIT_SOLVER_INVALID);
    index_state = cm_trait_index_state_const(index);
    if (!cm_trait_index_is_current(index_state)
        || !cm_param_env_is_current(environment)
        || cm_param_env_hir(environment) != index_state->hir
        || typeck == NULL
        || cm_typeck_hir_context(typeck) != index_state->hir
        || goal == NULL
        || !cm_hir_def_id_equal(goal->owner,
            cm_param_env_exact_owner(environment))) return result;
    if (substitution == NULL || substitution->exact == NULL
        || !cm_typeck_instantiation_is_valid(typeck, substitution->exact)
        || !cm_hir_def_id_equal(substitution->exact->parameter_owner,
            goal->owner)) return result;
    enclosing_owner = cm_param_env_enclosing_owner(environment);
    if (!cm_hir_def_id_is_none(enclosing_owner)) {
        if (substitution->enclosing == NULL
            || !cm_typeck_instantiation_is_valid(typeck,
                substitution->enclosing)
            || !cm_hir_def_id_equal(
                substitution->enclosing->parameter_owner,
                enclosing_owner)) return result;
    } else if (substitution->enclosing != NULL) {
        return result;
    }
    typeck_status = cm_typeck_resolve(typeck, goal->projection_type,
        &resolved_projection);
    if (typeck_status == CM_TYPECK_OVERFLOW) {
        result.kind = CM_TRAIT_SOLVER_OVERFLOW;
        result.typeck_status = typeck_status;
        return result;
    }
    if (typeck_status != CM_TYPECK_OK
        || cm_typeck_get_type(typeck, goal->expected_type) == NULL) {
        return result;
    }
    projection = cm_typeck_get_type(typeck, resolved_projection);
    if (projection == NULL
        || projection->kind != CM_TYPECK_TYPE_PROJECTION) return result;
    projection_self = projection->data.projection_type.self_type;
    projection_trait = projection->data.projection_type.trait_type;
    projection_associated_definition =
        projection->data.projection_type.associated_type.definition;
    if (projection->data.projection_type.associated_type.argument_count != 0u
        || projection->data.projection_type.associated_type.arguments
            != NULL) {
        result.kind = CM_TRAIT_SOLVER_UNSUPPORTED;
        return result;
    }
    trait_item = cm_trait_find_definition_item(index_state->hir,
        projection_trait.definition);
    associated_item = cm_trait_find_definition_item(index_state->hir,
        projection_associated_definition);
    if (trait_item == NULL) {
        if (cm_trait_definition_is_known_foreign(index_state,
                projection_trait.definition, CM_HIR_ITEM_TRAIT)) {
            result.kind = CM_TRAIT_SOLVER_DEFERRED_METADATA;
        }
        return result;
    }
    if (trait_item->kind != CM_HIR_ITEM_TRAIT) return result;
    if (associated_item == NULL) {
        if (trait_item->definition.crate_id != index_state->local_crate
            || cm_trait_definition_is_known_foreign(index_state,
                projection_associated_definition,
                CM_HIR_ITEM_TYPE_ALIAS)) {
            result.kind = CM_TRAIT_SOLVER_DEFERRED_METADATA;
        }
        return result;
    }
    if (associated_item->kind != CM_HIR_ITEM_TYPE_ALIAS
        || !cm_hir_def_id_is_none(associated_item->data.type_alias_item
                .trait_item_definition)
        || !cm_hir_def_id_equal(associated_item->parent_definition,
            trait_item->definition)) return result;
    if (associated_item->data.type_alias_item.target != CM_HIR_TYPE_NONE) {
        result.kind = CM_TRAIT_SOLVER_UNSUPPORTED;
        return result;
    }
    if (trait_item->definition.crate_id != index_state->local_crate
        || associated_item->definition.crate_id
            != index_state->local_crate) {
        result.kind = CM_TRAIT_SOLVER_DEFERRED_METADATA;
        return result;
    }
    if (associated_item->generic_parameter_count != 0u) {
        result.kind = CM_TRAIT_SOLVER_UNSUPPORTED;
        return result;
    }
    expected_scan = cm_trait_scan_typeck_type(typeck, goal->expected_type);
    if (expected_scan == CM_TRAIT_TYPE_SCAN_PROJECTION
        || expected_scan == CM_TRAIT_TYPE_SCAN_UNSUPPORTED) {
        result.kind = CM_TRAIT_SOLVER_UNSUPPORTED;
        return result;
    }
    if (expected_scan == CM_TRAIT_TYPE_SCAN_OVERFLOW) {
        result.kind = CM_TRAIT_SOLVER_OVERFLOW;
        result.typeck_status = CM_TYPECK_OVERFLOW;
        return result;
    }
    if (expected_scan == CM_TRAIT_TYPE_SCAN_INVALID) return result;
    projection_goal.associated_definition = associated_item->definition;
    projection_goal.expected_type = goal->expected_type;
    return cm_trait_solver_select_inner(index, typeck,
        projection_self, &projection_trait, goal->owner, evaluator,
        &projection_goal);
}

const char *cm_trait_solver_result_name(CmTraitSolverResultKind result)
{
    switch (result) {
    case CM_TRAIT_SOLVER_PROVEN: return "proven";
    case CM_TRAIT_SOLVER_NEGATIVE: return "negative";
    case CM_TRAIT_SOLVER_NO_SOLUTION: return "no-solution";
    case CM_TRAIT_SOLVER_AMBIGUOUS: return "ambiguous";
    case CM_TRAIT_SOLVER_DEFERRED_INFERENCE: return "deferred-inference";
    case CM_TRAIT_SOLVER_DEFERRED_METADATA: return "deferred-metadata";
    case CM_TRAIT_SOLVER_UNSUPPORTED: return "unsupported";
    case CM_TRAIT_SOLVER_OVERFLOW: return "overflow";
    case CM_TRAIT_SOLVER_INVALID: return "invalid";
    case CM_TRAIT_SOLVER_TYPECK_FAILURE: return "typeck-failure";
    }
    return "unknown";
}
