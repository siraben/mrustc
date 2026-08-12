#include "cm/mir/model.h"

#include "cm/alloc.h"

#include <string.h>

#define CM_MIR_STORAGE_ALIGNMENT ((size_t)16u)
#define CM_MIR_EXPRESSION_RECURSION_LIMIT ((size_t)512u)

static int cm_mir_context_valid(const CmMirContext *context)
{
    return context != NULL
        && (context->pointer_bits == 0u || context->pointer_bits == 32u
            || context->pointer_bits == 64u)
        && context->bodies.elem_size == sizeof(CmMirBody)
        && context->bodies.len <= context->bodies.cap
        && (context->bodies.cap == 0u) == (context->bodies.data == NULL);
}

static int cm_mir_instance_is_empty(const CmMirInstance *instance)
{
    return instance != NULL
        && cm_hir_def_id_is_none(instance->definition)
        && instance->substitutions == NULL
        && instance->substitution_count == 0u;
}

static int cm_mir_instance_equal(const CmMirInstance *left,
    const CmMirInstance *right)
{
    uint32_t index;

    if (left == NULL || right == NULL
        || !cm_hir_def_id_equal(left->definition, right->definition)
        || left->substitution_count != right->substitution_count) {
        return 0;
    }
    if ((left->substitution_count != 0u && left->substitutions == NULL)
        || (right->substitution_count != 0u
            && right->substitutions == NULL)) {
        return 0;
    }
    for (index = 0u; index < left->substitution_count; ++index) {
        if (left->substitutions[index] != right->substitutions[index]) {
            return 0;
        }
    }
    return 1;
}

static int cm_mir_local_id_valid(const CmMirBody *body, CmMirLocalId local)
{
    return body != NULL && (size_t)local < (size_t)body->local_count;
}

static int cm_mir_legacy_body_shape_valid(const CmMirBody *body)
{
    const CmMirBasicBlock *block;
    const CmMirStatement *statement;

    if (body == NULL || !cm_mir_instance_is_empty(&body->instance)
        || body->owned_storage != NULL
        || body->owner.crate_id == CM_HIR_CRATE_NONE
        || body->owner.index == CM_HIR_DEF_INDEX_NONE
        || body->source_body == CM_HIR_BODY_NONE
        || body->local_count != 1u || body->locals == NULL
        || body->basic_block_count != 1u || body->basic_blocks == NULL) {
        return 0;
    }
    if (body->locals[CM_MIR_RETURN_LOCAL].kind != CM_MIR_LOCAL_RETURN
        || body->locals[CM_MIR_RETURN_LOCAL].type == CM_HIR_TYPE_NONE) {
        return 0;
    }
    block = &body->basic_blocks[CM_MIR_ENTRY_BLOCK];
    if (block->statement_count != 1u || block->statements == NULL
        || block->terminator.kind != CM_MIR_TERMINATOR_RETURN) {
        return 0;
    }
    statement = &block->statements[0];
    return statement->kind == CM_MIR_STATEMENT_ASSIGN
        && statement->data.assign.destination == CM_MIR_RETURN_LOCAL
        && statement->data.assign.value.kind == CM_MIR_RVALUE_USE
        && statement->data.assign.value.type
            == body->locals[CM_MIR_RETURN_LOCAL].type
        && statement->data.assign.value.data.use.kind
            == CM_MIR_CONSTANT_I32
        && statement->data.assign.value.data.use.type
            == statement->data.assign.value.type;
}

static int cm_mir_type_is_u32(const CmHirContext *hir, CmHirTypeId id)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, id);
    return type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_U32;
}

static int cm_mir_type_is_usize(const CmHirContext *hir, CmHirTypeId id)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, id);
    return type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_USIZE;
}

static int cm_mir_pointer_bits_valid(unsigned int pointer_bits)
{
    return pointer_bits == 32u || pointer_bits == 64u;
}

static int cm_mir_usize_value_valid(unsigned int pointer_bits,
    uint64_t value)
{
    return pointer_bits == 64u
        || (pointer_bits == 32u && value <= (uint64_t)UINT32_MAX);
}

static int cm_mir_type_is_unsigned_scalar(const CmHirContext *hir,
    CmHirTypeId id, unsigned int pointer_bits)
{
    return cm_mir_type_is_u32(hir, id)
        || (cm_mir_pointer_bits_valid(pointer_bits)
            && cm_mir_type_is_usize(hir, id));
}

static int cm_mir_type_is_i32(const CmHirContext *hir, CmHirTypeId id)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, id);
    return type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_I32;
}

static int cm_mir_type_is_bool(const CmHirContext *hir, CmHirTypeId id)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, id);
    return type != NULL && type->kind == CM_HIR_TYPE_BOOL_KIND;
}

static const CmHirItem *cm_mir_named_struct(const CmHirContext *hir,
    CmHirDefId definition_id);

static int cm_mir_type_equal(const CmHirContext *hir, CmHirTypeId left,
    CmHirTypeId right)
{
    const CmHirType *left_type;
    const CmHirType *right_type;

    if (left == right) return 1;
    if (cm_mir_type_is_u32(hir, left) && cm_mir_type_is_u32(hir, right)) {
        return 1;
    }
    if (cm_mir_type_is_usize(hir, left)
        && cm_mir_type_is_usize(hir, right)) {
        return 1;
    }
    if (cm_mir_type_is_bool(hir, left)
        && cm_mir_type_is_bool(hir, right)) {
        return 1;
    }
    left_type = cm_hir_get_type(hir, left);
    right_type = cm_hir_get_type(hir, right);
    return left_type != NULL && right_type != NULL
        && left_type->kind == CM_HIR_TYPE_ADT_KIND
        && right_type->kind == CM_HIR_TYPE_ADT_KIND
        && left_type->data.named_type.argument_count == 0u
        && left_type->data.named_type.arguments == NULL
        && right_type->data.named_type.argument_count == 0u
        && right_type->data.named_type.arguments == NULL
        && cm_hir_def_id_equal(left_type->data.named_type.definition,
            right_type->data.named_type.definition);
}

static int cm_mir_type_supported(const CmHirContext *hir, CmHirTypeId id,
    unsigned int pointer_bits)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, id);
    return type != NULL
        && ((type->kind == CM_HIR_TYPE_INTEGER_KIND
                && (type->data.integer_type.kind == CM_HIR_INT_I32
                    || type->data.integer_type.kind == CM_HIR_INT_U32
                    || (type->data.integer_type.kind == CM_HIR_INT_USIZE
                        && cm_mir_pointer_bits_valid(pointer_bits))))
            || (type->kind == CM_HIR_TYPE_ADT_KIND
                && type->data.named_type.argument_count == 0u
                && type->data.named_type.arguments == NULL)
            || type->kind == CM_HIR_TYPE_BOOL_KIND);
}

static int cm_mir_type_is_checked_aggregate(const CmHirContext *hir,
    CmHirTypeId id, CmHirCrateId owner_crate)
{
    const CmHirType *type;
    const CmHirItem *item;

    type = cm_hir_get_type(hir, id);
    item = type == NULL || type->kind != CM_HIR_TYPE_ADT_KIND
            || type->data.named_type.argument_count != 0u
            || type->data.named_type.arguments != NULL
        ? NULL : cm_mir_named_struct(hir,
            type->data.named_type.definition);
    return item != NULL && item->definition.crate_id == owner_crate
        && item->data.aggregate_item.field_count != 0u
        && item->data.aggregate_item.field_count
            <= CM_MIR_MAX_AGGREGATE_FIELDS;
}

static const CmHirItem *cm_mir_named_struct(const CmHirContext *hir,
    CmHirDefId definition_id)
{
    const CmHirDefinition *definition;
    const CmHirItem *item;

    definition = cm_hir_lookup_definition(hir, definition_id);
    item = definition == NULL
            || definition->kind != CM_HIR_DEFINITION_ITEM
            || definition->state != CM_HIR_DEFINITION_BOUND
        ? NULL : cm_hir_get_item(hir, definition->entity.item_id);
    return item != NULL && item->kind == CM_HIR_ITEM_STRUCT
            && cm_hir_def_id_equal(item->definition, definition_id)
            && cm_hir_def_id_is_none(item->parent_definition)
            && item->generic_parameter_count == 0u
            && item->data.aggregate_item.form == CM_HIR_AGGREGATE_NAMED
            && (item->data.aggregate_item.field_count == 0u)
                == (item->data.aggregate_item.fields == NULL)
        ? item : NULL;
}

static int cm_mir_type_target_valid(const CmHirContext *hir,
    CmHirTypeId id, unsigned int pointer_bits, size_t depth)
{
    const CmHirType *type;
    const CmHirItem *item;
    uint32_t index;

    if (depth >= CM_MIR_EXPRESSION_RECURSION_LIMIT) return 0;
    type = cm_hir_get_type(hir, id);
    if (type == NULL) return 0;
    if (type->kind == CM_HIR_TYPE_INTEGER_KIND) {
        return type->data.integer_type.kind != CM_HIR_INT_USIZE
            || cm_mir_pointer_bits_valid(pointer_bits);
    }
    if (type->kind != CM_HIR_TYPE_ADT_KIND) return 1;
    item = type->data.named_type.argument_count != 0u
            || type->data.named_type.arguments != NULL
        ? NULL : cm_mir_named_struct(hir,
            type->data.named_type.definition);
    if (item == NULL) return 0;
    for (index = 0u; index < item->data.aggregate_item.field_count;
         ++index) {
        if (!cm_mir_type_target_valid(hir,
                item->data.aggregate_item.fields[index].type,
                pointer_bits, depth + 1u)) {
            return 0;
        }
    }
    return 1;
}

static int cm_mir_span_within(CmSpan inner, CmSpan outer)
{
    return inner.source != 0u && inner.source == outer.source
        && inner.start <= inner.end && inner.start >= outer.start
        && inner.end <= outer.end;
}

static int cm_mir_place_present(const CmMirPlace *place)
{
    return place != NULL && (place->type != CM_HIR_TYPE_NONE
        || place->projections != NULL || place->projection_count != 0u
        || place->span.source != 0u || place->span.start != 0u
        || place->span.end != 0u);
}

