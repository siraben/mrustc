#include "cm/hir/type_alias.h"

#include "cm/alloc.h"

#include <stdint.h>
#include <string.h>

#define CM_ALIAS_RECURSION_LIMIT 512u

typedef struct CmAliasFrame {
    CmHirDefId definition;
    CmHirGenericParamId parameter_start;
    uint32_t parameter_count;
    const CmHirGenericArg *arguments;
    const struct CmAliasFrame *previous;
} CmAliasFrame;

typedef struct CmAliasNormalizeState {
    CmHirContext *context;
    CmHirTypeAliasResult result;
    CmVec active_aliases;
    const CmAliasFrame *frame;
    size_t recursion_depth;
} CmAliasNormalizeState;

static int cm_alias_normalize_type(CmAliasNormalizeState *state,
    CmHirTypeId source_id, CmHirTypeId *out_type);
static const CmHirItem *cm_alias_bound_owner(const CmHirContext *context,
    CmHirDefId owner);

static int cm_alias_const_argument_valid(
    const CmAliasNormalizeState *state, const CmHirNamedType *source,
    uint32_t index)
{
    const CmHirGenericArg *argument;
    const CmHirGenericParam *parameter;
    const CmHirGenericParam *source_parameter;
    const CmHirItem *owner;
    const CmHirType *parameter_type;
    const CmHirType *source_type;
    CmHirGenericParamId parameter_id;
    int permits_external_parameter;
    int compatible;

    argument = &source->arguments[index];
    if (argument->kind != CM_HIR_GENERIC_ARG_CONST
        || argument->data.constant.kind != CM_HIR_CONST_PARAMETER) {
        return 0;
    }
    owner = cm_alias_bound_owner(state->context, source->definition);
    if (owner == NULL || owner->generic_parameter_start
            == CM_HIR_GENERIC_PARAM_NONE
        || index >= owner->generic_parameter_count
        || index > UINT32_MAX - owner->generic_parameter_start) {
        return 0;
    }
    parameter_id = owner->generic_parameter_start + index;
    parameter = cm_hir_get_generic_param(state->context, parameter_id);
    source_parameter = cm_hir_get_generic_param(state->context,
        argument->data.constant.data.parameter);
    parameter_type = parameter == NULL ? NULL
        : cm_hir_get_type(state->context, parameter->declared_type);
    source_type = source_parameter == NULL ? NULL
        : cm_hir_get_type(state->context, source_parameter->declared_type);
    compatible = parameter_type != NULL && source_type != NULL
        && parameter_type->kind == source_type->kind
        && ((parameter_type->kind == CM_HIR_TYPE_INTEGER_KIND
                && parameter_type->data.integer_type.kind
                    == source_type->data.integer_type.kind)
            || parameter_type->kind == CM_HIR_TYPE_BOOL_KIND
            || parameter_type->kind == CM_HIR_TYPE_CHAR_KIND);
    permits_external_parameter = owner->kind == CM_HIR_ITEM_STRUCT
        || owner->kind == CM_HIR_ITEM_UNION
        || owner->kind == CM_HIR_ITEM_ENUM;
    return parameter != NULL
        && parameter->kind == CM_HIR_GENERIC_CONST
        && parameter->index == index
        && cm_hir_def_id_equal(parameter->owner, owner->definition)
        && parameter->declared_type != CM_HIR_TYPE_NONE
        && parameter->declared_type == argument->data.constant.type
        && source_parameter != NULL
        && source_parameter->kind == CM_HIR_GENERIC_CONST
        && (argument->data.constant.data.parameter == parameter_id
            || permits_external_parameter)
        && compatible;
}

static int cm_alias_normalize_type_inner(CmAliasNormalizeState *state,
    CmHirTypeId source_id, CmHirTypeId *out_type);

static void cm_alias_state_init(CmAliasNormalizeState *state,
    CmHirContext *context)
{
    memset(state, 0, sizeof(*state));
    state->context = context;
    state->result.type = CM_HIR_TYPE_NONE;
    state->result.source_type = CM_HIR_TYPE_NONE;
    state->result.alias_definition = cm_hir_def_id_none();
    state->result.parameter = CM_HIR_GENERIC_PARAM_NONE;
    state->result.hir_status = CM_HIR_OK;
    cm_vec_init(&state->active_aliases, sizeof(CmHirDefId));
}

static int cm_alias_fail(CmAliasNormalizeState *state,
    CmHirTypeAliasStatus status, CmHirTypeId source_type,
    CmHirDefId alias_definition, CmHirGenericParamId parameter,
    CmHirStatus hir_status)
{
    if (state->result.status == CM_HIR_TYPE_ALIAS_OK) {
        state->result.status = status;
        state->result.source_type = source_type;
        state->result.alias_definition = alias_definition;
        state->result.parameter = parameter;
        state->result.hir_status = hir_status;
    }
    return 0;
}

static int cm_alias_def_is_none(CmHirDefId definition)
{
    return cm_hir_def_id_is_none(definition);
}

static int cm_alias_active(const CmAliasNormalizeState *state,
    CmHirDefId definition)
{
    size_t index;

    for (index = 0u; index < state->active_aliases.len; ++index) {
        const CmHirDefId *active;

        active = (const CmHirDefId *)cm_vec_at_const(&state->active_aliases,
            index);
        if (active != NULL && cm_hir_def_id_equal(*active, definition)) {
            return 1;
        }
    }
    return 0;
}

static int cm_alias_parameter_id(CmHirGenericParamId start, uint32_t offset,
    CmHirGenericParamId *out_parameter)
{
    uint32_t start_value;

    start_value = (uint32_t)start;
    if (start == CM_HIR_GENERIC_PARAM_NONE
        || offset > UINT32_MAX - start_value) {
        *out_parameter = CM_HIR_GENERIC_PARAM_NONE;
        return 0;
    }
    *out_parameter = (CmHirGenericParamId)(start_value + offset);
    return *out_parameter != CM_HIR_GENERIC_PARAM_NONE;
}

