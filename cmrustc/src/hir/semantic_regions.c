#include "cm/hir/semantic_regions.h"

#include "cm/alloc.h"
#include "cm/hir/body.h"

#include <string.h>

#define CM_SEMANTIC_REGIONS_NESTING_LIMIT ((size_t)128u)
#define CM_SEMANTIC_REGIONS_SLICE_LIMIT ((uint32_t)4096u)

typedef enum CmSemanticRegionsNamedTarget {
    CM_SEMANTIC_REGIONS_NAMED_FUNCTION = 0,
    CM_SEMANTIC_REGIONS_NAMED_ADT,
    CM_SEMANTIC_REGIONS_NAMED_ALIAS,
    CM_SEMANTIC_REGIONS_NAMED_TRAIT,
    CM_SEMANTIC_REGIONS_NAMED_FOREIGN
} CmSemanticRegionsNamedTarget;

typedef struct CmSemanticRegionsScratch {
    const CmHirContext *hir;
    const CmHirBodyId *bodies;
    size_t body_count;
    unsigned char *visited;
    unsigned char *type_gray;
    size_t body_index;
    CmHirBodyId body;
    CmHirExprId expression;
    const CmHirItem *owner;
    const CmHirItem *parent;
    const CmHirItem *scope_owner;
    const CmHirItem *scope_parent;
    uint32_t scope_parameter_limit;
    int scope_parameter_limited;
    CmSemanticRegionsResult result;
} CmSemanticRegionsScratch;

static CmSemanticRegionsResult cm_semantic_regions_result(
    CmSemanticRegionsStatus status)
{
    CmSemanticRegionsResult result;

    memset(&result, 0, sizeof(result));
    result.status = status;
    result.body_index = CM_SEMANTIC_REGIONS_BODY_INDEX_NONE;
    result.body = CM_HIR_BODY_NONE;
    result.expression = CM_HIR_EXPR_NONE;
    result.type = CM_HIR_TYPE_NONE;
    result.generic_parameter = CM_HIR_GENERIC_PARAM_NONE;
    return result;
}

static int cm_semantic_regions_fail(CmSemanticRegionsScratch *scratch,
    CmSemanticRegionsStatus status, CmHirTypeId type)
{
    scratch->result.status = status;
    scratch->result.body_index = scratch->body_index;
    scratch->result.body = scratch->body;
    scratch->result.expression = scratch->expression;
    scratch->result.type = type;
    return 0;
}

static int cm_semantic_regions_fail_region(
    CmSemanticRegionsScratch *scratch, CmSemanticRegionsStatus status,
    CmHirTypeId type, const CmHirRegion *region)
{
    cm_semantic_regions_fail(scratch, status, type);
    scratch->result.has_region = 1;
    if (region != NULL) {
        scratch->result.region_kind = region->kind;
        if (region->kind == CM_HIR_REGION_EARLY_BOUND) {
            scratch->result.generic_parameter = region->data.parameter;
        }
    }
    return 0;
}

static const CmHirItem *cm_semantic_regions_item(
    const CmHirContext *hir, CmHirDefId definition)
{
    const CmHirDefinition *record;
    const CmHirItem *item;

    record = cm_hir_lookup_definition(hir, definition);
    item = record == NULL || record->kind != CM_HIR_DEFINITION_ITEM
            || record->state != CM_HIR_DEFINITION_BOUND
        ? NULL : cm_hir_get_item(hir, record->entity.item_id);
    return item != NULL
            && cm_hir_def_id_equal(item->definition, definition)
        ? item : NULL;
}

static int cm_semantic_regions_constraints_empty(const CmHirItem *item)
{
    return item != NULL
        && item->predicate_scope_count == 0u
        && item->predicate_scopes == NULL
        && item->predicate_count == 0u
        && item->predicates == NULL
        && item->outlives_predicate_count == 0u
        && item->outlives_predicates == NULL;
}

static int cm_semantic_regions_parameter_owned_by(
    const CmSemanticRegionsScratch *scratch, CmHirGenericParamId id,
    const CmHirItem *item, CmHirGenericParamKind kind)
{
    const CmHirGenericParam *parameter;
    uint32_t offset;

    if (item == NULL || id == CM_HIR_GENERIC_PARAM_NONE
        || item->generic_parameter_count == 0u
        || item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE
        || id < item->generic_parameter_start) return 0;
    offset = id - item->generic_parameter_start;
    if (offset >= item->generic_parameter_count) return 0;
    parameter = cm_hir_get_generic_param(scratch->hir, id);
    return parameter != NULL && parameter->kind == kind
        && parameter->index == offset
        && cm_hir_def_id_equal(parameter->owner, item->definition);
}

static int cm_semantic_regions_parameter_in_scope(
    const CmSemanticRegionsScratch *scratch, CmHirGenericParamId id,
    CmHirGenericParamKind kind)
{
    if (cm_semantic_regions_parameter_owned_by(scratch, id,
            scratch->scope_owner, kind)) {
        return !scratch->scope_parameter_limited
            || id - scratch->scope_owner->generic_parameter_start
                < scratch->scope_parameter_limit;
    }
    return !scratch->scope_parameter_limited
        && cm_semantic_regions_parameter_owned_by(scratch, id,
            scratch->scope_parent, kind);
}

static int cm_semantic_regions_type_equal(
    const CmHirContext *hir, CmHirTypeId left_id, CmHirTypeId right_id,
    size_t depth);

