#include "cm/hir/semantic_item.h"

#include "cm/alloc.h"

#include <string.h>

#define CM_SEMANTIC_ITEM_TYPE_DEPTH ((size_t)128u)

typedef enum CmSemanticItemTypeScan {
    CM_SEMANTIC_ITEM_TYPE_OK = 0,
    CM_SEMANTIC_ITEM_TYPE_CROSS_CRATE,
    CM_SEMANTIC_ITEM_TYPE_GENERIC,
    CM_SEMANTIC_ITEM_TYPE_PROJECTION,
    CM_SEMANTIC_ITEM_TYPE_UNSUPPORTED,
    CM_SEMANTIC_ITEM_TYPE_OVERFLOW,
    CM_SEMANTIC_ITEM_TYPE_INVALID
} CmSemanticItemTypeScan;

static CmSemanticItemResult cm_semantic_item_result(
    CmSemanticItemStatus status)
{
    CmSemanticItemResult result;

    memset(&result, 0, sizeof(result));
    result.status = status;
    result.impl_definition = cm_hir_def_id_none();
    result.trait_definition = cm_hir_def_id_none();
    result.impl_member = cm_hir_def_id_none();
    result.trait_member = cm_hir_def_id_none();
    result.parameter_index = CM_SEMANTIC_ITEM_PARAMETER_NONE;
    result.typeck_status = CM_TYPECK_OK;
    result.solver_kind = CM_TRAIT_SOLVER_INVALID;
    return result;
}

static const CmHirItem *cm_semantic_item_lookup(const CmHirContext *hir,
    CmHirDefId definition)
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

static CmSemanticItemTypeScan cm_semantic_item_scan_type(
    const CmHirContext *hir, CmHirTypeId type_id,
    CmHirCrateId local_crate, CmHirDefId parameter_owner, size_t depth);

static CmSemanticItemTypeScan cm_semantic_item_scan_merge(
    CmSemanticItemTypeScan left, CmSemanticItemTypeScan right)
{
    return right > left ? right : left;
}

static CmSemanticItemTypeScan cm_semantic_item_scan_const(
    const CmHirContext *hir, const CmHirConstArg *constant,
    CmHirCrateId local_crate, CmHirDefId parameter_owner, size_t depth)
{
    CmSemanticItemTypeScan type_status;

    if (constant == NULL) return CM_SEMANTIC_ITEM_TYPE_INVALID;
    type_status = cm_semantic_item_scan_type(hir, constant->type,
        local_crate, parameter_owner, depth + 1u);
    if (type_status != CM_SEMANTIC_ITEM_TYPE_OK) return type_status;
    switch (constant->kind) {
    case CM_HIR_CONST_VALUE: return CM_SEMANTIC_ITEM_TYPE_OK;
    case CM_HIR_CONST_PARAMETER:
    case CM_HIR_CONST_INFER: return CM_SEMANTIC_ITEM_TYPE_GENERIC;
    case CM_HIR_CONST_UNEVALUATED:
        return constant->data.definition.crate_id != local_crate
            ? CM_SEMANTIC_ITEM_TYPE_CROSS_CRATE
            : CM_SEMANTIC_ITEM_TYPE_UNSUPPORTED;
    case CM_HIR_CONST_ERROR: return CM_SEMANTIC_ITEM_TYPE_INVALID;
    }
    return CM_SEMANTIC_ITEM_TYPE_INVALID;
}

static CmSemanticItemTypeScan cm_semantic_item_scan_named(
    const CmHirContext *hir, const CmHirNamedType *named,
    CmHirCrateId local_crate, CmHirDefId parameter_owner, size_t depth)
{
    CmSemanticItemTypeScan result;
    uint32_t index;

    if (named == NULL || cm_hir_def_id_is_none(named->definition)
        || (named->argument_count == 0u) != (named->arguments == NULL)) {
        return CM_SEMANTIC_ITEM_TYPE_INVALID;
    }
    if (named->definition.crate_id != local_crate) {
        return CM_SEMANTIC_ITEM_TYPE_CROSS_CRATE;
    }
    result = CM_SEMANTIC_ITEM_TYPE_OK;
    for (index = 0u; index < named->argument_count; ++index) {
        CmSemanticItemTypeScan child;

        switch (named->arguments[index].kind) {
        case CM_HIR_GENERIC_ARG_TYPE:
            child = cm_semantic_item_scan_type(hir,
                named->arguments[index].data.type, local_crate,
                parameter_owner, depth + 1u);
            break;
        case CM_HIR_GENERIC_ARG_LIFETIME:
            child = named->arguments[index].data.lifetime.kind
                    == CM_HIR_REGION_STATIC
                ? CM_SEMANTIC_ITEM_TYPE_OK
                : CM_SEMANTIC_ITEM_TYPE_GENERIC;
            break;
        case CM_HIR_GENERIC_ARG_CONST:
            child = cm_semantic_item_scan_const(hir,
                &named->arguments[index].data.constant, local_crate,
                parameter_owner, depth + 1u);
            break;
        default:
            child = CM_SEMANTIC_ITEM_TYPE_INVALID;
            break;
        }
        result = cm_semantic_item_scan_merge(result, child);
    }
    return result;
}