static int cm_mir_place_valid(const CmHirContext *hir,
    const CmMirBody *body, const CmMirPlace *place)
{
    const CmHirBody *source_body;
    const CmHirDefinition *body_owner;
    CmHirTypeId current_type;
    uint32_t index;

    if (hir == NULL || body == NULL || place == NULL
        || !cm_mir_local_id_valid(body, place->base)
        || place->type == CM_HIR_TYPE_NONE
        || place->projection_count > CM_MIR_MAX_PLACE_PROJECTIONS
        || (place->projection_count == 0u)
            != (place->projections == NULL)) {
        return 0;
    }
    source_body = cm_hir_get_body(hir, body->source_body);
    body_owner = source_body == NULL ? NULL
        : cm_hir_lookup_definition(hir, source_body->owner);
    if (source_body == NULL || source_body->state != CM_HIR_BODY_TYPED
        || body_owner == NULL || body_owner->kind != CM_HIR_DEFINITION_ITEM
        || !cm_hir_def_id_equal(source_body->owner, body->owner)
        || !cm_mir_span_within(place->span, source_body->span)) {
        return 0;
    }
    current_type = body->locals[place->base].type;
    for (index = 0u; index < place->projection_count; ++index) {
        const CmMirFieldProjection *projection;
        const CmHirType *type;
        const CmHirItem *item;

        projection = &place->projections[index];
        type = cm_hir_get_type(hir, current_type);
        item = type == NULL || type->kind != CM_HIR_TYPE_ADT_KIND
                || type->data.named_type.argument_count != 0u
                || type->data.named_type.arguments != NULL
                || !cm_hir_def_id_equal(
                    type->data.named_type.definition,
                    projection->definition)
            ? NULL : cm_mir_named_struct(hir, projection->definition);
        if (item == NULL
            || item->definition.crate_id != body_owner->id.crate_id
            || projection->field_index
                >= item->data.aggregate_item.field_count) {
            return 0;
        }
        current_type = item->data.aggregate_item
            .fields[projection->field_index].type;
    }
    return cm_mir_type_equal(hir, current_type, place->type);
}

CmMirStatus cm_mir_validate_place(const CmHirContext *hir,
    const CmMirBody *body, const CmMirPlace *place)
{
    if (hir == NULL || body == NULL || place == NULL) {
        return CM_MIR_INVALID_ARGUMENT;
    }
    return cm_mir_place_valid(hir, body, place) ? CM_MIR_OK
        : CM_MIR_INVARIANT_VIOLATION;
}

static const CmHirItem *cm_mir_instance_function(const CmHirContext *hir,
    const CmMirBody *body)
{
    const CmHirDefinition *definition;
    const CmHirItem *item;
    const CmHirBody *source_body;

    if (hir == NULL || body == NULL
        || cm_hir_def_id_is_none(body->instance.definition)
        || !cm_hir_def_id_equal(body->owner, body->instance.definition)
        || body->source_body == CM_HIR_BODY_NONE) {
        return NULL;
    }
    definition = cm_hir_lookup_definition(hir, body->instance.definition);
    if (definition == NULL || definition->kind != CM_HIR_DEFINITION_ITEM
        || definition->state != CM_HIR_DEFINITION_BOUND) {
        return NULL;
    }
    item = cm_hir_get_item(hir, definition->entity.item_id);
    source_body = cm_hir_get_body(hir, body->source_body);
    if (item == NULL || item->kind != CM_HIR_ITEM_FUNCTION
        || !cm_hir_def_id_equal(item->definition,
            body->instance.definition)
        || item->data.function_item.body != body->source_body
        || source_body == NULL
        || !cm_hir_def_id_equal(source_body->owner,
            body->instance.definition)) {
        return NULL;
    }
    return item;
}

static int cm_mir_instance_substitutions_valid(const CmHirContext *hir,
    const CmHirItem *item, const CmMirInstance *instance)
{
    uint32_t index;

    if (item == NULL || instance == NULL
        || instance->substitution_count != item->generic_parameter_count
        || (instance->substitution_count != 0u
            && instance->substitutions == NULL)) {
        return 0;
    }
    for (index = 0u; index < instance->substitution_count; ++index) {
        const CmHirGenericParam *parameter;
        CmHirGenericParamId parameter_id;

        parameter_id = (CmHirGenericParamId)(
            item->generic_parameter_start + index);
        parameter = cm_hir_get_generic_param(hir, parameter_id);
        if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_TYPE
            || !cm_hir_def_id_equal(parameter->owner, item->definition)
            || parameter->index != index
            || !cm_mir_type_is_u32(hir,
                instance->substitutions[index])) {
            return 0;
        }
    }
    return 1;
}

static int cm_mir_instantiate_u32_type(const CmHirContext *hir,
    const CmHirItem *item, const CmMirInstance *instance,
    CmHirTypeId declared, CmHirTypeId *out_type)
{
    const CmHirType *type;
    const CmHirGenericParam *parameter;
    uint32_t index;

    type = cm_hir_get_type(hir, declared);
    if (type == NULL || out_type == NULL) return 0;
    if (type->kind == CM_HIR_TYPE_INTEGER_KIND
        && (type->data.integer_type.kind == CM_HIR_INT_I32
            || type->data.integer_type.kind == CM_HIR_INT_U32
            || type->data.integer_type.kind == CM_HIR_INT_USIZE)) {
        *out_type = declared;
        return 1;
    }
    if (type->kind == CM_HIR_TYPE_BOOL_KIND) {
        *out_type = declared;
        return 1;
    }
    if (type->kind == CM_HIR_TYPE_ADT_KIND
        && type->data.named_type.argument_count == 0u
        && type->data.named_type.arguments == NULL
        && cm_mir_named_struct(hir,
            type->data.named_type.definition) != NULL) {
        *out_type = declared;
        return 1;
    }
    if (type->kind != CM_HIR_TYPE_PARAMETER_KIND) return 0;
    parameter = cm_hir_get_generic_param(hir,
        type->data.parameter_type.parameter);
    if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_TYPE
        || !cm_hir_def_id_equal(parameter->owner, item->definition)
        || parameter->index >= instance->substitution_count) {
        return 0;
    }
    index = parameter->index;
    if (item->generic_parameter_start + index
            != type->data.parameter_type.parameter
        || !cm_mir_type_is_u32(hir, instance->substitutions[index])) {
        return 0;
    }
    *out_type = instance->substitutions[index];
    return 1;
}

static int cm_mir_move_operand_valid(const CmHirContext *hir,
    const CmMirBody *body, const CmMirOperand *operand)
{
    if (body == NULL || operand == NULL || operand->type == CM_HIR_TYPE_NONE) {
        return 0;
    }
    if (operand->kind == CM_MIR_OPERAND_MOVE) {
        return cm_mir_local_id_valid(body, operand->data.local)
            && cm_mir_type_equal(hir,
                body->locals[operand->data.local].type, operand->type);
    }
    if (operand->kind == CM_MIR_OPERAND_MOVE_PLACE
        || operand->kind == CM_MIR_OPERAND_COPY_PLACE) {
        return cm_mir_place_valid(hir, body, &operand->data.place)
            && cm_mir_type_equal(hir, operand->data.place.type,
                operand->type);
    }
    return 0;
}

static int cm_mir_unsigned_operand_valid(const CmHirContext *hir,
    const CmMirBody *body, const CmMirOperand *operand,
    unsigned int pointer_bits)
{
    if (operand == NULL || !cm_mir_type_is_unsigned_scalar(hir,
            operand->type, pointer_bits)) {
        return 0;
    }
    if (operand->kind == CM_MIR_OPERAND_MOVE) {
        return cm_mir_move_operand_valid(hir, body, operand);
    }
    if (operand->kind == CM_MIR_OPERAND_MOVE_PLACE
        || operand->kind == CM_MIR_OPERAND_COPY_PLACE) {
        return cm_mir_move_operand_valid(hir, body, operand);
    }
    if (cm_mir_type_is_u32(hir, operand->type)) {
        return operand->kind == CM_MIR_CONSTANT_U32;
    }
    return operand->kind == CM_MIR_CONSTANT_USIZE
        && cm_mir_usize_value_valid(pointer_bits,
            operand->data.usize_value);
}

static int cm_mir_operand_valid(const CmHirContext *hir,
    const CmMirBody *body, const CmMirOperand *operand,
    unsigned int pointer_bits)
{
    if (operand == NULL) return 0;
    if (operand->kind == CM_MIR_CONSTANT_I32) {
        return cm_mir_type_is_i32(hir, operand->type);
    }
    if (operand->kind == CM_MIR_CONSTANT_U32) {
        return cm_mir_type_is_u32(hir, operand->type);
    }
    if (operand->kind == CM_MIR_CONSTANT_USIZE) {
        return cm_mir_type_is_usize(hir, operand->type)
            && cm_mir_usize_value_valid(pointer_bits,
                operand->data.usize_value);
    }
    return cm_mir_move_operand_valid(hir, body, operand);
}

static int cm_mir_aggregate_rvalue_valid(const CmHirContext *hir,
    const CmMirBody *body, const CmMirRvalue *rvalue,
    unsigned int pointer_bits)
{
    const CmHirBody *source_body;
    const CmHirType *type;
    const CmHirItem *item;
    uint32_t index;

    source_body = cm_hir_get_body(hir, body->source_body);
    type = cm_hir_get_type(hir, rvalue->type);
    item = type == NULL || type->kind != CM_HIR_TYPE_ADT_KIND
            || type->data.named_type.argument_count != 0u
            || type->data.named_type.arguments != NULL
            || !cm_hir_def_id_equal(type->data.named_type.definition,
                rvalue->data.aggregate.definition)
        ? NULL : cm_mir_named_struct(hir,
            rvalue->data.aggregate.definition);
    if (source_body == NULL || source_body->state != CM_HIR_BODY_TYPED
        || item == NULL
        || item->definition.crate_id != source_body->owner.crate_id
        || item->data.aggregate_item.field_count
            != rvalue->data.aggregate.field_count
        || rvalue->data.aggregate.field_count
            > CM_MIR_MAX_AGGREGATE_FIELDS
        || (rvalue->data.aggregate.field_count == 0u)
            != (rvalue->data.aggregate.fields == NULL)
        || !cm_mir_span_within(rvalue->span, source_body->span)) {
        return 0;
    }
    for (index = 0u; index < rvalue->data.aggregate.field_count; ++index) {
        const CmMirAggregateField *field;

        field = &rvalue->data.aggregate.fields[index];
        if (field->field_index != index
            || !cm_mir_operand_valid(hir, body, &field->value,
                pointer_bits)
            || !cm_mir_type_equal(hir, field->value.type,
                item->data.aggregate_item.fields[index].type)) {
            return 0;
        }
    }
    return 1;
}