static int cm_semantic_regions_region_equal(const CmHirRegion *left,
    const CmHirRegion *right)
{
    if (left == NULL || right == NULL || left->kind != right->kind) return 0;
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

static int cm_semantic_regions_const_equal(const CmHirContext *hir,
    const CmHirConstArg *left, const CmHirConstArg *right, size_t depth)
{
    if (left == NULL || right == NULL || left->kind != right->kind
        || !cm_semantic_regions_type_equal(hir, left->type, right->type,
            depth + 1u)) return 0;
    switch (left->kind) {
    case CM_HIR_CONST_VALUE:
        return left->data.value.low_bits == right->data.value.low_bits
            && left->data.value.high_bits == right->data.value.high_bits;
    case CM_HIR_CONST_PARAMETER:
        return left->data.parameter == right->data.parameter;
    case CM_HIR_CONST_UNEVALUATED:
    case CM_HIR_CONST_INFER:
    case CM_HIR_CONST_ERROR:
        return 0;
    }
    return 0;
}

static int cm_semantic_regions_argument_equal(const CmHirContext *hir,
    const CmHirGenericArg *left, const CmHirGenericArg *right, size_t depth)
{
    if (left == NULL || right == NULL || left->kind != right->kind) return 0;
    switch (left->kind) {
    case CM_HIR_GENERIC_ARG_LIFETIME:
        return cm_semantic_regions_region_equal(&left->data.lifetime,
            &right->data.lifetime);
    case CM_HIR_GENERIC_ARG_TYPE:
        return cm_semantic_regions_type_equal(hir, left->data.type,
            right->data.type, depth + 1u);
    case CM_HIR_GENERIC_ARG_CONST:
        return cm_semantic_regions_const_equal(hir, &left->data.constant,
            &right->data.constant, depth + 1u);
    }
    return 0;
}

static int cm_semantic_regions_named_equal(const CmHirContext *hir,
    const CmHirNamedType *left, const CmHirNamedType *right, size_t depth)
{
    uint32_t index;

    if (left == NULL || right == NULL
        || !cm_hir_def_id_equal(left->definition, right->definition)
        || left->argument_count != right->argument_count
        || left->argument_count > CM_SEMANTIC_REGIONS_SLICE_LIMIT
        || (left->argument_count == 0u) != (left->arguments == NULL)
        || (right->argument_count == 0u) != (right->arguments == NULL)) {
        return 0;
    }
    for (index = 0u; index < left->argument_count; ++index) {
        if (!cm_semantic_regions_argument_equal(hir,
                &left->arguments[index], &right->arguments[index],
                depth + 1u)) return 0;
    }
    return 1;
}

static int cm_semantic_regions_type_equal(
    const CmHirContext *hir, CmHirTypeId left_id, CmHirTypeId right_id,
    size_t depth)
{
    const CmHirType *left;
    const CmHirType *right;
    uint32_t index;

    if (left_id == right_id) return left_id != CM_HIR_TYPE_NONE;
    if (hir == NULL || depth >= CM_SEMANTIC_REGIONS_NESTING_LIMIT) return 0;
    left = cm_hir_get_type(hir, left_id);
    right = cm_hir_get_type(hir, right_id);
    if (left == NULL || right == NULL || left->kind != right->kind) return 0;
    switch (left->kind) {
    case CM_HIR_TYPE_NEVER_KIND:
    case CM_HIR_TYPE_UNIT_KIND:
    case CM_HIR_TYPE_BOOL_KIND:
    case CM_HIR_TYPE_CHAR_KIND:
    case CM_HIR_TYPE_STR_KIND:
        return 1;
    case CM_HIR_TYPE_INTEGER_KIND:
        return left->data.integer_type.kind
            == right->data.integer_type.kind;
    case CM_HIR_TYPE_FLOAT_KIND:
        return left->data.float_type.kind == right->data.float_type.kind;
    case CM_HIR_TYPE_REFERENCE_KIND:
        return left->data.reference_type.mutability
                == right->data.reference_type.mutability
            && cm_semantic_regions_region_equal(
                &left->data.reference_type.region,
                &right->data.reference_type.region)
            && cm_semantic_regions_type_equal(hir,
                left->data.reference_type.pointee,
                right->data.reference_type.pointee, depth + 1u);
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        return left->data.raw_pointer_type.mutability
                == right->data.raw_pointer_type.mutability
            && cm_semantic_regions_type_equal(hir,
                left->data.raw_pointer_type.pointee,
                right->data.raw_pointer_type.pointee, depth + 1u);
    case CM_HIR_TYPE_TUPLE_KIND:
        if (left->data.tuple_type.element_count
                != right->data.tuple_type.element_count
            || left->data.tuple_type.element_count
                > CM_SEMANTIC_REGIONS_SLICE_LIMIT
            || (left->data.tuple_type.element_count == 0u)
                != (left->data.tuple_type.elements == NULL)
            || (right->data.tuple_type.element_count == 0u)
                != (right->data.tuple_type.elements == NULL)) return 0;
        for (index = 0u; index < left->data.tuple_type.element_count;
             ++index) {
            if (!cm_semantic_regions_type_equal(hir,
                    left->data.tuple_type.elements[index],
                    right->data.tuple_type.elements[index], depth + 1u)) {
                return 0;
            }
        }
        return 1;
    case CM_HIR_TYPE_ARRAY_KIND:
        return cm_semantic_regions_type_equal(hir,
                left->data.array_type.element,
                right->data.array_type.element, depth + 1u)
            && cm_semantic_regions_const_equal(hir,
                &left->data.array_type.length,
                &right->data.array_type.length, depth + 1u);
    case CM_HIR_TYPE_SLICE_KIND:
        return cm_semantic_regions_type_equal(hir,
            left->data.slice_type.element, right->data.slice_type.element,
            depth + 1u);
    case CM_HIR_TYPE_FN_POINTER_KIND:
        if (left->data.fn_pointer_type.parameter_count
                != right->data.fn_pointer_type.parameter_count
            || left->data.fn_pointer_type.parameter_count
                > CM_SEMANTIC_REGIONS_SLICE_LIMIT
            || left->data.fn_pointer_type.abi
                != right->data.fn_pointer_type.abi
            || left->data.fn_pointer_type.safety
                != right->data.fn_pointer_type.safety
            || left->data.fn_pointer_type.is_variadic
                != right->data.fn_pointer_type.is_variadic
            || (left->data.fn_pointer_type.parameter_count == 0u)
                != (left->data.fn_pointer_type.parameters == NULL)
            || (right->data.fn_pointer_type.parameter_count == 0u)
                != (right->data.fn_pointer_type.parameters == NULL)) {
            return 0;
        }
        for (index = 0u;
             index < left->data.fn_pointer_type.parameter_count; ++index) {
            if (!cm_semantic_regions_type_equal(hir,
                    left->data.fn_pointer_type.parameters[index],
                    right->data.fn_pointer_type.parameters[index],
                    depth + 1u)) return 0;
        }
        return cm_semantic_regions_type_equal(hir,
            left->data.fn_pointer_type.return_type,
            right->data.fn_pointer_type.return_type, depth + 1u);
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
    case CM_HIR_TYPE_ADT_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
        return cm_semantic_regions_named_equal(hir, &left->data.named_type,
            &right->data.named_type, depth + 1u);
    case CM_HIR_TYPE_SELF_KIND:
        return cm_hir_def_id_equal(left->data.self_type.owner,
            right->data.self_type.owner);
    case CM_HIR_TYPE_PARAMETER_KIND:
        return left->data.parameter_type.parameter
            == right->data.parameter_type.parameter;
    case CM_HIR_TYPE_PROJECTION_KIND:
        return cm_semantic_regions_type_equal(hir,
                left->data.projection_type.self_type,
                right->data.projection_type.self_type, depth + 1u)
            && cm_semantic_regions_named_equal(hir,
                &left->data.projection_type.trait_type,
                &right->data.projection_type.trait_type, depth + 1u)
            && cm_semantic_regions_named_equal(hir,
                &left->data.projection_type.associated_type,
                &right->data.projection_type.associated_type, depth + 1u);
    case CM_HIR_TYPE_DYN_TRAIT_KIND:
        return cm_semantic_regions_named_equal(hir,
                &left->data.dyn_trait_type.principal_trait,
                &right->data.dyn_trait_type.principal_trait, depth + 1u)
            && cm_semantic_regions_region_equal(
                &left->data.dyn_trait_type.region,
                &right->data.dyn_trait_type.region);
    case CM_HIR_TYPE_ERROR_KIND:
    case CM_HIR_TYPE_INFER_KIND:
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
    case CM_HIR_TYPE_OPAQUE_KIND:
    case CM_HIR_TYPE_CLOSURE_KIND:
        return 0;
    default:
        return 0;
    }
}

static int cm_semantic_regions_scan_type(
    CmSemanticRegionsScratch *scratch, CmHirTypeId type_id, size_t depth);

static int cm_semantic_regions_scan_region(
    CmSemanticRegionsScratch *scratch, CmHirTypeId type_id,
    const CmHirRegion *region)
{
    if (region == NULL) {
        return cm_semantic_regions_fail(scratch,
            CM_SEMANTIC_REGIONS_INVALID_HIR, type_id);
    }
    switch (region->kind) {
    case CM_HIR_REGION_STATIC:
    case CM_HIR_REGION_ERASED:
        return 1;
    case CM_HIR_REGION_EARLY_BOUND:
        if (cm_semantic_regions_parameter_in_scope(scratch,
                region->data.parameter, CM_HIR_GENERIC_LIFETIME)) return 1;
        return cm_semantic_regions_fail_region(scratch,
            CM_SEMANTIC_REGIONS_INVALID_HIR, type_id, region);
    case CM_HIR_REGION_LATE_BOUND:
    case CM_HIR_REGION_INFER:
    case CM_HIR_REGION_ERROR:
        return cm_semantic_regions_fail_region(scratch,
            CM_SEMANTIC_REGIONS_UNRESOLVED_REGION, type_id, region);
    }
    return cm_semantic_regions_fail_region(scratch,
        CM_SEMANTIC_REGIONS_INVALID_HIR, type_id, region);
}

static int cm_semantic_regions_scan_const(
    CmSemanticRegionsScratch *scratch, const CmHirConstArg *constant,
    size_t depth)
{
    const CmHirGenericParam *parameter;

    if (constant == NULL
        || !cm_semantic_regions_scan_type(scratch, constant->type,
            depth + 1u)) return 0;
    switch (constant->kind) {
    case CM_HIR_CONST_VALUE:
        return 1;
    case CM_HIR_CONST_PARAMETER:
        parameter = cm_hir_get_generic_param(scratch->hir,
            constant->data.parameter);
        if (cm_semantic_regions_parameter_in_scope(scratch,
                constant->data.parameter, CM_HIR_GENERIC_CONST)
            && parameter != NULL
            && cm_semantic_regions_type_equal(scratch->hir,
                constant->type, parameter->declared_type, 0u)) return 1;
        scratch->result.generic_parameter = constant->data.parameter;
        return cm_semantic_regions_fail(scratch,
            CM_SEMANTIC_REGIONS_INVALID_HIR, constant->type);
    case CM_HIR_CONST_UNEVALUATED:
        /* No stable type-position body atom authenticates this expression. */
        return cm_semantic_regions_fail(scratch,
            CM_SEMANTIC_REGIONS_INVALID_HIR, constant->type);
    case CM_HIR_CONST_INFER:
        return cm_semantic_regions_fail(scratch,
            CM_SEMANTIC_REGIONS_UNRESOLVED_REGION, constant->type);
    case CM_HIR_CONST_ERROR:
        return cm_semantic_regions_fail(scratch,
            CM_SEMANTIC_REGIONS_INVALID_HIR, constant->type);
    }
    return cm_semantic_regions_fail(scratch,
        CM_SEMANTIC_REGIONS_INVALID_HIR, constant->type);
}

static int cm_semantic_regions_scan_argument(
    CmSemanticRegionsScratch *scratch, const CmHirGenericArg *argument,
    CmHirTypeId container_type, size_t depth)
{
    if (argument == NULL) {
        return cm_semantic_regions_fail(scratch,
            CM_SEMANTIC_REGIONS_INVALID_HIR, container_type);
    }
    switch (argument->kind) {
    case CM_HIR_GENERIC_ARG_LIFETIME:
        return cm_semantic_regions_scan_region(scratch, container_type,
            &argument->data.lifetime);
    case CM_HIR_GENERIC_ARG_TYPE:
        return cm_semantic_regions_scan_type(scratch, argument->data.type,
            depth + 1u);
    case CM_HIR_GENERIC_ARG_CONST:
        return cm_semantic_regions_scan_const(scratch,
            &argument->data.constant, depth + 1u);
    }
    return cm_semantic_regions_fail(scratch,
        CM_SEMANTIC_REGIONS_INVALID_HIR, container_type);
}

static int cm_semantic_regions_scan_named(
    CmSemanticRegionsScratch *scratch, const CmHirNamedType *named,
    CmHirTypeId container_type, CmSemanticRegionsNamedTarget target_kind,
    size_t depth)
{
    const CmHirItem *target;
    uint32_t index;

    if (named == NULL || cm_hir_def_id_is_none(named->definition)
        || (named->argument_count == 0u) != (named->arguments == NULL)
        || named->argument_count > CM_SEMANTIC_REGIONS_SLICE_LIMIT) {
        return cm_semantic_regions_fail(scratch,
            CM_SEMANTIC_REGIONS_INVALID_HIR, container_type);
    }
    target = cm_semantic_regions_item(scratch->hir, named->definition);
    if (target == NULL
        || (target_kind == CM_SEMANTIC_REGIONS_NAMED_FUNCTION
                ? target->kind != CM_HIR_ITEM_FUNCTION
            : target_kind == CM_SEMANTIC_REGIONS_NAMED_ADT
                ? target->kind != CM_HIR_ITEM_STRUCT
                    && target->kind != CM_HIR_ITEM_UNION
                    && target->kind != CM_HIR_ITEM_ENUM
            : target_kind == CM_SEMANTIC_REGIONS_NAMED_ALIAS
                ? target->kind != CM_HIR_ITEM_TYPE_ALIAS
            : target_kind == CM_SEMANTIC_REGIONS_NAMED_TRAIT
                ? target->kind != CM_HIR_ITEM_TRAIT
            : target_kind == CM_SEMANTIC_REGIONS_NAMED_FOREIGN
                ? target->kind != CM_HIR_ITEM_EXTERN_TYPE
                : 1)
        || ((target_kind == CM_SEMANTIC_REGIONS_NAMED_FUNCTION
                || target_kind == CM_SEMANTIC_REGIONS_NAMED_ADT
                || target_kind == CM_SEMANTIC_REGIONS_NAMED_TRAIT
                || target_kind == CM_SEMANTIC_REGIONS_NAMED_FOREIGN)
            && !cm_hir_def_id_is_none(target->parent_definition))
        || (target_kind == CM_SEMANTIC_REGIONS_NAMED_FUNCTION
            && !cm_hir_def_id_is_none(
                target->data.function_item.trait_item_definition))
        || (target_kind == CM_SEMANTIC_REGIONS_NAMED_FOREIGN
            && target->generic_parameter_count != 0u)
        || named->argument_count != target->generic_parameter_count
        || (target->generic_parameter_count == 0u
                ? target->generic_parameter_start
                    != CM_HIR_GENERIC_PARAM_NONE
                : target->generic_parameter_start
                        == CM_HIR_GENERIC_PARAM_NONE
                    || target->generic_parameter_count - 1u > UINT32_MAX
                        - target->generic_parameter_start
                    || (size_t)target->generic_parameter_start
                            + (size_t)target->generic_parameter_count - 1u
                        > scratch->hir->generic_parameters.len)) {
        return cm_semantic_regions_fail(scratch,
            CM_SEMANTIC_REGIONS_INVALID_HIR, container_type);
    }
    for (index = 0u; index < named->argument_count; ++index) {
        const CmHirGenericParam *parameter;

        parameter = cm_hir_get_generic_param(scratch->hir,
            target->generic_parameter_start + index);
        if (parameter == NULL || parameter->index != index
            || !cm_hir_def_id_equal(parameter->owner,
                target->definition)
            || (named->arguments[index].kind == CM_HIR_GENERIC_ARG_LIFETIME
                    ? parameter->kind != CM_HIR_GENERIC_LIFETIME
                : named->arguments[index].kind == CM_HIR_GENERIC_ARG_TYPE
                    ? parameter->kind != CM_HIR_GENERIC_TYPE
                : named->arguments[index].kind == CM_HIR_GENERIC_ARG_CONST
                    ? parameter->kind != CM_HIR_GENERIC_CONST
                    : 1)) {
            return cm_semantic_regions_fail(scratch,
                CM_SEMANTIC_REGIONS_INVALID_HIR, container_type);
        }
        if (parameter->kind == CM_HIR_GENERIC_CONST
            && !cm_semantic_regions_type_equal(scratch->hir,
                named->arguments[index].data.constant.type,
                parameter->declared_type, 0u)) {
            return cm_semantic_regions_fail(scratch,
                CM_SEMANTIC_REGIONS_INVALID_HIR, container_type);
        }
        if (!cm_semantic_regions_scan_argument(scratch,
                &named->arguments[index], container_type, depth + 1u)) {
            return 0;
        }
    }
    return 1;
}

static int cm_semantic_regions_scan_type(
    CmSemanticRegionsScratch *scratch, CmHirTypeId type_id, size_t depth)
{
    const CmHirType *type;
    uint32_t index;

    size_t slot;
    int ok;

    if (type_id == CM_HIR_TYPE_NONE
        || depth >= CM_SEMANTIC_REGIONS_NESTING_LIMIT) {
        return cm_semantic_regions_fail(scratch,
            CM_SEMANTIC_REGIONS_INVALID_HIR, type_id);
    }
    type = cm_hir_get_type(scratch->hir, type_id);
    if (type == NULL) {
        return cm_semantic_regions_fail(scratch,
            CM_SEMANTIC_REGIONS_INVALID_HIR, type_id);
    }
    slot = (size_t)type_id - 1u;
    if (scratch->type_gray[slot] != 0u) {
        return cm_semantic_regions_fail(scratch,
            CM_SEMANTIC_REGIONS_INVALID_HIR, type_id);
    }
    scratch->type_gray[slot] = 1u;
    ok = 0;
    switch (type->kind) {
    case CM_HIR_TYPE_ERROR_KIND:
        ok = cm_semantic_regions_fail(scratch,
            CM_SEMANTIC_REGIONS_INVALID_HIR, type_id);
        break;
    case CM_HIR_TYPE_INFER_KIND:
        ok = cm_semantic_regions_fail(scratch,
            CM_SEMANTIC_REGIONS_UNRESOLVED_REGION, type_id);
        break;
    case CM_HIR_TYPE_NEVER_KIND:
    case CM_HIR_TYPE_UNIT_KIND:
    case CM_HIR_TYPE_BOOL_KIND:
    case CM_HIR_TYPE_CHAR_KIND:
    case CM_HIR_TYPE_STR_KIND:
        ok = 1;
        break;
    case CM_HIR_TYPE_INTEGER_KIND:
        ok = (unsigned int)type->data.integer_type.kind
            <= (unsigned int)CM_HIR_INT_USIZE;
        if (!ok) ok = cm_semantic_regions_fail(scratch,
            CM_SEMANTIC_REGIONS_INVALID_HIR, type_id);
        break;
    case CM_HIR_TYPE_FLOAT_KIND:
        ok = (unsigned int)type->data.float_type.kind
            <= (unsigned int)CM_HIR_FLOAT_F128;
        if (!ok) ok = cm_semantic_regions_fail(scratch,
            CM_SEMANTIC_REGIONS_INVALID_HIR, type_id);
        break;
    case CM_HIR_TYPE_REFERENCE_KIND:
        ok = (unsigned int)type->data.reference_type.mutability
                <= (unsigned int)CM_HIR_MUTABLE
            && cm_semantic_regions_scan_region(scratch, type_id,
                &type->data.reference_type.region)
            && cm_semantic_regions_scan_type(scratch,
                type->data.reference_type.pointee, depth + 1u);
        if (!ok && scratch->result.status == CM_SEMANTIC_REGIONS_OK) {
            ok = cm_semantic_regions_fail(scratch,
                CM_SEMANTIC_REGIONS_INVALID_HIR, type_id);
        }
        break;
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        ok = (unsigned int)type->data.raw_pointer_type.mutability
                <= (unsigned int)CM_HIR_MUTABLE
            && cm_semantic_regions_scan_type(scratch,
                type->data.raw_pointer_type.pointee, depth + 1u);
        if (!ok && scratch->result.status == CM_SEMANTIC_REGIONS_OK) {
            ok = cm_semantic_regions_fail(scratch,
                CM_SEMANTIC_REGIONS_INVALID_HIR, type_id);
        }
        break;
    case CM_HIR_TYPE_TUPLE_KIND:
        if ((type->data.tuple_type.element_count == 0u)
                != (type->data.tuple_type.elements == NULL)
            || type->data.tuple_type.element_count
                > CM_SEMANTIC_REGIONS_SLICE_LIMIT) {
            ok = cm_semantic_regions_fail(scratch,
                CM_SEMANTIC_REGIONS_INVALID_HIR, type_id);
            break;
        }
        for (index = 0u; index < type->data.tuple_type.element_count;
             ++index) {
            if (!cm_semantic_regions_scan_type(scratch,
                    type->data.tuple_type.elements[index], depth + 1u)) {
                ok = 0;
                goto done;
            }
        }
        ok = 1;
        break;
    case CM_HIR_TYPE_ARRAY_KIND:
        ok = cm_semantic_regions_scan_type(scratch,
                type->data.array_type.element, depth + 1u)
            && cm_semantic_regions_scan_const(scratch,
                &type->data.array_type.length, depth + 1u);
        break;
    case CM_HIR_TYPE_SLICE_KIND:
        ok = cm_semantic_regions_scan_type(scratch,
            type->data.slice_type.element, depth + 1u);
        break;
    case CM_HIR_TYPE_FN_POINTER_KIND:
        if ((type->data.fn_pointer_type.parameter_count == 0u)
                != (type->data.fn_pointer_type.parameters == NULL)
            || type->data.fn_pointer_type.parameter_count
                > CM_SEMANTIC_REGIONS_SLICE_LIMIT
            || cm_interner_get(&scratch->hir->strings,
                    type->data.fn_pointer_type.abi) == NULL
            || (unsigned int)type->data.fn_pointer_type.safety
                > (unsigned int)CM_HIR_UNSAFE
            || (type->data.fn_pointer_type.is_variadic != 0
                && type->data.fn_pointer_type.is_variadic != 1)) {
            ok = cm_semantic_regions_fail(scratch,
                CM_SEMANTIC_REGIONS_INVALID_HIR, type_id);
            break;
        }
        for (index = 0u;
             index < type->data.fn_pointer_type.parameter_count; ++index) {
            if (!cm_semantic_regions_scan_type(scratch,
                    type->data.fn_pointer_type.parameters[index],
                    depth + 1u)) {
                ok = 0;
                goto done;
            }
        }
        ok = cm_semantic_regions_scan_type(scratch,
            type->data.fn_pointer_type.return_type, depth + 1u);
        break;
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
        ok = cm_semantic_regions_scan_named(scratch,
            &type->data.named_type, type_id,
            CM_SEMANTIC_REGIONS_NAMED_FUNCTION, depth + 1u);
        break;
    case CM_HIR_TYPE_ADT_KIND:
        ok = cm_semantic_regions_scan_named(scratch,
            &type->data.named_type, type_id,
            CM_SEMANTIC_REGIONS_NAMED_ADT, depth + 1u);
        break;
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
    case CM_HIR_TYPE_OPAQUE_KIND:
    case CM_HIR_TYPE_CLOSURE_KIND:
        ok = cm_semantic_regions_fail(scratch,
            CM_SEMANTIC_REGIONS_INVALID_HIR, type_id);
        break;
    case CM_HIR_TYPE_FOREIGN_KIND:
        ok = cm_semantic_regions_scan_named(scratch,
            &type->data.named_type, type_id,
            CM_SEMANTIC_REGIONS_NAMED_FOREIGN, depth + 1u);
        break;
    case CM_HIR_TYPE_SELF_KIND:
        if (scratch->parent != NULL
            && cm_hir_def_id_equal(type->data.self_type.owner,
                scratch->parent->definition)) {
            ok = 1;
        } else {
            ok = cm_semantic_regions_fail(scratch,
                CM_SEMANTIC_REGIONS_INVALID_HIR, type_id);
        }
        break;
    case CM_HIR_TYPE_PARAMETER_KIND:
        if (cm_semantic_regions_parameter_in_scope(scratch,
                type->data.parameter_type.parameter,
                CM_HIR_GENERIC_TYPE)) {
            ok = 1;
        } else {
            scratch->result.generic_parameter =
                type->data.parameter_type.parameter;
            ok = cm_semantic_regions_fail(scratch,
                CM_SEMANTIC_REGIONS_INVALID_HIR, type_id);
        }
        break;
    case CM_HIR_TYPE_PROJECTION_KIND:
    {
        const CmHirItem *trait_item;
        const CmHirItem *associated_item;

        trait_item = cm_semantic_regions_item(scratch->hir,
            type->data.projection_type.trait_type.definition);
        associated_item = cm_semantic_regions_item(scratch->hir,
            type->data.projection_type.associated_type.definition);
        ok = trait_item != NULL && trait_item->kind == CM_HIR_ITEM_TRAIT
            && associated_item != NULL
            && associated_item->kind == CM_HIR_ITEM_TYPE_ALIAS
            && associated_item->data.type_alias_item.target
                == CM_HIR_TYPE_NONE
            && cm_hir_def_id_equal(associated_item->parent_definition,
                trait_item->definition)
            && cm_semantic_regions_scan_type(scratch,
                type->data.projection_type.self_type, depth + 1u)
            && cm_semantic_regions_scan_named(scratch,
                &type->data.projection_type.trait_type, type_id,
                CM_SEMANTIC_REGIONS_NAMED_TRAIT, depth + 1u)
            && cm_semantic_regions_scan_named(scratch,
                &type->data.projection_type.associated_type, type_id,
                CM_SEMANTIC_REGIONS_NAMED_ALIAS, depth + 1u);
        if (!ok && scratch->result.status == CM_SEMANTIC_REGIONS_OK) {
            ok = cm_semantic_regions_fail(scratch,
                CM_SEMANTIC_REGIONS_INVALID_HIR, type_id);
        }
        break;
    }
    case CM_HIR_TYPE_DYN_TRAIT_KIND:
        ok = cm_semantic_regions_scan_named(scratch,
                &type->data.dyn_trait_type.principal_trait, type_id,
                CM_SEMANTIC_REGIONS_NAMED_TRAIT, depth + 1u)
            && cm_semantic_regions_scan_region(scratch, type_id,
                &type->data.dyn_trait_type.region);
        break;
    default:
        ok = cm_semantic_regions_fail(scratch,
            CM_SEMANTIC_REGIONS_INVALID_HIR, type_id);
        break;
    }
done:
    scratch->type_gray[slot] = 0u;
    return ok;
}

static int cm_semantic_regions_scan_generic_parameters(
    CmSemanticRegionsScratch *scratch, const CmHirItem *item)
{
    const CmHirItem *saved_scope_owner;
    const CmHirItem *saved_scope_parent;
    uint32_t saved_scope_parameter_limit;
    int saved_scope_parameter_limited;
    uint32_t index;

    if (item == NULL) return 1;
    if (item->generic_parameter_count > CM_SEMANTIC_REGIONS_SLICE_LIMIT
        || (item->generic_parameter_count == 0u)
            != (item->generic_parameter_start
                == CM_HIR_GENERIC_PARAM_NONE)
        || (item->generic_parameter_count != 0u
            && (item->generic_parameter_count - 1u > UINT32_MAX
                    - item->generic_parameter_start
                || (size_t)item->generic_parameter_start
                        + (size_t)item->generic_parameter_count - 1u
                    > scratch->hir->generic_parameters.len))) {
        return cm_semantic_regions_fail(scratch,
            CM_SEMANTIC_REGIONS_INVALID_HIR, CM_HIR_TYPE_NONE);
    }
    saved_scope_owner = scratch->scope_owner;
    saved_scope_parent = scratch->scope_parent;
    saved_scope_parameter_limit = scratch->scope_parameter_limit;
    saved_scope_parameter_limited = scratch->scope_parameter_limited;
    scratch->scope_owner = item;
    scratch->scope_parent = NULL;
    scratch->scope_parameter_limited = 1;
    for (index = 0u; index < item->generic_parameter_count; ++index) {
        const CmHirGenericParam *parameter;
        CmHirGenericParamId id;

        if (index > UINT32_MAX - item->generic_parameter_start) {
            cm_semantic_regions_fail(scratch,
                CM_SEMANTIC_REGIONS_INVALID_HIR, CM_HIR_TYPE_NONE);
            goto fail;
        }
        id = item->generic_parameter_start + index;
        parameter = cm_hir_get_generic_param(scratch->hir, id);
        if (parameter == NULL || parameter->index != index
            || (unsigned int)parameter->kind
                > (unsigned int)CM_HIR_GENERIC_CONST
            || (parameter->is_relaxed_sized != 0
                && parameter->is_relaxed_sized != 1)
            || (parameter->is_relaxed_sized
                && parameter->kind != CM_HIR_GENERIC_TYPE)
            || (parameter->has_default != 0
                && parameter->has_default != 1)
            || !cm_hir_def_id_equal(parameter->owner, item->definition)) {
            scratch->result.generic_parameter = id;
            cm_semantic_regions_fail(scratch,
                CM_SEMANTIC_REGIONS_INVALID_HIR, CM_HIR_TYPE_NONE);
            goto fail;
        }
        scratch->scope_parameter_limit = index;
        if (parameter->kind == CM_HIR_GENERIC_CONST) {
            if (!cm_semantic_regions_scan_type(scratch,
                    parameter->declared_type, 0u)) goto fail;
        } else if (parameter->declared_type != CM_HIR_TYPE_NONE) {
            cm_semantic_regions_fail(scratch,
                CM_SEMANTIC_REGIONS_INVALID_HIR,
                parameter->declared_type);
            goto fail;
        }
        if (parameter->kind == CM_HIR_GENERIC_LIFETIME
            && parameter->has_default) {
            cm_semantic_regions_fail(scratch,
                CM_SEMANTIC_REGIONS_INVALID_HIR,
                CM_HIR_TYPE_NONE);
            goto fail;
        }
        if (parameter->has_default
            && (parameter->kind == CM_HIR_GENERIC_TYPE
                    ? parameter->default_argument.kind
                        != CM_HIR_GENERIC_ARG_TYPE
                : parameter->kind == CM_HIR_GENERIC_CONST
                    ? parameter->default_argument.kind
                        != CM_HIR_GENERIC_ARG_CONST
                : 1)) {
            cm_semantic_regions_fail(scratch,
                CM_SEMANTIC_REGIONS_INVALID_HIR,
                parameter->declared_type);
            goto fail;
        }
        if (parameter->kind == CM_HIR_GENERIC_CONST
            && parameter->has_default
            && !cm_semantic_regions_type_equal(scratch->hir,
                parameter->default_argument.data.constant.type,
                parameter->declared_type, 0u)) {
            cm_semantic_regions_fail(scratch,
                CM_SEMANTIC_REGIONS_INVALID_HIR,
                parameter->declared_type);
            goto fail;
        }
        if (parameter->has_default
            && !cm_semantic_regions_scan_argument(scratch,
                &parameter->default_argument, parameter->declared_type,
                0u)) goto fail;
    }
    scratch->scope_owner = saved_scope_owner;
    scratch->scope_parent = saved_scope_parent;
    scratch->scope_parameter_limit = saved_scope_parameter_limit;
    scratch->scope_parameter_limited = saved_scope_parameter_limited;
    return 1;

fail:
    scratch->scope_owner = saved_scope_owner;
    scratch->scope_parent = saved_scope_parent;
    scratch->scope_parameter_limit = saved_scope_parameter_limit;
    scratch->scope_parameter_limited = saved_scope_parameter_limited;
    return 0;
}

static int cm_semantic_regions_scan_declaration(
    CmSemanticRegionsScratch *scratch, const CmHirBody *body)
{
    uint32_t index;

    if (!cm_semantic_regions_scan_generic_parameters(scratch,
            scratch->parent)
        || !cm_semantic_regions_scan_generic_parameters(scratch,
            scratch->owner)) return 0;
    if (scratch->parent != NULL
        && scratch->parent->kind == CM_HIR_ITEM_IMPL) {
        scratch->scope_owner = scratch->parent;
        scratch->scope_parent = NULL;
        scratch->scope_parameter_limited = 0;
        if (!cm_semantic_regions_scan_type(scratch,
                scratch->parent->data.impl_item.self_type, 0u)
            || (scratch->parent->data.impl_item.has_trait
                && !cm_semantic_regions_scan_named(scratch,
                    &scratch->parent->data.impl_item.trait_type,
                    scratch->parent->data.impl_item.self_type,
                    CM_SEMANTIC_REGIONS_NAMED_TRAIT, 0u))) {
            return 0;
        }
    }
    scratch->scope_owner = scratch->owner;
    scratch->scope_parent = scratch->parent;
    scratch->scope_parameter_limited = 0;
    if (!cm_semantic_regions_scan_type(scratch, body->expected_type,
            0u)) return 0;
    if (scratch->owner->kind == CM_HIR_ITEM_FUNCTION) {
        const CmHirFunctionSignature *signature;

        signature = &scratch->owner->data.function_item.signature;
        if (scratch->owner->data.function_item.body != scratch->body
            || body->expected_type != signature->return_type
            || (signature->parameter_count == 0u)
                != (signature->parameters == NULL)
            || signature->parameter_count
                > CM_SEMANTIC_REGIONS_SLICE_LIMIT) {
            return cm_semantic_regions_fail(scratch,
                CM_SEMANTIC_REGIONS_INVALID_HIR, CM_HIR_TYPE_NONE);
        }
        for (index = 0u; index < signature->parameter_count; ++index) {
            if (!cm_semantic_regions_scan_type(scratch,
                    signature->parameters[index].type, 0u)) return 0;
        }
        return cm_semantic_regions_scan_type(scratch,
            signature->return_type, 0u);
    }
    if (scratch->owner->kind == CM_HIR_ITEM_CONST
        || scratch->owner->kind == CM_HIR_ITEM_STATIC) {
        if (scratch->owner->data.value_item.body != scratch->body
            || body->expected_type
                != scratch->owner->data.value_item.type) {
            return cm_semantic_regions_fail(scratch,
                CM_SEMANTIC_REGIONS_INVALID_HIR, CM_HIR_TYPE_NONE);
        }
        return cm_semantic_regions_scan_type(scratch,
            scratch->owner->data.value_item.type, 0u);
    }
    return cm_semantic_regions_fail(scratch,
        CM_SEMANTIC_REGIONS_INVALID_HIR, CM_HIR_TYPE_NONE);
}

static int cm_semantic_regions_builtin_copy(const CmHirContext *hir,
    CmHirTypeId type_id)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, type_id);
    if (type == NULL) return 0;
    switch (type->kind) {
    case CM_HIR_TYPE_NEVER_KIND:
    case CM_HIR_TYPE_UNIT_KIND:
    case CM_HIR_TYPE_BOOL_KIND:
    case CM_HIR_TYPE_CHAR_KIND:
    case CM_HIR_TYPE_INTEGER_KIND:
    case CM_HIR_TYPE_FLOAT_KIND:
    case CM_HIR_TYPE_RAW_POINTER_KIND:
    case CM_HIR_TYPE_FN_POINTER_KIND:
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
        return 1;
    case CM_HIR_TYPE_REFERENCE_KIND:
        return type->data.reference_type.mutability == CM_HIR_IMMUTABLE;
    default:
        return 0;
    }
}

