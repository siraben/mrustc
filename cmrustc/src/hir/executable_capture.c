#include "cm/hir/executable_capture.h"

#include "cm/alloc.h"
#include "cm/sha256.h"
#include "admission_authority_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct CmExecCaptureItemRef {
    const CmHirItem *item;
    const CmInternedString *name;
    CmHirItemId id;
    uint8_t value_kind;
} CmExecCaptureItemRef;

typedef struct CmExecCaptureState {
    const CmHirExecutableCaptureInput *input;
    const CmHirContext *hir;
    const CmHirCrate *crate_value;
    const CmHirModule *root;
    CmExecCaptureItemRef *traits;
    size_t trait_count;
    CmExecCaptureItemRef *impls;
    size_t impl_count;
    CmExecCaptureItemRef *values;
    size_t value_count;
    uint32_t primitive_mask;
} CmExecCaptureState;

static uint32_t cm_capture_popcount(uint32_t value)
{
    uint32_t count = 0u;
    while (value != 0u) {
        count += value & UINT32_C(1);
        value >>= 1u;
    }
    return count;
}

static CmHirExecutableCaptureResult cm_capture_result(
    CmHirExecutableCaptureStatus status)
{
    CmHirExecutableCaptureResult result;
    memset(&result, 0, sizeof(result));
    result.status = status;
    result.metadata_status = CM_HIR_EXEC_METADATA_OK;
    return result;
}

static int cm_capture_string_equal(const CmInternedString *value,
    const char *text)
{
    size_t length = strlen(text);
    return value != NULL && value->len == length
        && memcmp(value->bytes, text, length) == 0;
}

static int cm_capture_crate_envelope(const CmHirContext *hir,
    const CmHirCrate *crate_value)
{
    int feature = 0;
    int no_core = 0;
    int no_main = 0;
    uint32_t index;
    if (crate_value == NULL || crate_value->inner_attribute_count != 3u
        || crate_value->inner_attributes == NULL) return 0;
    for (index = 0u; index < 3u; ++index) {
        const CmInternedString *metadata = cm_interner_get(&hir->strings,
            crate_value->inner_attributes[index].metadata);
        if (cm_capture_string_equal(metadata, "feature(no_core)")) {
            if (feature) return 0;
            feature = 1;
        } else if (cm_capture_string_equal(metadata, "no_core")) {
            if (no_core) return 0;
            no_core = 1;
        } else if (cm_capture_string_equal(metadata, "no_main")) {
            if (no_main) return 0;
            no_main = 1;
        } else return 0;
    }
    return feature && no_core && no_main;
}

static int cm_capture_string_compare(const CmInternedString *left,
    const CmInternedString *right)
{
    size_t common;
    int order;
    if (left == NULL || right == NULL) return left == right ? 0 : left ? 1 : -1;
    common = left->len < right->len ? left->len : right->len;
    order = common == 0u ? 0 : memcmp(left->bytes, right->bytes, common);
    if (order != 0) return order;
    return left->len < right->len ? -1 : left->len > right->len;
}

static int cm_capture_ref_name_compare(const void *left, const void *right)
{
    const CmExecCaptureItemRef *a = (const CmExecCaptureItemRef *)left;
    const CmExecCaptureItemRef *b = (const CmExecCaptureItemRef *)right;
    int order = cm_capture_string_compare(a->name, b->name);
    if (order != 0) return order;
    if (a->item->span.start != b->item->span.start)
        return a->item->span.start < b->item->span.start ? -1 : 1;
    return a->id < b->id ? -1 : a->id > b->id;
}

static int cm_capture_impl_compare(const void *left, const void *right)
{
    const CmExecCaptureItemRef *a = (const CmExecCaptureItemRef *)left;
    const CmExecCaptureItemRef *b = (const CmExecCaptureItemRef *)right;
    if (a->item->span.start != b->item->span.start)
        return a->item->span.start < b->item->span.start ? -1 : 1;
    return a->id < b->id ? -1 : a->id > b->id;
}

static int cm_capture_copy_bytes(CmHirExecutableString *out,
    const void *bytes, size_t length)
{
    out->data = length == 0u ? NULL : (unsigned char *)cm_alloc(length);
    out->length = length;
    if (length != 0u) memcpy(out->data, bytes, length);
    return 1;
}

static int cm_capture_copy_intern(CmHirExecutableString *out,
    const CmInternedString *value)
{
    return value != NULL
        && cm_capture_copy_bytes(out, value->bytes, value->len);
}

static int cm_capture_attributes_exact_no_mangle(
    const CmHirContext *hir, const CmHirItem *item)
{
    const CmInternedString *name;
    if (item->attribute_count != 1u || item->attributes == NULL) return 0;
    name = cm_interner_get(&hir->strings, item->attributes[0].metadata);
    return cm_capture_string_equal(name, "no_mangle");
}