static CmSemanticItemTypeScan cm_semantic_item_scan_type(
    const CmHirContext *hir, CmHirTypeId type_id,
    CmHirCrateId local_crate, CmHirDefId parameter_owner, size_t depth)
{
    const CmHirType *type;
    CmSemanticItemTypeScan result;
    uint32_t index;

    if (depth >= CM_SEMANTIC_ITEM_TYPE_DEPTH) {
        return CM_SEMANTIC_ITEM_TYPE_OVERFLOW;
    }
    type = cm_hir_get_type(hir, type_id);
    if (type == NULL) return CM_SEMANTIC_ITEM_TYPE_INVALID;
    switch (type->kind) {
    case CM_HIR_TYPE_NEVER_KIND:
    case CM_HIR_TYPE_UNIT_KIND:
    case CM_HIR_TYPE_BOOL_KIND:
    case CM_HIR_TYPE_CHAR_KIND:
    case CM_HIR_TYPE_STR_KIND:
    case CM_HIR_TYPE_INTEGER_KIND:
    case CM_HIR_TYPE_FLOAT_KIND:
    case CM_HIR_TYPE_SELF_KIND:
        return CM_SEMANTIC_ITEM_TYPE_OK;
    case CM_HIR_TYPE_PARAMETER_KIND:
    {
        const CmHirGenericParam *parameter;

        parameter = cm_hir_get_generic_param(hir,
            type->data.parameter_type.parameter);
        if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_TYPE) {
            return CM_SEMANTIC_ITEM_TYPE_INVALID;
        }
        return !cm_hir_def_id_is_none(parameter_owner)
                && cm_hir_def_id_equal(parameter->owner, parameter_owner)
            ? CM_SEMANTIC_ITEM_TYPE_OK : CM_SEMANTIC_ITEM_TYPE_GENERIC;
    }
    case CM_HIR_TYPE_INFER_KIND: return CM_SEMANTIC_ITEM_TYPE_GENERIC;
    case CM_HIR_TYPE_PROJECTION_KIND:
        return cm_semantic_item_scan_merge(CM_SEMANTIC_ITEM_TYPE_PROJECTION,
            cm_semantic_item_scan_merge(
                cm_semantic_item_scan_type(hir,
                    type->data.projection_type.self_type, local_crate,
                    parameter_owner, depth + 1u),
                cm_semantic_item_scan_merge(
                    cm_semantic_item_scan_named(hir,
                        &type->data.projection_type.trait_type, local_crate,
                        parameter_owner, depth + 1u),
                    cm_semantic_item_scan_named(hir,
                        &type->data.projection_type.associated_type,
                        local_crate, parameter_owner, depth + 1u))));
    case CM_HIR_TYPE_REFERENCE_KIND:
        result = type->data.reference_type.region.kind
                == CM_HIR_REGION_STATIC
                || type->data.reference_type.region.kind
                    == CM_HIR_REGION_ERASED
            ? CM_SEMANTIC_ITEM_TYPE_OK : CM_SEMANTIC_ITEM_TYPE_GENERIC;
        return cm_semantic_item_scan_merge(result,
            cm_semantic_item_scan_type(hir,
                type->data.reference_type.pointee, local_crate,
                parameter_owner, depth + 1u));
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        return cm_semantic_item_scan_type(hir,
            type->data.raw_pointer_type.pointee, local_crate,
            parameter_owner, depth + 1u);
    case CM_HIR_TYPE_TUPLE_KIND:
        if ((type->data.tuple_type.element_count == 0u)
                != (type->data.tuple_type.elements == NULL)) {
            return CM_SEMANTIC_ITEM_TYPE_INVALID;
        }
        result = CM_SEMANTIC_ITEM_TYPE_OK;
        for (index = 0u; index < type->data.tuple_type.element_count;
             ++index) {
            result = cm_semantic_item_scan_merge(result,
                cm_semantic_item_scan_type(hir,
                    type->data.tuple_type.elements[index], local_crate,
                    parameter_owner, depth + 1u));
        }
        return result;
    case CM_HIR_TYPE_ARRAY_KIND:
        return cm_semantic_item_scan_merge(
            cm_semantic_item_scan_type(hir,
                type->data.array_type.element, local_crate,
                parameter_owner, depth + 1u),
            cm_semantic_item_scan_const(hir,
                &type->data.array_type.length, local_crate,
                parameter_owner, depth + 1u));
    case CM_HIR_TYPE_SLICE_KIND:
        return cm_semantic_item_scan_type(hir,
            type->data.slice_type.element, local_crate,
            parameter_owner, depth + 1u);
    case CM_HIR_TYPE_FN_POINTER_KIND:
        if ((type->data.fn_pointer_type.parameter_count == 0u)
                != (type->data.fn_pointer_type.parameters == NULL)) {
            return CM_SEMANTIC_ITEM_TYPE_INVALID;
        }
        result = cm_semantic_item_scan_type(hir,
            type->data.fn_pointer_type.return_type, local_crate,
            parameter_owner, depth + 1u);
        for (index = 0u;
             index < type->data.fn_pointer_type.parameter_count; ++index) {
            result = cm_semantic_item_scan_merge(result,
                cm_semantic_item_scan_type(hir,
                    type->data.fn_pointer_type.parameters[index],
                    local_crate, parameter_owner, depth + 1u));
        }
        return result;
    case CM_HIR_TYPE_ADT_KIND:
        return cm_semantic_item_scan_named(hir, &type->data.named_type,
            local_crate, parameter_owner, depth + 1u);
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
        return CM_SEMANTIC_ITEM_TYPE_PROJECTION;
    case CM_HIR_TYPE_ERROR_KIND: return CM_SEMANTIC_ITEM_TYPE_INVALID;
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
    case CM_HIR_TYPE_DYN_TRAIT_KIND:
    case CM_HIR_TYPE_OPAQUE_KIND:
    case CM_HIR_TYPE_CLOSURE_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
        return CM_SEMANTIC_ITEM_TYPE_UNSUPPORTED;
    }
    return CM_SEMANTIC_ITEM_TYPE_INVALID;
}

static CmSemanticItemStatus cm_semantic_item_scan_status(
    CmSemanticItemTypeScan status)
{
    switch (status) {
    case CM_SEMANTIC_ITEM_TYPE_OK: return CM_SEMANTIC_ITEM_OK;
    case CM_SEMANTIC_ITEM_TYPE_CROSS_CRATE:
        return CM_SEMANTIC_ITEM_PENDING_CROSS_CRATE;
    case CM_SEMANTIC_ITEM_TYPE_GENERIC:
        return CM_SEMANTIC_ITEM_PENDING_GENERIC;
    case CM_SEMANTIC_ITEM_TYPE_PROJECTION:
        return CM_SEMANTIC_ITEM_PENDING_PROJECTION;
    case CM_SEMANTIC_ITEM_TYPE_UNSUPPORTED:
        return CM_SEMANTIC_ITEM_UNSUPPORTED;
    case CM_SEMANTIC_ITEM_TYPE_OVERFLOW:
        return CM_SEMANTIC_ITEM_OVERFLOW;
    case CM_SEMANTIC_ITEM_TYPE_INVALID: return CM_SEMANTIC_ITEM_INVALID;
    }
    return CM_SEMANTIC_ITEM_INVALID;
}

static CmSemanticItemStatus cm_semantic_item_declaration_shape(
    const CmHirItem *item)
{
    uint32_t index;

    if (item->predicate_scope_count != 0u) {
        return CM_SEMANTIC_ITEM_PENDING_HIGHER_RANKED;
    }
    if (item->outlives_predicate_count != 0u) {
        return CM_SEMANTIC_ITEM_PENDING_OUTLIVES;
    }
    if (item->predicate_count != 0u) {
        for (index = 0u; index < item->predicate_count; ++index) {
            if (item->predicates[index].equality_count != 0u) {
                return CM_SEMANTIC_ITEM_PENDING_PROJECTION;
            }
        }
        return CM_SEMANTIC_ITEM_PENDING_PREDICATE;
    }
    return CM_SEMANTIC_ITEM_OK;
}