static int cm_semantic_regions_visit_expression(
    CmSemanticRegionsScratch *scratch, CmHirExprId expression_id,
    CmHirValueUsage expected_usage, size_t depth)
{
    const CmHirExpr *expression;
    const CmHirBody *body;
    size_t slot;
    uint32_t index;

    scratch->expression = expression_id;
    if (expression_id == CM_HIR_EXPR_NONE
        || (size_t)expression_id > scratch->hir->expressions.len
        || depth >= CM_SEMANTIC_REGIONS_NESTING_LIMIT) {
        return cm_semantic_regions_fail(scratch,
            CM_SEMANTIC_REGIONS_INVALID_HIR, CM_HIR_TYPE_NONE);
    }
    slot = (size_t)expression_id - 1u;
    expression = cm_hir_get_expr(scratch->hir, expression_id);
    body = cm_hir_get_body(scratch->hir, scratch->body);
    if (expression == NULL || body == NULL
        || expression->owner_body != scratch->body
        || scratch->visited[slot] != 0u
        || expression->usage != expected_usage
        || expression->static_borrow_state
            != CM_HIR_STATIC_BORROW_NOT_PROMOTED) {
        return cm_semantic_regions_fail(scratch,
            CM_SEMANTIC_REGIONS_INVALID_HIR,
            expression == NULL ? CM_HIR_TYPE_NONE : expression->type);
    }
    scratch->visited[slot] = 1u;
    if (!cm_semantic_regions_scan_type(scratch, expression->type, 0u)) {
        return 0;
    }
    switch (expression->kind) {
    case CM_HIR_EXPR_INTEGER:
        break;
    case CM_HIR_EXPR_LOCAL:
        if (expression->data.local.local_index >= body->local_count
            || !cm_semantic_regions_type_equal(scratch->hir,
                expression->type, body->locals[
                    expression->data.local.local_index].type, 0u)) {
            return cm_semantic_regions_fail(scratch,
                CM_SEMANTIC_REGIONS_INVALID_HIR, expression->type);
        }
        break;
    case CM_HIR_EXPR_BLOCK:
        if ((expression->data.block.statement_count == 0u)
                != (expression->data.block.statements == NULL)
            || expression->data.block.statement_count > body->local_count) {
            return cm_semantic_regions_fail(scratch,
                CM_SEMANTIC_REGIONS_INVALID_HIR, expression->type);
        }
        for (index = 0u; index < expression->data.block.statement_count;
             ++index) {
            const CmHirStatement *statement;
            CmHirValueUsage initializer_usage;

            statement = &expression->data.block.statements[index];
            if (statement->kind != CM_HIR_STATEMENT_LET
                || statement->data.let_statement.local_index
                    >= body->local_count) {
                return cm_semantic_regions_fail(scratch,
                    CM_SEMANTIC_REGIONS_INVALID_HIR, expression->type);
            }
            initializer_usage = cm_semantic_regions_builtin_copy(
                    scratch->hir, body->locals[statement->data.let_statement
                        .local_index].type)
                ? CM_HIR_USAGE_BORROW : CM_HIR_USAGE_MOVE;
            if (!cm_semantic_regions_visit_expression(scratch,
                    statement->data.let_statement.initializer,
                    initializer_usage, depth + 1u)) return 0;
        }
        if (!cm_semantic_regions_visit_expression(scratch,
                expression->data.block.tail_expression,
                CM_HIR_USAGE_MOVE, depth + 1u)) {
            return 0;
        }
        break;
    case CM_HIR_EXPR_CALL:
    {
        const CmHirItem *callee;

        callee = cm_semantic_regions_item(scratch->hir,
            expression->data.call.callee);
        if (callee == NULL || callee->kind != CM_HIR_ITEM_FUNCTION
            || !cm_hir_def_id_is_none(callee->parent_definition)
            || !cm_hir_def_id_is_none(
                callee->data.function_item.trait_item_definition)
            || expression->data.call.type_substitution_count
                != callee->generic_parameter_count
            || expression->data.call.argument_count
                != callee->data.function_item.signature.parameter_count
            || (expression->data.call.type_substitution_count != 0u
                && expression->data.call.type_substitutions == NULL)
            || (expression->data.call.argument_count == 0u)
                != (expression->data.call.arguments == NULL)
            || expression->data.call.type_substitution_count
                > CM_SEMANTIC_REGIONS_SLICE_LIMIT
            || expression->data.call.argument_count
                > CM_SEMANTIC_REGIONS_SLICE_LIMIT) {
            return cm_semantic_regions_fail(scratch,
                CM_SEMANTIC_REGIONS_INVALID_HIR, expression->type);
        }
        for (index = 0u;
             index < expression->data.call.type_substitution_count;
             ++index) {
            const CmHirGenericParam *parameter;

            parameter = callee->generic_parameter_start
                    == CM_HIR_GENERIC_PARAM_NONE
                ? NULL : cm_hir_get_generic_param(scratch->hir,
                    callee->generic_parameter_start + index);
            if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_TYPE
                || parameter->index != index
                || !cm_hir_def_id_equal(parameter->owner,
                    callee->definition)
                || !cm_semantic_regions_scan_type(scratch,
                    expression->data.call.type_substitutions[index], 0u)) {
                return 0;
            }
        }
        for (index = 0u; index < expression->data.call.argument_count;
             ++index) {
            if (!cm_semantic_regions_visit_expression(scratch,
                    expression->data.call.arguments[index],
                    CM_HIR_USAGE_MOVE, depth + 1u)) {
                return 0;
            }
        }
        break;
    }
    case CM_HIR_EXPR_BINARY:
    {
        CmHirValueUsage operand_usage;

        if ((unsigned int)expression->data.binary.operator_kind
                > (unsigned int)CM_HIR_BINARY_LESS) {
            return cm_semantic_regions_fail(scratch,
                CM_SEMANTIC_REGIONS_INVALID_HIR, expression->type);
        }
        operand_usage = expression->data.binary.operator_kind
                    == CM_HIR_BINARY_ADD
                || expression->data.binary.operator_kind
                    == CM_HIR_BINARY_SUBTRACT
            ? CM_HIR_USAGE_MOVE : CM_HIR_USAGE_BORROW;
        if (!cm_semantic_regions_visit_expression(scratch,
                expression->data.binary.left, operand_usage, depth + 1u)
            || !cm_semantic_regions_visit_expression(scratch,
                expression->data.binary.right, operand_usage,
                depth + 1u)) return 0;
        break;
    }
    case CM_HIR_EXPR_AGGREGATE:
        if ((expression->data.aggregate.field_count == 0u)
                != (expression->data.aggregate.fields == NULL)
            || expression->data.aggregate.field_count
                > CM_SEMANTIC_REGIONS_SLICE_LIMIT) {
            return cm_semantic_regions_fail(scratch,
                CM_SEMANTIC_REGIONS_INVALID_HIR, expression->type);
        }
        for (index = 0u; index < expression->data.aggregate.field_count;
             ++index) {
            if (!cm_semantic_regions_visit_expression(scratch,
                    expression->data.aggregate.fields[index].value,
                    CM_HIR_USAGE_MOVE, depth + 1u)) return 0;
        }
        break;
    case CM_HIR_EXPR_FIELD:
        if (!cm_semantic_regions_visit_expression(scratch,
                expression->data.field.base,
                expected_usage == CM_HIR_USAGE_MOVE
                        && cm_semantic_regions_builtin_copy(scratch->hir,
                            expression->type)
                    ? CM_HIR_USAGE_BORROW : expected_usage,
                depth + 1u)) return 0;
        break;
    case CM_HIR_EXPR_IF:
        if (!cm_semantic_regions_visit_expression(scratch,
                expression->data.if_expr.condition,
                CM_HIR_USAGE_BORROW, depth + 1u)
            || !cm_semantic_regions_visit_expression(scratch,
                expression->data.if_expr.then_expression,
                CM_HIR_USAGE_MOVE, depth + 1u)
            || !cm_semantic_regions_visit_expression(scratch,
                expression->data.if_expr.else_expression,
                CM_HIR_USAGE_MOVE, depth + 1u)) {
            return 0;
        }
        break;
    case CM_HIR_EXPR_METHOD_CALL:
    case CM_HIR_EXPR_QUALIFIED_CALL:
    case CM_HIR_EXPR_BORROW_SHARED:
    case CM_HIR_EXPR_DEREFERENCE:
        return cm_semantic_regions_fail(scratch,
            CM_SEMANTIC_REGIONS_UNSUPPORTED_EXPRESSION, expression->type);
    default:
        return cm_semantic_regions_fail(scratch,
            CM_SEMANTIC_REGIONS_INVALID_HIR, expression->type);
    }
    scratch->visited[slot] = 2u;
    return 1;
}