static int cm_capture_plain_item(const CmHirItem *item)
{
    return item->generic_parameter_count == 0u
        && item->predicate_scope_count == 0u
        && item->predicate_count == 0u
        && item->outlives_predicate_count == 0u
        && cm_hir_def_id_is_none(item->parent_definition)
        && !item->is_specializable;
}

static int cm_capture_function_common(const CmHirItem *item)
{
    const CmHirFunctionSignature *signature =
        &item->data.function_item.signature;
    uint32_t index;
    if (!(item->visibility.kind == CM_HIR_VIS_PUBLIC
        && cm_hir_def_id_is_none(item->visibility.restriction)
        && signature->receiver == CM_HIR_RECEIVER_NONE
        && signature->safety == CM_HIR_SAFE
        && !signature->is_const && !signature->is_async
        && !signature->is_variadic
        && !item->data.function_item.has_default_body
        && cm_hir_def_id_is_none(
            item->data.function_item.trait_item_definition)
        && signature->parameter_count != 0u
        && signature->parameter_count <= CM_HIR_EXEC_METADATA_MAX_PARAMETERS
        && signature->parameters != NULL)) return 0;
    for (index = 0u; index < signature->parameter_count; ++index) {
        if (signature->parameters[index].binding_kind != CM_HIR_BINDING_NAMED
            || signature->parameters[index].binding_mode
                != CM_HIR_PARAMETER_BINDING_MOVE) return 0;
    }
    return 1;
}

static int cm_capture_same_owner_parameter(const CmHirContext *hir,
    CmHirTypeId left_id, CmHirTypeId right_id, CmHirDefId owner)
{
    const CmHirType *left = cm_hir_get_type(hir, left_id);
    const CmHirType *right = cm_hir_get_type(hir, right_id);
    const CmHirGenericParam *parameter;
    if (left == NULL || right == NULL
        || left->kind != CM_HIR_TYPE_PARAMETER_KIND
        || right->kind != CM_HIR_TYPE_PARAMETER_KIND
        || left->data.parameter_type.parameter
            != right->data.parameter_type.parameter) return 0;
    parameter = cm_hir_get_generic_param(hir,
        left->data.parameter_type.parameter);
    return parameter != NULL && parameter->kind == CM_HIR_GENERIC_TYPE
        && parameter->index == 0u
        && cm_hir_def_id_equal(parameter->owner, owner);
}

static int cm_capture_recipe_shape(const CmExecCaptureState *state,
    const CmHirItem *item, uint32_t *out_parameter_index)
{
    const CmHirFunctionSignature *signature =
        &item->data.function_item.signature;
    const CmInternedString *abi;
    const CmHirGenericParam *generic;
    const CmHirBody *body;
    const CmHirExpr *root;
    const CmHirExpr *tail;
    uint32_t local_index;
    uint32_t index;
    if (!cm_capture_function_common(item) || item->attribute_count != 0u
        || item->generic_parameter_count != 1u
        || item->predicate_count == 0u
        || item->predicate_scope_count != 0u
        || item->outlives_predicate_count != 0u) return 0;
    abi = cm_interner_get(&state->hir->strings, signature->abi);
    if (!cm_capture_string_equal(abi, "Rust")) return 0;
    generic = cm_hir_get_generic_param(state->hir,
        item->generic_parameter_start);
    if (generic == NULL || generic->kind != CM_HIR_GENERIC_TYPE
        || !cm_hir_def_id_equal(generic->owner, item->definition)
        || generic->index != 0u || generic->is_relaxed_sized
        || generic->has_default
        || cm_interner_get(&state->hir->strings, generic->name) == NULL)
        return 0;
    for (index = 0u; index < item->predicate_count; ++index) {
        const CmHirTraitPredicate *predicate = &item->predicates[index];
        const CmHirType *subject = cm_hir_get_type(state->hir,
            predicate->subject);
        if (subject == NULL || subject->kind != CM_HIR_TYPE_PARAMETER_KIND
            || subject->data.parameter_type.parameter
                != item->generic_parameter_start
            || predicate->trait_type.definition.crate_id
                != state->input->crate_id
            || predicate->trait_type.argument_count != 0u
            || predicate->trait_type.arguments != NULL
            || predicate->equality_count != 0u
            || predicate->equalities != NULL
            || predicate->scope != CM_HIR_PREDICATE_SCOPE_NONE
            || predicate->binder.lifetime_count != 0u
            || predicate->binder.lifetimes != NULL
            || predicate->modifier != CM_HIR_PREDICATE_REQUIRED)
            return 0;
    }
    body = cm_hir_get_body(state->hir,
        item->data.function_item.body);
    if (body == NULL || body->state != CM_HIR_BODY_TYPED
        || !cm_hir_def_id_equal(body->owner, item->definition)
        || body->origin.kind != CM_HIR_BODY_ORIGIN_ITEM_SOURCE
        || body->expected_type != signature->return_type
        || body->parameter_count != signature->parameter_count
        || body->root_expression == CM_HIR_EXPR_NONE) return 0;
    root = cm_hir_get_expr(state->hir, body->root_expression);
    if (root == NULL || root->owner_body != item->data.function_item.body
        || root->kind != CM_HIR_EXPR_BLOCK
        || root->data.block.statement_count != 0u
        || root->data.block.statements != NULL
        || root->data.block.tail_expression == CM_HIR_EXPR_NONE
        || root->type != signature->return_type) return 0;
    tail = cm_hir_get_expr(state->hir, root->data.block.tail_expression);
    if (tail == NULL || tail->owner_body != item->data.function_item.body
        || tail->kind != CM_HIR_EXPR_LOCAL
        || tail->type != signature->return_type) return 0;
    local_index = tail->data.local.local_index;
    if (local_index >= body->local_count
        || body->locals[local_index].parameter_index
            == CM_HIR_PARAMETER_INDEX_NONE
        || body->locals[local_index].parameter_index
            >= signature->parameter_count) return 0;
    *out_parameter_index = body->locals[local_index].parameter_index;
    return cm_capture_same_owner_parameter(state->hir,
        signature->parameters[*out_parameter_index].type,
        signature->return_type, item->definition);
}

