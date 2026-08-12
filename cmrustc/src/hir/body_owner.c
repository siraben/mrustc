#include "cm/hir/body.h"

#define CM_HIR_BODY_OWNER_TYPE_DEPTH ((size_t)128u)

static const CmHirItem *cm_hir_body_owner_item(
    const CmHirContext *context, CmHirDefId definition)
{
    const CmHirDefinition *record;
    const CmHirItem *item;

    record = context == NULL ? NULL
        : cm_hir_lookup_definition(context, definition);
    item = record == NULL || record->kind != CM_HIR_DEFINITION_ITEM
            || record->state != CM_HIR_DEFINITION_BOUND
        ? NULL : cm_hir_get_item(context, record->entity.item_id);
    return item != NULL && cm_hir_def_id_equal(item->definition, definition)
        ? item : NULL;
}

static int cm_hir_body_owner_type_concrete(const CmHirContext *context,
    CmHirTypeId type_id, size_t depth);

static int cm_hir_body_owner_region_concrete(const CmHirRegion *region)
{
    return region != NULL
        && (region->kind == CM_HIR_REGION_STATIC
            || region->kind == CM_HIR_REGION_ERASED);
}

static int cm_hir_body_owner_const_concrete(const CmHirContext *context,
    const CmHirConstArg *constant, size_t depth)
{
    return constant != NULL && constant->kind == CM_HIR_CONST_VALUE
        && cm_hir_body_owner_type_concrete(context, constant->type,
            depth + 1u);
}

static int cm_hir_body_owner_named_concrete(const CmHirContext *context,
    const CmHirNamedType *named, size_t depth)
{
    uint32_t index;

    if (named == NULL || cm_hir_def_id_is_none(named->definition)
        || depth >= CM_HIR_BODY_OWNER_TYPE_DEPTH
        || (named->argument_count == 0u) != (named->arguments == NULL)) {
        return 0;
    }
    for (index = 0u; index < named->argument_count; ++index) {
        const CmHirGenericArg *argument;

        argument = &named->arguments[index];
        if (argument->kind == CM_HIR_GENERIC_ARG_TYPE) {
            if (!cm_hir_body_owner_type_concrete(context,
                    argument->data.type, depth + 1u)) return 0;
        } else if (argument->kind == CM_HIR_GENERIC_ARG_LIFETIME) {
            if (!cm_hir_body_owner_region_concrete(
                    &argument->data.lifetime)) return 0;
        } else if (argument->kind == CM_HIR_GENERIC_ARG_CONST) {
            if (!cm_hir_body_owner_const_concrete(context,
                    &argument->data.constant, depth + 1u)) return 0;
        } else {
            return 0;
        }
    }
    return 1;
}

static int cm_hir_body_owner_type_concrete(const CmHirContext *context,
    CmHirTypeId type_id, size_t depth)
{
    const CmHirType *type;
    uint32_t index;

    if (context == NULL || depth >= CM_HIR_BODY_OWNER_TYPE_DEPTH) return 0;
    type = cm_hir_get_type(context, type_id);
    if (type == NULL) return 0;
    switch (type->kind) {
    case CM_HIR_TYPE_NEVER_KIND:
    case CM_HIR_TYPE_UNIT_KIND:
    case CM_HIR_TYPE_BOOL_KIND:
    case CM_HIR_TYPE_CHAR_KIND:
    case CM_HIR_TYPE_STR_KIND:
    case CM_HIR_TYPE_INTEGER_KIND:
    case CM_HIR_TYPE_FLOAT_KIND:
        return 1;
    case CM_HIR_TYPE_REFERENCE_KIND:
        return cm_hir_body_owner_region_concrete(
                &type->data.reference_type.region)
            && cm_hir_body_owner_type_concrete(context,
                type->data.reference_type.pointee, depth + 1u);
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        return cm_hir_body_owner_type_concrete(context,
            type->data.raw_pointer_type.pointee, depth + 1u);
    case CM_HIR_TYPE_TUPLE_KIND:
        if ((type->data.tuple_type.element_count == 0u)
                != (type->data.tuple_type.elements == NULL)) return 0;
        for (index = 0u; index < type->data.tuple_type.element_count;
             ++index) {
            if (!cm_hir_body_owner_type_concrete(context,
                    type->data.tuple_type.elements[index],
                    depth + 1u)) return 0;
        }
        return 1;
    case CM_HIR_TYPE_ARRAY_KIND:
        return cm_hir_body_owner_type_concrete(context,
                type->data.array_type.element, depth + 1u)
            && cm_hir_body_owner_const_concrete(context,
                &type->data.array_type.length, depth + 1u);
    case CM_HIR_TYPE_SLICE_KIND:
        return cm_hir_body_owner_type_concrete(context,
            type->data.slice_type.element, depth + 1u);
    case CM_HIR_TYPE_FN_POINTER_KIND:
        if ((type->data.fn_pointer_type.parameter_count == 0u)
                != (type->data.fn_pointer_type.parameters == NULL)
            || !cm_hir_body_owner_type_concrete(context,
                type->data.fn_pointer_type.return_type, depth + 1u)) {
            return 0;
        }
        for (index = 0u;
             index < type->data.fn_pointer_type.parameter_count; ++index) {
            if (!cm_hir_body_owner_type_concrete(context,
                    type->data.fn_pointer_type.parameters[index],
                    depth + 1u)) return 0;
        }
        return 1;
    case CM_HIR_TYPE_ADT_KIND:
        return cm_hir_body_owner_named_concrete(context,
            &type->data.named_type, depth + 1u);
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
        return 0;
    }
    return 0;
}

CmHirBodyFunctionOwnerKind cm_hir_body_function_owner_kind(
    const CmHirContext *context, const CmHirItem *item)
{
    const CmHirItem *parent;
    const CmHirItem *trait_method;

    if (context == NULL || item == NULL
        || item->kind != CM_HIR_ITEM_FUNCTION) {
        return CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED;
    }
    if (cm_hir_def_id_is_none(item->parent_definition)) {
        return cm_hir_def_id_is_none(
                item->data.function_item.trait_item_definition)
            ? CM_HIR_BODY_FUNCTION_OWNER_FREE
            : CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED;
    }
    parent = cm_hir_body_owner_item(context, item->parent_definition);
    if (parent == NULL || parent->kind != CM_HIR_ITEM_IMPL
        || parent->generic_parameter_count != 0u
        || !parent->data.impl_item.has_trait
        || parent->data.impl_item.is_negative
        || item->generic_parameter_count != 0u
        || cm_hir_def_id_is_none(
            item->data.function_item.trait_item_definition)
        || !cm_hir_body_owner_type_concrete(context,
            parent->data.impl_item.self_type, 0u)
        || !cm_hir_body_owner_named_concrete(context,
            &parent->data.impl_item.trait_type, 0u)) {
        return CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED;
    }
    trait_method = cm_hir_body_owner_item(context,
        item->data.function_item.trait_item_definition);
    if (trait_method == NULL || trait_method->kind != CM_HIR_ITEM_FUNCTION
        || trait_method->generic_parameter_count != 0u
        || !cm_hir_def_id_equal(trait_method->parent_definition,
            parent->data.impl_item.trait_type.definition)) {
        return CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED;
    }
    return CM_HIR_BODY_FUNCTION_OWNER_CONCRETE_TRAIT_IMPL_METHOD;
}