CmSemanticRegionsResult cm_hir_semantic_check_regions(
    const CmHirContext *hir, const CmHirBodyId *bodies,
    size_t body_count)
{
    CmSemanticRegionsScratch scratch;
    size_t body_index;
    size_t expression_index;

    if (hir == NULL || body_count > hir->bodies.len
        || (body_count == 0u) != (bodies == NULL)) {
        return cm_semantic_regions_result(
            CM_SEMANTIC_REGIONS_INVALID_ARGUMENT);
    }
    memset(&scratch, 0, sizeof(scratch));
    scratch.hir = hir;
    scratch.bodies = bodies;
    scratch.body_count = body_count;
    scratch.result = cm_semantic_regions_result(CM_SEMANTIC_REGIONS_OK);
    scratch.visited = (unsigned char *)cm_alloc_zeroed(
        hir->expressions.len == 0u ? 1u : hir->expressions.len,
        sizeof(*scratch.visited));
    scratch.type_gray = (unsigned char *)cm_alloc_zeroed(
        hir->types.len == 0u ? 1u : hir->types.len,
        sizeof(*scratch.type_gray));
    for (body_index = 0u; body_index < body_count; ++body_index) {
        const CmHirBody *body;
        const CmHirExpr *root;
        size_t prior;
        uint32_t local_index;

        scratch.body_index = body_index;
        scratch.body = bodies[body_index];
        scratch.expression = CM_HIR_EXPR_NONE;
        scratch.owner = NULL;
        scratch.parent = NULL;
        scratch.scope_owner = NULL;
        scratch.scope_parent = NULL;
        scratch.scope_parameter_limited = 0;
        for (prior = 0u; prior < body_index; ++prior) {
            if (bodies[prior] == scratch.body) {
                cm_semantic_regions_fail(&scratch,
                    CM_SEMANTIC_REGIONS_INVALID_HIR, CM_HIR_TYPE_NONE);
                goto done;
            }
        }
        body = cm_hir_get_body(hir, scratch.body);
        if (body == NULL || body->state != CM_HIR_BODY_TYPED
            || body->root_expression == CM_HIR_EXPR_NONE) {
            cm_semantic_regions_fail(&scratch,
                CM_SEMANTIC_REGIONS_INVALID_HIR, CM_HIR_TYPE_NONE);
            goto done;
        }
        scratch.owner = cm_semantic_regions_item(hir, body->owner);
        if (scratch.owner == NULL
            || !cm_semantic_regions_constraints_empty(scratch.owner)
            || (scratch.owner->kind == CM_HIR_ITEM_FUNCTION
                ? cm_hir_body_function_owner_kind(hir, scratch.owner)
                    == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED
                : scratch.owner->kind == CM_HIR_ITEM_CONST
                        || scratch.owner->kind == CM_HIR_ITEM_STATIC
                    ? cm_hir_body_value_owner_kind(hir, scratch.owner)
                        == CM_HIR_BODY_VALUE_OWNER_UNSUPPORTED
                    : 1)) {
            cm_semantic_regions_fail(&scratch,
                CM_SEMANTIC_REGIONS_INVALID_HIR, CM_HIR_TYPE_NONE);
            goto done;
        }
        if (!cm_hir_def_id_is_none(scratch.owner->parent_definition)) {
            scratch.parent = cm_semantic_regions_item(hir,
                scratch.owner->parent_definition);
            if (scratch.parent == NULL
                || !cm_semantic_regions_constraints_empty(scratch.parent)
                || (scratch.parent->kind != CM_HIR_ITEM_TRAIT
                    && scratch.parent->kind != CM_HIR_ITEM_IMPL)
                || scratch.parent->owner_module
                    != scratch.owner->owner_module) {
                cm_semantic_regions_fail(&scratch,
                    CM_SEMANTIC_REGIONS_INVALID_HIR, CM_HIR_TYPE_NONE);
                goto done;
            }
        }
        if (!cm_semantic_regions_scan_declaration(&scratch, body)
            || (body->local_count == 0u) != (body->locals == NULL)
            || body->local_count > CM_SEMANTIC_REGIONS_SLICE_LIMIT) {
            if (scratch.result.status == CM_SEMANTIC_REGIONS_OK) {
                cm_semantic_regions_fail(&scratch,
                    CM_SEMANTIC_REGIONS_INVALID_HIR, CM_HIR_TYPE_NONE);
            }
            goto done;
        }
        for (local_index = 0u; local_index < body->local_count;
             ++local_index) {
            if (!cm_semantic_regions_scan_type(&scratch,
                    body->locals[local_index].type, 0u)) goto done;
        }
        root = cm_hir_get_expr(hir, body->root_expression);
        if (root == NULL
            || !cm_semantic_regions_type_equal(hir,
                root->type, body->expected_type, 0u)) {
            cm_semantic_regions_fail(&scratch,
                CM_SEMANTIC_REGIONS_INVALID_HIR,
                root == NULL ? CM_HIR_TYPE_NONE : root->type);
            goto done;
        }
        if (!cm_semantic_regions_visit_expression(&scratch,
                body->root_expression, CM_HIR_USAGE_MOVE, 0u)) goto done;
    }
    for (expression_index = 0u;
         expression_index < hir->expressions.len; ++expression_index) {
        const CmHirExpr *expression;
        int belongs_to_manifest;

        expression = (const CmHirExpr *)cm_vec_at_const(&hir->expressions,
            expression_index);
        belongs_to_manifest = 0;
        if (expression == NULL) {
            scratch.body_index = CM_SEMANTIC_REGIONS_BODY_INDEX_NONE;
            scratch.body = CM_HIR_BODY_NONE;
            scratch.expression = (CmHirExprId)(expression_index + 1u);
            cm_semantic_regions_fail(&scratch,
                CM_SEMANTIC_REGIONS_INVALID_HIR, CM_HIR_TYPE_NONE);
            goto done;
        }
        for (body_index = 0u; body_index < body_count; ++body_index) {
            if (expression->owner_body == bodies[body_index]) {
                belongs_to_manifest = 1;
                break;
            }
        }
        if (belongs_to_manifest != (scratch.visited[expression_index] == 2u)) {
            scratch.body_index = belongs_to_manifest
                ? body_index : CM_SEMANTIC_REGIONS_BODY_INDEX_NONE;
            scratch.body = belongs_to_manifest
                ? expression->owner_body : CM_HIR_BODY_NONE;
            scratch.expression = (CmHirExprId)(expression_index + 1u);
            cm_semantic_regions_fail(&scratch,
                CM_SEMANTIC_REGIONS_INVALID_HIR, expression->type);
            goto done;
        }
    }
    scratch.result = cm_semantic_regions_result(CM_SEMANTIC_REGIONS_OK);

done:
    cm_free(scratch.type_gray);
    cm_free(scratch.visited);
    return scratch.result;
}

const char *cm_semantic_regions_status_name(
    CmSemanticRegionsStatus status)
{
    switch (status) {
    case CM_SEMANTIC_REGIONS_OK: return "ok";
    case CM_SEMANTIC_REGIONS_INVALID_ARGUMENT: return "invalid argument";
    case CM_SEMANTIC_REGIONS_INVALID_HIR: return "invalid HIR";
    case CM_SEMANTIC_REGIONS_UNRESOLVED_REGION:
        return "unresolved region";
    case CM_SEMANTIC_REGIONS_UNSUPPORTED_EXPRESSION:
        return "unsupported expression";
    }
    return "unknown";
}
