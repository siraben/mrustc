#include "cm/hir/layout.h"

#include "cm/alloc.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

typedef enum CmHirLayoutVisit {
    CM_HIR_LAYOUT_UNSEEN = 0,
    CM_HIR_LAYOUT_VISITING,
    CM_HIR_LAYOUT_DONE
} CmHirLayoutVisit;

typedef struct CmHirLayoutNode {
    CmHirLayoutVisit visit;
    size_t size;
    size_t alignment;
} CmHirLayoutNode;

typedef struct CmHirLayoutState {
    const CmHirContext *context;
    CmHirCrateId crate_id;
    unsigned int pointer_bits;
    size_t address_limit;
    CmHirLayoutNode *nodes;
} CmHirLayoutState;

static const CmHirItem *cm_hir_layout_item(const CmHirContext *context,
    CmHirDefId definition, size_t *out_index)
{
    const CmHirDefinition *record;
    const CmHirItem *item;
    size_t index;

    record = cm_hir_lookup_definition(context, definition);
    item = record == NULL || record->kind != CM_HIR_DEFINITION_ITEM
            || record->state != CM_HIR_DEFINITION_BOUND
        ? NULL : cm_hir_get_item(context, record->entity.item_id);
    if (item == NULL || !cm_hir_def_id_equal(item->definition, definition)) {
        return NULL;
    }
    index = (size_t)record->entity.item_id - 1u;
    if (index >= context->items.len
        || cm_vec_at_const(&context->items, index) != item) {
        return NULL;
    }
    if (out_index != NULL) *out_index = index;
    return item;
}

static int cm_hir_layout_has_repr(const CmHirContext *context,
    const CmHirItem *item)
{
    uint32_t index;

    for (index = 0u; index < item->attribute_count; ++index) {
        const CmInternedString *metadata;

        metadata = cm_interner_get(&context->strings,
            item->attributes[index].metadata);
        if (metadata != NULL && metadata->len >= 4u
            && memcmp(metadata->bytes, "repr", 4u) == 0) {
            return 1;
        }
    }
    return 0;
}

static CmHirLayoutStatus cm_hir_layout_validate_item(
    const CmHirLayoutState *state, const CmHirItem *item)
{
    const CmHirModule *module;

    if (item == NULL || item->kind != CM_HIR_ITEM_STRUCT
        || item->definition.crate_id != state->crate_id
        || !cm_hir_def_id_is_none(item->parent_definition)
        || item->generic_parameter_count != 0u
        || item->data.aggregate_item.form != CM_HIR_AGGREGATE_NAMED
        || item->data.aggregate_item.field_count == 0u
        || item->data.aggregate_item.fields == NULL
        || cm_hir_layout_has_repr(state->context, item)) {
        return CM_HIR_LAYOUT_UNSUPPORTED_TYPE;
    }
    module = cm_hir_get_module(state->context, item->owner_module);
    if (module == NULL || module->crate_id != state->crate_id) {
        return CM_HIR_LAYOUT_INVALID_DEFINITION;
    }
    return CM_HIR_LAYOUT_OK;
}

static int cm_hir_layout_add(size_t left, size_t right, size_t *out_value)
{
    if (left > SIZE_MAX - right) return 0;
    *out_value = left + right;
    return 1;
}

static int cm_hir_layout_align_up(size_t value, size_t alignment,
    size_t *out_value)
{
    size_t remainder;

    if (alignment == 0u) return 0;
    remainder = value % alignment;
    return remainder == 0u
        ? ((*out_value = value), 1)
        : cm_hir_layout_add(value, alignment - remainder, out_value);
}

static CmHirLayoutStatus cm_hir_layout_type(CmHirLayoutState *state,
    CmHirTypeId type_id, size_t depth, size_t *out_size,
    size_t *out_alignment);

static CmHirLayoutStatus cm_hir_layout_struct(CmHirLayoutState *state,
    CmHirDefId definition, size_t depth, CmHirFieldLayout *root_fields,
    size_t *out_size, size_t *out_alignment)
{
    const CmHirItem *item;
    CmHirLayoutNode *node;
    CmHirLayoutStatus status;
    size_t item_index;
    size_t offset;
    size_t maximum_alignment;
    uint32_t field_index;

    if (depth > state->context->items.len) {
        return CM_HIR_LAYOUT_RECURSIVE_TYPE;
    }
    item = cm_hir_layout_item(state->context, definition, &item_index);
    if (item == NULL) return CM_HIR_LAYOUT_INVALID_DEFINITION;
    status = cm_hir_layout_validate_item(state, item);
    if (status != CM_HIR_LAYOUT_OK) return status;
    node = &state->nodes[item_index];
    if (node->visit == CM_HIR_LAYOUT_VISITING) {
        return CM_HIR_LAYOUT_RECURSIVE_TYPE;
    }
    if (node->visit == CM_HIR_LAYOUT_DONE && root_fields == NULL) {
        *out_size = node->size;
        *out_alignment = node->alignment;
        return CM_HIR_LAYOUT_OK;
    }

    node->visit = CM_HIR_LAYOUT_VISITING;
    offset = 0u;
    maximum_alignment = 1u;
    for (field_index = 0u;
         field_index < item->data.aggregate_item.field_count;
         ++field_index) {
        const CmHirField *field;
        size_t field_size;
        size_t field_alignment;
        size_t field_end;

        field = &item->data.aggregate_item.fields[field_index];
        status = cm_hir_layout_type(state, field->type, depth + 1u,
            &field_size, &field_alignment);
        if (status != CM_HIR_LAYOUT_OK) return status;
        if (!cm_hir_layout_align_up(offset, field_alignment, &offset)
            || !cm_hir_layout_add(offset, field_size, &field_end)
            || offset > state->address_limit
            || field_size > state->address_limit
            || field_end > state->address_limit) {
            return CM_HIR_LAYOUT_OVERFLOW;
        }
        if (root_fields != NULL) {
            root_fields[field_index].type = field->type;
            root_fields[field_index].offset = offset;
            root_fields[field_index].size = field_size;
            root_fields[field_index].alignment = field_alignment;
        }
        offset = field_end;
        if (field_alignment > maximum_alignment) {
            maximum_alignment = field_alignment;
        }
    }
    if (!cm_hir_layout_align_up(offset, maximum_alignment, &offset)
        || offset > state->address_limit) {
        return CM_HIR_LAYOUT_OVERFLOW;
    }
    node->visit = CM_HIR_LAYOUT_DONE;
    node->size = offset;
    node->alignment = maximum_alignment;
    *out_size = offset;
    *out_alignment = maximum_alignment;
    return CM_HIR_LAYOUT_OK;
}