static const CmHirGenericArg *cm_alias_find_argument(
    const CmAliasNormalizeState *state, CmHirGenericParamId parameter)
{
    const CmAliasFrame *frame;

    for (frame = state->frame; frame != NULL; frame = frame->previous) {
        uint32_t index;

        for (index = 0u; index < frame->parameter_count; ++index) {
            CmHirGenericParamId candidate;

            if (cm_alias_parameter_id(frame->parameter_start, index,
                    &candidate)
                && candidate == parameter) {
                return &frame->arguments[index];
            }
        }
    }
    return NULL;
}

static int cm_alias_region_equal(const CmHirRegion *left,
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
        return left->data.inference_variable ==
            right->data.inference_variable;
    case CM_HIR_REGION_ERROR:
        return left->data.error_reason == right->data.error_reason;
    }
    return 0;
}

static int cm_alias_normalize_region(CmAliasNormalizeState *state,
    CmHirTypeId source_type, const CmHirRegion *source,
    CmHirRegion *out_region)
{
    const CmHirGenericParam *parameter;
    const CmHirGenericArg *argument;

    *out_region = *source;
    switch (source->kind) {
    case CM_HIR_REGION_STATIC:
    case CM_HIR_REGION_LATE_BOUND:
    case CM_HIR_REGION_INFER:
    case CM_HIR_REGION_ERASED:
        return 1;
    case CM_HIR_REGION_EARLY_BOUND:
        parameter = cm_hir_get_generic_param(state->context,
            source->data.parameter);
        if (parameter == NULL
            || parameter->kind != CM_HIR_GENERIC_LIFETIME) {
            return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_TYPE,
                source_type, cm_hir_def_id_none(), source->data.parameter,
                CM_HIR_INVALID_ID);
        }
        argument = cm_alias_find_argument(state, source->data.parameter);
        if (argument != NULL) {
            if (argument->kind != CM_HIR_GENERIC_ARG_LIFETIME) {
                return cm_alias_fail(state,
                    CM_HIR_TYPE_ALIAS_ARGUMENT_KIND, source_type,
                    parameter->owner, source->data.parameter,
                    CM_HIR_INVARIANT_VIOLATION);
            }
            *out_region = argument->data.lifetime;
            return 1;
        }
        if (state->frame != NULL) {
            return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_ALIAS,
                source_type, state->frame->definition,
                source->data.parameter,
                CM_HIR_INVARIANT_VIOLATION);
        }
        return 1;
    case CM_HIR_REGION_ERROR:
        return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_TYPE,
            source_type, cm_hir_def_id_none(), CM_HIR_GENERIC_PARAM_NONE,
            CM_HIR_INVALID_ID);
    }
    return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_TYPE, source_type,
        cm_hir_def_id_none(), CM_HIR_GENERIC_PARAM_NONE,
        CM_HIR_INVALID_ARGUMENT);
}

static int cm_alias_add_type(CmAliasNormalizeState *state,
    CmHirTypeId source_type, const CmHirType *type, CmHirTypeId *out_type)
{
    CmHirStatus status;

    status = cm_hir_add_type(state->context, type, out_type);
    if (status != CM_HIR_OK) {
        return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_HIR_FAILURE,
            source_type, cm_hir_def_id_none(), CM_HIR_GENERIC_PARAM_NONE,
            status);
    }
    return 1;
}

static int cm_alias_normalize_named_arguments(CmAliasNormalizeState *state,
    CmHirTypeId source_type, const CmHirNamedType *source,
    CmHirGenericArg **out_arguments, int *out_changed)
{
    CmHirGenericArg *arguments;
    uint32_t index;

    *out_arguments = NULL;
    *out_changed = 0;
    if (source->argument_count == 0u) return 1;
    if (source->arguments == NULL) {
        return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_TYPE,
            source_type, source->definition, CM_HIR_GENERIC_PARAM_NONE,
            CM_HIR_INVALID_ID);
    }
    arguments = (CmHirGenericArg *)cm_alloc_zeroed(
        (size_t)source->argument_count, sizeof(*arguments));
    for (index = 0u; index < source->argument_count; ++index) {
        arguments[index] = source->arguments[index];
        switch (source->arguments[index].kind) {
        case CM_HIR_GENERIC_ARG_LIFETIME:
            if (!cm_alias_normalize_region(state, source_type,
                    &source->arguments[index].data.lifetime,
                    &arguments[index].data.lifetime)) {
                cm_free(arguments);
                return 0;
            }
            if (!cm_alias_region_equal(
                    &source->arguments[index].data.lifetime,
                    &arguments[index].data.lifetime)) {
                *out_changed = 1;
            }
            break;
        case CM_HIR_GENERIC_ARG_TYPE:
            if (!cm_alias_normalize_type(state,
                    source->arguments[index].data.type,
                    &arguments[index].data.type)) {
                cm_free(arguments);
                return 0;
            }
            if (arguments[index].data.type !=
                    source->arguments[index].data.type) {
                *out_changed = 1;
            }
            break;
        case CM_HIR_GENERIC_ARG_CONST:
            if (state->frame != NULL
                || !cm_alias_const_argument_valid(state, source,
                    index)) {
                cm_free(arguments);
                return cm_alias_fail(state,
                    CM_HIR_TYPE_ALIAS_UNSUPPORTED_CONST, source_type,
                    source->definition, CM_HIR_GENERIC_PARAM_NONE,
                    CM_HIR_OK);
            }
            break;
        default:
            cm_free(arguments);
            return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_TYPE,
                source_type, source->definition, CM_HIR_GENERIC_PARAM_NONE,
                CM_HIR_INVALID_ARGUMENT);
        }
    }
    *out_arguments = arguments;
    return 1;
}

