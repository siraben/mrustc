#include "cm/hir/body.h"

#include <string.h>

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

static int cm_hir_body_owner_type_supported(const CmHirContext *context,
    const CmHirItem *impl_item, CmHirTypeId type_id, size_t depth);

typedef enum CmHirBodyOwnerNamedTargetKind {
    CM_HIR_BODY_OWNER_NAMED_ADT = 0,
    CM_HIR_BODY_OWNER_NAMED_TRAIT
} CmHirBodyOwnerNamedTargetKind;

static int cm_hir_body_owner_type_parameters_supported(
    const CmHirContext *context, const CmHirItem *item)
{
    uint32_t index;

    if (context == NULL || item == NULL) return 0;
    if (item->generic_parameter_count == 0u) {
        return item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE;
    }
    if (item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE
        || item->generic_parameter_count - 1u > UINT32_MAX
            - item->generic_parameter_start) {
        return 0;
    }
    for (index = 0u; index < item->generic_parameter_count; ++index) {
        const CmHirGenericParam *parameter;

        parameter = cm_hir_get_generic_param(context,
            item->generic_parameter_start + index);
        if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_TYPE
            || parameter->index != index || parameter->is_relaxed_sized
            || parameter->has_default
            || !cm_hir_def_id_equal(parameter->owner,
                item->definition)) {
            return 0;
        }
    }
    return 1;
}

static int cm_hir_body_owner_constraints_empty(const CmHirItem *item)
{
    return item != NULL
        && item->predicate_scope_count == 0u
        && item->predicate_scopes == NULL
        && item->predicate_count == 0u
        && item->predicates == NULL
        && item->outlives_predicate_count == 0u
        && item->outlives_predicates == NULL;
}

static int cm_hir_body_owner_parameter_bindings_supported(
    const CmHirItem *item)
{
    const CmHirFunctionSignature *signature;
    uint32_t index;

    if (item == NULL || item->kind != CM_HIR_ITEM_FUNCTION) return 0;
    signature = &item->data.function_item.signature;
    for (index = 0u; index < signature->parameter_count; ++index) {
        switch (signature->parameters[index].binding_mode) {
        case CM_HIR_PARAMETER_BINDING_MOVE:
            break;
        case CM_HIR_PARAMETER_BINDING_REF_SHARED:
        case CM_HIR_PARAMETER_BINDING_REF_MUTABLE:
        case CM_HIR_PARAMETER_BINDING_DEREF_SHARED:
        default:
            return 0;
        }
    }
    return 1;
}

static int cm_hir_body_trait_default_scalar_type(
    const CmHirContext *context, CmHirTypeId type_id)
{
    const CmHirType *type;

    type = cm_hir_get_type(context, type_id);
    return type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && (type->data.integer_type.kind == CM_HIR_INT_I32
            || type->data.integer_type.kind == CM_HIR_INT_U32
            || type->data.integer_type.kind == CM_HIR_INT_USIZE);
}

static int cm_hir_body_trait_default_rust_abi(
    const CmHirContext *context, CmInternId abi)
{
    const CmInternedString *name;

    name = context == NULL ? NULL : cm_interner_get(&context->strings, abi);
    return name != NULL && name->len == 4u
        && memcmp(name->bytes, "Rust", 4u) == 0;
}

/*
 * Until typeck has a rigid trait-owned Self term, admit only defaults whose
 * signature is observationally independent of the enclosing trait.  Keep
 * this deliberately narrower than the concrete-impl type predicate below:
 * body.c pairs it with a syntax capability check that rejects all lookup and
 * dispatch forms requiring a current-trait environment.
 */