static int cm_mir_rvalue_valid(const CmHirContext *hir,
    const CmMirBody *body, const CmMirRvalue *rvalue,
    unsigned int pointer_bits)
{
    if (hir == NULL || body == NULL || rvalue == NULL
        || cm_hir_get_type(hir, rvalue->type) == NULL) {
        return 0;
    }
    if (rvalue->kind == CM_MIR_RVALUE_USE) {
        return cm_mir_operand_valid(hir, body, &rvalue->data.use,
                pointer_bits)
            && cm_mir_type_equal(hir, rvalue->type,
                rvalue->data.use.type);
    }
    if (rvalue->kind == CM_MIR_RVALUE_BINARY) {
        return (rvalue->data.binary.operator_kind == CM_MIR_BINARY_ADD
                || rvalue->data.binary.operator_kind
                    == CM_MIR_BINARY_SUBTRACT)
            && cm_mir_unsigned_operand_valid(hir, body,
                &rvalue->data.binary.left, pointer_bits)
            && cm_mir_unsigned_operand_valid(hir, body,
                &rvalue->data.binary.right, pointer_bits)
            && cm_mir_type_equal(hir, rvalue->type,
                rvalue->data.binary.left.type)
            && cm_mir_type_equal(hir, rvalue->type,
                rvalue->data.binary.right.type)
            && cm_mir_type_is_unsigned_scalar(hir, rvalue->type,
                pointer_bits);
    }
    if (rvalue->kind == CM_MIR_RVALUE_EQUAL) {
        return cm_mir_type_is_bool(hir, rvalue->type)
            && cm_mir_type_is_u32(hir, rvalue->data.equal.left.type)
            && cm_mir_type_is_u32(hir, rvalue->data.equal.right.type)
            && cm_mir_unsigned_operand_valid(hir, body,
                &rvalue->data.equal.left, pointer_bits)
            && cm_mir_unsigned_operand_valid(hir, body,
                &rvalue->data.equal.right, pointer_bits);
    }
    if (rvalue->kind == CM_MIR_RVALUE_LESS) {
        return cm_mir_type_is_bool(hir, rvalue->type)
            && cm_mir_type_is_usize(hir, rvalue->data.less.left.type)
            && cm_mir_type_is_usize(hir, rvalue->data.less.right.type)
            && cm_mir_unsigned_operand_valid(hir, body,
                &rvalue->data.less.left, pointer_bits)
            && cm_mir_unsigned_operand_valid(hir, body,
                &rvalue->data.less.right, pointer_bits);
    }
    if (rvalue->kind == CM_MIR_RVALUE_AGGREGATE) {
        return cm_mir_aggregate_rvalue_valid(hir, body, rvalue,
            pointer_bits);
    }
    return 0;
}

static int cm_mir_destination_type(const CmHirContext *hir,
    const CmMirBody *body, CmMirLocalId legacy,
    const CmMirPlace *place, CmHirTypeId *out_type)
{
    if (!cm_mir_local_id_valid(body, legacy) || out_type == NULL) return 0;
    if (!cm_mir_place_present(place)) {
        *out_type = body->locals[legacy].type;
        return 1;
    }
    if (place->base != legacy || !cm_mir_place_valid(hir, body, place)) {
        return 0;
    }
    *out_type = place->type;
    return 1;
}

typedef struct CmMirTreeMatch {
    const CmHirContext *hir;
    const CmHirItem *item;
    const CmMirBody *body;
    unsigned int pointer_bits;
    uint32_t basic_block_index;
    uint32_t statement_index;
    CmMirLocalId next_temporary;
    uint32_t visible_local_count;
    CmHirExprId allowed_if_expression;
    CmMirFieldProjection expected_projections[
        CM_MIR_EXPRESSION_RECURSION_LIMIT];
    size_t expected_projection_count;
} CmMirTreeMatch;

static int cm_mir_place_equal(const CmHirContext *hir,
    const CmMirPlace *left, const CmMirPlace *right)
{
    uint32_t index;

    if (left->base != right->base
        || !cm_mir_type_equal(hir, left->type, right->type)
        || left->projection_count != right->projection_count
        || left->span.source != right->span.source
        || left->span.start != right->span.start
        || left->span.end != right->span.end
        || (left->projection_count == 0u)
            != (left->projections == NULL)
        || (right->projection_count == 0u)
            != (right->projections == NULL)) {
        return 0;
    }
    for (index = 0u; index < left->projection_count; ++index) {
        if (!cm_hir_def_id_equal(left->projections[index].definition,
                right->projections[index].definition)
            || left->projections[index].field_index
                != right->projections[index].field_index) {
            return 0;
        }
    }
    return 1;
}

static int cm_mir_operand_equal(const CmHirContext *hir,
    const CmMirOperand *actual, const CmMirOperand *expected)
{
    if (actual->kind != expected->kind
        || !cm_mir_type_equal(hir, actual->type, expected->type)) {
        return 0;
    }
    if (actual->kind == CM_MIR_OPERAND_MOVE) {
        return actual->data.local == expected->data.local;
    }
    if (actual->kind == CM_MIR_OPERAND_MOVE_PLACE
        || actual->kind == CM_MIR_OPERAND_COPY_PLACE) {
        return cm_mir_place_equal(hir, &actual->data.place,
            &expected->data.place);
    }
    if (actual->kind == CM_MIR_CONSTANT_U32) {
        return actual->data.u32_value == expected->data.u32_value;
    }
    if (actual->kind == CM_MIR_CONSTANT_USIZE) {
        return actual->data.usize_value == expected->data.usize_value;
    }
    if (actual->kind == CM_MIR_CONSTANT_I32) {
        return actual->data.i32_value == expected->data.i32_value;
    }
    return 0;
}

static int cm_mir_use_assignment_matches(CmMirTreeMatch *match,
    CmMirLocalId destination, CmHirTypeId instantiated,
    const CmMirOperand *operand)
{
    const CmMirBasicBlock *block;
    const CmMirStatement *statement;
    const CmMirRvalue *rvalue;

    if (!cm_mir_local_id_valid(match->body, destination)
        || match->basic_block_index >= match->body->basic_block_count) {
        return 0;
    }
    block = &match->body->basic_blocks[match->basic_block_index];
    if (match->statement_index >= block->statement_count) return 0;
    statement = &block->statements[match->statement_index];
    rvalue = &statement->data.assign.value;
    if (statement->kind != CM_MIR_STATEMENT_ASSIGN
        || statement->data.assign.destination != destination
        || cm_mir_place_present(&statement->data.assign.destination_place)
        || rvalue->kind != CM_MIR_RVALUE_USE
        || !cm_mir_type_equal(match->hir, rvalue->type, instantiated)
        || !cm_mir_type_equal(match->hir,
            match->body->locals[destination].type, instantiated)
        || !cm_mir_operand_equal(match->hir, &rvalue->data.use, operand)) {
        return 0;
    }
    ++match->statement_index;
    return 1;
}