static int cm_alias_normalize_nominal(CmAliasNormalizeState *state,
    CmHirTypeId source_id, const CmHirType *source, CmHirTypeId *out_type)
{
    CmHirType replacement;
    CmHirGenericArg *arguments;
    int changed;

    if (cm_alias_def_is_none(source->data.named_type.definition)) {
        return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_TYPE, source_id,
            source->data.named_type.definition, CM_HIR_GENERIC_PARAM_NONE,
            CM_HIR_INVALID_ID);
    }
    if (!cm_alias_normalize_named_arguments(state, source_id,
            &source->data.named_type, &arguments, &changed)) {
        return 0;
    }
    if (!changed) {
        cm_free(arguments);
        *out_type = source_id;
        return 1;
    }
    replacement = *source;
    replacement.data.named_type.arguments = arguments;
    if (!cm_alias_add_type(state, source_id, &replacement, out_type)) {
        cm_free(arguments);
        return 0;
    }
    cm_free(arguments);
    return 1;
}

static int cm_alias_normalize_dyn_trait(CmAliasNormalizeState *state,
    CmHirTypeId source_id, const CmHirType *source, CmHirTypeId *out_type)
{
    CmHirType replacement;
    CmHirGenericArg *arguments;
    CmHirRegion region;
    int arguments_changed;

    if (cm_alias_def_is_none(
            source->data.dyn_trait_type.principal_trait.definition)) {
        return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_TYPE,
            source_id,
            source->data.dyn_trait_type.principal_trait.definition,
            CM_HIR_GENERIC_PARAM_NONE, CM_HIR_INVALID_ID);
    }
    if (!cm_alias_normalize_named_arguments(state, source_id,
            &source->data.dyn_trait_type.principal_trait, &arguments,
            &arguments_changed)) {
        return 0;
    }
    if (!cm_alias_normalize_region(state, source_id,
            &source->data.dyn_trait_type.region, &region)) {
        cm_free(arguments);
        return 0;
    }
    if (!arguments_changed && cm_alias_region_equal(&region,
            &source->data.dyn_trait_type.region)) {
        cm_free(arguments);
        *out_type = source_id;
        return 1;
    }
    replacement = *source;
    replacement.data.dyn_trait_type.principal_trait.arguments = arguments;
    replacement.data.dyn_trait_type.region = region;
    if (!cm_alias_add_type(state, source_id, &replacement, out_type)) {
        cm_free(arguments);
        return 0;
    }
    cm_free(arguments);
    return 1;
}

static int cm_alias_validate_projection(CmAliasNormalizeState *state,
    CmHirTypeId source_id, const CmHirType *source)
{
    const CmHirDefinition *trait_definition;
    const CmHirDefinition *associated_definition;
    const CmHirItem *trait_item;
    const CmHirItem *associated_item;

    trait_definition = cm_hir_lookup_definition(state->context,
        source->data.projection_type.trait_type.definition);
    associated_definition = cm_hir_lookup_definition(state->context,
        source->data.projection_type.associated_type.definition);
    if (trait_definition == NULL
        || trait_definition->kind != CM_HIR_DEFINITION_ITEM
        || trait_definition->state != CM_HIR_DEFINITION_BOUND) {
        return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_TYPE,
            source_id, source->data.projection_type.trait_type.definition,
            CM_HIR_GENERIC_PARAM_NONE, CM_HIR_INVALID_ID);
    }
    if (associated_definition == NULL
        || associated_definition->kind != CM_HIR_DEFINITION_ITEM
        || associated_definition->state != CM_HIR_DEFINITION_BOUND) {
        return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_TYPE,
            source_id,
            source->data.projection_type.associated_type.definition,
            CM_HIR_GENERIC_PARAM_NONE, CM_HIR_INVALID_ID);
    }
    trait_item = cm_hir_get_item(state->context,
        trait_definition->entity.item_id);
    associated_item = cm_hir_get_item(state->context,
        associated_definition->entity.item_id);
    if (trait_item == NULL || trait_item->kind != CM_HIR_ITEM_TRAIT
        || !cm_hir_def_id_equal(trait_item->definition,
            trait_definition->id)) {
        return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_TYPE,
            source_id, trait_definition->id,
            CM_HIR_GENERIC_PARAM_NONE, CM_HIR_INVARIANT_VIOLATION);
    }
    if (associated_item == NULL
        || associated_item->kind != CM_HIR_ITEM_TYPE_ALIAS
        || !cm_hir_def_id_equal(associated_item->definition,
            associated_definition->id)
        || !cm_hir_def_id_equal(associated_item->parent_definition,
            trait_definition->id)) {
        return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_TYPE,
            source_id, associated_definition->id,
            CM_HIR_GENERIC_PARAM_NONE, CM_HIR_INVARIANT_VIOLATION);
    }
    return 1;
}

static int cm_alias_normalize_projection(CmAliasNormalizeState *state,
    CmHirTypeId source_id, const CmHirType *source, CmHirTypeId *out_type)
{
    CmHirType replacement;
    CmHirGenericArg *trait_arguments;
    CmHirGenericArg *associated_arguments;
    CmHirTypeId self_type;
    int trait_changed;
    int associated_changed;

    if (!cm_alias_validate_projection(state, source_id, source)) return 0;
    if (!cm_alias_normalize_type(state,
            source->data.projection_type.self_type, &self_type)
        || !cm_alias_normalize_named_arguments(state, source_id,
            &source->data.projection_type.trait_type, &trait_arguments,
            &trait_changed)) {
        return 0;
    }
    if (!cm_alias_normalize_named_arguments(state, source_id,
            &source->data.projection_type.associated_type,
            &associated_arguments, &associated_changed)) {
        cm_free(trait_arguments);
        return 0;
    }
    if (self_type == source->data.projection_type.self_type
        && !trait_changed && !associated_changed) {
        cm_free(associated_arguments);
        cm_free(trait_arguments);
        *out_type = source_id;
        return 1;
    }
    replacement = *source;
    replacement.data.projection_type.self_type = self_type;
    replacement.data.projection_type.trait_type.arguments = trait_arguments;
    replacement.data.projection_type.associated_type.arguments =
        associated_arguments;
    if (!cm_alias_add_type(state, source_id, &replacement, out_type)) {
        cm_free(associated_arguments);
        cm_free(trait_arguments);
        return 0;
    }
    cm_free(associated_arguments);
    cm_free(trait_arguments);
    return 1;
}

