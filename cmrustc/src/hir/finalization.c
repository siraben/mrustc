#include "cm/hir/finalization.h"

#include "cm/alloc.h"

#include <string.h>

typedef struct CmHirCrateFinalizationState {
    const CmHirContext *hir;
    CmHirCrateId local_crate;
    uint64_t storage_lifetime_id;
    uint64_t semantic_generation;
    uint64_t rewind_generation;
    size_t crate_count;
    size_t module_count;
    size_t item_count;
    size_t body_count;
    size_t closure_count;
    size_t expression_count;
    size_t type_count;
    size_t generic_parameter_count;
    size_t definition_count;
    size_t prebound_associated_type_count;
} CmHirCrateFinalizationState;

static const CmHirCrateFinalizationState *cm_finalization_state(
    const CmHirCrateFinalization *finalization)
{
    return finalization == NULL ? NULL
        : (const CmHirCrateFinalizationState *)finalization->state;
}

static int cm_finalization_current(
    const CmHirCrateFinalizationState *state)
{
    const CmHirContext *hir;

    if (state == NULL || state->hir == NULL) return 0;
    hir = state->hir;
    return hir->storage.lifetime_id == state->storage_lifetime_id
        && hir->semantic_generation == state->semantic_generation
        && hir->rewind_generation == state->rewind_generation
        && hir->crates.len == state->crate_count
        && hir->modules.len == state->module_count
        && hir->items.len == state->item_count
        && hir->bodies.len == state->body_count
        && hir->closures.len == state->closure_count
        && hir->expressions.len == state->expression_count
        && hir->types.len == state->type_count
        && hir->generic_parameters.len
            == state->generic_parameter_count
        && hir->definitions.len == state->definition_count
        && hir->prebound_associated_types.len
            == state->prebound_associated_type_count;
}

static int cm_finalization_definition_is_exact(
    const CmHirContext *hir, CmHirDefId id, CmHirDefinitionKind kind)
{
    const CmHirDefinition *definition;

    definition = cm_hir_lookup_definition(hir, id);
    return definition != NULL && definition->kind == kind
        && definition->state == CM_HIR_DEFINITION_BOUND;
}

static int cm_finalization_local_module_valid(const CmHirContext *hir,
    CmHirCrateId local_crate, CmHirModuleId module_id,
    const CmHirModule *module, CmHirModuleId root_module)
{
    const CmHirDefinition *definition;
    const CmHirModule *parent;

    if (module == NULL || module->crate_id != local_crate
        || module->definition.crate_id != local_crate
        || !cm_finalization_definition_is_exact(hir,
            module->definition, CM_HIR_DEFINITION_MODULE)) return 0;
    definition = cm_hir_lookup_definition(hir, module->definition);
    if (definition->entity.module_id != module_id) return 0;
    if (module_id == root_module) {
        return module->parent == CM_HIR_MODULE_NONE;
    }
    parent = cm_hir_get_module(hir, module->parent);
    return parent != NULL && parent->crate_id == local_crate;
}

static int cm_finalization_item_body_valid(const CmHirContext *hir,
    const CmHirItem *item, CmHirBodyId body_id)
{
    const CmHirBody *body;

    if (body_id == CM_HIR_BODY_NONE) return 1;
    body = cm_hir_get_body(hir, body_id);
    return body != NULL
        && body->origin.kind == CM_HIR_BODY_ORIGIN_ITEM_SOURCE
        && cm_hir_def_id_equal(body->owner, item->definition)
        && cm_hir_def_id_equal(body->origin.definition, item->definition)
        && cm_hir_def_id_equal(body->origin.enclosing_definition,
            item->definition)
        && cm_hir_def_id_equal(
            body->origin.data.item_source.item_definition,
            item->definition);
}