static int cm_mir_expression_matches(CmMirTreeMatch *match,
    CmHirExprId expression_id, int has_destination,
    CmMirLocalId requested_destination, size_t depth,
    CmMirOperand *out_operand)
{
    const CmHirExpr *expression;
    CmHirTypeId instantiated;

    if (depth >= match->hir->expressions.len
        || depth >= CM_MIR_EXPRESSION_RECURSION_LIMIT) {
        return 0;
    }
    expression = cm_hir_get_expr(match->hir, expression_id);
    if (expression == NULL
        || expression->owner_body != match->body->source_body
        || !cm_mir_instantiate_u32_type(match->hir, match->item,
            &match->body->instance, expression->type, &instantiated)) {
        return 0;
    }
    memset(out_operand, 0, sizeof(*out_operand));
    out_operand->type = instantiated;
    if (expression->kind == CM_HIR_EXPR_BLOCK) {
        const CmHirExpr *tail;

        tail = cm_hir_get_expr(match->hir,
            expression->data.block.tail_expression);
        return expression->data.block.statement_count == 0u
            && expression->data.block.statements == NULL
            && tail != NULL
            && tail->owner_body == match->body->source_body
            && cm_mir_type_equal(match->hir, expression->type, tail->type)
            && cm_mir_span_within(tail->span, expression->span)
            && cm_mir_expression_matches(match,
                expression->data.block.tail_expression, has_destination,
                requested_destination, depth + 1u, out_operand);
    }
    if (expression->kind == CM_HIR_EXPR_LOCAL) {
        if (expression->data.local.local_index == UINT32_MAX
            || expression->data.local.local_index
                >= match->visible_local_count) {
            return 0;
        }
        out_operand->kind = CM_MIR_OPERAND_MOVE;
        out_operand->data.local = expression->data.local.local_index + 1u;
        if (has_destination) {
            if (!cm_mir_use_assignment_matches(match, requested_destination,
                    instantiated, out_operand)) {
                return 0;
            }
            out_operand->data.local = requested_destination;
        }
        return 1;
    }
    if (expression->kind == CM_HIR_EXPR_INTEGER) {
        if (expression->data.integer.high_bits != 0u
            || (cm_mir_type_is_u32(match->hir, instantiated)
                && expression->data.integer.low_bits > (uint64_t)UINT32_MAX)
            || (cm_mir_type_is_i32(match->hir, instantiated)
                && expression->data.integer.low_bits > (uint64_t)INT32_MAX)
            || (cm_mir_type_is_usize(match->hir, instantiated)
                && !cm_mir_usize_value_valid(match->pointer_bits,
                    expression->data.integer.low_bits))
            || (!cm_mir_type_is_u32(match->hir, instantiated)
                && !cm_mir_type_is_i32(match->hir, instantiated)
                && !cm_mir_type_is_usize(match->hir, instantiated))) {
            return 0;
        }
        if (cm_mir_type_is_i32(match->hir, instantiated)) {
            out_operand->kind = CM_MIR_CONSTANT_I32;
            out_operand->data.i32_value =
                (int32_t)expression->data.integer.low_bits;
        } else if (cm_mir_type_is_u32(match->hir, instantiated)) {
            out_operand->kind = CM_MIR_CONSTANT_U32;
            out_operand->data.u32_value =
                (uint32_t)expression->data.integer.low_bits;
        } else {
            out_operand->kind = CM_MIR_CONSTANT_USIZE;
            out_operand->data.usize_value =
                expression->data.integer.low_bits;
        }
        if (has_destination) {
            if (!cm_mir_use_assignment_matches(match, requested_destination,
                    instantiated, out_operand)) {
                return 0;
            }
            out_operand->kind = CM_MIR_OPERAND_MOVE;
            out_operand->data.local = requested_destination;
        }
        return 1;
    }
    if (expression->kind == CM_HIR_EXPR_FIELD) {
        CmMirOperand base;
        CmMirPlace place;
        size_t projection_start;
        uint32_t base_projection_count;

        if (!cm_mir_expression_matches(match, expression->data.field.base,
                0, CM_MIR_RETURN_LOCAL, depth + 1u, &base)) {
            return 0;
        }
        memset(&place, 0, sizeof(place));
        if (base.kind == CM_MIR_OPERAND_MOVE) {
            place.base = base.data.local;
            base_projection_count = 0u;
        } else if (base.kind == CM_MIR_OPERAND_MOVE_PLACE
            || base.kind == CM_MIR_OPERAND_COPY_PLACE) {
            place.base = base.data.place.base;
            base_projection_count = base.data.place.projection_count;
        } else {
            return 0;
        }
        if (base_projection_count >= CM_MIR_MAX_PLACE_PROJECTIONS
            || match->expected_projection_count
                > CM_MIR_EXPRESSION_RECURSION_LIMIT
                    - ((size_t)base_projection_count + 1u)) {
            return 0;
        }
        projection_start = match->expected_projection_count;
        place.projections = &match->expected_projections[projection_start];
        place.projection_count = base_projection_count + 1u;
        if (base_projection_count != 0u) {
            memcpy(place.projections, base.data.place.projections,
                (size_t)base_projection_count
                    * sizeof(CmMirFieldProjection));
        }
        place.projections[base_projection_count].definition =
            expression->data.field.definition;
        place.projections[base_projection_count].field_index =
            expression->data.field.field_index;
        match->expected_projection_count += place.projection_count;
        place.type = instantiated;
        place.span = expression->span;
        out_operand->kind = cm_mir_type_is_i32(match->hir, instantiated)
                || cm_mir_type_is_u32(match->hir, instantiated)
            ? CM_MIR_OPERAND_COPY_PLACE : CM_MIR_OPERAND_MOVE_PLACE;
        out_operand->data.place = place;
        if (!cm_mir_place_valid(match->hir, match->body, &place)) return 0;
        if (has_destination) {
            if (!cm_mir_use_assignment_matches(match, requested_destination,
                    instantiated, out_operand)) {
                return 0;
            }
            out_operand->kind = CM_MIR_OPERAND_MOVE;
            out_operand->data.local = requested_destination;
        }
        return 1;
    }
    if (expression->kind == CM_HIR_EXPR_AGGREGATE) {
        const CmMirBasicBlock *block;
        const CmMirStatement *statement;
        const CmMirRvalue *rvalue;
        CmMirOperand fields[CM_MIR_MAX_AGGREGATE_FIELDS];
        int seen[CM_MIR_MAX_AGGREGATE_FIELDS];
        CmMirLocalId destination;
        uint32_t index;

        if (expression->data.aggregate.field_count
                > CM_MIR_MAX_AGGREGATE_FIELDS
            || (expression->data.aggregate.field_count == 0u)
                != (expression->data.aggregate.fields == NULL)) {
            return 0;
        }
        memset(fields, 0, sizeof(fields));
        memset(seen, 0, sizeof(seen));
        for (index = 0u; index < expression->data.aggregate.field_count;
             ++index) {
            const CmHirAggregateFieldValue *field;

            field = &expression->data.aggregate.fields[index];
            if (field->field_index >= expression->data.aggregate.field_count
                || seen[field->field_index]
                || !cm_mir_expression_matches(match, field->value, 0,
                    CM_MIR_RETURN_LOCAL, depth + 1u,
                    &fields[field->field_index])) {
                return 0;
            }
            seen[field->field_index] = 1;
        }
        if (match->basic_block_index >= match->body->basic_block_count) {
            return 0;
        }
        block = &match->body->basic_blocks[match->basic_block_index];
        if (match->statement_index >= block->statement_count) return 0;
        statement = &block->statements[match->statement_index];
        rvalue = &statement->data.assign.value;
        destination = has_destination ? requested_destination
                                      : match->next_temporary;
        if (!cm_mir_local_id_valid(match->body, destination)
            || statement->kind != CM_MIR_STATEMENT_ASSIGN
            || statement->data.assign.destination != destination
            || cm_mir_place_present(
                &statement->data.assign.destination_place)
            || rvalue->kind != CM_MIR_RVALUE_AGGREGATE
            || !cm_hir_def_id_equal(rvalue->data.aggregate.definition,
                expression->data.aggregate.definition)
            || rvalue->data.aggregate.field_count
                != expression->data.aggregate.field_count
            || rvalue->span.source != expression->span.source
            || rvalue->span.start != expression->span.start
            || rvalue->span.end != expression->span.end
            || !cm_mir_type_equal(match->hir, rvalue->type, instantiated)
            || !cm_mir_type_equal(match->hir,
                match->body->locals[destination].type, instantiated)) {
            return 0;
        }
        for (index = 0u; index < rvalue->data.aggregate.field_count;
             ++index) {
            if (rvalue->data.aggregate.fields[index].field_index != index
                || !cm_mir_operand_equal(match->hir,
                    &rvalue->data.aggregate.fields[index].value,
                    &fields[index])) {
                return 0;
            }
        }
        ++match->statement_index;
        if (!has_destination) {
            if (match->body->locals[destination].kind
                    != CM_MIR_LOCAL_TEMPORARY
                || destination == UINT32_MAX) {
                return 0;
            }
            ++match->next_temporary;
        }
        out_operand->kind = CM_MIR_OPERAND_MOVE;
        out_operand->data.local = destination;
        return 1;
    }
    if (expression->kind == CM_HIR_EXPR_BINARY) {
        const CmMirBasicBlock *block;
        const CmMirStatement *statement;
        const CmMirRvalue *rvalue;
        CmMirOperand left;
        CmMirOperand right;
        CmMirLocalId destination;

        if ((expression->data.binary.operator_kind != CM_HIR_BINARY_ADD
                && expression->data.binary.operator_kind
                    != CM_HIR_BINARY_SUBTRACT
                && expression->data.binary.operator_kind
                    != CM_HIR_BINARY_EQUAL
                && expression->data.binary.operator_kind
                    != CM_HIR_BINARY_LESS)
            || !cm_mir_expression_matches(match,
                expression->data.binary.left, 0, CM_MIR_RETURN_LOCAL,
                depth + 1u, &left)
            || !cm_mir_expression_matches(match,
                expression->data.binary.right, 0, CM_MIR_RETURN_LOCAL,
                depth + 1u, &right)
            || match->basic_block_index >= match->body->basic_block_count) {
            return 0;
        }
        block = &match->body->basic_blocks[match->basic_block_index];
        if (match->statement_index >= block->statement_count) return 0;
        statement = &block->statements[match->statement_index];
        rvalue = &statement->data.assign.value;
        destination = has_destination ? requested_destination
                                      : match->next_temporary;
        if (!cm_mir_local_id_valid(match->body, destination)
            || statement->kind != CM_MIR_STATEMENT_ASSIGN
            || statement->data.assign.destination != destination
            || cm_mir_place_present(
                &statement->data.assign.destination_place)
            || !cm_mir_type_equal(match->hir, rvalue->type, instantiated)
            || !cm_mir_type_equal(match->hir,
                match->body->locals[destination].type, instantiated)) {
            return 0;
        }
        if (expression->data.binary.operator_kind == CM_HIR_BINARY_EQUAL) {
            if (rvalue->kind != CM_MIR_RVALUE_EQUAL
                || !cm_mir_type_is_bool(match->hir, instantiated)
                || !cm_mir_operand_equal(match->hir,
                    &rvalue->data.equal.left, &left)
                || !cm_mir_operand_equal(match->hir,
                    &rvalue->data.equal.right, &right)) {
                return 0;
            }
        } else if (expression->data.binary.operator_kind
                == CM_HIR_BINARY_LESS) {
            if (rvalue->kind != CM_MIR_RVALUE_LESS
                || !cm_mir_type_is_bool(match->hir, instantiated)
                || !cm_mir_type_is_usize(match->hir, left.type)
                || !cm_mir_type_is_usize(match->hir, right.type)
                || !cm_mir_operand_equal(match->hir,
                    &rvalue->data.less.left, &left)
                || !cm_mir_operand_equal(match->hir,
                    &rvalue->data.less.right, &right)) {
                return 0;
            }
        } else if (rvalue->kind != CM_MIR_RVALUE_BINARY
            || rvalue->data.binary.operator_kind
                != (expression->data.binary.operator_kind
                        == CM_HIR_BINARY_ADD
                    ? CM_MIR_BINARY_ADD : CM_MIR_BINARY_SUBTRACT)
            || !cm_mir_operand_equal(match->hir,
                &rvalue->data.binary.left, &left)
            || !cm_mir_operand_equal(match->hir,
                &rvalue->data.binary.right, &right)) {
            return 0;
        }
        ++match->statement_index;
        if (!has_destination) {
            if (match->body->locals[destination].kind
                    != CM_MIR_LOCAL_TEMPORARY
                || destination == UINT32_MAX) {
                return 0;
            }
            ++match->next_temporary;
        }
        out_operand->kind = CM_MIR_OPERAND_MOVE;
        out_operand->data.local = destination;
        return 1;
    }
    if (expression->kind == CM_HIR_EXPR_IF) {
        const CmMirBasicBlock *switch_block;
        const CmMirBasicBlock *then_end_block;
        const CmMirBasicBlock *else_end_block;
        const CmMirTerminator *switch_terminator;
        CmMirOperand condition;
        CmMirOperand branch_result;
        CmMirLocalId destination;
        CmMirBasicBlockId switch_block_id;
        CmMirBasicBlockId then_end;
        CmMirBasicBlockId else_end;
        CmMirBasicBlockId join;

        if (expression_id != match->allowed_if_expression
            || !cm_mir_expression_matches(match,
                expression->data.if_expr.condition, 0,
                CM_MIR_RETURN_LOCAL, depth + 1u, &condition)
            || condition.kind != CM_MIR_OPERAND_MOVE
            || !cm_mir_type_is_bool(match->hir, condition.type)
            || match->basic_block_index >= match->body->basic_block_count) {
            return 0;
        }
        destination = has_destination ? requested_destination
                                      : match->next_temporary;
        if (!cm_mir_local_id_valid(match->body, destination)
            || !cm_mir_type_equal(match->hir,
                match->body->locals[destination].type, instantiated)) {
            return 0;
        }
        if (!has_destination) {
            if (match->body->locals[destination].kind
                    != CM_MIR_LOCAL_TEMPORARY
                || destination == UINT32_MAX) {
                return 0;
            }
            ++match->next_temporary;
        }
        switch_block_id = match->basic_block_index;
        switch_block = &match->body->basic_blocks[switch_block_id];
        switch_terminator = &switch_block->terminator;
        if (match->statement_index != switch_block->statement_count
            || switch_terminator->kind != CM_MIR_TERMINATOR_SWITCH_BOOL
            || !cm_mir_operand_equal(match->hir,
                &switch_terminator->data.switch_bool.condition, &condition)
            || switch_terminator->data.switch_bool.true_target
                != switch_block_id + 1u
            || switch_terminator->data.switch_bool.true_target
                >= match->body->basic_block_count
            || switch_terminator->data.switch_bool.false_target
                >= match->body->basic_block_count) {
            return 0;
        }

        match->basic_block_index =
            switch_terminator->data.switch_bool.true_target;
        match->statement_index = 0u;
        if (!cm_mir_expression_matches(match,
                expression->data.if_expr.then_expression, 1, destination,
                depth + 1u, &branch_result)
            || branch_result.kind != CM_MIR_OPERAND_MOVE
            || branch_result.data.local != destination
            || match->basic_block_index >= match->body->basic_block_count) {
            return 0;
        }
        then_end = match->basic_block_index;
        then_end_block = &match->body->basic_blocks[then_end];
        if (match->statement_index != then_end_block->statement_count
            || then_end_block->terminator.kind != CM_MIR_TERMINATOR_GOTO
            || switch_terminator->data.switch_bool.false_target
                != then_end + 1u) {
            return 0;
        }

        match->basic_block_index =
            switch_terminator->data.switch_bool.false_target;
        match->statement_index = 0u;
        if (!cm_mir_expression_matches(match,
                expression->data.if_expr.else_expression, 1, destination,
                depth + 1u, &branch_result)
            || branch_result.kind != CM_MIR_OPERAND_MOVE
            || branch_result.data.local != destination
            || match->basic_block_index >= match->body->basic_block_count) {
            return 0;
        }
        else_end = match->basic_block_index;
        else_end_block = &match->body->basic_blocks[else_end];
        join = then_end_block->terminator.data.goto_block.target;
        if (match->statement_index != else_end_block->statement_count
            || else_end_block->terminator.kind != CM_MIR_TERMINATOR_GOTO
            || else_end_block->terminator.data.goto_block.target != join
            || join != else_end + 1u
            || join >= match->body->basic_block_count) {
            return 0;
        }
        match->basic_block_index = join;
        match->statement_index = 0u;
        out_operand->kind = CM_MIR_OPERAND_MOVE;
        out_operand->data.local = destination;
        return 1;
    }
    if (expression->kind == CM_HIR_EXPR_CALL) {
        const CmMirBasicBlock *block;
        const CmMirTerminator *terminator;
        CmMirOperand arguments[2];
        CmMirLocalId destination;
        uint32_t index;

        if (expression->data.call.argument_count == 0u
            || expression->data.call.argument_count > 2u
            || expression->data.call.arguments == NULL
            || (expression->data.call.type_substitution_count != 0u
                && expression->data.call.type_substitutions == NULL)) {
            return 0;
        }
        memset(arguments, 0, sizeof(arguments));
        for (index = 0u; index < expression->data.call.argument_count;
             ++index) {
            if (!cm_mir_expression_matches(match,
                    expression->data.call.arguments[index], 0,
                    CM_MIR_RETURN_LOCAL,
                    depth + 1u, &arguments[index])
                || !cm_mir_operand_valid(match->hir, match->body,
                    &arguments[index], match->pointer_bits)) {
                return 0;
            }
        }
        if (match->basic_block_index >= match->body->basic_block_count) {
            return 0;
        }
        block = &match->body->basic_blocks[match->basic_block_index];
        terminator = &block->terminator;
        destination = has_destination ? requested_destination
                                      : match->next_temporary;
        if (match->statement_index != block->statement_count
            || terminator->kind != CM_MIR_TERMINATOR_CALL
            || !cm_mir_local_id_valid(match->body, destination)
            || terminator->data.call.destination != destination
            || cm_mir_place_present(
                &terminator->data.call.destination_place)
            || terminator->data.call.target
                != match->basic_block_index + 1u
            || terminator->data.call.target
                >= match->body->basic_block_count
            || !cm_hir_def_id_equal(expression->data.call.callee,
                terminator->data.call.callee.definition)
            || expression->data.call.type_substitution_count
                != terminator->data.call.callee.substitution_count
            || expression->data.call.argument_count
                != terminator->data.call.argument_count
            || !cm_mir_type_equal(match->hir,
                match->body->locals[destination].type, instantiated)) {
            return 0;
        }
        for (index = 0u;
             index < expression->data.call.type_substitution_count;
             ++index) {
            CmHirTypeId substitution;

            if (!cm_mir_instantiate_u32_type(match->hir, match->item,
                    &match->body->instance,
                    expression->data.call.type_substitutions[index],
                    &substitution)
                || substitution
                    != terminator->data.call.callee.substitutions[index]) {
                return 0;
            }
        }
        for (index = 0u; index < expression->data.call.argument_count;
             ++index) {
            if (!cm_mir_operand_equal(match->hir,
                    &terminator->data.call.arguments[index],
                    &arguments[index])) {
                return 0;
            }
        }
        if (!has_destination) {
            if (match->body->locals[destination].kind
                    != CM_MIR_LOCAL_TEMPORARY
                || destination == UINT32_MAX) {
                return 0;
            }
            ++match->next_temporary;
        }
        out_operand->kind = CM_MIR_OPERAND_MOVE;
        out_operand->data.local = destination;
        match->basic_block_index = terminator->data.call.target;
        match->statement_index = 0u;
        return 1;
    }
    return 0;
}