static CmSemanticItemStatus cm_semantic_item_associated_type_shape(
    const CmHirItem *item)
{
    CmSemanticItemStatus status;
    uint32_t index;

    status = cm_semantic_item_declaration_shape(item);
    if (status != CM_SEMANTIC_ITEM_OK) return status;
    if (item->generic_parameter_count != 0u) {
        return CM_SEMANTIC_ITEM_PENDING_GENERIC;
    }
    if ((item->data.type_alias_item.bound_count == 0u)
            != (item->data.type_alias_item.bounds == NULL)) {
        return CM_SEMANTIC_ITEM_INVALID;
    }
    for (index = 0u; index < item->data.type_alias_item.bound_count;
         ++index) {
        if (item->data.type_alias_item.bounds[index].equality_count != 0u) {
            return CM_SEMANTIC_ITEM_PENDING_PROJECTION;
        }
    }
    return item->data.type_alias_item.bound_count == 0u
        ? CM_SEMANTIC_ITEM_OK : CM_SEMANTIC_ITEM_PENDING_PREDICATE;
}

static size_t cm_semantic_item_find_impl_members(
    const CmHirContext *hir, CmHirDefId impl_definition,
    CmHirDefId trait_member, const CmHirItem **out_member)
{
    size_t index;
    size_t count;

    count = 0u;
    *out_member = NULL;
    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *candidate;
        CmHirDefId linked;

        candidate = (const CmHirItem *)cm_vec_at_const(&hir->items, index);
        if (candidate == NULL
            || !cm_hir_def_id_equal(candidate->parent_definition,
                impl_definition)) continue;
        if (candidate->kind == CM_HIR_ITEM_FUNCTION) {
            linked = candidate->data.function_item.trait_item_definition;
        } else if (candidate->kind == CM_HIR_ITEM_TYPE_ALIAS) {
            linked = candidate->data.type_alias_item.trait_item_definition;
        } else if (candidate->kind == CM_HIR_ITEM_CONST) {
            linked = candidate->data.value_item.trait_item_definition;
        } else {
            linked = cm_hir_def_id_none();
        }
        if (cm_hir_def_id_equal(linked, trait_member)) {
            *out_member = candidate;
            count += 1u;
        }
    }
    return count;
}

static CmSemanticItemStatus cm_semantic_item_typeck_failure(
    CmTypeckStatus status)
{
    if (status == CM_TYPECK_OVERFLOW) return CM_SEMANTIC_ITEM_OVERFLOW;
    return CM_SEMANTIC_ITEM_TYPECK_FAILURE;
}

static CmSemanticItemStatus cm_semantic_item_compare_type(
    CmTypeckContext *typeck, CmHirTypeId expected, CmHirTypeId actual,
    const CmTypeckInstantiation *trait_instantiation,
    const CmTypeckInstantiation *impl_instantiation,
    CmSemanticItemStatus mismatch, CmTypeckStatus *out_typeck)
{
    CmTypeckTypeId expected_type;
    CmTypeckTypeId actual_type;
    CmTypeckStatus status;

    status = cm_typeck_instantiate_hir_type(typeck, expected,
        trait_instantiation, &expected_type);
    if (status == CM_TYPECK_OK) {
        status = cm_typeck_instantiate_hir_type(typeck, actual,
            impl_instantiation, &actual_type);
    }
    if (status == CM_TYPECK_OK) {
        status = cm_typeck_unify(typeck, expected_type, actual_type);
    }
    *out_typeck = status;
    if (status == CM_TYPECK_OK) return CM_SEMANTIC_ITEM_OK;
    if (status == CM_TYPECK_TYPE_MISMATCH
        || status == CM_TYPECK_KIND_CONFLICT) return mismatch;
    return cm_semantic_item_typeck_failure(status);
}

static CmSemanticItemStatus cm_semantic_item_compare_method(
    const CmHirContext *hir, CmHirCrateId local_crate,
    const CmHirItem *impl_item, const CmHirItem *trait_method,
    const CmHirItem *impl_method, CmTypeckContext *typeck,
    const CmTypeckInstantiation *trait_instantiation,
    CmTypeckTypeId self_type, uint32_t *out_parameter,
    CmTypeckStatus *out_typeck)
{
    const CmHirFunctionSignature *expected;
    const CmHirFunctionSignature *actual;
    CmTypeckInstantiation impl_instantiation;
    CmSemanticItemStatus status;
    uint32_t index;

    status = cm_semantic_item_declaration_shape(trait_method);
    if (status != CM_SEMANTIC_ITEM_OK) return status;
    status = cm_semantic_item_declaration_shape(impl_method);
    if (status != CM_SEMANTIC_ITEM_OK) return status;
    if (trait_method->generic_parameter_count != 0u
        || impl_method->generic_parameter_count != 0u) {
        return CM_SEMANTIC_ITEM_PENDING_GENERIC;
    }
    expected = &trait_method->data.function_item.signature;
    actual = &impl_method->data.function_item.signature;
    if (expected->receiver != actual->receiver) {
        return CM_SEMANTIC_ITEM_RECEIVER_MISMATCH;
    }
    if (expected->parameter_count != actual->parameter_count) {
        return CM_SEMANTIC_ITEM_PARAMETER_COUNT_MISMATCH;
    }
    if (expected->abi != actual->abi) return CM_SEMANTIC_ITEM_ABI_MISMATCH;
    if (expected->safety != actual->safety) {
        return CM_SEMANTIC_ITEM_SAFETY_MISMATCH;
    }
    if (expected->is_const != actual->is_const) {
        return CM_SEMANTIC_ITEM_CONST_MISMATCH;
    }
    if (expected->is_async != actual->is_async) {
        return CM_SEMANTIC_ITEM_ASYNC_MISMATCH;
    }
    if (expected->is_variadic != actual->is_variadic) {
        return CM_SEMANTIC_ITEM_VARIADIC_MISMATCH;
    }
    cm_typeck_instantiation_init(typeck, &impl_instantiation);
    impl_instantiation.parameter_owner = impl_method->definition;
    impl_instantiation.self_owner = impl_item->definition;
    impl_instantiation.self_type = self_type;
    if (!cm_typeck_instantiation_is_valid(typeck, &impl_instantiation)) {
        return CM_SEMANTIC_ITEM_INVALID;
    }
    for (index = 0u; index < expected->parameter_count; ++index) {
        CmSemanticItemTypeScan scan;

        scan = cm_semantic_item_scan_merge(
            cm_semantic_item_scan_type(hir,
                expected->parameters[index].type, local_crate,
                trait_method->parent_definition, 0u),
            cm_semantic_item_scan_type(hir,
                actual->parameters[index].type, local_crate,
                cm_hir_def_id_none(), 0u));
        status = cm_semantic_item_scan_status(scan);
        if (status != CM_SEMANTIC_ITEM_OK) {
            *out_parameter = index;
            return status;
        }
        status = cm_semantic_item_compare_type(typeck,
            expected->parameters[index].type,
            actual->parameters[index].type, trait_instantiation,
            &impl_instantiation, CM_SEMANTIC_ITEM_PARAMETER_TYPE_MISMATCH,
            out_typeck);
        if (status != CM_SEMANTIC_ITEM_OK) {
            *out_parameter = index;
            return status;
        }
    }
    status = cm_semantic_item_scan_status(cm_semantic_item_scan_merge(
        cm_semantic_item_scan_type(hir, expected->return_type,
            local_crate, trait_method->parent_definition, 0u),
        cm_semantic_item_scan_type(hir, actual->return_type,
            local_crate, cm_hir_def_id_none(), 0u)));
    if (status != CM_SEMANTIC_ITEM_OK) return status;
    return cm_semantic_item_compare_type(typeck, expected->return_type,
        actual->return_type, trait_instantiation, &impl_instantiation,
        CM_SEMANTIC_ITEM_RETURN_TYPE_MISMATCH, out_typeck);
}