static int cm_finalization_local_item_valid(const CmHirContext *hir,
    CmHirCrateId local_crate, CmHirItemId item_id,
    const CmHirItem *item)
{
    const CmHirDefinition *definition;
    const CmHirModule *module;
    const CmHirDefinition *parent;
    uint32_t generic_index;

    if (item == NULL || item->definition.crate_id != local_crate
        || !cm_finalization_definition_is_exact(hir,
            item->definition, CM_HIR_DEFINITION_ITEM)) return 0;
    definition = cm_hir_lookup_definition(hir, item->definition);
    if (definition->entity.item_id != item_id) return 0;
    module = cm_hir_get_module(hir, item->owner_module);
    if (module == NULL || module->crate_id != local_crate) return 0;
    if (!cm_hir_def_id_is_none(item->parent_definition)) {
        parent = cm_hir_lookup_definition(hir, item->parent_definition);
        if (parent == NULL || parent->id.crate_id != local_crate
            || parent->kind != CM_HIR_DEFINITION_ITEM
            || parent->state != CM_HIR_DEFINITION_BOUND) return 0;
    }
    if (item->generic_parameter_count != 0u
        && (item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE
            || (size_t)item->generic_parameter_start
                + (size_t)item->generic_parameter_count - 1u
                > hir->generic_parameters.len)) return 0;
    for (generic_index = 0u;
         generic_index < item->generic_parameter_count; ++generic_index) {
        const CmHirGenericParam *parameter;

        parameter = cm_hir_get_generic_param(hir,
            item->generic_parameter_start + generic_index);
        if (parameter == NULL
            || !cm_hir_def_id_equal(parameter->owner, item->definition)
            || parameter->index != generic_index) return 0;
    }
    if (item->kind == CM_HIR_ITEM_FUNCTION) {
        return cm_finalization_item_body_valid(hir, item,
            item->data.function_item.body);
    }
    if (item->kind == CM_HIR_ITEM_CONST
        || item->kind == CM_HIR_ITEM_STATIC) {
        return cm_finalization_item_body_valid(hir, item,
            item->data.value_item.body);
    }
    return 1;
}

static int cm_finalization_local_definition_valid(
    const CmHirContext *hir, CmHirCrateId local_crate,
    const CmHirDefinition *definition)
{
    const CmHirModule *module;
    const CmHirItem *item;
    const CmHirVariant *variant;

    if (definition->id.crate_id != local_crate) return 1;
    if (definition->state != CM_HIR_DEFINITION_BOUND) return 0;
    switch (definition->kind) {
    case CM_HIR_DEFINITION_MODULE:
        module = cm_hir_get_module(hir, definition->entity.module_id);
        return module != NULL && module->crate_id == local_crate
            && cm_hir_def_id_equal(module->definition, definition->id);
    case CM_HIR_DEFINITION_ITEM:
        item = cm_hir_get_item(hir, definition->entity.item_id);
        return item != NULL
            && cm_hir_def_id_equal(item->definition, definition->id)
            && cm_finalization_local_item_valid(hir, local_crate,
                definition->entity.item_id, item);
    case CM_HIR_DEFINITION_ENUM_VARIANT:
        item = cm_hir_get_item(hir,
            definition->entity.enum_variant.enum_item_id);
        if (item == NULL || item->kind != CM_HIR_ITEM_ENUM
            || item->definition.crate_id != local_crate
            || definition->entity.enum_variant.variant_index
                >= item->data.enum_item.variant_count) return 0;
        variant = &item->data.enum_item.variants[
            definition->entity.enum_variant.variant_index];
        return cm_hir_def_id_equal(variant->definition, definition->id);
    case CM_HIR_DEFINITION_MACRO:
        module = cm_hir_get_module(hir,
            definition->entity.macro_definition.owner_module);
        return module != NULL && module->crate_id == local_crate;
    }
    return 0;
}