static int cm_capture_native_shape(const CmExecCaptureState *state,
    const CmHirItem *item)
{
    const CmHirFunctionSignature *signature =
        &item->data.function_item.signature;
    const CmInternedString *abi;
    if (!cm_capture_function_common(item) || !cm_capture_plain_item(item)
        || !cm_capture_attributes_exact_no_mangle(state->hir, item)) return 0;
    abi = cm_interner_get(&state->hir->strings, signature->abi);
    return cm_capture_string_equal(abi, "C");
}

static int cm_capture_trait_shape(const CmHirItem *item)
{
    return cm_capture_plain_item(item)
        && item->visibility.kind == CM_HIR_VIS_PUBLIC
        && cm_hir_def_id_is_none(item->visibility.restriction)
        && item->attribute_count == 0u
        && item->data.trait_item.safety == CM_HIR_SAFE
        && !item->data.trait_item.is_auto && !item->data.trait_item.is_const
        && item->data.trait_item.supertrait_count == 0u
        && item->data.trait_item.supertraits == NULL;
}

static int cm_capture_impl_shape(const CmExecCaptureState *state,
    const CmHirItem *item)
{
    const CmHirDefinition *trait_definition;
    const CmHirItem *trait_item;
    if (!cm_capture_plain_item(item) || item->attribute_count != 0u
        || item->visibility.kind != CM_HIR_VIS_PRIVATE
        || !cm_hir_def_id_is_none(item->visibility.restriction)
        || !item->data.impl_item.has_trait
        || item->data.impl_item.trait_type.argument_count != 0u
        || item->data.impl_item.trait_type.arguments != NULL
        || item->data.impl_item.safety != CM_HIR_SAFE
        || item->data.impl_item.polarity != CM_HIR_IMPL_POSITIVE
        || item->data.impl_item.is_const) return 0;
    trait_definition = cm_hir_lookup_definition(state->hir,
        item->data.impl_item.trait_type.definition);
    trait_item = trait_definition == NULL
            || trait_definition->kind != CM_HIR_DEFINITION_ITEM
            || trait_definition->state != CM_HIR_DEFINITION_BOUND
        ? NULL : cm_hir_get_item(state->hir,
            trait_definition->entity.item_id);
    return trait_item != NULL && trait_item->kind == CM_HIR_ITEM_TRAIT
        && trait_item->definition.crate_id == state->input->crate_id;
}

static int cm_capture_primitive(const CmHirType *type, uint8_t *out)
{
    if (type == NULL) return 0;
    if (type->kind == CM_HIR_TYPE_BOOL_KIND) {
        *out = CM_HIR_EXEC_PRIMITIVE_BOOL;
        return 1;
    }
    if (type->kind == CM_HIR_TYPE_INTEGER_KIND) {
        switch (type->data.integer_type.kind) {
        case CM_HIR_INT_I8: *out = CM_HIR_EXEC_PRIMITIVE_I8; return 1;
        case CM_HIR_INT_U8: *out = CM_HIR_EXEC_PRIMITIVE_U8; return 1;
        case CM_HIR_INT_I16: *out = CM_HIR_EXEC_PRIMITIVE_I16; return 1;
        case CM_HIR_INT_U16: *out = CM_HIR_EXEC_PRIMITIVE_U16; return 1;
        case CM_HIR_INT_I32: *out = CM_HIR_EXEC_PRIMITIVE_I32; return 1;
        case CM_HIR_INT_U32: *out = CM_HIR_EXEC_PRIMITIVE_U32; return 1;
        case CM_HIR_INT_I64: *out = CM_HIR_EXEC_PRIMITIVE_I64; return 1;
        case CM_HIR_INT_U64: *out = CM_HIR_EXEC_PRIMITIVE_U64; return 1;
        case CM_HIR_INT_ISIZE: *out = CM_HIR_EXEC_PRIMITIVE_ISIZE; return 1;
        case CM_HIR_INT_USIZE: *out = CM_HIR_EXEC_PRIMITIVE_USIZE; return 1;
        default: return 0;
        }
    }
    if (type->kind == CM_HIR_TYPE_FLOAT_KIND) {
        if (type->data.float_type.kind == CM_HIR_FLOAT_F32)
            *out = CM_HIR_EXEC_PRIMITIVE_F32;
        else if (type->data.float_type.kind == CM_HIR_FLOAT_F64)
            *out = CM_HIR_EXEC_PRIMITIVE_F64;
        else return 0;
        return 1;
    }
    return 0;
}