static CmSemanticItemStatus cm_semantic_item_compare_finalized_method(
    const CmHirContext *hir, CmHirCrateId local_crate,
    const CmHirCrateFinalization *finalization,
    CmProjectionNormalizeLimits limits, const CmHirItem *impl_item,
    const CmHirItem *trait_method, const CmHirItem *impl_method,
    uint32_t *out_parameter, CmTypeckStatus *out_typeck,
    CmTraitSolverResultKind *out_solver);

static CmSemanticItemResult cm_semantic_item_check_impl(
    const CmHirContext *hir, CmHirCrateId local_crate,
    const CmHirItem *impl_item,
    const CmHirCrateFinalization *finalization,
    CmProjectionNormalizeLimits normalize_limits)
{
    CmSemanticItemResult result;
    const CmHirItem *trait_item;
    CmSemanticItemStatus status;
    CmTypeckContext typeck;
    CmTypeckInstantiation trait_instantiation;
    CmTypeckGenericArg *trait_arguments;
    CmTypeckTypeId self_type;
    CmTypeckStatus typeck_status;
    uint32_t argument_index;
    size_t item_index;

    result = cm_semantic_item_result(CM_SEMANTIC_ITEM_INVALID);
    result.impl_definition = impl_item->definition;
    result.trait_definition = impl_item->data.impl_item.trait_type.definition;
    if (impl_item->data.impl_item.is_negative) {
        result.status = CM_SEMANTIC_ITEM_PENDING_NEGATIVE;
        return result;
    }
    status = cm_semantic_item_declaration_shape(impl_item);
    if (status != CM_SEMANTIC_ITEM_OK) {
        result.status = status;
        return result;
    }
    if (impl_item->generic_parameter_count != 0u) {
        result.status = CM_SEMANTIC_ITEM_PENDING_GENERIC;
        return result;
    }
    if (result.trait_definition.crate_id != local_crate) {
        result.status = CM_SEMANTIC_ITEM_PENDING_CROSS_CRATE;
        return result;
    }
    trait_item = cm_semantic_item_lookup(hir, result.trait_definition);
    if (trait_item == NULL || trait_item->kind != CM_HIR_ITEM_TRAIT
        || !cm_hir_def_id_is_none(trait_item->parent_definition)) {
        return result;
    }
    if (trait_item->data.trait_item.is_auto) {
        result.status = CM_SEMANTIC_ITEM_UNSUPPORTED;
        return result;
    }
    status = cm_semantic_item_declaration_shape(trait_item);
    if (status != CM_SEMANTIC_ITEM_OK) {
        result.status = status;
        return result;
    }
    if (trait_item->data.trait_item.supertrait_count != 0u) {
        result.status = CM_SEMANTIC_ITEM_PENDING_PREDICATE;
        return result;
    }
    if (trait_item->data.trait_item.safety
            != impl_item->data.impl_item.safety) {
        result.status = CM_SEMANTIC_ITEM_SAFETY_MISMATCH;
        return result;
    }
    if (trait_item->generic_parameter_count
            != impl_item->data.impl_item.trait_type.argument_count
        || (trait_item->generic_parameter_count == 0u)
            != (impl_item->data.impl_item.trait_type.arguments == NULL)) {
        result.status = CM_SEMANTIC_ITEM_PENDING_GENERIC;
        return result;
    }

    memset(&typeck, 0, sizeof(typeck));
    cm_typeck_context_init(&typeck, hir);
    trait_arguments = NULL;
    if (trait_item->generic_parameter_count != 0u) {
        trait_arguments = (CmTypeckGenericArg *)cm_alloc_zeroed(
            trait_item->generic_parameter_count, sizeof(*trait_arguments));
    }
    cm_typeck_instantiation_init(&typeck, &trait_instantiation);
    trait_instantiation.parameter_owner = trait_item->definition;
    for (argument_index = 0u;
         argument_index < trait_item->generic_parameter_count;
         ++argument_index) {
        const CmHirGenericParam *parameter;
        const CmHirGenericArg *argument;
        CmSemanticItemTypeScan scan;

        parameter = cm_hir_get_generic_param(hir,
            trait_item->generic_parameter_start + argument_index);
        argument = &impl_item->data.impl_item
            .trait_type.arguments[argument_index];
        if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_TYPE
            || parameter->has_default
            || argument->kind != CM_HIR_GENERIC_ARG_TYPE) {
            result.status = parameter != NULL && parameter->has_default
                ? CM_SEMANTIC_ITEM_PENDING_DEFAULT
                : CM_SEMANTIC_ITEM_PENDING_GENERIC;
            goto cleanup;
        }
        scan = cm_semantic_item_scan_type(hir, argument->data.type,
            local_crate, cm_hir_def_id_none(), 0u);
        result.status = cm_semantic_item_scan_status(scan);
        if (result.status != CM_SEMANTIC_ITEM_OK) goto cleanup;
        trait_arguments[argument_index].kind = CM_HIR_GENERIC_ARG_TYPE;
        typeck_status = cm_typeck_import_hir_type(&typeck,
            argument->data.type,
            &trait_arguments[argument_index].data.type);
        if (typeck_status != CM_TYPECK_OK) {
            result.status = cm_semantic_item_typeck_failure(typeck_status);
            result.typeck_status = typeck_status;
            goto cleanup;
        }
    }
    trait_instantiation.arguments = trait_arguments;
    trait_instantiation.argument_count = trait_item->generic_parameter_count;
    result.status = cm_semantic_item_scan_status(cm_semantic_item_scan_type(
        hir, impl_item->data.impl_item.self_type, local_crate,
        cm_hir_def_id_none(), 0u));
    if (result.status != CM_SEMANTIC_ITEM_OK) goto cleanup;
    typeck_status = cm_typeck_import_hir_type(&typeck,
        impl_item->data.impl_item.self_type, &self_type);
    if (typeck_status != CM_TYPECK_OK) {
        result.status = cm_semantic_item_typeck_failure(typeck_status);
        result.typeck_status = typeck_status;
        goto cleanup;
    }
    trait_instantiation.self_owner = trait_item->definition;
    trait_instantiation.self_type = self_type;
    if (!cm_typeck_instantiation_is_valid(&typeck,
            &trait_instantiation)) {
        result.status = CM_SEMANTIC_ITEM_INVALID;
        goto cleanup;
    }

    for (item_index = 0u; item_index < hir->items.len; ++item_index) {
        const CmHirItem *trait_member;
        const CmHirItem *impl_member;
        size_t count;

        trait_member = (const CmHirItem *)cm_vec_at_const(&hir->items,
            item_index);
        if (trait_member == NULL
            || !cm_hir_def_id_equal(trait_member->parent_definition,
                trait_item->definition)) continue;
        result.trait_member = trait_member->definition;
        count = cm_semantic_item_find_impl_members(hir,
            impl_item->definition, trait_member->definition, &impl_member);
        if (count > 1u) {
            result.status = CM_SEMANTIC_ITEM_DUPLICATE_ASSOCIATED_ITEM;
            goto cleanup;
        }
        if (trait_member->kind == CM_HIR_ITEM_TYPE_ALIAS) {
            result.status = cm_semantic_item_associated_type_shape(
                trait_member);
            if (result.status != CM_SEMANTIC_ITEM_OK) goto cleanup;
            if (trait_member->data.type_alias_item.target
                    != CM_HIR_TYPE_NONE) {
                result.status = CM_SEMANTIC_ITEM_PENDING_DEFAULT;
                goto cleanup;
            }
            if (count == 0u) {
                result.status = CM_SEMANTIC_ITEM_MISSING_ASSOCIATED_TYPE;
                goto cleanup;
            }
            result.impl_member = impl_member->definition;
            if (impl_member->kind != CM_HIR_ITEM_TYPE_ALIAS
                || impl_member->data.type_alias_item.target
                    == CM_HIR_TYPE_NONE) {
                result.status = CM_SEMANTIC_ITEM_WRONG_ASSOCIATION;
                goto cleanup;
            }
            result.status = cm_semantic_item_associated_type_shape(
                impl_member);
            if (result.status != CM_SEMANTIC_ITEM_OK) goto cleanup;
            result.status = cm_semantic_item_scan_status(
                cm_semantic_item_scan_type(hir,
                    impl_member->data.type_alias_item.target,
                    local_crate, cm_hir_def_id_none(), 0u));
            if (result.status != CM_SEMANTIC_ITEM_OK) goto cleanup;
        } else if (trait_member->kind == CM_HIR_ITEM_FUNCTION) {
            if (count == 0u) {
                if (trait_member->data.function_item.body
                        != CM_HIR_BODY_NONE) continue;
                result.status = CM_SEMANTIC_ITEM_MISSING_REQUIRED_METHOD;
                goto cleanup;
            }
            result.impl_member = impl_member->definition;
            if (impl_member->kind != CM_HIR_ITEM_FUNCTION) {
                result.status = CM_SEMANTIC_ITEM_WRONG_ASSOCIATION;
                goto cleanup;
            }
            result.status = finalization == NULL
                ? cm_semantic_item_compare_method(hir,
                    local_crate, impl_item, trait_member, impl_member,
                    &typeck, &trait_instantiation, self_type,
                    &result.parameter_index, &result.typeck_status)
                : cm_semantic_item_compare_finalized_method(hir,
                    local_crate, finalization, normalize_limits, impl_item,
                    trait_member, impl_member, &result.parameter_index,
                    &result.typeck_status, &result.solver_kind);
            if (result.status != CM_SEMANTIC_ITEM_OK) goto cleanup;
        } else {
            result.status = CM_SEMANTIC_ITEM_UNSUPPORTED;
            goto cleanup;
        }
        result.impl_member = cm_hir_def_id_none();
        result.trait_member = cm_hir_def_id_none();
    }

    for (item_index = 0u; item_index < hir->items.len; ++item_index) {
        const CmHirItem *impl_member;
        CmHirDefId linked;
        const CmHirItem *target;

        impl_member = (const CmHirItem *)cm_vec_at_const(&hir->items,
            item_index);
        if (impl_member == NULL
            || !cm_hir_def_id_equal(impl_member->parent_definition,
                impl_item->definition)) continue;
        if (impl_member->kind == CM_HIR_ITEM_FUNCTION) {
            linked = impl_member->data.function_item.trait_item_definition;
        } else if (impl_member->kind == CM_HIR_ITEM_TYPE_ALIAS) {
            linked = impl_member->data.type_alias_item.trait_item_definition;
        } else if (impl_member->kind == CM_HIR_ITEM_CONST) {
            linked = impl_member->data.value_item.trait_item_definition;
        } else {
            linked = cm_hir_def_id_none();
        }
        target = cm_semantic_item_lookup(hir, linked);
        if (target == NULL
            || !cm_hir_def_id_equal(target->parent_definition,
                trait_item->definition)
            || target->kind != impl_member->kind) {
            result.impl_member = impl_member->definition;
            result.trait_member = linked;
            result.status = CM_SEMANTIC_ITEM_WRONG_ASSOCIATION;
            goto cleanup;
        }
    }
    result = cm_semantic_item_result(CM_SEMANTIC_ITEM_OK);