static int cm_mir_root_shape_valid(const CmHirContext *hir,
    const CmHirItem *item, const CmMirBody *body,
    unsigned int pointer_bits)
{
    const CmHirBody *source_body;
    const CmHirExpr *root;
    const CmMirBasicBlock *final_block;
    const CmHirFunctionSignature *signature;
    CmMirTreeMatch match;
    CmMirOperand result;
    CmHirExprId root_id;
    uint32_t statement_index;

    source_body = cm_hir_get_body(hir, body->source_body);
    if (source_body == NULL || source_body->state != CM_HIR_BODY_TYPED
        || source_body->root_expression == CM_HIR_EXPR_NONE
        || body->basic_block_count == 0u) {
        return 0;
    }
    signature = &item->data.function_item.signature;
    memset(&match, 0, sizeof(match));
    match.hir = hir;
    match.item = item;
    match.body = body;
    match.pointer_bits = pointer_bits;
    match.next_temporary = source_body->local_count + 1u;
    match.visible_local_count = signature->parameter_count;
    root_id = source_body->root_expression;
    root = cm_hir_get_expr(hir, root_id);
    if (root == NULL) return 0;
    if (root->kind == CM_HIR_EXPR_BLOCK) {
        if (root->owner_body != body->source_body
            || !cm_mir_type_equal(hir, root->type,
                source_body->expected_type)
            || root->data.block.tail_expression == CM_HIR_EXPR_NONE
            || root->data.block.statement_count
                != source_body->local_count - signature->parameter_count
            || (root->data.block.statement_count == 0u)
                != (root->data.block.statements == NULL)) {
            return 0;
        }
        for (statement_index = 0u;
             statement_index < root->data.block.statement_count;
             ++statement_index) {
            const CmHirStatement *statement;
            const CmHirLocal *local;
            CmMirLocalId destination;

            statement = &root->data.block.statements[statement_index];
            destination = signature->parameter_count + statement_index + 1u;
            if (statement->kind != CM_HIR_STATEMENT_LET
                || statement->data.let_statement.local_index
                    != signature->parameter_count + statement_index
                || statement->data.let_statement.initializer
                    == CM_HIR_EXPR_NONE
                || statement->data.let_statement.local_index
                    >= source_body->local_count) {
                return 0;
            }
            local = &source_body->locals[
                statement->data.let_statement.local_index];
            if (local->parameter_index != CM_HIR_PARAMETER_INDEX_NONE
                || local->mutability != CM_HIR_IMMUTABLE
                || body->locals[destination].kind != CM_MIR_LOCAL_USER
                || !cm_mir_expression_matches(&match,
                    statement->data.let_statement.initializer, 1,
                    destination, 0u, &result)
                || result.kind != CM_MIR_OPERAND_MOVE
                || result.data.local != destination) {
                return 0;
            }
            ++match.visible_local_count;
        }
        root_id = root->data.block.tail_expression;
        root = cm_hir_get_expr(hir, root_id);
        if (root == NULL) return 0;
    } else if (source_body->local_count != signature->parameter_count) {
        return 0;
    }
    match.allowed_if_expression = root_id;
    if (match.visible_local_count != source_body->local_count
        || root->owner_body != body->source_body
        || !cm_mir_expression_matches(&match, root_id, 1,
            CM_MIR_RETURN_LOCAL, 0u, &result)
        || result.kind != CM_MIR_OPERAND_MOVE
        || result.data.local != CM_MIR_RETURN_LOCAL
        || match.basic_block_index + 1u != body->basic_block_count) {
        return 0;
    }
    final_block = &body->basic_blocks[match.basic_block_index];
    return match.statement_index == final_block->statement_count
        && final_block->terminator.kind == CM_MIR_TERMINATOR_RETURN
        && match.next_temporary == body->local_count;
}