static uint32_t cm_capture_trait_local(const CmExecCaptureState *state,
    CmHirDefId definition)
{
    size_t index;
    for (index = 0u; index < state->trait_count; ++index)
        if (cm_hir_def_id_equal(state->traits[index].item->definition,
                definition)) return (uint32_t)(index + 1u);
    return 0u;
}

static uint32_t cm_capture_value_local(const CmExecCaptureState *state,
    CmHirDefId definition)
{
    size_t index;
    for (index = 0u; index < state->value_count; ++index)
        if (cm_hir_def_id_equal(state->values[index].item->definition,
                definition)) return (uint32_t)(index + 1u);
    return 0u;
}

static uint32_t cm_capture_type_local(CmExecCaptureState *state,
    CmHirTypeId type_id, CmHirDefId owner, int allow_generic)
{
    const CmHirType *type = cm_hir_get_type(state->hir, type_id);
    uint8_t primitive;
    uint32_t before;
    uint32_t bit;
    uint32_t value_local;
    const CmHirGenericParam *parameter;
    if (cm_capture_primitive(type, &primitive)) {
        bit = UINT32_C(1) << primitive;
        state->primitive_mask |= bit;
        before = state->primitive_mask & (bit - 1u);
        /* The returned index is recomputed after collection. */
        return cm_capture_popcount(before) + 1u;
    }
    if (!allow_generic || type == NULL
        || type->kind != CM_HIR_TYPE_PARAMETER_KIND) return 0u;
    parameter = cm_hir_get_generic_param(state->hir,
        type->data.parameter_type.parameter);
    if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_TYPE
        || parameter->index != 0u
        || !cm_hir_def_id_equal(parameter->owner, owner)) return 0u;
    value_local = cm_capture_value_local(state, owner);
    return value_local == 0u ? 0u : UINT32_C(0x80000000) | value_local;
}

static uint32_t cm_capture_final_type_local(const CmExecCaptureState *state,
    CmHirTypeId type_id, CmHirDefId owner, int allow_generic)
{
    const CmHirType *type = cm_hir_get_type(state->hir, type_id);
    uint8_t primitive;
    uint32_t bit;
    uint32_t primitive_count = cm_capture_popcount(state->primitive_mask);
    uint32_t value_local;
    size_t index;
    uint32_t recipe_rank = 0u;
    if (cm_capture_primitive(type, &primitive)) {
        bit = UINT32_C(1) << primitive;
        return cm_capture_popcount(state->primitive_mask & (bit - 1u))
            + 1u;
    }
    if (!allow_generic || type == NULL
        || type->kind != CM_HIR_TYPE_PARAMETER_KIND) return 0u;
    value_local = cm_capture_value_local(state, owner);
    if (value_local == 0u) return 0u;
    for (index = 0u; index < value_local; ++index)
        if (state->values[index].value_kind == CM_HIR_EXEC_VALUE_RECIPE)
            recipe_rank += 1u;
    return primitive_count + recipe_rank;
}