static int cm_finalization_local_crate_valid(const CmHirContext *hir,
    CmHirCrateId local_crate)
{
    const CmHirCrate *crate_value;
    const CmHirModule *root;
    size_t index;

    crate_value = cm_hir_get_crate(hir, local_crate);
    if (crate_value == NULL || crate_value->root_module == CM_HIR_MODULE_NONE) {
        return 0;
    }
    root = cm_hir_get_module(hir, crate_value->root_module);
    if (root == NULL || root->crate_id != local_crate
        || root->parent != CM_HIR_MODULE_NONE) return 0;
    for (index = 0u; index < hir->definitions.len; ++index) {
        const CmHirDefinition *definition;

        definition = (const CmHirDefinition *)cm_vec_at_const(
            &hir->definitions, index);
        if (!cm_finalization_local_definition_valid(hir, local_crate,
                definition)) return 0;
    }
    for (index = 0u; index < hir->modules.len; ++index) {
        const CmHirModule *module;

        module = cm_hir_get_module(hir, (CmHirModuleId)(index + 1u));
        if (module != NULL && module->crate_id == local_crate
            && !cm_finalization_local_module_valid(hir, local_crate,
                (CmHirModuleId)(index + 1u), module,
                crate_value->root_module)) return 0;
    }
    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *item;
        const CmHirModule *module;

        item = cm_hir_get_item(hir, (CmHirItemId)(index + 1u));
        if (item == NULL) return 0;
        module = cm_hir_get_module(hir, item->owner_module);
        if (module != NULL && module->crate_id == local_crate
            && !cm_finalization_local_item_valid(hir, local_crate,
                (CmHirItemId)(index + 1u), item)) return 0;
    }
    for (index = 0u; index < hir->bodies.len; ++index) {
        const CmHirBody *body;
        const CmHirDefinition *owner;
        const CmHirItem *item;
        CmHirBodyId body_id;
        CmHirBodyId expected;

        body_id = (CmHirBodyId)(index + 1u);
        body = cm_hir_get_body(hir, body_id);
        if (body == NULL || body->owner.crate_id != local_crate) continue;
        if (body->origin.kind != CM_HIR_BODY_ORIGIN_ITEM_SOURCE
            || !cm_hir_def_id_equal(body->origin.definition, body->owner)
            || !cm_hir_def_id_equal(body->origin.enclosing_definition,
                body->owner)
            || !cm_hir_def_id_equal(
                body->origin.data.item_source.item_definition,
                body->owner)) return 0;
        owner = cm_hir_lookup_definition(hir, body->owner);
        if (owner == NULL || owner->kind != CM_HIR_DEFINITION_ITEM
            || owner->state != CM_HIR_DEFINITION_BOUND) return 0;
        item = cm_hir_get_item(hir, owner->entity.item_id);
        if (item == NULL) return 0;
        expected = CM_HIR_BODY_NONE;
        if (item->kind == CM_HIR_ITEM_FUNCTION) {
            expected = item->data.function_item.body;
        } else if (item->kind == CM_HIR_ITEM_CONST
                || item->kind == CM_HIR_ITEM_STATIC) {
            expected = item->data.value_item.body;
        }
        if (expected != body_id) return 0;
    }
    for (index = 0u; index < hir->closures.len; ++index) {
        const CmHirClosure *closure;
        const CmHirBody *body;
        const CmHirExpr *closure_root;
        const CmHirType *return_type;
        uint32_t parameter_index;

        closure = cm_hir_get_closure(hir, (CmHirClosureId)(index + 1u));
        body = closure == NULL ? NULL
            : cm_hir_get_body(hir, closure->owner_body);
        closure_root = closure == NULL ? NULL
            : cm_hir_get_expr(hir, closure->body_expression);
        return_type = closure == NULL ? NULL
            : cm_hir_get_type(hir, closure->return_type);
        if (closure == NULL || body == NULL
            || body->owner.crate_id != local_crate) continue;
        if (closure->state != CM_HIR_CLOSURE_BODY_BOUND
            || closure->source_expression_id == 0u
            || closure_root == NULL
            || closure_root->owner_body != closure->owner_body
            || closure_root->type != closure->return_type
            || return_type == NULL
            || closure->visible_local_count > body->local_count
            || closure->span.source != body->source
            || closure->span.start < body->span.start
            || closure->span.end > body->span.end
            || closure_root->span.source != closure->span.source
            || closure_root->span.start < closure->span.start
            || closure_root->span.end > closure->span.end
            || (closure->parameter_count == 0u)
                != (closure->parameters == NULL)
            || (closure->is_move != 0 && closure->is_move != 1)) return 0;
        for (parameter_index = 0u;
             parameter_index < closure->parameter_count;
             ++parameter_index) {
            const CmHirClosureParam *parameter;

            parameter = &closure->parameters[parameter_index];
            if (cm_hir_get_type(hir, parameter->type) == NULL
                || parameter->span.source != closure->span.source
                || parameter->span.start < closure->span.start
                || parameter->span.end > closure->span.end
                || (parameter->binding_kind != CM_HIR_BINDING_NAMED
                    && parameter->binding_kind
                        != CM_HIR_BINDING_DISCARD)) return 0;
        }
    }
    for (index = 0u; index < hir->generic_parameters.len; ++index) {
        const CmHirGenericParam *parameter;
        const CmHirDefinition *owner;
        const CmHirItem *item;
        CmHirGenericParamId parameter_id;

        parameter_id = (CmHirGenericParamId)(index + 1u);
        parameter = cm_hir_get_generic_param(hir, parameter_id);
        if (parameter == NULL || parameter->owner.crate_id != local_crate) {
            continue;
        }
        owner = cm_hir_lookup_definition(hir, parameter->owner);
        if (owner == NULL || owner->kind != CM_HIR_DEFINITION_ITEM
            || owner->state != CM_HIR_DEFINITION_BOUND) return 0;
        item = cm_hir_get_item(hir, owner->entity.item_id);
        if (item == NULL || parameter->index >= item->generic_parameter_count
            || item->generic_parameter_start + parameter->index
                != parameter_id) return 0;
    }
    return 1;
}