static int cm_mir_exact_body_shape_valid(const CmMirContext *context,
    const CmHirContext *hir, const CmMirBody *body, int stored)
{
    const CmHirItem *item;
    const CmHirBody *source_body;
    const CmHirFunctionSignature *signature;
    CmHirTypeId instantiated;
    uint32_t index;

    if (body == NULL || (body->owned_storage != NULL) != stored
        || body->basic_block_count == 0u || body->basic_blocks == NULL) {
        return 0;
    }
    item = cm_mir_instance_function(hir, body);
    if (item == NULL || !cm_mir_instance_substitutions_valid(hir, item,
            &body->instance)) {
        return 0;
    }
    signature = &item->data.function_item.signature;
    source_body = cm_hir_get_body(hir, body->source_body);
    if (signature->is_variadic
        || signature->parameter_count == UINT32_MAX
        || source_body == NULL
        || source_body->parameter_count != signature->parameter_count
        || source_body->local_count < signature->parameter_count
        || source_body->local_count == UINT32_MAX
        || body->local_count < source_body->local_count + 1u
        || body->locals == NULL
        || !cm_mir_instantiate_u32_type(hir, item, &body->instance,
            signature->return_type, &instantiated)
        || !cm_mir_type_supported(hir, instantiated,
            context->pointer_bits)
        || !cm_mir_type_target_valid(hir, instantiated,
            context->pointer_bits, 0u)
        || cm_mir_type_is_bool(hir, instantiated)
        || body->locals[0].kind != CM_MIR_LOCAL_RETURN
        || body->locals[0].type != instantiated) {
        return 0;
    }
    for (index = 0u; index < signature->parameter_count; ++index) {
        if (source_body->locals[index].parameter_index != index
            || source_body->locals[index].type
                != signature->parameters[index].type
            || !cm_mir_instantiate_u32_type(hir, item, &body->instance,
                source_body->locals[index].type, &instantiated)
            || !cm_mir_type_supported(hir, instantiated,
                context->pointer_bits)
            || !cm_mir_type_target_valid(hir, instantiated,
                context->pointer_bits, 0u)
            || cm_mir_type_is_bool(hir, instantiated)
            || body->locals[index + 1u].kind != CM_MIR_LOCAL_ARGUMENT
            || body->locals[index + 1u].type != instantiated) {
            return 0;
        }
    }
    for (index = signature->parameter_count;
         index < source_body->local_count; ++index) {
        if (source_body->locals[index].parameter_index
                != CM_HIR_PARAMETER_INDEX_NONE
            || source_body->locals[index].mutability != CM_HIR_IMMUTABLE
            || !cm_mir_instantiate_u32_type(hir, item, &body->instance,
                source_body->locals[index].type, &instantiated)
            || !cm_mir_type_supported(hir, instantiated,
                context->pointer_bits)
            || !cm_mir_type_target_valid(hir, instantiated,
                context->pointer_bits, 0u)
            || body->locals[index + 1u].kind != CM_MIR_LOCAL_USER
            || body->locals[index + 1u].type != instantiated) {
            return 0;
        }
    }
    for (index = source_body->local_count + 1u;
        index < body->local_count; ++index) {
        if (body->locals[index].kind != CM_MIR_LOCAL_TEMPORARY
            || !cm_mir_type_supported(hir, body->locals[index].type,
                context->pointer_bits)
            || !cm_mir_type_target_valid(hir,
                body->locals[index].type, context->pointer_bits, 0u)) {
            return 0;
        }
    }

    for (index = 0u; index < body->basic_block_count; ++index) {
        const CmMirBasicBlock *block;
        uint32_t statement_index;

        block = &body->basic_blocks[index];
        if ((block->statement_count == 0u) != (block->statements == NULL)) {
            return 0;
        }
        for (statement_index = 0u;
             statement_index < block->statement_count; ++statement_index) {
            const CmMirStatement *statement;
            const CmMirRvalue *rvalue;
            CmHirTypeId destination_type;

            statement = &block->statements[statement_index];
            if (statement->kind != CM_MIR_STATEMENT_ASSIGN
                || !cm_mir_destination_type(hir, body,
                    statement->data.assign.destination,
                    &statement->data.assign.destination_place,
                    &destination_type)) {
                return 0;
            }
            rvalue = &statement->data.assign.value;
            if (!cm_mir_rvalue_valid(hir, body, rvalue,
                    context->pointer_bits)
                || !cm_mir_type_equal(hir,
                    destination_type, rvalue->type)) {
                return 0;
            }
        }

        if (block->terminator.kind == CM_MIR_TERMINATOR_CALL) {
            const CmMirBody *callee;
            const CmHirItem *callee_item;
            const CmMirTerminator *terminator;
            CmHirTypeId destination_type;
            int has_aggregate_argument;
            uint32_t argument_index;

            terminator = &block->terminator;
            callee = cm_mir_get_body(context,
                terminator->data.call.callee_instance);
            callee_item = callee == NULL ? NULL
                : cm_mir_instance_function(hir, callee);
            if (callee == NULL || cm_mir_instance_is_empty(&callee->instance)
                || callee_item == NULL || callee->local_count == 0u
                || callee->locals == NULL
                || callee->local_count
                    <= terminator->data.call.argument_count
                || !cm_mir_instance_equal(&terminator->data.call.callee,
                    &callee->instance)
                || !cm_mir_destination_type(hir, body,
                    terminator->data.call.destination,
                    &terminator->data.call.destination_place,
                    &destination_type)
                || terminator->data.call.target >= body->basic_block_count
                || terminator->data.call.argument_count
                    != callee_item->data.function_item.signature
                        .parameter_count
                || (terminator->data.call.argument_count != 0u
                    && terminator->data.call.arguments == NULL)
                || !cm_mir_type_equal(hir,
                    destination_type,
                    callee->locals[CM_MIR_RETURN_LOCAL].type)
                || !cm_mir_type_is_unsigned_scalar(hir,
                    callee->locals[CM_MIR_RETURN_LOCAL].type,
                    context->pointer_bits)) {
                return 0;
            }
            has_aggregate_argument = 0;
            for (argument_index = 0u;
                 argument_index < terminator->data.call.argument_count;
                 ++argument_index) {
                const CmMirOperand *argument;
                CmHirTypeId parameter_type;

                argument = &terminator->data.call.arguments[argument_index];
                parameter_type = callee->locals[argument_index + 1u].type;
                if ((!cm_mir_type_is_unsigned_scalar(hir, parameter_type,
                            context->pointer_bits)
                        && !cm_mir_type_is_checked_aggregate(hir,
                            parameter_type,
                            callee_item->definition.crate_id))
                    || !cm_mir_operand_valid(hir, body, argument,
                        context->pointer_bits)
                    || argument->kind == CM_MIR_CONSTANT_U32
                    || !cm_mir_type_equal(hir, argument->type,
                        parameter_type)) {
                    return 0;
                }
                if (cm_mir_type_is_checked_aggregate(hir, parameter_type,
                        callee_item->definition.crate_id)) {
                    has_aggregate_argument = 1;
                }
            }
            if (has_aggregate_argument
                && (callee_item->generic_parameter_count != 0u
                    || callee->instance.substitution_count != 0u
                    || terminator->data.call.callee.substitution_count
                        != 0u
                    || callee_item->definition.crate_id
                        != item->definition.crate_id)) {
                return 0;
            }
        } else if (block->terminator.kind == CM_MIR_TERMINATOR_GOTO) {
            if (block->terminator.data.goto_block.target
                    >= body->basic_block_count) {
                return 0;
            }
        } else if (block->terminator.kind
                == CM_MIR_TERMINATOR_SWITCH_BOOL) {
            const CmMirOperand *condition;

            condition = &block->terminator.data.switch_bool.condition;
            if (condition->kind != CM_MIR_OPERAND_MOVE
                || !cm_mir_type_is_bool(hir, condition->type)
                || !cm_mir_move_operand_valid(hir, body, condition)
                || block->terminator.data.switch_bool.true_target
                    >= body->basic_block_count
                || block->terminator.data.switch_bool.false_target
                    >= body->basic_block_count
                || block->terminator.data.switch_bool.true_target
                    == block->terminator.data.switch_bool.false_target) {
                return 0;
            }
        } else if (block->terminator.kind != CM_MIR_TERMINATOR_RETURN) {
            return 0;
        }
    }
    return cm_mir_root_shape_valid(hir, item, body,
        context->pointer_bits);
}

static int cm_mir_storage_add(size_t *total, size_t count,
    size_t element_size)
{
    size_t remainder;
    size_t padding;
    size_t bytes;
    size_t aligned;

    if (count == 0u) return 1;
    remainder = *total % CM_MIR_STORAGE_ALIGNMENT;
    padding = remainder == 0u ? 0u : CM_MIR_STORAGE_ALIGNMENT - remainder;
    return cm_size_add(*total, padding, &aligned)
        && cm_size_mul(count, element_size, &bytes)
        && cm_size_add(aligned, bytes, total);
}

static void *cm_mir_storage_take(unsigned char *storage, size_t *offset,
    size_t count, size_t element_size)
{
    size_t remainder;

    if (count == 0u) return NULL;
    remainder = *offset % CM_MIR_STORAGE_ALIGNMENT;
    if (remainder != 0u) *offset += CM_MIR_STORAGE_ALIGNMENT - remainder;
    {
        void *result;

        result = storage + *offset;
        *offset += count * element_size;
        return result;
    }
}