static int cm_capture_collect(CmExecCaptureState *state,
    CmHirExecutableCaptureResult *result)
{
    size_t index;
    size_t local_module_count = 0u;
    for (index = 0u; index < state->hir->modules.len; ++index) {
        const CmHirModule *module = (const CmHirModule *)cm_vec_at_const(
            &state->hir->modules, index);
        if (module != NULL && module->crate_id == state->input->crate_id)
            local_module_count += 1u;
    }
    if (local_module_count != 1u || state->root->parent != CM_HIR_MODULE_NONE
        || state->root->import_count != 0u
        || state->root->outer_attribute_count != 0u
        || state->root->inner_attribute_count != 0u
        || !cm_capture_crate_envelope(state->hir,
            state->crate_value)) return 0;
    state->traits = cm_alloc_zeroed(state->hir->items.len,
        sizeof(*state->traits));
    state->impls = cm_alloc_zeroed(state->hir->items.len,
        sizeof(*state->impls));
    state->values = cm_alloc_zeroed(state->hir->items.len,
        sizeof(*state->values));
    for (index = 0u; index < state->hir->items.len; ++index) {
        const CmHirItem *item = (const CmHirItem *)cm_vec_at_const(
            &state->hir->items, index);
        CmExecCaptureItemRef reference;
        uint32_t ignored;
        if (item == NULL || item->definition.crate_id
                != state->input->crate_id) continue;
        memset(&reference, 0, sizeof(reference));
        reference.item = item;
        reference.id = (CmHirItemId)(index + 1u);
        reference.name = cm_interner_get(&state->hir->strings, item->name);
        if (item->owner_module != state->crate_value->root_module
            || (item->kind != CM_HIR_ITEM_IMPL
                && (reference.name == NULL || reference.name->len == 0u))) {
            result->rejected_item = reference.id;
            return 0;
        }
        if (item->kind == CM_HIR_ITEM_TRAIT
            && cm_capture_trait_shape(item)) {
            state->traits[state->trait_count++] = reference;
        } else if (item->kind == CM_HIR_ITEM_IMPL
            && cm_capture_impl_shape(state, item)) {
            /* Impl names are structurally empty in HIR and are not metadata. */
            state->impls[state->impl_count++] = reference;
        } else if (item->kind == CM_HIR_ITEM_FUNCTION
            && cm_capture_recipe_shape(state, item, &ignored)) {
            reference.value_kind = CM_HIR_EXEC_VALUE_RECIPE;
            state->values[state->value_count++] = reference;
        } else if (item->kind == CM_HIR_ITEM_FUNCTION
            && cm_capture_native_shape(state, item)) {
            reference.value_kind = CM_HIR_EXEC_VALUE_NATIVE_OBJECT;
            state->values[state->value_count++] = reference;
        } else {
            result->rejected_item = reference.id;
            return 0;
        }
    }
    if (state->trait_count == 0u || state->impl_count == 0u
        || state->value_count == 0u) return 0;
    qsort(state->traits, state->trait_count, sizeof(*state->traits),
        cm_capture_ref_name_compare);
    qsort(state->impls, state->impl_count, sizeof(*state->impls),
        cm_capture_impl_compare);
    qsort(state->values, state->value_count, sizeof(*state->values),
        cm_capture_ref_name_compare);
    for (index = 1u; index < state->trait_count; ++index)
        if (cm_capture_string_compare(state->traits[index - 1u].name,
                state->traits[index].name) == 0) return 0;
    for (index = 1u; index < state->value_count; ++index)
        if (cm_capture_string_compare(state->values[index - 1u].name,
                state->values[index].name) == 0) return 0;
    return 1;
}

static int cm_capture_edition(CmHirEdition edition, uint32_t *out)
{
    switch (edition) {
    case CM_HIR_EDITION_2015: *out = UINT32_C(2015); return 1;
    case CM_HIR_EDITION_2018: *out = UINT32_C(2018); return 1;
    case CM_HIR_EDITION_2021: *out = UINT32_C(2021); return 1;
    case CM_HIR_EDITION_2024: *out = UINT32_C(2024); return 1;
    }
    return 0;
}

static int cm_capture_identity(CmExecCaptureState *state,
    CmHirExecutableMetadata *metadata)
{
    const CmInternedString *crate_name = cm_interner_get(&state->hir->strings,
        state->crate_value->name);
    const CmHirArtifactConfig *config = state->input->configuration;
    size_t index;
    if (!cm_capture_copy_intern(&metadata->crate_name, crate_name)
        || !cm_capture_copy_bytes(&metadata->crate_disambiguator,
            state->input->crate_disambiguator.data,
            state->input->crate_disambiguator.length)
        || !cm_capture_edition(state->crate_value->edition,
            &metadata->edition)
        || metadata->edition != config->edition
        || !cm_capture_copy_bytes(&metadata->target_descriptor,
            config->target_descriptor.data, config->target_descriptor.length)
        || !cm_capture_copy_bytes(&metadata->panic_strategy,
            config->panic_strategy.data, config->panic_strategy.length)) return 0;
    metadata->cfg_count = config->cfg_count;
    metadata->cfgs = cm_alloc_zeroed(metadata->cfg_count,
        sizeof(*metadata->cfgs));
    for (index = 0u; index < metadata->cfg_count; ++index)
        cm_capture_copy_bytes(&metadata->cfgs[index], config->cfgs[index].data,
            config->cfgs[index].length);
    metadata->source_entries = state->input->source_entries;
    metadata->source_entry_count = state->input->source_entry_count;
    return cm_hir_artifact_source_closure_digest(metadata->source_entries,
        metadata->source_entry_count, &metadata->source_digest)
        == CM_HIR_ARTIFACT_IDENTITY_OK;
}