cleanup:
    cm_free(trait_arguments);
    cm_typeck_context_destroy(&typeck);
    return result;
}

CmSemanticItemResult cm_semantic_item_check_local_trait_impls(
    const CmHirContext *hir, CmHirCrateId local_crate)
{
    CmSemanticItemResult result;
    size_t index;

    result = cm_semantic_item_result(CM_SEMANTIC_ITEM_INVALID);
    if (hir == NULL || local_crate == CM_HIR_CRATE_NONE
        || cm_hir_get_crate(hir, local_crate) == NULL) return result;
    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *item;

        item = (const CmHirItem *)cm_vec_at_const(&hir->items, index);
        if (item == NULL || item->kind != CM_HIR_ITEM_IMPL
            || item->definition.crate_id != local_crate
            || !item->data.impl_item.has_trait) continue;
        result = cm_semantic_item_check_impl(hir, local_crate, item,
            NULL, (CmProjectionNormalizeLimits){0u, 0u});
        if (result.status != CM_SEMANTIC_ITEM_OK) return result;
    }
    return cm_semantic_item_result(CM_SEMANTIC_ITEM_OK);
}

CmSemanticItemResult cm_semantic_item_check_finalized_local_trait_impls(
    const CmHirCrateFinalization *finalization,
    CmProjectionNormalizeLimits normalize_limits)
{
    CmSemanticItemResult result;
    const CmHirContext *hir;
    CmHirCrateId local_crate;
    size_t index;

    result = cm_semantic_item_result(CM_SEMANTIC_ITEM_INVALID);
    if (!cm_hir_crate_finalization_is_current(finalization)
        || normalize_limits.max_nodes == 0u
        || normalize_limits.max_projection_steps == 0u) return result;
    hir = cm_hir_crate_finalization_hir(finalization);
    local_crate = cm_hir_crate_finalization_crate(finalization);
    if (hir == NULL || local_crate == CM_HIR_CRATE_NONE) return result;
    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *item;

        item = (const CmHirItem *)cm_vec_at_const(&hir->items, index);
        if (item == NULL || item->kind != CM_HIR_ITEM_IMPL
            || item->definition.crate_id != local_crate
            || !item->data.impl_item.has_trait) continue;
        result = cm_semantic_item_check_impl(hir, local_crate, item,
            finalization, normalize_limits);
        if (result.status != CM_SEMANTIC_ITEM_OK) return result;
    }
    result = cm_semantic_item_result(CM_SEMANTIC_ITEM_OK);
    result.solver_kind = CM_TRAIT_SOLVER_PROVEN;
    return result;
}