static int cm_mir_place_storage_size(size_t *total,
    const CmMirPlace *place)
{
    return !cm_mir_place_present(place)
        || cm_mir_storage_add(total, place->projection_count,
            sizeof(CmMirFieldProjection));
}

static int cm_mir_operand_storage_size(size_t *total,
    const CmMirOperand *operand)
{
    return operand->kind != CM_MIR_OPERAND_MOVE_PLACE
            && operand->kind != CM_MIR_OPERAND_COPY_PLACE
        ? 1 : cm_mir_place_storage_size(total, &operand->data.place);
}

static int cm_mir_rvalue_storage_size(size_t *total,
    const CmMirRvalue *rvalue)
{
    uint32_t index;

    if (rvalue->kind == CM_MIR_RVALUE_USE) {
        return cm_mir_operand_storage_size(total, &rvalue->data.use);
    }
    if (rvalue->kind == CM_MIR_RVALUE_BINARY) {
        return cm_mir_operand_storage_size(total,
                &rvalue->data.binary.left)
            && cm_mir_operand_storage_size(total,
                &rvalue->data.binary.right);
    }
    if (rvalue->kind == CM_MIR_RVALUE_EQUAL) {
        return cm_mir_operand_storage_size(total,
                &rvalue->data.equal.left)
            && cm_mir_operand_storage_size(total,
                &rvalue->data.equal.right);
    }
    if (rvalue->kind == CM_MIR_RVALUE_LESS) {
        return cm_mir_operand_storage_size(total,
                &rvalue->data.less.left)
            && cm_mir_operand_storage_size(total,
                &rvalue->data.less.right);
    }
    if (rvalue->kind != CM_MIR_RVALUE_AGGREGATE
        || !cm_mir_storage_add(total,
            rvalue->data.aggregate.field_count,
            sizeof(CmMirAggregateField))) {
        return 0;
    }
    for (index = 0u; index < rvalue->data.aggregate.field_count; ++index) {
        if (!cm_mir_operand_storage_size(total,
                &rvalue->data.aggregate.fields[index].value)) {
            return 0;
        }
    }
    return 1;
}

static void cm_mir_copy_place(unsigned char *storage, size_t *offset,
    const CmMirPlace *source, CmMirPlace *copy)
{
    *copy = *source;
    copy->projections = (CmMirFieldProjection *)cm_mir_storage_take(storage,
        offset, source->projection_count, sizeof(CmMirFieldProjection));
    if (source->projection_count != 0u) {
        memcpy(copy->projections, source->projections,
            (size_t)source->projection_count
                * sizeof(CmMirFieldProjection));
    }
}

static void cm_mir_copy_operand(unsigned char *storage, size_t *offset,
    const CmMirOperand *source, CmMirOperand *copy)
{
    *copy = *source;
    if (source->kind == CM_MIR_OPERAND_MOVE_PLACE
        || source->kind == CM_MIR_OPERAND_COPY_PLACE) {
        cm_mir_copy_place(storage, offset, &source->data.place,
            &copy->data.place);
    }
}

static void cm_mir_copy_rvalue(unsigned char *storage, size_t *offset,
    const CmMirRvalue *source, CmMirRvalue *copy)
{
    uint32_t index;

    *copy = *source;
    if (source->kind == CM_MIR_RVALUE_USE) {
        cm_mir_copy_operand(storage, offset, &source->data.use,
            &copy->data.use);
    } else if (source->kind == CM_MIR_RVALUE_BINARY) {
        cm_mir_copy_operand(storage, offset, &source->data.binary.left,
            &copy->data.binary.left);
        cm_mir_copy_operand(storage, offset, &source->data.binary.right,
            &copy->data.binary.right);
    } else if (source->kind == CM_MIR_RVALUE_EQUAL) {
        cm_mir_copy_operand(storage, offset, &source->data.equal.left,
            &copy->data.equal.left);
        cm_mir_copy_operand(storage, offset, &source->data.equal.right,
            &copy->data.equal.right);
    } else if (source->kind == CM_MIR_RVALUE_LESS) {
        cm_mir_copy_operand(storage, offset, &source->data.less.left,
            &copy->data.less.left);
        cm_mir_copy_operand(storage, offset, &source->data.less.right,
            &copy->data.less.right);
    } else if (source->kind == CM_MIR_RVALUE_AGGREGATE) {
        copy->data.aggregate.fields = (CmMirAggregateField *)
            cm_mir_storage_take(storage, offset,
                source->data.aggregate.field_count,
                sizeof(CmMirAggregateField));
        for (index = 0u; index < source->data.aggregate.field_count;
             ++index) {
            copy->data.aggregate.fields[index] =
                source->data.aggregate.fields[index];
            cm_mir_copy_operand(storage, offset,
                &source->data.aggregate.fields[index].value,
                &copy->data.aggregate.fields[index].value);
        }
    }
}

static int cm_mir_body_storage_size(const CmMirBody *body, size_t *out_size)
{
    size_t total;
    uint32_t block_index;

    total = 0u;
    if (!cm_mir_storage_add(&total, body->instance.substitution_count,
            sizeof(CmHirTypeId))
        || !cm_mir_storage_add(&total, body->local_count,
            sizeof(CmMirLocal))
        || !cm_mir_storage_add(&total, body->basic_block_count,
            sizeof(CmMirBasicBlock))) {
        return 0;
    }
    for (block_index = 0u; block_index < body->basic_block_count;
         ++block_index) {
        const CmMirTerminator *terminator;
        uint32_t statement_index;
        uint32_t argument_index;

        if (!cm_mir_storage_add(&total,
                body->basic_blocks[block_index].statement_count,
                sizeof(CmMirStatement))) {
            return 0;
        }
        for (statement_index = 0u;
             statement_index
                < body->basic_blocks[block_index].statement_count;
             ++statement_index) {
            const CmMirStatement *statement;

            statement = &body->basic_blocks[block_index]
                .statements[statement_index];
            if (!cm_mir_place_storage_size(&total,
                    &statement->data.assign.destination_place)
                || !cm_mir_rvalue_storage_size(&total,
                    &statement->data.assign.value)) {
                return 0;
            }
        }
        terminator = &body->basic_blocks[block_index].terminator;
        if (terminator->kind == CM_MIR_TERMINATOR_CALL
            && (!cm_mir_storage_add(&total,
                    terminator->data.call.argument_count,
                    sizeof(CmMirOperand))
                || !cm_mir_storage_add(&total,
                    terminator->data.call.callee.substitution_count,
                    sizeof(CmHirTypeId)))) {
            return 0;
        }
        if (terminator->kind == CM_MIR_TERMINATOR_CALL) {
            if (!cm_mir_place_storage_size(&total,
                    &terminator->data.call.destination_place)) {
                return 0;
            }
            for (argument_index = 0u;
                 argument_index < terminator->data.call.argument_count;
                 ++argument_index) {
                if (!cm_mir_operand_storage_size(&total,
                        &terminator->data.call.arguments[argument_index])) {
                    return 0;
                }
            }
        } else if (terminator->kind == CM_MIR_TERMINATOR_SWITCH_BOOL
            && !cm_mir_operand_storage_size(&total,
                &terminator->data.switch_bool.condition)) {
            return 0;
        }
    }
    *out_size = total;
    return 1;
}

static CmMirStatus cm_mir_copy_body(const CmMirBody *body, CmMirBody *copy)
{
    unsigned char *storage;
    size_t storage_size;
    size_t offset;
    uint32_t block_index;

    if (!cm_mir_body_storage_size(body, &storage_size)) {
        return CM_MIR_ID_EXHAUSTED;
    }
    storage = (unsigned char *)cm_alloc(storage_size);
    memset(copy, 0, sizeof(*copy));
    copy->owner = body->owner;
    copy->source_body = body->source_body;
    copy->instance.definition = body->instance.definition;
    copy->instance.substitution_count = body->instance.substitution_count;
    copy->local_count = body->local_count;
    copy->basic_block_count = body->basic_block_count;
    copy->owned_storage = storage;
    offset = 0u;
    copy->instance.substitutions = (CmHirTypeId *)cm_mir_storage_take(
        storage, &offset, body->instance.substitution_count,
        sizeof(CmHirTypeId));
    if (body->instance.substitution_count != 0u) {
        memcpy(copy->instance.substitutions, body->instance.substitutions,
            (size_t)body->instance.substitution_count * sizeof(CmHirTypeId));
    }
    copy->locals = (CmMirLocal *)cm_mir_storage_take(storage, &offset,
        body->local_count, sizeof(CmMirLocal));
    memcpy(copy->locals, body->locals,
        (size_t)body->local_count * sizeof(CmMirLocal));
    copy->basic_blocks = (CmMirBasicBlock *)cm_mir_storage_take(storage,
        &offset, body->basic_block_count, sizeof(CmMirBasicBlock));
    memset(copy->basic_blocks, 0,
        (size_t)body->basic_block_count * sizeof(CmMirBasicBlock));

    for (block_index = 0u; block_index < body->basic_block_count;
         ++block_index) {
        const CmMirBasicBlock *source_block;
        CmMirBasicBlock *copy_block;

        source_block = &body->basic_blocks[block_index];
        copy_block = &copy->basic_blocks[block_index];
        copy_block->statement_count = source_block->statement_count;
        copy_block->statements = (CmMirStatement *)cm_mir_storage_take(
            storage, &offset, source_block->statement_count,
            sizeof(CmMirStatement));
        if (source_block->statement_count != 0u) {
            memcpy(copy_block->statements, source_block->statements,
                (size_t)source_block->statement_count
                    * sizeof(CmMirStatement));
        }
        {
            uint32_t statement_index;

            for (statement_index = 0u;
                 statement_index < source_block->statement_count;
                 ++statement_index) {
                const CmMirStatement *source_statement;
                CmMirStatement *copy_statement;

                source_statement = &source_block->statements[statement_index];
                copy_statement = &copy_block->statements[statement_index];
                if (cm_mir_place_present(
                        &source_statement->data.assign.destination_place)) {
                    cm_mir_copy_place(storage, &offset,
                        &source_statement->data.assign.destination_place,
                        &copy_statement->data.assign.destination_place);
                }
                cm_mir_copy_rvalue(storage, &offset,
                    &source_statement->data.assign.value,
                    &copy_statement->data.assign.value);
            }
        }
        copy_block->terminator = source_block->terminator;
        if (source_block->terminator.kind == CM_MIR_TERMINATOR_CALL) {
            const CmMirTerminator *source_terminator;
            CmMirTerminator *copy_terminator;

            source_terminator = &source_block->terminator;
            copy_terminator = &copy_block->terminator;
            if (cm_mir_place_present(
                    &source_terminator->data.call.destination_place)) {
                cm_mir_copy_place(storage, &offset,
                    &source_terminator->data.call.destination_place,
                    &copy_terminator->data.call.destination_place);
            }
            copy_terminator->data.call.arguments =
                (CmMirOperand *)cm_mir_storage_take(storage, &offset,
                    source_terminator->data.call.argument_count,
                    sizeof(CmMirOperand));
            if (source_terminator->data.call.argument_count != 0u) {
                uint32_t argument_index;

                for (argument_index = 0u;
                     argument_index
                        < source_terminator->data.call.argument_count;
                     ++argument_index) {
                    cm_mir_copy_operand(storage, &offset,
                        &source_terminator->data.call
                            .arguments[argument_index],
                        &copy_terminator->data.call
                            .arguments[argument_index]);
                }
            }
            copy_terminator->data.call.callee.substitutions =
                (CmHirTypeId *)cm_mir_storage_take(storage, &offset,
                    source_terminator->data.call.callee.substitution_count,
                    sizeof(CmHirTypeId));
            if (source_terminator->data.call.callee.substitution_count != 0u) {
                memcpy(copy_terminator->data.call.callee.substitutions,
                    source_terminator->data.call.callee.substitutions,
                    (size_t)source_terminator->data.call.callee
                        .substitution_count * sizeof(CmHirTypeId));
            }
        } else if (source_block->terminator.kind
                == CM_MIR_TERMINATOR_SWITCH_BOOL) {
            cm_mir_copy_operand(storage, &offset,
                &source_block->terminator.data.switch_bool.condition,
                &copy_block->terminator.data.switch_bool.condition);
        }
    }
    return CM_MIR_OK;
}