static int cm_hir_body_trait_default_signature_supported(
    const CmHirContext *context, const CmHirItem *trait_item,
    const CmHirItem *method, const CmHirBody *body)
{
    const CmHirFunctionSignature *signature;
    uint32_t index;
    uint32_t local_index;

    if (context == NULL || trait_item == NULL || method == NULL
        || body == NULL || trait_item->kind != CM_HIR_ITEM_TRAIT
        || method->kind != CM_HIR_ITEM_FUNCTION
        || trait_item->definition.crate_id != method->definition.crate_id
        || trait_item->owner_module != method->owner_module
        || trait_item->data.trait_item.safety != CM_HIR_SAFE
        || trait_item->data.trait_item.is_auto
        || trait_item->data.trait_item.supertrait_count != 0u
        || trait_item->data.trait_item.supertraits != NULL
        || !cm_hir_def_id_equal(method->parent_definition,
            trait_item->definition)
        || method->data.function_item.body == CM_HIR_BODY_NONE
        || cm_hir_def_id_is_none(method->definition)
        || !cm_hir_def_id_is_none(
            method->data.function_item.trait_item_definition)
        || !cm_hir_def_id_equal(body->owner, method->definition)
        || body->source == 0u || body->source_expression_id == 0u
        || body->span.source != body->source
        || body->span.start > body->span.end
        || (body->state == CM_HIR_BODY_UNLOWERED
                ? body->root_expression != CM_HIR_EXPR_NONE
            : body->state == CM_HIR_BODY_TYPED
                ? body->root_expression == CM_HIR_EXPR_NONE
                : 1)
        || body->error_reason != CM_INTERN_ID_NONE) {
        return 0;
    }
    signature = &method->data.function_item.signature;
    if (signature->receiver != CM_HIR_RECEIVER_NONE
        || signature->safety != CM_HIR_SAFE || signature->is_const
        || signature->is_async || signature->is_variadic
        || !cm_hir_body_trait_default_rust_abi(context, signature->abi)
        || body->expected_type != signature->return_type
        || body->parameter_count != signature->parameter_count
        || !cm_hir_body_trait_default_scalar_type(context,
            signature->return_type)
        || (signature->parameter_count == 0u)
            != (signature->parameters == NULL)
        || (body->local_count == 0u) != (body->locals == NULL)) {
        return 0;
    }
    local_index = 0u;
    for (index = 0u; index < signature->parameter_count; ++index) {
        const CmHirFunctionParameter *parameter;

        parameter = &signature->parameters[index];
        if (!cm_hir_body_trait_default_scalar_type(context,
                parameter->type)) {
            return 0;
        }
        if (parameter->binding_kind == CM_HIR_BINDING_DISCARD) continue;
        if (parameter->binding_kind != CM_HIR_BINDING_NAMED
            || local_index >= body->local_count
            || body->locals[local_index].parameter_index != index
            || body->locals[local_index].name != parameter->name
            || body->locals[local_index].type != parameter->type) {
            return 0;
        }
        ++local_index;
    }
    for (; local_index < body->local_count; ++local_index) {
        if (body->locals[local_index].parameter_index
                != CM_HIR_PARAMETER_INDEX_NONE
            || !cm_hir_body_trait_default_scalar_type(context,
                body->locals[local_index].type)) {
            return 0;
        }
    }
    return 1;
}

static int cm_hir_body_owner_impl_type_parameter(
    const CmHirContext *context, const CmHirItem *impl_item,
    CmHirGenericParamId parameter_id)
{
    const CmHirGenericParam *parameter;
    uint32_t index;

    if (context == NULL || impl_item == NULL
        || impl_item->generic_parameter_count == 0u
        || impl_item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE
        || parameter_id < impl_item->generic_parameter_start) {
        return 0;
    }
    index = parameter_id - impl_item->generic_parameter_start;
    if (index >= impl_item->generic_parameter_count) return 0;
    parameter = cm_hir_get_generic_param(context, parameter_id);
    return parameter != NULL && parameter->kind == CM_HIR_GENERIC_TYPE
        && !parameter->is_relaxed_sized && !parameter->has_default
        && parameter->index == index
        && cm_hir_def_id_equal(parameter->owner, impl_item->definition);
}

static int cm_hir_body_owner_region_concrete(const CmHirRegion *region)
{
    return region != NULL
        && (region->kind == CM_HIR_REGION_STATIC
            || region->kind == CM_HIR_REGION_ERASED);
}

static int cm_hir_body_owner_const_concrete(const CmHirContext *context,
    const CmHirItem *impl_item, const CmHirConstArg *constant, size_t depth)
{
    return constant != NULL && constant->kind == CM_HIR_CONST_VALUE
        && cm_hir_body_owner_type_supported(context, impl_item,
            constant->type, depth + 1u);
}