static int cm_alias_validate_signature(CmAliasNormalizeState *state,
    CmHirTypeId source_id, const CmHirItem *item,
    const CmHirNamedType *application)
{
    size_t owned_count;
    size_t generic_index;
    uint32_t index;

    if ((item->generic_parameter_count == 0u
            && item->generic_parameter_start != CM_HIR_GENERIC_PARAM_NONE)
        || (item->generic_parameter_count != 0u
            && item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE)) {
        return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_ALIAS,
            source_id, item->definition, CM_HIR_GENERIC_PARAM_NONE,
            CM_HIR_INVARIANT_VIOLATION);
    }
    if (application->argument_count > item->generic_parameter_count) {
        return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_ARGUMENT_COUNT,
            source_id, item->definition, CM_HIR_GENERIC_PARAM_NONE,
            CM_HIR_OK);
    }
    if (application->argument_count != 0u
        && application->arguments == NULL) {
        return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_ALIAS,
            source_id, item->definition, CM_HIR_GENERIC_PARAM_NONE,
            CM_HIR_INVALID_ID);
    }
    for (index = 0u; index < item->generic_parameter_count; ++index) {
        CmHirGenericParamId parameter_id;
        const CmHirGenericParam *parameter;
        CmHirGenericArgKind expected_kind;

        if (!cm_alias_parameter_id(item->generic_parameter_start, index,
                &parameter_id)
            || (parameter = cm_hir_get_generic_param(state->context,
                parameter_id)) == NULL
            || !cm_hir_def_id_equal(parameter->owner, item->definition)
            || parameter->index != index) {
            return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_ALIAS,
                source_id, item->definition, parameter_id,
                CM_HIR_INVARIANT_VIOLATION);
        }
        if (parameter->kind == CM_HIR_GENERIC_CONST) {
            return cm_alias_fail(state,
                CM_HIR_TYPE_ALIAS_UNSUPPORTED_CONST, source_id,
                item->definition, parameter_id, CM_HIR_OK);
        }
        if (parameter->kind == CM_HIR_GENERIC_TYPE) {
            expected_kind = CM_HIR_GENERIC_ARG_TYPE;
        } else if (parameter->kind == CM_HIR_GENERIC_LIFETIME) {
            expected_kind = CM_HIR_GENERIC_ARG_LIFETIME;
        } else {
            return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_ALIAS,
                source_id, item->definition, parameter_id,
                CM_HIR_INVARIANT_VIOLATION);
        }
        if (index < application->argument_count) {
            if (application->arguments[index].kind != expected_kind) {
                return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_ARGUMENT_KIND,
                    source_id, item->definition, parameter_id, CM_HIR_OK);
            }
        } else if (parameter->kind != CM_HIR_GENERIC_TYPE
            || !parameter->has_default
            || parameter->default_argument.kind
                != CM_HIR_GENERIC_ARG_TYPE) {
            return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_ARGUMENT_COUNT,
                source_id, item->definition, parameter_id, CM_HIR_OK);
        }
    }
    owned_count = 0u;
    for (generic_index = 0u;
         generic_index < state->context->generic_parameters.len;
         ++generic_index) {
        const CmHirGenericParam *parameter;

        parameter = (const CmHirGenericParam *)cm_vec_at_const(
            &state->context->generic_parameters, generic_index);
        if (parameter != NULL
            && cm_hir_def_id_equal(parameter->owner, item->definition)) {
            owned_count += 1u;
        }
    }
    if (owned_count != (size_t)item->generic_parameter_count) {
        return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_ALIAS,
            source_id, item->definition, CM_HIR_GENERIC_PARAM_NONE,
            CM_HIR_INVARIANT_VIOLATION);
    }
    return 1;
}

static const CmHirItem *cm_alias_bound_owner(const CmHirContext *context,
    CmHirDefId owner)
{
    const CmHirDefinition *definition;
    const CmHirItem *item;

    definition = cm_hir_lookup_definition(context, owner);
    if (definition == NULL || definition->kind != CM_HIR_DEFINITION_ITEM
        || definition->state != CM_HIR_DEFINITION_BOUND) {
        return NULL;
    }
    item = cm_hir_get_item(context, definition->entity.item_id);
    if (item == NULL || !cm_hir_def_id_equal(item->definition, owner)) {
        return NULL;
    }
    return item;
}