static CmSemanticItemStatus cm_semantic_item_solver_status(
    CmTraitSolverResultKind kind)
{
    switch (kind) {
    case CM_TRAIT_SOLVER_PROVEN: return CM_SEMANTIC_ITEM_OK;
    case CM_TRAIT_SOLVER_DEFERRED_INFERENCE:
        return CM_SEMANTIC_ITEM_PENDING_GENERIC;
    case CM_TRAIT_SOLVER_DEFERRED_METADATA:
        return CM_SEMANTIC_ITEM_PENDING_CROSS_CRATE;
    case CM_TRAIT_SOLVER_AMBIGUOUS:
        return CM_SEMANTIC_ITEM_PENDING_SPECIALIZATION;
    case CM_TRAIT_SOLVER_NO_SOLUTION:
    case CM_TRAIT_SOLVER_NEGATIVE:
        return CM_SEMANTIC_ITEM_PENDING_PROJECTION;
    case CM_TRAIT_SOLVER_UNSUPPORTED:
        return CM_SEMANTIC_ITEM_UNSUPPORTED;
    case CM_TRAIT_SOLVER_OVERFLOW: return CM_SEMANTIC_ITEM_OVERFLOW;
    case CM_TRAIT_SOLVER_TYPECK_FAILURE:
        return CM_SEMANTIC_ITEM_TYPECK_FAILURE;
    case CM_TRAIT_SOLVER_INVALID:
        return CM_SEMANTIC_ITEM_INVALID;
    }
    return CM_SEMANTIC_ITEM_INVALID;
}

static CmSemanticItemStatus cm_semantic_item_compare_normalized_type(
    CmSemanticSession *session, CmTypeckContext *typeck,
    const CmParamEnvSubstitution *substitution,
    CmProjectionNormalizeLimits limits, CmHirTypeId expected,
    CmHirTypeId actual,
    const CmTypeckInstantiation *trait_instantiation,
    const CmTypeckInstantiation *impl_instantiation,
    CmSemanticItemStatus mismatch, CmTypeckStatus *out_typeck,
    CmTraitSolverResultKind *out_solver)
{
    CmTypeckSnapshot snapshot;
    CmProjectionNormalizeResult normalization;
    CmTypeckTypeId expected_type;
    CmTypeckTypeId actual_type;
    CmTypeckStatus typeck_status;
    CmSemanticItemStatus status;

    memset(&snapshot, 0, sizeof(snapshot));
    typeck_status = cm_typeck_snapshot(typeck, &snapshot);
    if (typeck_status != CM_TYPECK_OK) goto typeck_failure;
    typeck_status = cm_typeck_instantiate_hir_type(typeck, expected,
        trait_instantiation, &expected_type);
    if (typeck_status != CM_TYPECK_OK) goto rollback_typeck_failure;
    typeck_status = cm_typeck_instantiate_hir_type(typeck, actual,
        impl_instantiation, &actual_type);
    if (typeck_status != CM_TYPECK_OK) goto rollback_typeck_failure;
    normalization = cm_semantic_session_normalize_type(session, typeck,
        substitution, expected_type, limits);
    *out_solver = normalization.kind;
    *out_typeck = normalization.typeck_status;
    if (normalization.kind != CM_TRAIT_SOLVER_PROVEN) {
        status = normalization.kind == CM_TRAIT_SOLVER_NO_SOLUTION
                || normalization.kind == CM_TRAIT_SOLVER_NEGATIVE
                || normalization.kind == CM_TRAIT_SOLVER_AMBIGUOUS
            ? mismatch : cm_semantic_item_solver_status(normalization.kind);
        goto rollback;
    }
    expected_type = normalization.type;
    normalization = cm_semantic_session_normalize_type(session, typeck,
        substitution, actual_type, limits);
    *out_solver = normalization.kind;
    *out_typeck = normalization.typeck_status;
    if (normalization.kind != CM_TRAIT_SOLVER_PROVEN) {
        status = normalization.kind == CM_TRAIT_SOLVER_NO_SOLUTION
                || normalization.kind == CM_TRAIT_SOLVER_NEGATIVE
                || normalization.kind == CM_TRAIT_SOLVER_AMBIGUOUS
            ? mismatch : cm_semantic_item_solver_status(normalization.kind);
        goto rollback;
    }
    actual_type = normalization.type;
    typeck_status = cm_typeck_unify(typeck, expected_type, actual_type);
    *out_typeck = typeck_status;
    if (typeck_status != CM_TYPECK_OK) {
        status = typeck_status == CM_TYPECK_TYPE_MISMATCH
                || typeck_status == CM_TYPECK_KIND_CONFLICT
            ? mismatch : cm_semantic_item_typeck_failure(typeck_status);
        goto rollback;
    }
    typeck_status = cm_typeck_commit(typeck, &snapshot);
    *out_typeck = typeck_status;
    return typeck_status == CM_TYPECK_OK ? CM_SEMANTIC_ITEM_OK
        : cm_semantic_item_typeck_failure(typeck_status);

rollback_typeck_failure:
    *out_typeck = typeck_status;
    status = cm_semantic_item_typeck_failure(typeck_status);
rollback:
    typeck_status = cm_typeck_rollback(typeck, &snapshot);
    if (typeck_status != CM_TYPECK_OK) {
        *out_typeck = typeck_status;
        return cm_semantic_item_typeck_failure(typeck_status);
    }
    return status;

typeck_failure:
    *out_typeck = typeck_status;
    return cm_semantic_item_typeck_failure(typeck_status);
}