static void cm_mir_body_storage_destroy(CmMirBody *body)
{
    if (body == NULL) return;
    cm_free(body->owned_storage);
    memset(body, 0, sizeof(*body));
}

void cm_mir_context_init(CmMirContext *context)
{
    if (context == NULL) return;
    memset(context, 0, sizeof(*context));
    cm_vec_init(&context->bodies, sizeof(CmMirBody));
}

void cm_mir_context_destroy(CmMirContext *context)
{
    size_t index;

    if (context == NULL) return;
    if (context->bodies.elem_size == sizeof(CmMirBody)) {
        for (index = 0u; index < context->bodies.len; ++index) {
            cm_mir_body_storage_destroy((CmMirBody *)cm_vec_at(
                &context->bodies, index));
        }
        cm_vec_destroy(&context->bodies);
    }
    memset(context, 0, sizeof(*context));
}

CmMirStatus cm_mir_context_set_pointer_bits(CmMirContext *context,
    unsigned int pointer_bits)
{
    if (!cm_mir_context_valid(context)
        || !cm_mir_pointer_bits_valid(pointer_bits)) {
        return CM_MIR_INVALID_ARGUMENT;
    }
    if (context->bodies.len != 0u
        || (context->pointer_bits != 0u
            && context->pointer_bits != pointer_bits)) {
        return CM_MIR_INVARIANT_VIOLATION;
    }
    context->pointer_bits = pointer_bits;
    return CM_MIR_OK;
}

unsigned int cm_mir_context_pointer_bits(const CmMirContext *context)
{
    return cm_mir_context_valid(context) ? context->pointer_bits : 0u;
}

CmMirStatus cm_mir_add_body(CmMirContext *context, const CmMirBody *body,
    CmMirBodyId *out_id)
{
    CmMirBody copy;
    CmMirStatus status;
    size_t index;

    if (out_id != NULL) *out_id = CM_MIR_BODY_NONE;
    if (!cm_mir_context_valid(context) || body == NULL || out_id == NULL) {
        return CM_MIR_INVALID_ARGUMENT;
    }
    if (!cm_mir_legacy_body_shape_valid(body)) {
        return CM_MIR_INVARIANT_VIOLATION;
    }
    if (context->bodies.len >= (size_t)UINT32_MAX) {
        return CM_MIR_ID_EXHAUSTED;
    }
    for (index = 0u; index < context->bodies.len; ++index) {
        const CmMirBody *old_body;

        old_body = (const CmMirBody *)cm_vec_at_const(&context->bodies,
            index);
        if (old_body == NULL || cm_hir_def_id_equal(old_body->owner,
                body->owner)
            || old_body->source_body == body->source_body) {
            return CM_MIR_INVARIANT_VIOLATION;
        }
    }
    cm_vec_reserve(&context->bodies, context->bodies.len + 1u);
    status = cm_mir_copy_body(body, &copy);
    if (status != CM_MIR_OK) return status;
    (void)cm_vec_push(&context->bodies, &copy);
    *out_id = (CmMirBodyId)context->bodies.len;
    return CM_MIR_OK;
}

CmMirStatus cm_mir_add_monomorphized_body(CmMirContext *context,
    const CmHirContext *hir, const CmMirBody *body, CmMirBodyId *out_id)
{
    CmMirBody copy;
    CmMirStatus status;
    size_t index;

    if (out_id != NULL) *out_id = CM_MIR_BODY_NONE;
    if (!cm_mir_context_valid(context) || hir == NULL || body == NULL
        || out_id == NULL || (context->hir_owner != NULL
            && context->hir_owner != hir)) {
        return CM_MIR_INVALID_ARGUMENT;
    }
    if (!cm_mir_exact_body_shape_valid(context, hir, body, 0)) {
        return CM_MIR_INVARIANT_VIOLATION;
    }
    if (context->bodies.len >= (size_t)UINT32_MAX) {
        return CM_MIR_ID_EXHAUSTED;
    }
    for (index = 0u; index < context->bodies.len; ++index) {
        const CmMirBody *old_body;

        old_body = (const CmMirBody *)cm_vec_at_const(&context->bodies,
            index);
        if (old_body == NULL
            || (cm_mir_instance_is_empty(&old_body->instance)
                && (cm_hir_def_id_equal(old_body->owner, body->owner)
                    || old_body->source_body == body->source_body))
            || (!cm_mir_instance_is_empty(&old_body->instance)
                && cm_mir_instance_equal(&old_body->instance,
                    &body->instance))) {
            return CM_MIR_INVARIANT_VIOLATION;
        }
    }
    cm_vec_reserve(&context->bodies, context->bodies.len + 1u);
    status = cm_mir_copy_body(body, &copy);
    if (status != CM_MIR_OK) return status;
    (void)cm_vec_push(&context->bodies, &copy);
    if (context->hir_owner == NULL) context->hir_owner = hir;
    *out_id = (CmMirBodyId)context->bodies.len;
    return CM_MIR_OK;
}

CmMirStatus cm_mir_validate_monomorphized_body(
    const CmMirContext *context, const CmHirContext *hir, CmMirBodyId id)
{
    const CmMirBody *body;

    if (!cm_mir_context_valid(context) || hir == NULL
        || context->hir_owner != hir) {
        return CM_MIR_INVALID_ARGUMENT;
    }
    body = cm_mir_get_body(context, id);
    if (body == NULL) return CM_MIR_INVALID_ID;
    return cm_mir_exact_body_shape_valid(context, hir, body, 1)
        ? CM_MIR_OK : CM_MIR_INVARIANT_VIOLATION;
}

CmMirStatus cm_mir_find_instance(const CmMirContext *context,
    CmHirDefId definition, const CmHirTypeId *substitutions,
    uint32_t substitution_count, CmMirBodyId *out_id)
{
    CmMirInstance key;
    size_t index;

    if (out_id != NULL) *out_id = CM_MIR_BODY_NONE;
    if (!cm_mir_context_valid(context) || out_id == NULL
        || cm_hir_def_id_is_none(definition)
        || (substitution_count != 0u && substitutions == NULL)) {
        return CM_MIR_INVALID_ARGUMENT;
    }
    memset(&key, 0, sizeof(key));
    key.definition = definition;
    key.substitutions = (CmHirTypeId *)substitutions;
    key.substitution_count = substitution_count;
    for (index = 0u; index < context->bodies.len; ++index) {
        const CmMirBody *body;

        body = (const CmMirBody *)cm_vec_at_const(&context->bodies, index);
        if (body == NULL) return CM_MIR_INVARIANT_VIOLATION;
        if (cm_mir_instance_equal(&body->instance, &key)) {
            *out_id = (CmMirBodyId)(index + 1u);
            return CM_MIR_OK;
        }
    }
    return CM_MIR_INVALID_ID;
}

const CmMirBody *cm_mir_get_body(const CmMirContext *context,
    CmMirBodyId id)
{
    if (!cm_mir_context_valid(context) || id == CM_MIR_BODY_NONE
        || (size_t)id > context->bodies.len) {
        return NULL;
    }
    return (const CmMirBody *)cm_vec_at_const(&context->bodies,
        (size_t)id - 1u);
}

size_t cm_mir_body_count(const CmMirContext *context)
{
    return cm_mir_context_valid(context) ? context->bodies.len : 0u;
}

const char *cm_mir_status_name(CmMirStatus status)
{
    switch (status) {
    case CM_MIR_OK: return "ok";
    case CM_MIR_INVALID_ARGUMENT: return "invalid argument";
    case CM_MIR_INVALID_ID: return "invalid id";
    case CM_MIR_ID_EXHAUSTED: return "id exhausted";
    case CM_MIR_INVARIANT_VIOLATION: return "invariant violation";
    }
    return "unknown MIR status";
}