static int cm_alias_validate_instantiation(CmAliasNormalizeState *state,
    CmHirTypeId root, CmHirDefId owner_definition,
    const CmHirGenericArg *arguments, uint32_t argument_count,
    CmAliasFrame *out_frame)
{
    const CmHirItem *owner;
    size_t generic_index;
    size_t owned_count;
    uint32_t index;

    owner = cm_alias_bound_owner(state->context, owner_definition);
    if (owner == NULL) {
        return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_ALIAS, root,
            owner_definition, CM_HIR_GENERIC_PARAM_NONE, CM_HIR_INVALID_ID);
    }
    if ((owner->generic_parameter_count == 0u
            && owner->generic_parameter_start != CM_HIR_GENERIC_PARAM_NONE)
        || (owner->generic_parameter_count != 0u
            && (owner->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE
                || owner->generic_parameter_count - 1u > UINT32_MAX
                    - owner->generic_parameter_start))) {
        return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_ALIAS, root,
            owner_definition, CM_HIR_GENERIC_PARAM_NONE,
            CM_HIR_INVARIANT_VIOLATION);
    }
    if (argument_count != owner->generic_parameter_count) {
        return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_ARGUMENT_COUNT, root,
            owner_definition, CM_HIR_GENERIC_PARAM_NONE, CM_HIR_OK);
    }
    if (argument_count != 0u && arguments == NULL) {
        return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_ARGUMENT, root,
            owner_definition, CM_HIR_GENERIC_PARAM_NONE,
            CM_HIR_INVALID_ARGUMENT);
    }
    for (index = 0u; index < argument_count; ++index) {
        CmHirGenericParamId parameter_id;
        const CmHirGenericParam *parameter;
        CmHirGenericArgKind expected_kind;

        if (!cm_alias_parameter_id(owner->generic_parameter_start, index,
                &parameter_id)
            || (parameter = cm_hir_get_generic_param(state->context,
                parameter_id)) == NULL
            || !cm_hir_def_id_equal(parameter->owner, owner_definition)
            || parameter->index != index) {
            return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_ALIAS,
                root, owner_definition, parameter_id,
                CM_HIR_INVARIANT_VIOLATION);
        }
        if (parameter->kind == CM_HIR_GENERIC_CONST) {
            return cm_alias_fail(state,
                CM_HIR_TYPE_ALIAS_UNSUPPORTED_CONST, root,
                owner_definition, parameter_id, CM_HIR_OK);
        }
        expected_kind = parameter->kind == CM_HIR_GENERIC_TYPE
            ? CM_HIR_GENERIC_ARG_TYPE : CM_HIR_GENERIC_ARG_LIFETIME;
        if (arguments[index].kind != expected_kind) {
            return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_ARGUMENT_KIND,
                root, owner_definition, parameter_id, CM_HIR_OK);
        }
        if (expected_kind == CM_HIR_GENERIC_ARG_TYPE) {
            CmHirTypeId normalized;

            if (!cm_alias_normalize_type(state,
                    arguments[index].data.type, &normalized)) {
                return 0;
            }
            if (normalized != arguments[index].data.type) {
                return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_TYPE,
                    arguments[index].data.type, owner_definition,
                    parameter_id, CM_HIR_INVARIANT_VIOLATION);
            }
        } else {
            CmHirRegion normalized;

            if (!cm_alias_normalize_region(state, root,
                    &arguments[index].data.lifetime, &normalized)) {
                return 0;
            }
            if (!cm_alias_region_equal(&normalized,
                    &arguments[index].data.lifetime)) {
                return cm_alias_fail(state,
                    CM_HIR_TYPE_ALIAS_INVALID_TYPE, root,
                    owner_definition, parameter_id,
                    CM_HIR_INVARIANT_VIOLATION);
            }
        }
    }
    owned_count = 0u;
    for (generic_index = 0u;
         generic_index < state->context->generic_parameters.len;
         ++generic_index) {
        const CmHirGenericParam *parameter;

        parameter = (const CmHirGenericParam *)cm_vec_at_const(
            &state->context->generic_parameters, generic_index);
        if (parameter != NULL
            && cm_hir_def_id_equal(parameter->owner, owner_definition)) {
            owned_count += 1u;
        }
    }
    if (owned_count != (size_t)owner->generic_parameter_count) {
        return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_ALIAS, root,
            owner_definition, CM_HIR_GENERIC_PARAM_NONE,
            CM_HIR_INVARIANT_VIOLATION);
    }
    out_frame->definition = owner_definition;
    out_frame->parameter_start = owner->generic_parameter_start;
    out_frame->parameter_count = owner->generic_parameter_count;
    out_frame->arguments = arguments;
    out_frame->previous = NULL;
    return 1;
}

static int cm_alias_expand(CmAliasNormalizeState *state,
    CmHirTypeId source_id, const CmHirType *source, CmHirTypeId *out_type)
{
    const CmHirDefinition *definition;
    const CmHirItem *item;
    CmHirGenericArg *arguments;
    CmAliasFrame frame;
    CmHirTypeId target;
    const CmHirType *target_type;
    CmHirType replacement;
    uint32_t index;

    definition = cm_hir_lookup_definition(state->context,
        source->data.named_type.definition);
    if (definition == NULL || definition->kind != CM_HIR_DEFINITION_ITEM
        || definition->state != CM_HIR_DEFINITION_BOUND
        || (item = cm_hir_get_item(state->context,
            definition->entity.item_id)) == NULL
        || item->kind != CM_HIR_ITEM_TYPE_ALIAS
        || !cm_hir_def_id_equal(item->definition, definition->id)
        || item->data.type_alias_item.target == CM_HIR_TYPE_NONE
        || cm_hir_get_type(state->context,
            item->data.type_alias_item.target) == NULL) {
        return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_ALIAS,
            source_id, source->data.named_type.definition,
            CM_HIR_GENERIC_PARAM_NONE, CM_HIR_INVALID_ID);
    }
    if (cm_alias_active(state, definition->id)) {
        return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_CYCLE, source_id,
            definition->id, CM_HIR_GENERIC_PARAM_NONE, CM_HIR_OK);
    }
    if (!cm_alias_validate_signature(state, source_id, item,
            &source->data.named_type)) {
        return 0;
    }
    arguments = item->generic_parameter_count == 0u ? NULL
        : (CmHirGenericArg *)cm_alloc_zeroed(
            (size_t)item->generic_parameter_count, sizeof(*arguments));
    for (index = 0u; index < source->data.named_type.argument_count;
         ++index) {
        arguments[index] = source->data.named_type.arguments[index];
        if (arguments[index].kind == CM_HIR_GENERIC_ARG_TYPE) {
            if (!cm_alias_normalize_type(state,
                    arguments[index].data.type,
                    &arguments[index].data.type)) {
                cm_free(arguments);
                return 0;
            }
        } else if (!cm_alias_normalize_region(state, source_id,
                &arguments[index].data.lifetime,
                &arguments[index].data.lifetime)) {
            cm_free(arguments);
            return 0;
        }
    }
    (void)cm_vec_push(&state->active_aliases, &definition->id);
    frame.definition = definition->id;
    frame.parameter_start = item->generic_parameter_start;
    frame.parameter_count = 0u;
    frame.arguments = arguments;
    frame.previous = state->frame;
    for (index = source->data.named_type.argument_count;
         index < item->generic_parameter_count; ++index) {
        const CmHirGenericParam *parameter;

        parameter = cm_hir_get_generic_param(state->context,
            item->generic_parameter_start + index);
        if (parameter == NULL || !parameter->has_default
            || parameter->default_argument.kind
                != CM_HIR_GENERIC_ARG_TYPE) {
            cm_free(arguments);
            cm_vec_resize(&state->active_aliases,
                state->active_aliases.len - 1u);
            return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_ALIAS,
                source_id, definition->id,
                item->generic_parameter_start + index,
                CM_HIR_INVARIANT_VIOLATION);
        }
        arguments[index] = parameter->default_argument;
        frame.parameter_count = index;
        state->frame = &frame;
        if (!cm_alias_normalize_type(state,
                arguments[index].data.type,
                &arguments[index].data.type)) {
            state->frame = frame.previous;
            cm_free(arguments);
            cm_vec_resize(&state->active_aliases,
                state->active_aliases.len - 1u);
            return 0;
        }
        state->frame = frame.previous;
    }
    frame.parameter_count = item->generic_parameter_count;
    state->frame = &frame;
    if (!cm_alias_normalize_type(state, item->data.type_alias_item.target,
            &target)) {
        state->frame = frame.previous;
        cm_free(arguments);
        cm_vec_resize(&state->active_aliases,
            state->active_aliases.len - 1u);
        return 0;
    }
    state->frame = frame.previous;
    cm_free(arguments);
    cm_vec_resize(&state->active_aliases, state->active_aliases.len - 1u);

    target_type = cm_hir_get_type(state->context, target);
    if (target_type == NULL
        || target_type->kind == CM_HIR_TYPE_ALIAS_APPLICATION_KIND) {
        return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_ALIAS,
            source_id, definition->id, CM_HIR_GENERIC_PARAM_NONE,
            CM_HIR_INVARIANT_VIOLATION);
    }
    replacement = *target_type;
    replacement.span = source->span;
    return cm_alias_add_type(state, source_id, &replacement, out_type);
}