static int cm_hir_body_owner_named_supported(const CmHirContext *context,
    const CmHirItem *impl_item, const CmHirNamedType *named,
    CmHirBodyOwnerNamedTargetKind target_kind, size_t depth)
{
    const CmHirItem *target;
    uint32_t index;

    if (named == NULL || cm_hir_def_id_is_none(named->definition)
        || depth >= CM_HIR_BODY_OWNER_TYPE_DEPTH
        || (named->argument_count == 0u) != (named->arguments == NULL)) {
        return 0;
    }
    target = cm_hir_body_owner_item(context, named->definition);
    if (target == NULL
        || (target_kind == CM_HIR_BODY_OWNER_NAMED_ADT
            ? target->kind != CM_HIR_ITEM_STRUCT
                && target->kind != CM_HIR_ITEM_UNION
                && target->kind != CM_HIR_ITEM_ENUM
            : target->kind != CM_HIR_ITEM_TRAIT)
        || target->generic_parameter_count != named->argument_count
        || (target->generic_parameter_count == 0u
            ? target->generic_parameter_start != CM_HIR_GENERIC_PARAM_NONE
            : target->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE
                || target->generic_parameter_count - 1u > UINT32_MAX
                    - target->generic_parameter_start)) {
        return 0;
    }
    for (index = 0u; index < named->argument_count; ++index) {
        const CmHirGenericArg *argument;
        const CmHirGenericParam *parameter;

        argument = &named->arguments[index];
        parameter = cm_hir_get_generic_param(context,
            target->generic_parameter_start + index);
        if (parameter == NULL || parameter->index != index
            || !cm_hir_def_id_equal(parameter->owner, target->definition)
            || (argument->kind == CM_HIR_GENERIC_ARG_LIFETIME
                    ? parameter->kind != CM_HIR_GENERIC_LIFETIME
                : argument->kind == CM_HIR_GENERIC_ARG_TYPE
                    ? parameter->kind != CM_HIR_GENERIC_TYPE
                : argument->kind == CM_HIR_GENERIC_ARG_CONST
                    ? parameter->kind != CM_HIR_GENERIC_CONST
                    : 1)) {
            return 0;
        }
        if (argument->kind == CM_HIR_GENERIC_ARG_TYPE) {
            if (!cm_hir_body_owner_type_supported(context, impl_item,
                    argument->data.type, depth + 1u)) return 0;
        } else if (argument->kind == CM_HIR_GENERIC_ARG_LIFETIME) {
            if (!cm_hir_body_owner_region_concrete(
                    &argument->data.lifetime)) return 0;
        } else if (argument->kind == CM_HIR_GENERIC_ARG_CONST) {
            if (!cm_hir_body_owner_const_concrete(context, impl_item,
                    &argument->data.constant, depth + 1u)) return 0;
        } else {
            return 0;
        }
    }
    return 1;
}