static CmHirLayoutStatus cm_hir_layout_type(CmHirLayoutState *state,
    CmHirTypeId type_id, size_t depth, size_t *out_size,
    size_t *out_alignment)
{
    const CmHirType *type;

    type = cm_hir_get_type(state->context, type_id);
    if (type == NULL) return CM_HIR_LAYOUT_UNSUPPORTED_TYPE;
    if (type->kind == CM_HIR_TYPE_INTEGER_KIND
        && (type->data.integer_type.kind == CM_HIR_INT_I32
            || type->data.integer_type.kind == CM_HIR_INT_U32)) {
        *out_size = 4u;
        *out_alignment = 4u;
        return CM_HIR_LAYOUT_OK;
    }
    if (type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_USIZE) {
        *out_size = (size_t)(state->pointer_bits / 8u);
        *out_alignment = *out_size;
        return CM_HIR_LAYOUT_OK;
    }
    if (type->kind != CM_HIR_TYPE_ADT_KIND
        || type->data.named_type.argument_count != 0u
        || type->data.named_type.arguments != NULL
        || type->data.named_type.definition.crate_id != state->crate_id) {
        return CM_HIR_LAYOUT_UNSUPPORTED_TYPE;
    }
    return cm_hir_layout_struct(state, type->data.named_type.definition,
        depth, NULL, out_size, out_alignment);
}

CmHirLayoutStatus cm_hir_layout_named_struct(const CmHirContext *context,
    unsigned int pointer_bits, CmHirDefId definition,
    CmHirNamedStructLayout *out_layout, CmHirFieldLayout *out_fields,
    uint32_t field_capacity)
{
    const CmHirItem *item;
    CmHirLayoutState state;
    CmHirNamedStructLayout layout;
    CmHirFieldLayout *fields;
    CmHirLayoutStatus status;
    size_t field_bytes;

    if (context == NULL || out_layout == NULL
        || cm_hir_def_id_is_none(definition)
        || (pointer_bits != 32u && pointer_bits != 64u)
        || pointer_bits > (unsigned int)(sizeof(size_t) * CHAR_BIT)) {
        return CM_HIR_LAYOUT_INVALID_ARGUMENT;
    }
    item = cm_hir_layout_item(context, definition, NULL);
    if (item == NULL) return CM_HIR_LAYOUT_INVALID_DEFINITION;
    memset(&state, 0, sizeof(state));
    state.context = context;
    state.crate_id = definition.crate_id;
    state.pointer_bits = pointer_bits;
    state.address_limit = pointer_bits == 32u
        ? (size_t)UINT32_MAX : SIZE_MAX;
    status = cm_hir_layout_validate_item(&state, item);
    if (status != CM_HIR_LAYOUT_OK) return status;
    if (item->data.aggregate_item.field_count > field_capacity
        || (item->data.aggregate_item.field_count != 0u
            && out_fields == NULL)) {
        return CM_HIR_LAYOUT_INSUFFICIENT_CAPACITY;
    }
    if (!cm_size_mul((size_t)item->data.aggregate_item.field_count,
            sizeof(CmHirFieldLayout), &field_bytes)) {
        return CM_HIR_LAYOUT_OVERFLOW;
    }
    state.nodes = (CmHirLayoutNode *)cm_alloc_zeroed(context->items.len,
        sizeof(CmHirLayoutNode));
    fields = (CmHirFieldLayout *)cm_alloc_zeroed(
        (size_t)item->data.aggregate_item.field_count,
        sizeof(CmHirFieldLayout));
    memset(&layout, 0, sizeof(layout));
    layout.definition = definition;
    layout.field_count = item->data.aggregate_item.field_count;
    status = cm_hir_layout_struct(&state, definition, 0u, fields,
        &layout.size, &layout.alignment);
    if (status == CM_HIR_LAYOUT_OK) {
        *out_layout = layout;
        memcpy(out_fields, fields, field_bytes);
    }
    cm_free(fields);
    cm_free(state.nodes);
    return status;
}

const char *cm_hir_layout_status_name(CmHirLayoutStatus status)
{
    switch (status) {
    case CM_HIR_LAYOUT_OK: return "ok";
    case CM_HIR_LAYOUT_INVALID_ARGUMENT: return "invalid-argument";
    case CM_HIR_LAYOUT_INVALID_DEFINITION: return "invalid-definition";
    case CM_HIR_LAYOUT_UNSUPPORTED_TYPE: return "unsupported-type";
    case CM_HIR_LAYOUT_RECURSIVE_TYPE: return "recursive-type";
    case CM_HIR_LAYOUT_INSUFFICIENT_CAPACITY:
        return "insufficient-capacity";
    case CM_HIR_LAYOUT_OVERFLOW: return "overflow";
    }
    return "unknown";
}