static int cm_alias_normalize_type(CmAliasNormalizeState *state,
    CmHirTypeId source_id, CmHirTypeId *out_type)
{
    int success;

    if (state->recursion_depth >= CM_ALIAS_RECURSION_LIMIT) {
        return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_RECURSION_LIMIT,
            source_id, state->frame == NULL ? cm_hir_def_id_none()
                : state->frame->definition,
            CM_HIR_GENERIC_PARAM_NONE, CM_HIR_OK);
    }
    state->recursion_depth += 1u;
    success = cm_alias_normalize_type_inner(state, source_id, out_type);
    state->recursion_depth -= 1u;
    return success;
}

static int cm_alias_normalize_type_inner(CmAliasNormalizeState *state,
    CmHirTypeId source_id, CmHirTypeId *out_type)
{
    const CmHirType *borrowed;
    CmHirType source;
    CmHirType replacement;
    CmHirTypeId child;
    uint32_t index;

    borrowed = cm_hir_get_type(state->context, source_id);
    if (borrowed == NULL) {
        return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_TYPE, source_id,
            cm_hir_def_id_none(), CM_HIR_GENERIC_PARAM_NONE,
            CM_HIR_INVALID_ID);
    }
    source = *borrowed;
    replacement = source;
    switch (source.kind) {
    case CM_HIR_TYPE_ERROR_KIND:
        return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_TYPE, source_id,
            cm_hir_def_id_none(), CM_HIR_GENERIC_PARAM_NONE,
            CM_HIR_INVALID_ID);
    case CM_HIR_TYPE_INFER_KIND:
    case CM_HIR_TYPE_NEVER_KIND:
    case CM_HIR_TYPE_UNIT_KIND:
    case CM_HIR_TYPE_BOOL_KIND:
    case CM_HIR_TYPE_CHAR_KIND:
    case CM_HIR_TYPE_STR_KIND:
    case CM_HIR_TYPE_INTEGER_KIND:
    case CM_HIR_TYPE_FLOAT_KIND:
    case CM_HIR_TYPE_SELF_KIND:
        *out_type = source_id;
        return 1;
    case CM_HIR_TYPE_REFERENCE_KIND:
    {
        CmHirRegion region;

        if (!cm_alias_normalize_type(state,
                source.data.reference_type.pointee, &child)
            || !cm_alias_normalize_region(state, source_id,
                &source.data.reference_type.region, &region)) {
            return 0;
        }
        if (child == source.data.reference_type.pointee
            && cm_alias_region_equal(&region,
                &source.data.reference_type.region)) {
            *out_type = source_id;
            return 1;
        }
        replacement.data.reference_type.pointee = child;
        replacement.data.reference_type.region = region;
        return cm_alias_add_type(state, source_id, &replacement, out_type);
    }
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        if (!cm_alias_normalize_type(state,
                source.data.raw_pointer_type.pointee, &child)) {
            return 0;
        }
        if (child == source.data.raw_pointer_type.pointee) {
            *out_type = source_id;
            return 1;
        }
        replacement.data.raw_pointer_type.pointee = child;
        return cm_alias_add_type(state, source_id, &replacement, out_type);
    case CM_HIR_TYPE_TUPLE_KIND:
    {
        CmHirTypeId *elements;
        int changed;

        if (source.data.tuple_type.element_count != 0u
            && source.data.tuple_type.elements == NULL) {
            return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_TYPE,
                source_id, cm_hir_def_id_none(),
                CM_HIR_GENERIC_PARAM_NONE, CM_HIR_INVALID_ID);
        }
        elements = source.data.tuple_type.element_count == 0u ? NULL
            : (CmHirTypeId *)cm_alloc_zeroed(
                (size_t)source.data.tuple_type.element_count,
                sizeof(*elements));
        changed = 0;
        for (index = 0u; index < source.data.tuple_type.element_count;
             ++index) {
            if (!cm_alias_normalize_type(state,
                    source.data.tuple_type.elements[index],
                    &elements[index])) {
                cm_free(elements);
                return 0;
            }
            if (elements[index] != source.data.tuple_type.elements[index]) {
                changed = 1;
            }
        }
        if (!changed) {
            cm_free(elements);
            *out_type = source_id;
            return 1;
        }
        replacement.data.tuple_type.elements = elements;
        if (!cm_alias_add_type(state, source_id, &replacement, out_type)) {
            cm_free(elements);
            return 0;
        }
        cm_free(elements);
        return 1;
    }
    case CM_HIR_TYPE_ARRAY_KIND:
        if (source.data.array_type.length.kind == CM_HIR_CONST_PARAMETER) {
            const CmHirGenericParam *parameter;

            parameter = cm_hir_get_generic_param(state->context,
                source.data.array_type.length.data.parameter);
            if (parameter == NULL
                || parameter->kind != CM_HIR_GENERIC_CONST) {
                return cm_alias_fail(state,
                    CM_HIR_TYPE_ALIAS_INVALID_TYPE, source_id,
                    cm_hir_def_id_none(),
                    source.data.array_type.length.data.parameter,
                    CM_HIR_INVALID_ID);
            }
            if (state->frame != NULL) {
                return cm_alias_fail(state,
                    CM_HIR_TYPE_ALIAS_UNSUPPORTED_CONST, source_id,
                    state->frame->definition,
                    source.data.array_type.length.data.parameter,
                    CM_HIR_OK);
            }
        } else if (source.data.array_type.length.kind != CM_HIR_CONST_VALUE
            && source.data.array_type.length.kind
                != CM_HIR_CONST_UNEVALUATED) {
            return cm_alias_fail(state,
                CM_HIR_TYPE_ALIAS_UNSUPPORTED_CONST, source_id,
                cm_hir_def_id_none(), CM_HIR_GENERIC_PARAM_NONE,
                CM_HIR_OK);
        }
        if (!cm_alias_normalize_type(state,
                source.data.array_type.element, &child)) {
            return 0;
        }
        replacement.data.array_type.element = child;
        if (!cm_alias_normalize_type(state,
                source.data.array_type.length.type,
                &replacement.data.array_type.length.type)) {
            return 0;
        }
        if (replacement.data.array_type.element ==
                source.data.array_type.element
            && replacement.data.array_type.length.type ==
                source.data.array_type.length.type) {
            *out_type = source_id;
            return 1;
        }
        return cm_alias_add_type(state, source_id, &replacement, out_type);
    case CM_HIR_TYPE_SLICE_KIND:
        if (!cm_alias_normalize_type(state,
                source.data.slice_type.element, &child)) {
            return 0;
        }
        if (child == source.data.slice_type.element) {
            *out_type = source_id;
            return 1;
        }
        replacement.data.slice_type.element = child;
        return cm_alias_add_type(state, source_id, &replacement, out_type);
    case CM_HIR_TYPE_FN_POINTER_KIND:
    {
        CmHirTypeId *parameters;
        int changed;

        if (source.data.fn_pointer_type.parameter_count != 0u
            && source.data.fn_pointer_type.parameters == NULL) {
            return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_TYPE,
                source_id, cm_hir_def_id_none(),
                CM_HIR_GENERIC_PARAM_NONE, CM_HIR_INVALID_ID);
        }
        parameters = source.data.fn_pointer_type.parameter_count == 0u ? NULL
            : (CmHirTypeId *)cm_alloc_zeroed(
                (size_t)source.data.fn_pointer_type.parameter_count,
                sizeof(*parameters));
        changed = 0;
        for (index = 0u;
             index < source.data.fn_pointer_type.parameter_count; ++index) {
            if (!cm_alias_normalize_type(state,
                    source.data.fn_pointer_type.parameters[index],
                    &parameters[index])) {
                cm_free(parameters);
                return 0;
            }
            if (parameters[index] !=
                    source.data.fn_pointer_type.parameters[index]) {
                changed = 1;
            }
        }
        if (!cm_alias_normalize_type(state,
                source.data.fn_pointer_type.return_type, &child)) {
            cm_free(parameters);
            return 0;
        }
        if (child != source.data.fn_pointer_type.return_type) changed = 1;
        if (!changed) {
            cm_free(parameters);
            *out_type = source_id;
            return 1;
        }
        replacement.data.fn_pointer_type.parameters = parameters;
        replacement.data.fn_pointer_type.return_type = child;
        if (!cm_alias_add_type(state, source_id, &replacement, out_type)) {
            cm_free(parameters);
            return 0;
        }
        cm_free(parameters);
        return 1;
    }
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
    case CM_HIR_TYPE_ADT_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
        return cm_alias_normalize_nominal(state, source_id, &source,
            out_type);
    case CM_HIR_TYPE_CLOSURE_KIND:
        *out_type = source_id;
        return 1;
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
        return cm_alias_expand(state, source_id, &source, out_type);
    case CM_HIR_TYPE_PARAMETER_KIND:
    {
        const CmHirGenericParam *parameter;
        const CmHirGenericArg *argument;

        parameter = cm_hir_get_generic_param(state->context,
            source.data.parameter_type.parameter);
        if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_TYPE) {
            return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_TYPE,
                source_id, cm_hir_def_id_none(),
                source.data.parameter_type.parameter, CM_HIR_INVALID_ID);
        }
        argument = cm_alias_find_argument(state,
            source.data.parameter_type.parameter);
        if (argument != NULL) {
            if (argument->kind != CM_HIR_GENERIC_ARG_TYPE) {
                return cm_alias_fail(state,
                    CM_HIR_TYPE_ALIAS_ARGUMENT_KIND, source_id,
                    parameter->owner, source.data.parameter_type.parameter,
                    CM_HIR_INVARIANT_VIOLATION);
            }
            *out_type = argument->data.type;
            return 1;
        }
        if (state->frame != NULL) {
            return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_ALIAS,
                source_id, state->frame->definition,
                source.data.parameter_type.parameter,
                CM_HIR_INVARIANT_VIOLATION);
        }
        *out_type = source_id;
        return 1;
    }
    case CM_HIR_TYPE_PROJECTION_KIND:
        return cm_alias_normalize_projection(state, source_id, &source,
            out_type);
    case CM_HIR_TYPE_DYN_TRAIT_KIND:
        return cm_alias_normalize_dyn_trait(state, source_id, &source,
            out_type);
    case CM_HIR_TYPE_OPAQUE_KIND:
        return cm_alias_fail(state,
            CM_HIR_TYPE_ALIAS_UNSUPPORTED_OPAQUE, source_id,
            source.data.named_type.definition, CM_HIR_GENERIC_PARAM_NONE,
            CM_HIR_OK);
    }
    return cm_alias_fail(state, CM_HIR_TYPE_ALIAS_INVALID_TYPE, source_id,
        cm_hir_def_id_none(), CM_HIR_GENERIC_PARAM_NONE,
        CM_HIR_INVALID_ARGUMENT);
}