static int cm_hir_body_owner_type_supported(const CmHirContext *context,
    const CmHirItem *impl_item, CmHirTypeId type_id, size_t depth)
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
            && cm_hir_body_owner_type_supported(context, impl_item,
                type->data.reference_type.pointee, depth + 1u);
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        return cm_hir_body_owner_type_supported(context, impl_item,
            type->data.raw_pointer_type.pointee, depth + 1u);
    case CM_HIR_TYPE_TUPLE_KIND:
        if ((type->data.tuple_type.element_count == 0u)
                != (type->data.tuple_type.elements == NULL)) return 0;
        for (index = 0u; index < type->data.tuple_type.element_count;
             ++index) {
            if (!cm_hir_body_owner_type_supported(context, impl_item,
                    type->data.tuple_type.elements[index],
                    depth + 1u)) return 0;
        }
        return 1;
    case CM_HIR_TYPE_ARRAY_KIND:
        return cm_hir_body_owner_type_supported(context, impl_item,
                type->data.array_type.element, depth + 1u)
            && cm_hir_body_owner_const_concrete(context, impl_item,
                &type->data.array_type.length, depth + 1u);
    case CM_HIR_TYPE_SLICE_KIND:
        return cm_hir_body_owner_type_supported(context, impl_item,
            type->data.slice_type.element, depth + 1u);
    case CM_HIR_TYPE_FN_POINTER_KIND:
        if ((type->data.fn_pointer_type.parameter_count == 0u)
                != (type->data.fn_pointer_type.parameters == NULL)
            || !cm_hir_body_owner_type_supported(context, impl_item,
                type->data.fn_pointer_type.return_type, depth + 1u)) {
            return 0;
        }
        for (index = 0u;
             index < type->data.fn_pointer_type.parameter_count; ++index) {
            if (!cm_hir_body_owner_type_supported(context, impl_item,
                    type->data.fn_pointer_type.parameters[index],
                    depth + 1u)) return 0;
        }
        return 1;
    case CM_HIR_TYPE_ADT_KIND:
        return cm_hir_body_owner_named_supported(context, impl_item,
            &type->data.named_type, CM_HIR_BODY_OWNER_NAMED_ADT,
            depth + 1u);
    case CM_HIR_TYPE_PARAMETER_KIND:
        return cm_hir_body_owner_impl_type_parameter(context, impl_item,
            type->data.parameter_type.parameter);
    case CM_HIR_TYPE_ERROR_KIND:
    case CM_HIR_TYPE_INFER_KIND:
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
    case CM_HIR_TYPE_SELF_KIND:
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
    const CmHirBody *body;
    const CmHirItem *parent;
    const CmHirItem *trait_method;

    if (context == NULL || item == NULL
        || item->kind != CM_HIR_ITEM_FUNCTION
        || !cm_hir_body_owner_parameter_bindings_supported(item)) {
        return CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED;
    }
    if (cm_hir_def_id_is_none(item->parent_definition)) {
        return cm_hir_def_id_is_none(
                item->data.function_item.trait_item_definition)
            ? CM_HIR_BODY_FUNCTION_OWNER_FREE
            : CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED;
    }
    parent = cm_hir_body_owner_item(context, item->parent_definition);
    if (parent != NULL && parent->kind == CM_HIR_ITEM_TRAIT) {
        body = cm_hir_get_body(context,
            item->data.function_item.body);
        return cm_hir_body_owner_constraints_empty(parent)
                && cm_hir_body_owner_constraints_empty(item)
                && parent->generic_parameter_count == 0u
                && parent->generic_parameter_start
                    == CM_HIR_GENERIC_PARAM_NONE
                && item->generic_parameter_count == 0u
                && item->generic_parameter_start
                    == CM_HIR_GENERIC_PARAM_NONE
                && cm_hir_body_trait_default_signature_supported(context,
                    parent, item, body)
            ? CM_HIR_BODY_FUNCTION_OWNER_TRAIT_DEFAULT
            : CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED;
    }
    if (parent == NULL || parent->kind != CM_HIR_ITEM_IMPL
        || !cm_hir_body_owner_type_parameters_supported(context, parent)
        || !cm_hir_body_owner_constraints_empty(parent)
        || !parent->data.impl_item.has_trait
        || parent->data.impl_item.is_negative
        || !cm_hir_body_owner_type_parameters_supported(context, item)
        || item->generic_parameter_count != 0u
        || !cm_hir_body_owner_constraints_empty(item)
        || cm_hir_def_id_is_none(
            item->data.function_item.trait_item_definition)
        || !cm_hir_body_owner_type_supported(context, parent,
            parent->data.impl_item.self_type, 0u)
        || !cm_hir_body_owner_named_supported(context, parent,
            &parent->data.impl_item.trait_type,
            CM_HIR_BODY_OWNER_NAMED_TRAIT, 0u)) {
        return CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED;
    }
    trait_method = cm_hir_body_owner_item(context,
        item->data.function_item.trait_item_definition);
    if (trait_method == NULL || trait_method->kind != CM_HIR_ITEM_FUNCTION
        || !cm_hir_body_owner_type_parameters_supported(context,
            trait_method)
        || trait_method->generic_parameter_count != 0u
        || !cm_hir_body_owner_constraints_empty(trait_method)
        || !cm_hir_def_id_equal(trait_method->parent_definition,
            parent->data.impl_item.trait_type.definition)) {
        return CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED;
    }
    return parent->generic_parameter_count == 0u
        ? CM_HIR_BODY_FUNCTION_OWNER_CONCRETE_TRAIT_IMPL_METHOD
        : CM_HIR_BODY_FUNCTION_OWNER_TYPE_GENERIC_TRAIT_IMPL_METHOD;
}

CmHirBodyValueOwnerKind cm_hir_body_value_owner_kind(
    const CmHirContext *context, const CmHirItem *item)
{
    const CmHirBody *body;

    if (context == NULL || item == NULL
        || (item->kind != CM_HIR_ITEM_CONST
            && item->kind != CM_HIR_ITEM_STATIC)
        || !cm_hir_def_id_is_none(item->parent_definition)
        || !cm_hir_def_id_is_none(
            item->data.value_item.trait_item_definition)
        || item->generic_parameter_start != CM_HIR_GENERIC_PARAM_NONE
        || item->generic_parameter_count != 0u
        || !cm_hir_body_owner_constraints_empty(item)
        || item->data.value_item.body == CM_HIR_BODY_NONE
        || item->data.value_item.type == CM_HIR_TYPE_NONE
        || item->data.value_item.mutability != CM_HIR_IMMUTABLE) {
        return CM_HIR_BODY_VALUE_OWNER_UNSUPPORTED;
    }
    body = cm_hir_get_body(context, item->data.value_item.body);
    if (body == NULL || !cm_hir_def_id_equal(body->owner, item->definition)
        || body->expected_type != item->data.value_item.type
        || body->local_count != 0u || body->locals != NULL
        || body->parameter_count != 0u) {
        return CM_HIR_BODY_VALUE_OWNER_UNSUPPORTED;
    }
    return item->kind == CM_HIR_ITEM_CONST
        ? CM_HIR_BODY_VALUE_OWNER_FREE_CONST
        : CM_HIR_BODY_VALUE_OWNER_FREE_STATIC;
}