static int cm_capture_records(CmExecCaptureState *state,
    CmHirExecutableMetadata *metadata, CmHirExecutableCaptureResult *result)
{
    size_t index;
    size_t parameter;
    size_t predicate_count = 0u;
    size_t recipe_count = 0u;
    size_t native_count = 0u;
    size_t predicate_cursor = 0u;
    size_t body_cursor = 0u;
    size_t symbol_cursor = 0u;
    uint32_t primitive_count;
    metadata->module_count = 1u;
    metadata->modules = cm_alloc_zeroed(1u, sizeof(*metadata->modules));
    cm_capture_copy_intern(&metadata->modules[0].name,
        cm_interner_get(&state->hir->strings, state->crate_value->name));
    metadata->trait_count = state->trait_count;
    metadata->traits = cm_alloc_zeroed(state->trait_count,
        sizeof(*metadata->traits));
    for (index = 0u; index < state->trait_count; ++index) {
        metadata->traits[index].owner_module = 1u;
        cm_capture_copy_intern(&metadata->traits[index].name,
            state->traits[index].name);
        metadata->traits[index].source_ordinal =
            state->traits[index].item->span.start;
    }
    for (index = 0u; index < state->value_count; ++index) {
        const CmHirItem *item = state->values[index].item;
        if (state->values[index].value_kind == CM_HIR_EXEC_VALUE_RECIPE) {
            predicate_count += item->predicate_count;
            recipe_count += 1u;
        } else native_count += 1u;
        for (parameter = 0u;
            parameter < item->data.function_item.signature.parameter_count;
            ++parameter) {
            if (cm_capture_type_local(state,
                    item->data.function_item.signature.parameters[parameter].type,
                    item->definition,
                    state->values[index].value_kind
                        == CM_HIR_EXEC_VALUE_RECIPE) == 0u) {
                result->rejected_item = state->values[index].id;
                result->rejected_type =
                    item->data.function_item.signature.parameters[parameter].type;
                return 0;
            }
        }
        if (cm_capture_type_local(state,
                item->data.function_item.signature.return_type,
                item->definition,
                state->values[index].value_kind
                    == CM_HIR_EXEC_VALUE_RECIPE) == 0u) {
            result->rejected_item = state->values[index].id;
            result->rejected_type =
                item->data.function_item.signature.return_type;
            return 0;
        }
    }
    for (index = 0u; index < state->impl_count; ++index) {
        uint8_t ignored;
        const CmHirItem *item = state->impls[index].item;
        if (!cm_capture_primitive(cm_hir_get_type(state->hir,
                item->data.impl_item.self_type), &ignored)
            || cm_capture_trait_local(state,
                item->data.impl_item.trait_type.definition) == 0u) {
            result->rejected_item = state->impls[index].id;
            result->rejected_type = item->data.impl_item.self_type;
            return 0;
        }
        (void)cm_capture_type_local(state, item->data.impl_item.self_type,
            item->definition, 0);
    }
    primitive_count = cm_capture_popcount(state->primitive_mask);
    metadata->type_count = primitive_count + recipe_count;
    metadata->types = cm_alloc_zeroed(metadata->type_count,
        sizeof(*metadata->types));
    {
        uint32_t primitive;
        size_t out = 0u;
        for (primitive = CM_HIR_EXEC_PRIMITIVE_BOOL;
            primitive <= CM_HIR_EXEC_PRIMITIVE_F64; ++primitive) {
            if ((state->primitive_mask & (UINT32_C(1) << primitive)) != 0u) {
                metadata->types[out].kind = CM_HIR_EXEC_TYPE_PRIMITIVE;
                metadata->types[out].primitive = (uint8_t)primitive;
                out += 1u;
            }
        }
        for (index = 0u; index < state->value_count; ++index) {
            if (state->values[index].value_kind
                    == CM_HIR_EXEC_VALUE_RECIPE) {
                metadata->types[out].kind = CM_HIR_EXEC_TYPE_VALUE_GENERIC;
                metadata->types[out].owner_value = (uint32_t)(index + 1u);
                out += 1u;
            }
        }
    }
    metadata->impl_count = state->impl_count;
    metadata->impls = cm_alloc_zeroed(state->impl_count,
        sizeof(*metadata->impls));
    for (index = 0u; index < state->impl_count; ++index) {
        const CmHirItem *item = state->impls[index].item;
        metadata->impls[index].owner_module = 1u;
        metadata->impls[index].source_ordinal = item->span.start;
        metadata->impls[index].trait_local = cm_capture_trait_local(state,
            item->data.impl_item.trait_type.definition);
        metadata->impls[index].self_type = cm_capture_final_type_local(state,
            item->data.impl_item.self_type, item->definition, 0);
    }
    metadata->value_count = state->value_count;
    metadata->values = cm_alloc_zeroed(state->value_count,
        sizeof(*metadata->values));
    metadata->predicate_count = predicate_count;
    metadata->predicates = cm_alloc_zeroed(predicate_count,
        sizeof(*metadata->predicates));
    metadata->body_count = recipe_count;
    metadata->bodies = cm_alloc_zeroed(recipe_count,
        sizeof(*metadata->bodies));
    metadata->symbol_count = native_count;
    metadata->symbols = cm_alloc_zeroed(native_count,
        sizeof(*metadata->symbols));
    for (index = 0u; index < state->value_count; ++index) {
        const CmHirItem *item = state->values[index].item;
        const CmHirFunctionSignature *signature =
            &item->data.function_item.signature;
        CmHirExecutableValue *value = &metadata->values[index];
        uint32_t return_parameter = 0u;
        value->owner_module = 1u;
        cm_capture_copy_intern(&value->name, state->values[index].name);
        value->source_ordinal = item->span.start;
        value->kind = state->values[index].value_kind;
        value->parameter_count = signature->parameter_count;
        value->parameter_types = cm_alloc_zeroed(value->parameter_count,
            sizeof(*value->parameter_types));
        for (parameter = 0u; parameter < value->parameter_count; ++parameter)
            value->parameter_types[parameter] = cm_capture_final_type_local(
                state, signature->parameters[parameter].type,
                item->definition, value->kind == CM_HIR_EXEC_VALUE_RECIPE);
        value->return_type = cm_capture_final_type_local(state,
            signature->return_type, item->definition,
            value->kind == CM_HIR_EXEC_VALUE_RECIPE);
        if (value->kind == CM_HIR_EXEC_VALUE_RECIPE) {
            const CmHirGenericParam *generic = cm_hir_get_generic_param(
                state->hir, item->generic_parameter_start);
            size_t predicate;
            (void)cm_capture_recipe_shape(state, item, &return_parameter);
            cm_capture_copy_intern(&value->generic_name,
                cm_interner_get(&state->hir->strings, generic->name));
            value->predicate_start = (uint32_t)(predicate_cursor + 1u);
            value->predicate_count = item->predicate_count;
            value->execution_local = (uint32_t)(body_cursor + 1u);
            for (predicate = 0u; predicate < item->predicate_count;
                ++predicate) {
                CmHirExecutablePredicate *wire =
                    &metadata->predicates[predicate_cursor + predicate];
                wire->owner_value = (uint32_t)(index + 1u);
                wire->ordinal = (uint32_t)predicate;
                wire->subject_type = cm_capture_final_type_local(state,
                    item->predicates[predicate].subject, item->definition, 1);
                wire->trait_local = cm_capture_trait_local(state,
                    item->predicates[predicate].trait_type.definition);
                if (wire->trait_local == 0u) {
                    result->rejected_item = state->values[index].id;
                    return 0;
                }
            }
            metadata->bodies[body_cursor].owner_value =
                (uint32_t)(index + 1u);
            metadata->bodies[body_cursor].parameter_index = return_parameter;
            metadata->bodies[body_cursor].parameter_type = value->return_type;
            metadata->bodies[body_cursor].return_type = value->return_type;
            predicate_cursor += item->predicate_count;
            body_cursor += 1u;
        } else {
            value->execution_local = (uint32_t)(symbol_cursor + 1u);
            metadata->symbols[symbol_cursor].owner_value =
                (uint32_t)(index + 1u);
            metadata->symbols[symbol_cursor].object_local = 1u;
            cm_capture_copy_intern(
                &metadata->symbols[symbol_cursor].external_symbol,
                state->values[index].name);
            symbol_cursor += 1u;
        }
    }
    metadata->namespace_count = state->trait_count + state->value_count;
    metadata->namespace_entries = cm_alloc_zeroed(metadata->namespace_count,
        sizeof(*metadata->namespace_entries));
    for (index = 0u; index < state->trait_count; ++index) {
        CmHirExecutableNamespaceEntry *entry =
            &metadata->namespace_entries[index];
        entry->owner_module = 1u;
        entry->namespace_kind = CM_HIR_EXEC_NAMESPACE_TYPE;
        cm_capture_copy_intern(&entry->name, state->traits[index].name);
        entry->target_kind = CM_HIR_EXEC_NAMESPACE_TRAIT;
        entry->target_local = (uint32_t)(index + 1u);
        entry->export_ordinal = state->traits[index].item->span.start;
    }
    for (index = 0u; index < state->value_count; ++index) {
        CmHirExecutableNamespaceEntry *entry =
            &metadata->namespace_entries[state->trait_count + index];
        entry->owner_module = 1u;
        entry->namespace_kind = CM_HIR_EXEC_NAMESPACE_VALUE;
        cm_capture_copy_intern(&entry->name, state->values[index].name);
        entry->target_kind = CM_HIR_EXEC_NAMESPACE_VALUE_TARGET;
        entry->target_local = (uint32_t)(index + 1u);
        entry->export_ordinal = state->values[index].item->span.start;
    }
    metadata->object_count = 1u;
    metadata->objects = cm_alloc_zeroed(1u, sizeof(*metadata->objects));
    cm_capture_copy_bytes(&metadata->objects[0].archive_member_name,
        state->input->archive_member_name.data,
        state->input->archive_member_name.length);
    metadata->objects[0].byte_length = state->input->object_bytes.length;
    metadata->objects[0].object_bytes = state->input->object_bytes.data;
    metadata->objects[0].object_bytes_length =
        state->input->object_bytes.length;
    {
        CmSha256 sha;
        cm_sha256_init(&sha);
        cm_sha256_update(&sha, state->input->object_bytes.data,
            state->input->object_bytes.length);
        cm_sha256_final(&sha, metadata->objects[0].object_digest.bytes);
    }
    metadata->objects[0].symbol_start = 1u;
    metadata->objects[0].symbol_count = (uint32_t)native_count;
    result->trait_count = state->trait_count;
    result->impl_count = state->impl_count;
    result->recipe_count = recipe_count;
    result->native_object_value_count = native_count;
    return recipe_count != 0u && native_count != 0u;
}