static CmHirTypeAliasResult cm_alias_run(CmAliasNormalizeState *state,
    CmHirTypeId root, CmArenaMark arena_mark, size_t initial_type_count)
{
    if (state->result.status == CM_HIR_TYPE_ALIAS_OK
        && !cm_alias_normalize_type(state, root, &state->result.type)) {
        state->result.type = CM_HIR_TYPE_NONE;
    }
    if (state->result.status != CM_HIR_TYPE_ALIAS_OK) {
        cm_vec_resize(&state->context->types, initial_type_count);
        cm_arena_rewind(&state->context->storage, arena_mark);
        cm_arena_discard_mark(&state->context->storage, arena_mark);
        state->result.type = CM_HIR_TYPE_NONE;
        state->result.allocated_type_count = 0u;
    } else {
        state->result.allocated_type_count = state->context->types.len
            - initial_type_count;
        cm_arena_discard_mark(&state->context->storage, arena_mark);
    }
    cm_vec_destroy(&state->active_aliases);
    return state->result;
}

CmHirTypeAliasResult cm_hir_normalize_type_aliases(CmHirContext *context,
    CmHirTypeId root)
{
    CmAliasNormalizeState state;
    CmArenaMark arena_mark;
    size_t initial_type_count;

    cm_alias_state_init(&state, context);
    if (context == NULL || root == CM_HIR_TYPE_NONE) {
        state.result.status = CM_HIR_TYPE_ALIAS_INVALID_ARGUMENT;
        state.result.hir_status = CM_HIR_INVALID_ARGUMENT;
        cm_vec_destroy(&state.active_aliases);
        return state.result;
    }
    if (cm_hir_get_type(context, root) == NULL) {
        state.result.status = CM_HIR_TYPE_ALIAS_INVALID_TYPE;
        state.result.source_type = root;
        state.result.hir_status = CM_HIR_INVALID_ID;
        cm_vec_destroy(&state.active_aliases);
        return state.result;
    }
    initial_type_count = context->types.len;
    arena_mark = cm_arena_mark(&context->storage);
    return cm_alias_run(&state, root, arena_mark, initial_type_count);
}