CmHirStatus cm_hir_crate_finalization_init(
    CmHirCrateFinalization *finalization, const CmHirContext *hir,
    CmHirCrateId local_crate)
{
    CmHirCrateFinalizationState *state;

    if (finalization == NULL || finalization->state != NULL || hir == NULL
        || local_crate == CM_HIR_CRATE_NONE) return CM_HIR_INVALID_ARGUMENT;
    if (!cm_finalization_local_crate_valid(hir, local_crate)) {
        return cm_hir_get_crate(hir, local_crate) == NULL
            ? CM_HIR_INVALID_ID : CM_HIR_INVARIANT_VIOLATION;
    }
    state = (CmHirCrateFinalizationState *)cm_alloc_zeroed(1u,
        sizeof(CmHirCrateFinalizationState));
    state->hir = hir;
    state->local_crate = local_crate;
    state->storage_lifetime_id = hir->storage.lifetime_id;
    state->semantic_generation = hir->semantic_generation;
    state->rewind_generation = hir->rewind_generation;
    state->crate_count = hir->crates.len;
    state->module_count = hir->modules.len;
    state->item_count = hir->items.len;
    state->body_count = hir->bodies.len;
    state->closure_count = hir->closures.len;
    state->expression_count = hir->expressions.len;
    state->type_count = hir->types.len;
    state->generic_parameter_count = hir->generic_parameters.len;
    state->definition_count = hir->definitions.len;
    state->prebound_associated_type_count =
        hir->prebound_associated_types.len;
    finalization->state = state;
    return CM_HIR_OK;
}

void cm_hir_crate_finalization_destroy(
    CmHirCrateFinalization *finalization)
{
    CmHirCrateFinalizationState *state;

    if (finalization == NULL || finalization->state == NULL) return;
    state = (CmHirCrateFinalizationState *)finalization->state;
    finalization->state = NULL;
    memset(state, 0, sizeof(*state));
    cm_free(state);
}

int cm_hir_crate_finalization_is_current(
    const CmHirCrateFinalization *finalization)
{
    return cm_finalization_current(cm_finalization_state(finalization));
}

const CmHirContext *cm_hir_crate_finalization_hir(
    const CmHirCrateFinalization *finalization)
{
    const CmHirCrateFinalizationState *state;

    state = cm_finalization_state(finalization);
    return !cm_finalization_current(state) ? NULL : state->hir;
}

CmHirCrateId cm_hir_crate_finalization_crate(
    const CmHirCrateFinalization *finalization)
{
    const CmHirCrateFinalizationState *state;

    state = cm_finalization_state(finalization);
    return !cm_finalization_current(state)
        ? CM_HIR_CRATE_NONE : state->local_crate;
}

uint64_t cm_hir_crate_finalization_generation(
    const CmHirCrateFinalization *finalization)
{
    const CmHirCrateFinalizationState *state;

    state = cm_finalization_state(finalization);
    return !cm_finalization_current(state) ? UINT64_C(0)
        : state->semantic_generation;
}