CmHirExecutableCaptureResult cm_hir_executable_metadata_capture(
    const CmHirExecutableCaptureInput *input,
    CmHirExecutableMetadata *output)
{
    CmHirExecutableCaptureResult result;
    CmExecCaptureState state;
    CmSemanticAdmissionAuthority *authority;
    CmHirExecutableMetadata candidate;
    CmHirExecutableMetadata old;
    const CmHirCrate *crate_value;
    const CmHirModule *root;
    uint64_t generation;
    result = cm_capture_result(CM_HIR_EXEC_CAPTURE_INVALID_ARGUMENT);
    if (input == NULL || output == NULL || input->hir == NULL
        || input->regions_admission == NULL || input->configuration == NULL
        || input->crate_id == CM_HIR_CRATE_NONE
        || input->crate_disambiguator.data == NULL
        || input->crate_disambiguator.length == 0u
        || input->source_entries == NULL || input->source_entry_count == 0u
        || input->archive_member_name.data == NULL
        || input->archive_member_name.length == 0u
        || input->object_bytes.data == NULL || input->object_bytes.length == 0u)
        return result;
    authority = cm_semantic_admission_authority_retain(
        input->regions_admission, 1);
    if (authority == NULL
        || cm_semantic_admission_hir(input->regions_admission) != input->hir
        || cm_semantic_admission_crate(input->regions_admission)
            != input->crate_id
        || cm_semantic_admission_generation(input->regions_admission)
            != input->hir->semantic_generation) {
        cm_semantic_admission_authority_release(authority);
        return cm_capture_result(CM_HIR_EXEC_CAPTURE_INVALID_AUTHORITY);
    }
    generation = input->hir->semantic_generation;
    crate_value = cm_hir_get_crate(input->hir, input->crate_id);
    root = crate_value == NULL ? NULL
        : cm_hir_get_module(input->hir, crate_value->root_module);
    if (crate_value == NULL || root == NULL) {
        cm_semantic_admission_authority_release(authority);
        return result;
    }
    memset(&state, 0, sizeof(state));
    state.input = input;
    state.hir = input->hir;
    state.crate_value = crate_value;
    state.root = root;
    cm_hir_executable_metadata_init(&candidate);
    candidate.owns_storage = 1;
    result = cm_capture_result(CM_HIR_EXEC_CAPTURE_UNSUPPORTED_HIR);
    if (!cm_capture_collect(&state, &result)
        || !cm_capture_identity(&state, &candidate)
        || !cm_capture_records(&state, &candidate, &result)) goto done;
    result.metadata_status = cm_hir_executable_metadata_compute_identity(
        &candidate, &candidate.link_manifest_digest,
        &candidate.artifact_identity);
    if (result.metadata_status != CM_HIR_EXEC_METADATA_OK) {
        result.status = CM_HIR_EXEC_CAPTURE_METADATA_FAILURE;
        goto done;
    }
    if (!cm_semantic_admission_is_current(input->regions_admission)
        || cm_semantic_admission_generation(input->regions_admission)
            != generation || input->hir->semantic_generation != generation) {
        result.status = CM_HIR_EXEC_CAPTURE_INVALID_AUTHORITY;
        goto done;
    }
    old = *output;
    *output = candidate;
    cm_hir_executable_metadata_init(&candidate);
    cm_hir_executable_metadata_destroy(&old);
    result.status = CM_HIR_EXEC_CAPTURE_OK;
done:
    cm_free(state.traits);
    cm_free(state.impls);
    cm_free(state.values);
    cm_hir_executable_metadata_destroy(&candidate);
    cm_semantic_admission_authority_release(authority);
    return result;
}

const char *cm_hir_executable_capture_status_name(
    CmHirExecutableCaptureStatus status)
{
    switch (status) {
    case CM_HIR_EXEC_CAPTURE_OK: return "ok";
    case CM_HIR_EXEC_CAPTURE_INVALID_ARGUMENT: return "invalid argument";
    case CM_HIR_EXEC_CAPTURE_INVALID_AUTHORITY: return "invalid authority";
    case CM_HIR_EXEC_CAPTURE_UNSUPPORTED_HIR: return "unsupported HIR";
    case CM_HIR_EXEC_CAPTURE_METADATA_FAILURE: return "metadata failure";
    }
    return "unknown";
}