CmHirTypeAliasResult cm_hir_instantiate_type(CmHirContext *context,
    CmHirTypeId root, CmHirDefId owner_definition,
    const CmHirGenericArg *arguments, uint32_t argument_count)
{
    CmAliasNormalizeState state;
    CmAliasFrame frame;
    CmArenaMark arena_mark;
    size_t initial_type_count;

    cm_alias_state_init(&state, context);
    if (context == NULL || root == CM_HIR_TYPE_NONE
        || cm_hir_def_id_is_none(owner_definition)) {
        state.result.status = CM_HIR_TYPE_ALIAS_INVALID_ARGUMENT;
        state.result.hir_status = CM_HIR_INVALID_ARGUMENT;
        cm_vec_destroy(&state.active_aliases);
        return state.result;
    }
    if (cm_hir_get_type(context, root) == NULL) {
        state.result.status = CM_HIR_TYPE_ALIAS_INVALID_TYPE;
        state.result.source_type = root;
        state.result.hir_status = CM_HIR_INVALID_ID;
        cm_vec_destroy(&state.active_aliases);
        return state.result;
    }
    initial_type_count = context->types.len;
    arena_mark = cm_arena_mark(&context->storage);
    if (cm_alias_validate_instantiation(&state, root, owner_definition,
            arguments, argument_count, &frame)) {
        state.frame = &frame;
    }
    return cm_alias_run(&state, root, arena_mark, initial_type_count);
}

const char *cm_hir_type_alias_status_name(CmHirTypeAliasStatus status)
{
    switch (status) {
    case CM_HIR_TYPE_ALIAS_OK: return "ok";
    case CM_HIR_TYPE_ALIAS_INVALID_ARGUMENT: return "invalid argument";
    case CM_HIR_TYPE_ALIAS_INVALID_TYPE: return "invalid type";
    case CM_HIR_TYPE_ALIAS_INVALID_ALIAS: return "invalid alias";
    case CM_HIR_TYPE_ALIAS_ARGUMENT_COUNT: return "argument count mismatch";
    case CM_HIR_TYPE_ALIAS_ARGUMENT_KIND: return "argument kind mismatch";
    case CM_HIR_TYPE_ALIAS_CYCLE: return "alias cycle";
    case CM_HIR_TYPE_ALIAS_UNSUPPORTED_CONST: return "unsupported const";
    case CM_HIR_TYPE_ALIAS_UNSUPPORTED_DYN_TRAIT:
        return "unsupported dyn trait";
    case CM_HIR_TYPE_ALIAS_UNSUPPORTED_OPAQUE:
        return "unsupported opaque type";
    case CM_HIR_TYPE_ALIAS_RECURSION_LIMIT:
        return "type recursion limit exceeded";
    case CM_HIR_TYPE_ALIAS_HIR_FAILURE: return "HIR failure";
    }
    return "unknown alias normalization status";
}