static CmSemanticItemStatus cm_semantic_item_compare_finalized_method(
    const CmHirContext *hir, CmHirCrateId local_crate,
    const CmHirCrateFinalization *finalization,
    CmProjectionNormalizeLimits limits, const CmHirItem *impl_item,
    const CmHirItem *trait_method, const CmHirItem *impl_method,
    uint32_t *out_parameter, CmTypeckStatus *out_typeck,
    CmTraitSolverResultKind *out_solver)
{
    const CmHirFunctionSignature *expected;
    const CmHirFunctionSignature *actual;
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmTypeckContext *typeck;
    CmTypeckInstantiation trait_instantiation;
    CmTypeckInstantiation impl_instantiation;
    CmTypeckInstantiation impl_enclosing;
    CmTypeckGenericArg *trait_arguments;
    CmParamEnvSubstitution substitution;
    CmSemanticItemStatus status;
    uint32_t index;

    status = cm_semantic_item_declaration_shape(trait_method);
    if (status != CM_SEMANTIC_ITEM_OK) return status;
    status = cm_semantic_item_declaration_shape(impl_method);
    if (status != CM_SEMANTIC_ITEM_OK) return status;
    if (trait_method->generic_parameter_count != 0u
        || impl_method->generic_parameter_count != 0u) {
        return CM_SEMANTIC_ITEM_PENDING_GENERIC;
    }
    expected = &trait_method->data.function_item.signature;
    actual = &impl_method->data.function_item.signature;
    if (expected->receiver != actual->receiver) {
        return CM_SEMANTIC_ITEM_RECEIVER_MISMATCH;
    }
    if (expected->parameter_count != actual->parameter_count) {
        return CM_SEMANTIC_ITEM_PARAMETER_COUNT_MISMATCH;
    }
    if (expected->abi != actual->abi) return CM_SEMANTIC_ITEM_ABI_MISMATCH;
    if (expected->safety != actual->safety) {
        return CM_SEMANTIC_ITEM_SAFETY_MISMATCH;
    }
    if (expected->is_const != actual->is_const) {
        return CM_SEMANTIC_ITEM_CONST_MISMATCH;
    }
    if (expected->is_async != actual->is_async) {
        return CM_SEMANTIC_ITEM_ASYNC_MISMATCH;
    }
    if (expected->is_variadic != actual->is_variadic) {
        return CM_SEMANTIC_ITEM_VARIADIC_MISMATCH;
    }
    memset(&session, 0, sizeof(session));
    cm_semantic_session_options_init(&options);
    options.local_crate = local_crate;
    options.exact_owner = impl_method->definition;
    options.universe = CM_TRAIT_IMPL_UNIVERSE_SINGLE_LOCAL_CRATE_COMPLETE;
    options.finalization = finalization;
    *out_solver = cm_semantic_session_init(&session, hir, &options);
    if (*out_solver != CM_TRAIT_SOLVER_PROVEN) {
        return cm_semantic_item_solver_status(*out_solver);
    }
    if (cm_semantic_session_hir(&session) != hir
        || cm_semantic_session_local_crate(&session) != local_crate
        || cm_semantic_session_universe(&session)
            != CM_TRAIT_IMPL_UNIVERSE_SINGLE_LOCAL_CRATE_COMPLETE
        || !cm_hir_def_id_equal(cm_semantic_session_exact_owner(&session),
            impl_method->definition)
        || !cm_hir_def_id_equal(
            cm_semantic_session_enclosing_owner(&session),
            impl_item->definition)
        || !cm_hir_def_id_equal(impl_method->parent_definition,
            impl_item->definition)
        || !cm_hir_def_id_equal(
            impl_method->data.function_item.trait_item_definition,
            trait_method->definition)) {
        cm_semantic_session_destroy(&session);
        return CM_SEMANTIC_ITEM_INVALID;
    }
    typeck = cm_semantic_session_typeck(&session);
    trait_arguments = NULL;
    if (impl_item->data.impl_item.trait_type.argument_count != 0u) {
        trait_arguments = (CmTypeckGenericArg *)cm_alloc_zeroed(
            impl_item->data.impl_item.trait_type.argument_count,
            sizeof(*trait_arguments));
    }
    cm_typeck_instantiation_init(typeck, &impl_enclosing);
    impl_enclosing.parameter_owner = impl_item->definition;
    impl_enclosing.self_owner = impl_item->definition;
    *out_typeck = cm_typeck_import_hir_type(typeck,
        impl_item->data.impl_item.self_type, &impl_enclosing.self_type);
    if (*out_typeck != CM_TYPECK_OK) {
        status = cm_semantic_item_typeck_failure(*out_typeck);
        goto cleanup;
    }
    cm_typeck_instantiation_init(typeck, &trait_instantiation);
    trait_instantiation.parameter_owner =
        impl_item->data.impl_item.trait_type.definition;
    trait_instantiation.self_owner =
        impl_item->data.impl_item.trait_type.definition;
    trait_instantiation.self_type = impl_enclosing.self_type;
    for (index = 0u;
         index < impl_item->data.impl_item.trait_type.argument_count;
         ++index) {
        const CmHirGenericArg *argument;

        argument = &impl_item->data.impl_item.trait_type.arguments[index];
        if (argument->kind != CM_HIR_GENERIC_ARG_TYPE) {
            status = CM_SEMANTIC_ITEM_PENDING_GENERIC;
            goto cleanup;
        }
        trait_arguments[index].kind = CM_HIR_GENERIC_ARG_TYPE;
        *out_typeck = cm_typeck_import_hir_type(typeck,
            argument->data.type, &trait_arguments[index].data.type);
        if (*out_typeck != CM_TYPECK_OK) {
            status = cm_semantic_item_typeck_failure(*out_typeck);
            goto cleanup;
        }
    }
    trait_instantiation.arguments = trait_arguments;
    trait_instantiation.argument_count =
        impl_item->data.impl_item.trait_type.argument_count;
    cm_typeck_instantiation_init(typeck, &impl_instantiation);
    impl_instantiation.parameter_owner = impl_method->definition;
    impl_instantiation.self_owner = impl_item->definition;
    impl_instantiation.self_type = impl_enclosing.self_type;
    if (!cm_typeck_instantiation_is_valid(typeck, &trait_instantiation)
        || !cm_typeck_instantiation_is_valid(typeck, &impl_instantiation)
        || !cm_typeck_instantiation_is_valid(typeck, &impl_enclosing)) {
        status = CM_SEMANTIC_ITEM_INVALID;
        goto cleanup;
    }
    memset(&substitution, 0, sizeof(substitution));
    substitution.exact = &impl_instantiation;
    substitution.enclosing = &impl_enclosing;
    status = CM_SEMANTIC_ITEM_OK;
    for (index = 0u; index < expected->parameter_count; ++index) {
        CmSemanticItemTypeScan scan;

        scan = cm_semantic_item_scan_merge(
            cm_semantic_item_scan_type(hir, expected->parameters[index].type,
                local_crate, trait_method->parent_definition, 0u),
            cm_semantic_item_scan_type(hir, actual->parameters[index].type,
                local_crate, cm_hir_def_id_none(), 0u));
        if (scan != CM_SEMANTIC_ITEM_TYPE_OK
            && scan != CM_SEMANTIC_ITEM_TYPE_PROJECTION) {
            status = cm_semantic_item_scan_status(scan);
            *out_parameter = index;
            break;
        }
        status = cm_semantic_item_compare_normalized_type(&session, typeck,
            &substitution, limits, expected->parameters[index].type,
            actual->parameters[index].type, &trait_instantiation,
            &impl_instantiation, CM_SEMANTIC_ITEM_PARAMETER_TYPE_MISMATCH,
            out_typeck, out_solver);
        if (status != CM_SEMANTIC_ITEM_OK) {
            *out_parameter = index;
            break;
        }
    }
    if (status == CM_SEMANTIC_ITEM_OK) {
        CmSemanticItemTypeScan scan;

        scan = cm_semantic_item_scan_merge(
            cm_semantic_item_scan_type(hir, expected->return_type,
                local_crate, trait_method->parent_definition, 0u),
            cm_semantic_item_scan_type(hir, actual->return_type,
                local_crate, cm_hir_def_id_none(), 0u));
        status = scan != CM_SEMANTIC_ITEM_TYPE_OK
                && scan != CM_SEMANTIC_ITEM_TYPE_PROJECTION
            ? cm_semantic_item_scan_status(scan)
            : cm_semantic_item_compare_normalized_type(&session, typeck,
                &substitution, limits, expected->return_type,
                actual->return_type, &trait_instantiation,
                &impl_instantiation, CM_SEMANTIC_ITEM_RETURN_TYPE_MISMATCH,
                out_typeck, out_solver);
    }
cleanup:
    cm_free(trait_arguments);
    cm_semantic_session_destroy(&session);
    return status;
}

const char *cm_semantic_item_status_name(CmSemanticItemStatus status)
{
    switch (status) {
    case CM_SEMANTIC_ITEM_OK: return "ok";
    case CM_SEMANTIC_ITEM_PENDING_CROSS_CRATE: return "pending-cross-crate";
    case CM_SEMANTIC_ITEM_PENDING_GENERIC: return "pending-generic";
    case CM_SEMANTIC_ITEM_PENDING_HIGHER_RANKED:
        return "pending-higher-ranked";
    case CM_SEMANTIC_ITEM_PENDING_OUTLIVES: return "pending-outlives";
    case CM_SEMANTIC_ITEM_PENDING_PREDICATE: return "pending-predicate";
    case CM_SEMANTIC_ITEM_PENDING_PROJECTION: return "pending-projection";
    case CM_SEMANTIC_ITEM_PENDING_DEFAULT: return "pending-default";
    case CM_SEMANTIC_ITEM_PENDING_SPECIALIZATION:
        return "pending-specialization";
    case CM_SEMANTIC_ITEM_PENDING_NEGATIVE: return "pending-negative";
    case CM_SEMANTIC_ITEM_MISSING_ASSOCIATED_TYPE:
        return "missing-associated-type";
    case CM_SEMANTIC_ITEM_MISSING_REQUIRED_METHOD:
        return "missing-required-method";
    case CM_SEMANTIC_ITEM_DUPLICATE_ASSOCIATED_ITEM:
        return "duplicate-associated-item";
    case CM_SEMANTIC_ITEM_WRONG_ASSOCIATION: return "wrong-association";
    case CM_SEMANTIC_ITEM_RECEIVER_MISMATCH: return "receiver-mismatch";
    case CM_SEMANTIC_ITEM_PARAMETER_COUNT_MISMATCH:
        return "parameter-count-mismatch";
    case CM_SEMANTIC_ITEM_PARAMETER_TYPE_MISMATCH:
        return "parameter-type-mismatch";
    case CM_SEMANTIC_ITEM_RETURN_TYPE_MISMATCH:
        return "return-type-mismatch";
    case CM_SEMANTIC_ITEM_ABI_MISMATCH: return "abi-mismatch";
    case CM_SEMANTIC_ITEM_SAFETY_MISMATCH: return "safety-mismatch";
    case CM_SEMANTIC_ITEM_CONST_MISMATCH: return "const-mismatch";
    case CM_SEMANTIC_ITEM_ASYNC_MISMATCH: return "async-mismatch";
    case CM_SEMANTIC_ITEM_VARIADIC_MISMATCH: return "variadic-mismatch";
    case CM_SEMANTIC_ITEM_UNSUPPORTED: return "unsupported";
    case CM_SEMANTIC_ITEM_OVERFLOW: return "overflow";
    case CM_SEMANTIC_ITEM_TYPECK_FAILURE: return "typeck-failure";
    case CM_SEMANTIC_ITEM_INVALID: return "invalid";
    }
    return "unknown";
}
