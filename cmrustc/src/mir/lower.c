#include "cm/mir/lower.h"
#include "cm/alloc.h"
#include "cm/hir/instance.h"
#include "cm/hir/semantic_results.h"

#include "../hir/instance_internal.h"
#include "../hir/semantic_results_internal.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define CM_MIR_FLOW_RECURSION_LIMIT ((size_t)512u)

static void cm_mir_lower_fail(CmMirLowerResult *result,
    CmMirLowerErrorKind kind, CmHirBodyId body, CmHirExprId expression,
    CmMirStatus status, const char *message)
{
    result->body = CM_MIR_BODY_NONE;
    result->lowered_body_count = 0u;
    result->error_count = 1u;
    result->first_error.kind = kind;
    result->first_error.hir_body = body;
    result->first_error.hir_expression = expression;
    result->first_error.mir_status = status;
    (void)snprintf(result->first_error.message,
        sizeof(result->first_error.message), "%s", message);
}

static int cm_mir_seen_expression(const CmVec *seen, CmHirExprId id)
{
    size_t index;

    for (index = 0u; index < seen->len; ++index) {
        const CmHirExprId *old_id;

        old_id = (const CmHirExprId *)cm_vec_at_const(seen, index);
        if (old_id != NULL && *old_id == id) return 1;
    }
    return 0;
}

static CmHirTypeId cm_mir_lower_monomorphic_self_type(
    const CmHirContext *hir, CmHirTypeId id, size_t depth);

static int cm_mir_hir_type_equal_inner(const CmHirContext *hir,
    CmHirTypeId left_id, CmHirTypeId right_id, size_t depth)
{
    const CmHirType *left;
    const CmHirType *right;

    if (depth >= CM_MIR_FLOW_RECURSION_LIMIT) return 0;
    left_id = cm_mir_lower_monomorphic_self_type(hir, left_id, depth);
    right_id = cm_mir_lower_monomorphic_self_type(hir, right_id, depth);
    if (left_id == CM_HIR_TYPE_NONE || right_id == CM_HIR_TYPE_NONE) {
        return 0;
    }
    if (left_id == right_id) return 1;
    left = cm_hir_get_type(hir, left_id);
    right = cm_hir_get_type(hir, right_id);
    if (left == NULL || right == NULL || left->kind != right->kind) return 0;
    if (left->kind == CM_HIR_TYPE_INTEGER_KIND) {
        return left->data.integer_type.kind == right->data.integer_type.kind;
    }
    if (left->kind == CM_HIR_TYPE_PARAMETER_KIND) {
        return left->data.parameter_type.parameter
            == right->data.parameter_type.parameter;
    }
    if (left->kind == CM_HIR_TYPE_BOOL_KIND) return 1;
    if (left->kind == CM_HIR_TYPE_REFERENCE_KIND) {
        return left->data.reference_type.region.kind
                == right->data.reference_type.region.kind
            && left->data.reference_type.region.kind == CM_HIR_REGION_ERASED
            && left->data.reference_type.mutability
                == right->data.reference_type.mutability
            && cm_mir_hir_type_equal_inner(hir,
                left->data.reference_type.pointee,
                right->data.reference_type.pointee, depth + 1u);
    }
    if (left->kind == CM_HIR_TYPE_ADT_KIND) {
        if (left->data.named_type.argument_count == 1u
            && right->data.named_type.argument_count == 1u
            && left->data.named_type.arguments != NULL
            && right->data.named_type.arguments != NULL
            && left->data.named_type.arguments[0].kind
                == CM_HIR_GENERIC_ARG_TYPE
            && right->data.named_type.arguments[0].kind
                == CM_HIR_GENERIC_ARG_TYPE) {
            return cm_hir_def_id_equal(left->data.named_type.definition,
                    right->data.named_type.definition)
                && cm_mir_hir_type_equal_inner(hir,
                    left->data.named_type.arguments[0].data.type,
                    right->data.named_type.arguments[0].data.type,
                    depth + 1u);
        }
        return left->data.named_type.argument_count == 0u
            && left->data.named_type.arguments == NULL
            && right->data.named_type.argument_count == 0u
            && right->data.named_type.arguments == NULL
            && cm_hir_def_id_equal(left->data.named_type.definition,
                right->data.named_type.definition);
    }
    return 0;
}

static int cm_mir_hir_type_equal(const CmHirContext *hir,
    CmHirTypeId left_id, CmHirTypeId right_id)
{
    return cm_mir_hir_type_equal_inner(hir, left_id, right_id, 0u);
}

static const CmHirExpr *cm_mir_terminal_expression(
    CmMirLowerResult *result, const CmHirContext *hir,
    const CmHirBody *body, CmHirBodyId body_id,
    CmHirExprId *out_expression_id)
{
    CmVec seen;
    CmHirExprId expression_id;
    const CmHirExpr *expression;

    cm_vec_init(&seen, sizeof(CmHirExprId));
    expression_id = body->root_expression;
    expression = NULL;
    for (;;) {
        if (expression_id == CM_HIR_EXPR_NONE
            || cm_mir_seen_expression(&seen, expression_id)) {
            cm_mir_lower_fail(result, CM_MIR_LOWER_INVALID_HIR, body_id,
                expression_id, CM_MIR_OK,
                "typed HIR body contains a missing or cyclic expression");
            break;
        }
        (void)cm_vec_push(&seen, &expression_id);
        expression = cm_hir_get_expr(hir, expression_id);
        if (expression == NULL || !cm_mir_hir_type_equal(hir,
                expression->type, body->expected_type)) {
            cm_mir_lower_fail(result, CM_MIR_LOWER_INVALID_HIR, body_id,
                expression_id, CM_MIR_OK,
                "typed HIR expression is absent or has the wrong type");
            expression = NULL;
            break;
        }
        if (expression->kind != CM_HIR_EXPR_BLOCK) break;
        if (expression->data.block.statement_count != 0u) {
            cm_mir_lower_fail(result,
                CM_MIR_LOWER_UNSUPPORTED_EXPRESSION, body_id,
                expression_id, CM_MIR_OK,
                "legacy MIR lowering does not accept body statements");
            expression = NULL;
            break;
        }
        expression_id = expression->data.block.tail_expression;
    }
    cm_vec_destroy(&seen);
    if (expression != NULL && out_expression_id != NULL) {
        *out_expression_id = expression_id;
    }
    return expression;
}

static int cm_mir_i32_constant(const CmHirExpr *expression,
    int32_t *out_value)
{
    uint32_t low_bits;
    uint64_t upper_low_bits;

    if (expression->data.integer.high_bits == 0u
        && expression->data.integer.low_bits <= (uint64_t)INT32_MAX) {
        *out_value = (int32_t)expression->data.integer.low_bits;
        return 1;
    }
    low_bits = (uint32_t)expression->data.integer.low_bits;
    upper_low_bits = expression->data.integer.low_bits >> 32u;
    if (expression->data.integer.high_bits != UINT64_MAX
        || low_bits <= (uint32_t)INT32_MAX
        || (upper_low_bits != 0u
            && upper_low_bits != (uint64_t)UINT32_MAX)) {
        return 0;
    }
    /* Decode two's-complement bits without an out-of-range unsigned cast. */
    *out_value = -1 - (int32_t)(UINT32_MAX - low_bits);
    return 1;
}

CmMirLowerResult cm_mir_lower_body(CmMirContext *context,
    const CmHirContext *hir, CmHirBodyId body_id)
{
    CmMirLowerResult result;
    const CmHirBody *hir_body;
    const CmHirType *return_type;
    const CmHirExpr *terminal;
    CmMirLocal local;
    CmMirStatement statement;
    CmMirBasicBlock block;
    CmMirBody body;
    CmMirStatus status;
    int32_t constant_value;

    memset(&result, 0, sizeof(result));
    if (context == NULL || hir == NULL || body_id == CM_HIR_BODY_NONE) {
        cm_mir_lower_fail(&result, CM_MIR_LOWER_INVALID_ARGUMENT, body_id,
            CM_HIR_EXPR_NONE, CM_MIR_INVALID_ARGUMENT,
            "invalid HIR-to-MIR lowering arguments");
        return result;
    }
    hir_body = cm_hir_get_body(hir, body_id);
    if (hir_body == NULL || cm_hir_def_id_is_none(hir_body->owner)) {
        cm_mir_lower_fail(&result, CM_MIR_LOWER_INVALID_HIR, body_id,
            CM_HIR_EXPR_NONE, CM_MIR_OK,
            "MIR lowering requires an existing HIR body and owner");
        return result;
    }
    if (hir_body->state != CM_HIR_BODY_TYPED
        || hir_body->root_expression == CM_HIR_EXPR_NONE) {
        cm_mir_lower_fail(&result,
            CM_MIR_LOWER_UNSUPPORTED_BODY_STATE, body_id,
            hir_body->root_expression, CM_MIR_OK,
            "MIR lowering requires a fully typed HIR body");
        return result;
    }
    return_type = cm_hir_get_type(hir, hir_body->expected_type);
    if (return_type == NULL
        || return_type->kind != CM_HIR_TYPE_INTEGER_KIND
        || return_type->data.integer_type.kind != CM_HIR_INT_I32) {
        cm_mir_lower_fail(&result, CM_MIR_LOWER_UNSUPPORTED_TYPE, body_id,
            hir_body->root_expression, CM_MIR_OK,
            "initial MIR lowering supports only i32 return bodies");
        return result;
    }
    terminal = cm_mir_terminal_expression(&result, hir, hir_body, body_id,
        NULL);
    if (terminal == NULL) return result;
    if (terminal->kind != CM_HIR_EXPR_INTEGER) {
        cm_mir_lower_fail(&result, CM_MIR_LOWER_UNSUPPORTED_EXPRESSION,
            body_id, hir_body->root_expression, CM_MIR_OK,
            "legacy MIR lowering supports only an i32 integer tail");
        return result;
    }
    if (!cm_mir_i32_constant(terminal, &constant_value)) {
        cm_mir_lower_fail(&result, CM_MIR_LOWER_CONSTANT_OUT_OF_RANGE,
            body_id, hir_body->root_expression, CM_MIR_OK,
            "typed integer constant does not fit i32 MIR storage");
        return result;
    }

    memset(&local, 0, sizeof(local));
    local.kind = CM_MIR_LOCAL_RETURN;
    local.type = hir_body->expected_type;
    memset(&statement, 0, sizeof(statement));
    statement.kind = CM_MIR_STATEMENT_ASSIGN;
    statement.data.assign.destination = CM_MIR_RETURN_LOCAL;
    statement.data.assign.value.kind = CM_MIR_RVALUE_USE;
    statement.data.assign.value.type = hir_body->expected_type;
    statement.data.assign.value.data.use.kind = CM_MIR_CONSTANT_I32;
    statement.data.assign.value.data.use.type = hir_body->expected_type;
    statement.data.assign.value.data.use.data.i32_value = constant_value;
    memset(&block, 0, sizeof(block));
    block.statements = &statement;
    block.statement_count = 1u;
    block.terminator.kind = CM_MIR_TERMINATOR_RETURN;
    memset(&body, 0, sizeof(body));
    body.owner = hir_body->owner;
    body.source_body = body_id;
    body.locals = &local;
    body.local_count = 1u;
    body.basic_blocks = &block;
    body.basic_block_count = 1u;

    status = cm_mir_add_body(context, &body, &result.body);
    if (status != CM_MIR_OK) {
        cm_mir_lower_fail(&result, CM_MIR_LOWER_MODEL_FAILURE, body_id,
            hir_body->root_expression, status,
            "MIR model rejected the validated lowered body");
        return result;
    }
    result.lowered_body_count = 1u;
    return result;
}

static int cm_mir_lower_type(const CmHirContext *hir,
    const CmHirItem *item, const CmHirTypeId *substitutions,
    uint32_t substitution_count, CmHirTypeId declared,
    CmHirTypeId *out_type);

static const CmHirItem *cm_mir_lower_named_struct(
    const CmHirContext *hir, const CmHirItem *function,
    CmHirDefId definition_id)
{
    const CmHirDefinition *definition;
    const CmHirItem *item;

    definition = cm_hir_lookup_definition(hir, definition_id);
    item = definition == NULL
            || definition->kind != CM_HIR_DEFINITION_ITEM
            || definition->state != CM_HIR_DEFINITION_BOUND
        ? NULL : cm_hir_get_item(hir, definition->entity.item_id);
    return function != NULL && item != NULL
            && item->kind == CM_HIR_ITEM_STRUCT
            && cm_hir_def_id_equal(item->definition, definition_id)
            && item->definition.crate_id == function->definition.crate_id
            && cm_hir_def_id_is_none(item->parent_definition)
            && item->generic_parameter_count == 0u
            && item->data.aggregate_item.form == CM_HIR_AGGREGATE_NAMED
            && item->data.aggregate_item.field_count != 0u
            && item->data.aggregate_item.fields != NULL
            && item->data.aggregate_item.field_count
                <= CM_MIR_MAX_AGGREGATE_FIELDS
        ? item : NULL;
}

/*
 * Moving a field out of a Drop type is illegal.  MIR does not yet carry a
 * resolved Drop lang-item identity, so conservatively reject a local unary
 * newtype if any positive trait impl has the same ADT head.  This deliberately
 * over-rejects until lang-item identities are available.
 */
static int cm_mir_lower_newtype_has_positive_trait_impl(
    const CmHirContext *hir, CmHirDefId definition)
{
    size_t index;

    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *item;
        const CmHirType *self_type;

        item = (const CmHirItem *)cm_vec_at_const(&hir->items, index);
        if (item == NULL || item->kind != CM_HIR_ITEM_IMPL
            || !item->data.impl_item.has_trait
            || item->data.impl_item.polarity != CM_HIR_IMPL_POSITIVE) {
            continue;
        }
        self_type = cm_hir_get_type(hir, item->data.impl_item.self_type);
        if (self_type != NULL && self_type->kind == CM_HIR_TYPE_ADT_KIND
            && cm_hir_def_id_equal(self_type->data.named_type.definition,
                definition)) {
            return 1;
        }
    }
    return 0;
}

static const CmHirItem *cm_mir_lower_applied_newtype(
    const CmHirContext *hir, const CmHirItem *function,
    CmHirTypeId type_id, CmHirTypeId *out_field_type)
{
    const CmHirType *type;
    const CmHirDefinition *definition;
    const CmHirItem *item;
    const CmHirGenericParam *parameter;
    const CmHirType *declared_field;

    if (out_field_type != NULL) *out_field_type = CM_HIR_TYPE_NONE;
    type = cm_hir_get_type(hir, type_id);
    definition = type == NULL || type->kind != CM_HIR_TYPE_ADT_KIND
            || type->data.named_type.argument_count != 1u
            || type->data.named_type.arguments == NULL
            || type->data.named_type.arguments[0].kind
                != CM_HIR_GENERIC_ARG_TYPE
        ? NULL : cm_hir_lookup_definition(hir,
            type->data.named_type.definition);
    item = definition == NULL
            || definition->kind != CM_HIR_DEFINITION_ITEM
            || definition->state != CM_HIR_DEFINITION_BOUND
        ? NULL : cm_hir_get_item(hir, definition->entity.item_id);
    if (function == NULL || item == NULL || item->kind != CM_HIR_ITEM_STRUCT
        || !cm_hir_def_id_equal(item->definition,
            type->data.named_type.definition)
        || item->definition.crate_id != function->definition.crate_id
        || !cm_hir_def_id_is_none(item->parent_definition)
        || item->generic_parameter_count != 1u
        || item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE
        || item->data.aggregate_item.form != CM_HIR_AGGREGATE_TUPLE
        || item->data.aggregate_item.field_count != 1u
        || item->data.aggregate_item.fields == NULL
        || cm_hir_get_type(hir,
            type->data.named_type.arguments[0].data.type) == NULL
        || cm_mir_lower_newtype_has_positive_trait_impl(hir,
            item->definition)) {
        return NULL;
    }
    parameter = cm_hir_get_generic_param(hir,
        item->generic_parameter_start);
    declared_field = cm_hir_get_type(hir,
        item->data.aggregate_item.fields[0].type);
    if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_TYPE
        || parameter->index != 0u
        || !cm_hir_def_id_equal(parameter->owner, item->definition)
        || declared_field == NULL
        || declared_field->kind != CM_HIR_TYPE_PARAMETER_KIND
        || declared_field->data.parameter_type.parameter
            != item->generic_parameter_start) {
        return NULL;
    }
    if (out_field_type != NULL) {
        *out_field_type = type->data.named_type.arguments[0].data.type;
    }
    return item;
}

static CmHirTypeId cm_mir_lower_monomorphic_self_type(
    const CmHirContext *hir, CmHirTypeId id, size_t depth)
{
    const CmHirType *type;
    const CmHirDefinition *definition;
    const CmHirItem *owner;

    if (hir == NULL || id == CM_HIR_TYPE_NONE
        || depth >= CM_MIR_FLOW_RECURSION_LIMIT) {
        return CM_HIR_TYPE_NONE;
    }
    type = cm_hir_get_type(hir, id);
    if (type == NULL) return CM_HIR_TYPE_NONE;
    if (type->kind != CM_HIR_TYPE_SELF_KIND) return id;
    definition = cm_hir_lookup_definition(hir,
        type->data.self_type.owner);
    owner = definition == NULL
            || definition->kind != CM_HIR_DEFINITION_ITEM
            || definition->state != CM_HIR_DEFINITION_BOUND
        ? NULL : cm_hir_get_item(hir, definition->entity.item_id);
    if (owner == NULL || owner->kind != CM_HIR_ITEM_IMPL
        || !cm_hir_def_id_equal(owner->definition,
            type->data.self_type.owner)
        || owner->generic_parameter_count != 0u
        || owner->data.impl_item.self_type == id) {
        return CM_HIR_TYPE_NONE;
    }
    return cm_mir_lower_monomorphic_self_type(hir,
        owner->data.impl_item.self_type, depth + 1u);
}

static int cm_mir_lower_type_is_scalar(const CmHirContext *hir,
    CmHirTypeId type_id)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, type_id);
    return type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && (type->data.integer_type.kind == CM_HIR_INT_I32
            || type->data.integer_type.kind == CM_HIR_INT_U8
            || type->data.integer_type.kind == CM_HIR_INT_U32
            || type->data.integer_type.kind == CM_HIR_INT_USIZE);
}

static int cm_mir_lower_type_is_i32(const CmHirContext *hir,
    CmHirTypeId type_id)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, type_id);
    return type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_I32;
}

static int cm_mir_lower_type_is_u32(const CmHirContext *hir,
    CmHirTypeId type_id)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, type_id);
    return type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_U32;
}

static int cm_mir_lower_type_is_executable_substitution(
    const CmHirContext *hir, CmHirTypeId type_id)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, type_id);
    return type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && (type->data.integer_type.kind == CM_HIR_INT_U8
            || type->data.integer_type.kind == CM_HIR_INT_U32);
}

static int cm_mir_lower_type_is_call_scalar(const CmHirContext *hir,
    CmHirTypeId type_id)
{
    const CmHirType *type;
    CmHirTypeId pointee;

    type = cm_hir_get_type(hir, type_id);
    if (type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND) {
        return type->data.integer_type.kind == CM_HIR_INT_U8
            || type->data.integer_type.kind == CM_HIR_INT_U32
            || type->data.integer_type.kind == CM_HIR_INT_USIZE;
    }
    if (type == NULL || type->kind != CM_HIR_TYPE_REFERENCE_KIND
        || type->data.reference_type.region.kind != CM_HIR_REGION_ERASED
        || (type->data.reference_type.mutability != CM_HIR_IMMUTABLE
            && type->data.reference_type.mutability != CM_HIR_MUTABLE)) {
        return 0;
    }
    pointee = cm_mir_lower_monomorphic_self_type(hir,
        type->data.reference_type.pointee, 0u);
    return pointee != CM_HIR_TYPE_NONE
        && cm_mir_lower_type_is_call_scalar(hir, pointee);
}

static int cm_mir_lower_type_is_tuple_element(const CmHirContext *hir,
    CmHirTypeId type_id)
{
    const CmHirType *type;
    CmHirTypeId pointee;

    if (cm_mir_lower_type_is_scalar(hir, type_id)) return 1;
    type = cm_hir_get_type(hir, type_id);
    if (type != NULL && type->kind == CM_HIR_TYPE_PARAMETER_KIND) return 1;
    if (type == NULL || type->kind != CM_HIR_TYPE_REFERENCE_KIND
        || type->data.reference_type.region.kind != CM_HIR_REGION_ERASED
        || (type->data.reference_type.mutability != CM_HIR_IMMUTABLE
            && type->data.reference_type.mutability != CM_HIR_MUTABLE)) {
        return 0;
    }
    pointee = cm_mir_lower_monomorphic_self_type(hir,
        type->data.reference_type.pointee, 0u);
    return pointee != CM_HIR_TYPE_NONE
        && cm_mir_lower_type_is_call_scalar(hir, pointee);
}

static int cm_mir_lower_type_is_unary_tuple(const CmHirContext *hir,
    CmHirTypeId type_id)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, type_id);
    return type != NULL && type->kind == CM_HIR_TYPE_TUPLE_KIND
        && type->data.tuple_type.element_count == 1u
        && type->data.tuple_type.elements != NULL;
}

static int cm_mir_lower_type_is_usize(const CmHirContext *hir,
    CmHirTypeId type_id)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, type_id);
    return type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_USIZE;
}

static int cm_mir_lower_usize_value_valid(const CmMirContext *context,
    uint64_t value)
{
    unsigned int pointer_bits;

    pointer_bits = cm_mir_context_pointer_bits(context);
    return pointer_bits == 64u
        || (pointer_bits == 32u && value <= (uint64_t)UINT32_MAX);
}

static int cm_mir_lower_type_is_bool(const CmHirContext *hir,
    CmHirTypeId type_id)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, type_id);
    return type != NULL && type->kind == CM_HIR_TYPE_BOOL_KIND;
}

static int cm_mir_lower_type_is_aggregate(const CmHirContext *hir,
    const CmHirItem *function, CmHirTypeId type_id)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, type_id);
    return type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
        && type->data.named_type.argument_count == 0u
        && type->data.named_type.arguments == NULL
        && cm_mir_lower_named_struct(hir, function,
            type->data.named_type.definition) != NULL;
}

static int cm_mir_lower_type_target_valid(const CmMirContext *context,
    const CmHirContext *hir, const CmHirItem *function,
    CmHirTypeId type_id, size_t depth)
{
    const CmHirType *type;
    const CmHirItem *item;
    uint32_t index;

    if (depth >= CM_MIR_FLOW_RECURSION_LIMIT) return 0;
    type = cm_hir_get_type(hir, type_id);
    if (type == NULL) return 0;
    if (type->kind == CM_HIR_TYPE_INTEGER_KIND) {
        return type->data.integer_type.kind != CM_HIR_INT_USIZE
            || cm_mir_context_pointer_bits(context) == 32u
            || cm_mir_context_pointer_bits(context) == 64u;
    }
    if (type->kind == CM_HIR_TYPE_SELF_KIND) {
        CmHirTypeId concrete;

        concrete = cm_mir_lower_monomorphic_self_type(hir, type_id, depth);
        return concrete != CM_HIR_TYPE_NONE && concrete != type_id
            && cm_mir_lower_type_target_valid(context, hir, function,
                concrete, depth + 1u);
    }
    if (type->kind == CM_HIR_TYPE_REFERENCE_KIND) {
        return type->data.reference_type.region.kind == CM_HIR_REGION_ERASED
            && (type->data.reference_type.mutability == CM_HIR_IMMUTABLE
                || type->data.reference_type.mutability == CM_HIR_MUTABLE)
            && cm_mir_lower_type_target_valid(context, hir, function,
                type->data.reference_type.pointee, depth + 1u);
    }
    if (type->kind == CM_HIR_TYPE_TUPLE_KIND) {
        if (type->data.tuple_type.element_count == 0u
            || type->data.tuple_type.element_count
                > CM_HIR_TUPLE_PARAMETER_BINDING_COUNT
            || type->data.tuple_type.elements == NULL) {
            return 0;
        }
        for (index = 0u; index < type->data.tuple_type.element_count;
             ++index) {
            const CmHirType *element;

            element = cm_hir_get_type(hir,
                type->data.tuple_type.elements[index]);
            if (element == NULL
                || (type->data.tuple_type.element_count == 2u
                    && !cm_mir_lower_type_is_scalar(hir,
                        type->data.tuple_type.elements[index]))
                || !cm_mir_lower_type_target_valid(context, hir, function,
                    type->data.tuple_type.elements[index], depth + 1u)) {
                return 0;
            }
        }
        return 1;
    }
    if (type->kind != CM_HIR_TYPE_ADT_KIND) return 1;
    if (type->data.named_type.argument_count == 1u) {
        CmHirTypeId field_type;

        return cm_mir_lower_applied_newtype(hir, function, type_id,
                &field_type) != NULL
            && cm_mir_lower_type_target_valid(context, hir, function,
                field_type, depth + 1u);
    }
    item = type->data.named_type.argument_count != 0u
            || type->data.named_type.arguments != NULL
        ? NULL : cm_mir_lower_named_struct(hir, function,
            type->data.named_type.definition);
    if (item == NULL) return 0;
    for (index = 0u; index < item->data.aggregate_item.field_count;
         ++index) {
        if (!cm_mir_lower_type_target_valid(context, hir, function,
                item->data.aggregate_item.fields[index].type,
                depth + 1u)) {
            return 0;
        }
    }
    return 1;
}

static const CmHirItem *cm_mir_lower_function(const CmHirContext *hir,
    const CmHirBody *body)
{
    const CmHirDefinition *definition;
    const CmHirItem *item;

    definition = cm_hir_lookup_definition(hir, body->owner);
    if (definition == NULL || definition->kind != CM_HIR_DEFINITION_ITEM
        || definition->state != CM_HIR_DEFINITION_BOUND) {
        return NULL;
    }
    item = cm_hir_get_item(hir, definition->entity.item_id);
    return item != NULL && item->kind == CM_HIR_ITEM_FUNCTION
        && cm_hir_def_id_equal(item->definition, body->owner)
        && item->data.function_item.body != CM_HIR_BODY_NONE
        ? item : NULL;
}

static int cm_mir_lower_type(const CmHirContext *hir,
    const CmHirItem *item, const CmHirTypeId *substitutions,
    uint32_t substitution_count, CmHirTypeId declared,
    CmHirTypeId *out_type)
{
    const CmHirDefinition *self_owner;
    const CmHirItem *impl_item;
    const CmHirType *type;
    const CmHirGenericParam *parameter;
    uint32_t index;

    type = cm_hir_get_type(hir, declared);
    if (type == NULL || out_type == NULL) return 0;
    if (type->kind == CM_HIR_TYPE_INTEGER_KIND
        && (type->data.integer_type.kind == CM_HIR_INT_I32
            || type->data.integer_type.kind == CM_HIR_INT_U8
            || type->data.integer_type.kind == CM_HIR_INT_U32
            || type->data.integer_type.kind == CM_HIR_INT_USIZE)) {
        *out_type = declared;
        return 1;
    }
    if (type->kind == CM_HIR_TYPE_BOOL_KIND) {
        *out_type = declared;
        return 1;
    }
    if (type->kind == CM_HIR_TYPE_REFERENCE_KIND) {
        CmHirTypeId pointee;

        if (type->data.reference_type.region.kind != CM_HIR_REGION_ERASED
            || (type->data.reference_type.mutability != CM_HIR_IMMUTABLE
                && type->data.reference_type.mutability != CM_HIR_MUTABLE)
            || !cm_mir_lower_type(hir, item, substitutions,
                substitution_count, type->data.reference_type.pointee,
                &pointee)
            || !cm_mir_lower_type_is_call_scalar(hir, pointee)) {
            return 0;
        }
        *out_type = declared;
        return 1;
    }
    if (type->kind == CM_HIR_TYPE_TUPLE_KIND
        && type->data.tuple_type.element_count != 0u
        && type->data.tuple_type.element_count
            <= CM_HIR_TUPLE_PARAMETER_BINDING_COUNT
        && type->data.tuple_type.elements != NULL) {
        for (index = 0u; index < type->data.tuple_type.element_count;
             ++index) {
            if ((type->data.tuple_type.element_count == 2u
                    && !cm_mir_lower_type_is_scalar(hir,
                        type->data.tuple_type.elements[index]))
                || (type->data.tuple_type.element_count == 1u
                    && !cm_mir_lower_type_is_tuple_element(hir,
                        type->data.tuple_type.elements[index]))) {
                return 0;
            }
        }
        *out_type = declared;
        return 1;
    }
    if (type->kind == CM_HIR_TYPE_ADT_KIND
        && type->data.named_type.argument_count == 0u
        && type->data.named_type.arguments == NULL
        && cm_mir_lower_named_struct(hir, item,
            type->data.named_type.definition) != NULL) {
        *out_type = declared;
        return 1;
    }
    if (type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_mir_lower_applied_newtype(hir, item, declared, NULL) != NULL) {
        *out_type = declared;
        return 1;
    }
    if (type->kind == CM_HIR_TYPE_SELF_KIND) {
        self_owner = cm_hir_lookup_definition(hir,
            type->data.self_type.owner);
        impl_item = self_owner == NULL
                || self_owner->kind != CM_HIR_DEFINITION_ITEM
                || self_owner->state != CM_HIR_DEFINITION_BOUND
            ? NULL : cm_hir_get_item(hir, self_owner->entity.item_id);
        if (item == NULL || impl_item == NULL
            || impl_item->kind != CM_HIR_ITEM_IMPL
            || !cm_hir_def_id_equal(item->parent_definition,
                type->data.self_type.owner)
            || !cm_hir_def_id_equal(impl_item->definition,
                type->data.self_type.owner)
            || impl_item->data.impl_item.self_type == declared) return 0;
        return cm_mir_lower_type(hir, item, substitutions,
            substitution_count, impl_item->data.impl_item.self_type,
            out_type);
    }
    if (type->kind != CM_HIR_TYPE_PARAMETER_KIND || item == NULL) return 0;
    parameter = cm_hir_get_generic_param(hir,
        type->data.parameter_type.parameter);
    if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_TYPE) {
        return 0;
    }
    if (cm_hir_def_id_equal(parameter->owner, item->definition)) {
        if (parameter->index >= substitution_count) return 0;
    } else {
        self_owner = cm_hir_lookup_definition(hir,
            item->parent_definition);
        impl_item = self_owner == NULL
                || self_owner->kind != CM_HIR_DEFINITION_ITEM
                || self_owner->state != CM_HIR_DEFINITION_BOUND
            ? NULL : cm_hir_get_item(hir, self_owner->entity.item_id);
        if (impl_item == NULL || impl_item->kind != CM_HIR_ITEM_IMPL
            || !cm_hir_def_id_equal(impl_item->definition,
                parameter->owner)
            || item->generic_parameter_count != 0u
            || impl_item->generic_parameter_count != 1u
            || substitution_count != 1u || parameter->index != 0u) {
            return 0;
        }
    }
    index = parameter->index;
    if (!cm_mir_lower_type_is_executable_substitution(hir,
            substitutions[index])) {
        return 0;
    }
    *out_type = substitutions[index];
    return 1;
}

typedef struct CmMirLowerParameterLayout {
    uint32_t hir_parameter_local_count;
    uint32_t tuple_binding_local_count;
    uint32_t non_temporary_local_count;
} CmMirLowerParameterLayout;

/*
 * HIR names each lexical binding, while MIR must retain one local for each
 * incoming ABI argument.  A named parameter can therefore use its ABI local
 * directly, but a tuple-pattern parameter needs two additional lexical
 * locals populated by the entry-block destructuring prologue.
 */
static int cm_mir_lower_parameter_layout(const CmMirContext *context,
    const CmHirContext *hir, const CmHirItem *item,
    const CmHirBody *hir_body, const CmHirTypeId *substitutions,
    uint32_t substitution_count, CmHirTypeId argument_types[2],
    CmMirLocalId *hir_to_mir, CmMirLowerParameterLayout *out_layout)
{
    const CmHirFunctionSignature *signature;
    uint32_t hir_local_index;
    uint32_t parameter_index;
    uint32_t tuple_binding_index;
    uint32_t user_local_count;

    if (context == NULL || hir == NULL || item == NULL
        || hir_body == NULL || argument_types == NULL
        || out_layout == NULL || item->kind != CM_HIR_ITEM_FUNCTION) {
        return 0;
    }
    signature = &item->data.function_item.signature;
    if (signature->parameter_count > 2u
        || hir_body->parameter_count != signature->parameter_count
        || (signature->parameter_count != 0u
            && signature->parameters == NULL)
        || (hir_body->local_count != 0u && hir_body->locals == NULL)) {
        return 0;
    }
    memset(out_layout, 0, sizeof(*out_layout));
    hir_local_index = 0u;
    tuple_binding_index = 0u;
    for (parameter_index = 0u;
         parameter_index < signature->parameter_count; ++parameter_index) {
        const CmHirFunctionParameter *parameter;

        parameter = &signature->parameters[parameter_index];
        if (!cm_mir_lower_type(hir, item, substitutions,
                substitution_count, parameter->type,
                &argument_types[parameter_index])
            || !cm_mir_lower_type_target_valid(context, hir, item,
                argument_types[parameter_index], 0u)
            || cm_mir_lower_type_is_bool(hir,
                argument_types[parameter_index])) {
            return 0;
        }
        if (parameter->binding_kind == CM_HIR_BINDING_NAMED) {
            const CmHirLocal *local;
            CmHirTypeId local_type;

            if (cm_mir_lower_type_is_unary_tuple(hir, parameter->type)
                || hir_local_index >= hir_body->local_count) return 0;
            local = &hir_body->locals[hir_local_index];
            if (local->parameter_index != parameter_index
                || local->parameter_binding_index != 0u
                || local->name != parameter->name
                || !cm_mir_lower_type(hir, item, substitutions,
                    substitution_count, local->type, &local_type)
                || local_type != argument_types[parameter_index]) {
                return 0;
            }
            if (hir_to_mir != NULL) {
                hir_to_mir[hir_local_index] = parameter_index + 1u;
            }
            hir_local_index += 1u;
            continue;
        }
        if (parameter->binding_kind == CM_HIR_BINDING_DISCARD) {
            if (parameter->binding_mode != CM_HIR_PARAMETER_BINDING_MOVE
                || parameter->name != CM_INTERN_ID_NONE
                || cm_mir_lower_type_is_unary_tuple(hir,
                    parameter->type)) {
                return 0;
            }
            continue;
        }
        if (parameter->binding_kind == CM_HIR_BINDING_TUPLE_PATTERN) {
            const CmHirType *tuple_type;
            uint32_t binding_count;
            uint32_t field_index;

            tuple_type = cm_hir_get_type(hir, parameter->type);
            if (parameter->binding_mode != CM_HIR_PARAMETER_BINDING_MOVE
                || tuple_type == NULL
                || tuple_type->kind != CM_HIR_TYPE_TUPLE_KIND
                || tuple_type->data.tuple_type.element_count == 0u
                || tuple_type->data.tuple_type.element_count
                    > CM_HIR_TUPLE_PARAMETER_BINDING_COUNT
                || tuple_type->data.tuple_type.elements == NULL) {
                return 0;
            }
            binding_count = tuple_type->data.tuple_type.element_count;
            for (field_index = 0u;
                 field_index < binding_count;
                 ++field_index) {
                const CmHirLocal *local;
                CmHirTypeId element_type;

                if (hir_local_index >= hir_body->local_count
                    || !cm_mir_lower_type(hir, item, substitutions,
                        substitution_count,
                        tuple_type->data.tuple_type.elements[field_index],
                        &element_type)
                    || (binding_count == 2u
                        && (element_type
                            != tuple_type->data.tuple_type
                                .elements[field_index]
                            || !cm_mir_lower_type_is_scalar(hir,
                                element_type)))
                    || (binding_count == 1u
                        && !cm_mir_lower_type_is_tuple_element(hir,
                            element_type))
                    || !cm_mir_lower_type_target_valid(context, hir, item,
                        element_type, 0u)) {
                    return 0;
                }
                local = &hir_body->locals[hir_local_index];
                if (local->parameter_index != parameter_index
                    || local->parameter_binding_index != field_index
                    || local->name
                        != parameter->tuple_bindings[field_index].name
                    || local->type
                        != tuple_type->data.tuple_type.elements[field_index]
                    || local->mutability != CM_HIR_IMMUTABLE
                    || local->span.source
                        != parameter->tuple_bindings[field_index].span.source
                    || local->span.start
                        != parameter->tuple_bindings[field_index].span.start
                    || local->span.end
                        != parameter->tuple_bindings[field_index].span.end) {
                    return 0;
                }
                if (hir_to_mir != NULL) {
                    hir_to_mir[hir_local_index] = 1u
                        + signature->parameter_count
                        + tuple_binding_index;
                }
                hir_local_index += 1u;
                tuple_binding_index += 1u;
            }
            for (; field_index < CM_HIR_TUPLE_PARAMETER_BINDING_COUNT;
                 ++field_index) {
                const CmHirTupleParameterBinding *binding;

                binding = &parameter->tuple_bindings[field_index];
                if (binding->name != CM_INTERN_ID_NONE
                    || binding->span.source != 0u
                    || binding->span.start != 0u
                    || binding->span.end != 0u) {
                    return 0;
                }
            }
            continue;
        }
        if (parameter->binding_kind == CM_HIR_BINDING_NEWTYPE_PATTERN) {
            const CmHirLocal *local;
            CmHirTypeId declared_field_type;
            CmHirTypeId field_type;
            CmHirTypeId local_type;

            if (parameter->binding_mode != CM_HIR_PARAMETER_BINDING_MOVE
                || parameter->name != CM_INTERN_ID_NONE
                || cm_mir_lower_applied_newtype(hir, item, parameter->type,
                    &declared_field_type) == NULL
                || !cm_mir_lower_type(hir, item, substitutions,
                    substitution_count, declared_field_type, &field_type)
                || !cm_mir_lower_type_is_scalar(hir, field_type)
                || hir_local_index >= hir_body->local_count) {
                return 0;
            }
            local = &hir_body->locals[hir_local_index];
            if (!cm_mir_lower_type(hir, item, substitutions,
                    substitution_count, local->type, &local_type)
                || local_type != field_type
                || local->parameter_index != parameter_index
                || local->parameter_binding_index != 0u
                || local->name != parameter->newtype_binding.name
                || local->mutability != CM_HIR_IMMUTABLE
                || local->span.source
                    != parameter->newtype_binding.span.source
                || local->span.start
                    != parameter->newtype_binding.span.start
                || local->span.end
                    != parameter->newtype_binding.span.end) {
                return 0;
            }
            if (hir_to_mir != NULL) {
                hir_to_mir[hir_local_index] = 1u
                    + signature->parameter_count + tuple_binding_index;
            }
            hir_local_index += 1u;
            tuple_binding_index += 1u;
            continue;
        }
        /* Every future binding form remains outside this exact slice. */
        return 0;
    }
    user_local_count = hir_body->local_count - hir_local_index;
    if (signature->parameter_count > UINT32_MAX - 1u
        || tuple_binding_index
            > UINT32_MAX - 1u - signature->parameter_count
        || user_local_count > UINT32_MAX - 1u
            - signature->parameter_count - tuple_binding_index) {
        return 0;
    }
    if (hir_to_mir != NULL) {
        uint32_t user_index;

        for (user_index = 0u; user_index < user_local_count; ++user_index) {
            hir_to_mir[hir_local_index + user_index] = 1u
                + signature->parameter_count + tuple_binding_index
                + user_index;
        }
    }
    out_layout->hir_parameter_local_count = hir_local_index;
    out_layout->tuple_binding_local_count = tuple_binding_index;
    out_layout->non_temporary_local_count = 1u
        + signature->parameter_count + tuple_binding_index
        + user_local_count;
    return 1;
}

static int cm_mir_lower_hir_local_id(const CmHirContext *hir,
    const CmHirFunctionSignature *signature, const CmHirBody *body,
    const CmMirLowerParameterLayout *layout, uint32_t hir_local,
    CmMirLocalId *out_local)
{
    uint32_t parameter_index;
    uint32_t parameter_local;
    uint32_t tuple_binding;

    if (hir == NULL || signature == NULL || body == NULL || layout == NULL
        || out_local == NULL || hir_local >= body->local_count) return 0;
    if (hir_local >= layout->hir_parameter_local_count) {
        *out_local = 1u + signature->parameter_count
            + layout->tuple_binding_local_count
            + hir_local - layout->hir_parameter_local_count;
        return *out_local < layout->non_temporary_local_count;
    }
    parameter_local = 0u;
    tuple_binding = 0u;
    for (parameter_index = 0u;
         parameter_index < signature->parameter_count; ++parameter_index) {
        const CmHirFunctionParameter *parameter;

        parameter = &signature->parameters[parameter_index];
        if (parameter->binding_kind == CM_HIR_BINDING_NAMED) {
            if (parameter_local == hir_local) {
                *out_local = parameter_index + 1u;
                return 1;
            }
            parameter_local += 1u;
            continue;
        }
        if (parameter->binding_kind == CM_HIR_BINDING_DISCARD) continue;
        if (parameter->binding_kind == CM_HIR_BINDING_NEWTYPE_PATTERN) {
            if (parameter_local == hir_local) {
                *out_local = 1u + signature->parameter_count
                    + tuple_binding;
                return *out_local < layout->non_temporary_local_count;
            }
            parameter_local += 1u;
            tuple_binding += 1u;
            continue;
        }
        if (parameter->binding_kind != CM_HIR_BINDING_TUPLE_PATTERN) {
            return 0;
        }
        {
            const CmHirType *tuple_type;
            uint32_t binding_count;

            tuple_type = cm_hir_get_type(hir, parameter->type);
            if (tuple_type == NULL
                || tuple_type->kind != CM_HIR_TYPE_TUPLE_KIND
                || tuple_type->data.tuple_type.element_count == 0u
                || tuple_type->data.tuple_type.element_count
                    > CM_HIR_TUPLE_PARAMETER_BINDING_COUNT) {
                return 0;
            }
            binding_count = tuple_type->data.tuple_type.element_count;
            if (hir_local - parameter_local < binding_count) {
                *out_local = 1u + signature->parameter_count + tuple_binding
                    + hir_local - parameter_local;
                return *out_local < layout->non_temporary_local_count;
            }
            parameter_local += binding_count;
            tuple_binding += binding_count;
        }
    }
    return 0;
}

typedef enum CmMirFlowError {
    CM_MIR_FLOW_OK = 0,
    CM_MIR_FLOW_INVALID,
    CM_MIR_FLOW_UNSUPPORTED,
    CM_MIR_FLOW_CONSTANT_RANGE,
    CM_MIR_FLOW_CALLEE,
    CM_MIR_FLOW_ADMISSION
} CmMirFlowError;

typedef struct CmMirFlowCall {
    CmHirExprId expression;
    CmMirBodyId callee;
} CmMirFlowCall;

typedef struct CmMirFlowPlan {
    const CmMirContext *context;
    const CmMirPublication *publication;
    const CmHirContext *hir;
    const CmHirBody *body;
    const CmHirItem *item;
    const CmMirInstance *instance;
    const CmSemanticAdmission *admission;
    const CmSemanticResults *semantic_results;
    CmMirSemanticEvidenceKind semantic_evidence;
    CmHirTypeId expected_type;
    CmHirExprId allowed_if_expression;
    CmMirLowerParameterLayout parameter_layout;
    CmVec seen;
    CmVec calls;
    uint32_t binary_count;
    uint32_t call_count;
    uint32_t conditional_count;
    uint32_t call_argument_count;
    uint32_t temporary_count;
    uint32_t statement_count;
    uint32_t aggregate_field_count;
    uint32_t projection_count;
    CmMirFlowError error;
    CmHirExprId error_expression;
    CmMirStatus error_status;
} CmMirFlowPlan;

static int cm_mir_flow_callable_arguments(const CmHirExpr *expression,
    CmHirExprId storage[2], const CmHirExprId **out_arguments,
    uint32_t *out_count)
{
    uint32_t index;

    if (expression == NULL || storage == NULL || out_arguments == NULL
        || out_count == NULL) return 0;
    if (expression->kind == CM_HIR_EXPR_QUALIFIED_CALL) {
        if (expression->data.qualified_call.argument_count == 0u
            || expression->data.qualified_call.argument_count > 2u
            || expression->data.qualified_call.arguments == NULL) return 0;
        *out_arguments = expression->data.qualified_call.arguments;
        *out_count = expression->data.qualified_call.argument_count;
        return 1;
    }
    if (expression->kind != CM_HIR_EXPR_METHOD_CALL
        || expression->data.method_call.receiver == CM_HIR_EXPR_NONE
        || expression->data.method_call.argument_count > 1u
        || (expression->data.method_call.argument_count != 0u
            && expression->data.method_call.arguments == NULL)) return 0;
    storage[0] = expression->data.method_call.receiver;
    for (index = 0u; index < expression->data.method_call.argument_count;
         ++index) {
        storage[index + 1u] = expression->data.method_call.arguments[index];
    }
    *out_arguments = storage;
    *out_count = expression->data.method_call.argument_count + 1u;
    return 1;
}

static const CmHirItem *cm_mir_flow_definition_item(
    const CmHirContext *hir, CmHirDefId definition)
{
    const CmHirDefinition *record;
    const CmHirItem *item;

    record = cm_hir_lookup_definition(hir, definition);
    item = record == NULL || record->kind != CM_HIR_DEFINITION_ITEM
            || record->state != CM_HIR_DEFINITION_BOUND
        ? NULL : cm_hir_get_item(hir, record->entity.item_id);
    return item != NULL && cm_hir_def_id_equal(item->definition, definition)
        ? item : NULL;
}

static int cm_mir_flow_method_trait_in_scope(const CmHirExpr *expression,
    CmHirDefId trait_definition)
{
    uint32_t index;

    if (expression == NULL || expression->kind != CM_HIR_EXPR_METHOD_CALL) {
        return 0;
    }
    for (index = 0u;
         index < expression->data.method_call.in_scope_trait_count; ++index) {
        if (cm_hir_def_id_equal(
                expression->data.method_call.in_scope_traits[index],
                trait_definition)) return 1;
    }
    return 0;
}

static CmMirStatus cm_mir_flow_find_callee(const CmMirFlowPlan *plan,
    CmHirExprId expression, CmHirDefId definition,
    const CmHirTypeId *substitutions, uint32_t substitution_count,
    CmMirBodyId *out_id)
{
    CmHirCanonicalInstance caller;
    CmHirCanonicalInstance callee;
    CmMirInstance key;
    CmSemanticResultsStatus semantic_status;
    CmMirStatus status;

    if (plan->semantic_evidence
            != CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE
        || plan->instance->identity_bytes == NULL
        || plan->instance->identity_size == 0u) {
        return plan->publication == NULL
            ? cm_mir_find_instance(plan->context, definition, substitutions,
                substitution_count, out_id)
            : cm_mir_publication_find_instance(plan->publication,
                definition, substitutions, substitution_count, out_id);
    }
    cm_hir_canonical_instance_init(&caller);
    caller.definition = plan->instance->definition;
    caller.body_definition = plan->instance->body_definition;
    caller.body = plan->instance->body;
    caller.bytes = plan->instance->identity_bytes;
    caller.size = plan->instance->identity_size;
    cm_hir_canonical_instance_init(&callee);
    semantic_status =
        cm_semantic_results_canonical_instance_callee_identity(
            plan->semantic_results, plan->admission, &caller, expression,
            &callee);
    if (semantic_status != CM_SEMANTIC_RESULTS_OK
        || !cm_hir_def_id_equal(callee.definition, definition)) {
        cm_hir_canonical_instance_destroy(&callee);
        if (out_id != NULL) *out_id = CM_MIR_BODY_NONE;
        return CM_MIR_INVALID_ADMISSION;
    }
    memset(&key, 0, sizeof(key));
    key.definition = callee.definition;
    key.body_definition = callee.body_definition;
    key.substitutions = (CmHirTypeId *)substitutions;
    key.substitution_count = substitution_count;
    key.body = callee.body;
    key.identity_bytes = callee.bytes;
    key.identity_size = callee.size;
    status = plan->publication == NULL
        ? cm_mir_find_canonical(plan->context, &key, out_id)
        : cm_mir_publication_find_canonical(plan->publication, &key, out_id);
    cm_hir_canonical_instance_destroy(&callee);
    return status;
}

static int cm_mir_lower_instance_is_canonical(
    const CmMirInstance *instance)
{
    return instance != NULL && instance->body != CM_HIR_BODY_NONE
        && instance->identity_bytes != NULL && instance->identity_size != 0u;
}

static int cm_mir_lower_transitional_impl_instance(
    const CmHirContext *hir, const CmHirItem *item,
    const CmMirInstance *instance, const CmHirTypeId *substitutions,
    uint32_t substitution_count)
{
    const CmHirDefinition *definition;
    const CmHirItem *impl_item;

    if (hir == NULL || item == NULL || item->kind != CM_HIR_ITEM_FUNCTION
        || item->generic_parameter_count != 0u
        || cm_hir_def_id_is_none(item->parent_definition)
        || !cm_mir_lower_instance_is_canonical(instance)
        || substitution_count != 1u || substitutions == NULL
        || !cm_mir_lower_type_is_executable_substitution(hir,
            substitutions[0])) {
        return 0;
    }
    definition = cm_hir_lookup_definition(hir, item->parent_definition);
    impl_item = definition == NULL
            || definition->kind != CM_HIR_DEFINITION_ITEM
            || definition->state != CM_HIR_DEFINITION_BOUND
        ? NULL : cm_hir_get_item(hir, definition->entity.item_id);
    return impl_item != NULL && impl_item->kind == CM_HIR_ITEM_IMPL
        && cm_hir_def_id_equal(impl_item->definition,
            item->parent_definition)
        && impl_item->generic_parameter_count == 1u;
}

typedef struct CmMirLowerLegacyInstanceQuery {
    CmHirGenericArg *arguments;
    CmHirInstanceSpec spec;
} CmMirLowerLegacyInstanceQuery;

static int cm_mir_lower_legacy_instance_query_init(
    CmMirLowerLegacyInstanceQuery *query, const CmHirContext *hir,
    const CmMirInstance *instance)
{
    const CmHirDefinition *definition;
    const CmHirItem *item;
    const CmHirItem *impl_item;
    uint32_t index;

    memset(query, 0, sizeof(*query));
    if (instance == NULL
        || (instance->substitution_count == 0u)
            != (instance->substitutions == NULL)) return 0;
    if (instance->substitution_count != 0u) {
        query->arguments = (CmHirGenericArg *)cm_alloc_zeroed(
            instance->substitution_count, sizeof(*query->arguments));
    }
    cm_hir_instance_spec_init(&query->spec);
    query->spec.selected_callable = instance->definition;
    query->spec.body_definition = instance->body_definition;
    definition = cm_hir_lookup_definition(hir, instance->definition);
    item = definition == NULL || definition->kind != CM_HIR_DEFINITION_ITEM
            || definition->state != CM_HIR_DEFINITION_BOUND
        ? NULL : cm_hir_get_item(hir, definition->entity.item_id);
    if (item == NULL || item->kind != CM_HIR_ITEM_FUNCTION) goto invalid;
    if (cm_hir_def_id_is_none(item->parent_definition)) {
        query->spec.item_arguments = query->arguments;
        query->spec.item_argument_count = instance->substitution_count;
    } else {
        definition = cm_hir_lookup_definition(hir,
            item->parent_definition);
        impl_item = definition == NULL
                || definition->kind != CM_HIR_DEFINITION_ITEM
                || definition->state != CM_HIR_DEFINITION_BOUND
            ? NULL : cm_hir_get_item(hir, definition->entity.item_id);
        if (instance->substitution_count != 0u
            || impl_item == NULL || impl_item->kind != CM_HIR_ITEM_IMPL
            || impl_item->generic_parameter_count != 0u
            || !impl_item->data.impl_item.has_trait
            || impl_item->data.impl_item.polarity != CM_HIR_IMPL_POSITIVE
            || cm_hir_def_id_is_none(
                item->data.function_item.trait_item_definition)) goto invalid;
        query->spec.declared_trait_callable =
            item->data.function_item.trait_item_definition;
        query->spec.enclosing_impl = impl_item->definition;
        query->spec.implemented_trait =
            impl_item->data.impl_item.trait_type.definition;
        query->spec.self_owner = impl_item->definition;
        query->spec.self_type = impl_item->data.impl_item.self_type;
    }
    for (index = 0u; index < instance->substitution_count; ++index) {
        query->arguments[index].kind = CM_HIR_GENERIC_ARG_TYPE;
        query->arguments[index].data.type = instance->substitutions[index];
    }
    return 1;

invalid:
    cm_free(query->arguments);
    memset(query, 0, sizeof(*query));
    return 0;
}

static void cm_mir_lower_legacy_instance_query_destroy(
    CmMirLowerLegacyInstanceQuery *query)
{
    if (query == NULL) return;
    cm_free(query->arguments);
    memset(query, 0, sizeof(*query));
}

static CmSemanticResultsStatus cm_mir_flow_semantic_expression_query(
    const CmMirFlowPlan *plan, CmHirExprId expression,
    CmSemanticExpressionView *out_view)
{
    CmHirCanonicalInstance caller;
    CmMirLowerLegacyInstanceQuery legacy;
    CmSemanticResultsStatus status;

    if (plan->semantic_evidence == CM_MIR_SEMANTIC_EVIDENCE_BODY) {
        return cm_semantic_results_expression(plan->semantic_results,
            plan->admission, plan->item->data.function_item.body,
            expression, out_view);
    }
    if (plan->semantic_evidence
            != CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    if (!cm_mir_lower_instance_is_canonical(plan->instance)) {
        if (!cm_mir_lower_legacy_instance_query_init(&legacy, plan->hir,
                plan->instance)) return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
        status = cm_semantic_results_instance_expression(
            plan->semantic_results, plan->admission, &legacy.spec,
            expression, out_view);
        cm_mir_lower_legacy_instance_query_destroy(&legacy);
        return status;
    }
    cm_hir_canonical_instance_init(&caller);
    caller.definition = plan->instance->definition;
    caller.body_definition = plan->instance->body_definition;
    caller.body = plan->instance->body;
    caller.bytes = plan->instance->identity_bytes;
    caller.size = plan->instance->identity_size;
    return cm_semantic_results_canonical_instance_expression(
        plan->semantic_results, plan->admission, &caller, expression,
        out_view);
}

static CmSemanticResultsStatus cm_mir_flow_semantic_adjustment_query(
    const CmMirFlowPlan *plan, CmHirExprId expression, uint32_t adjustment,
    CmSemanticAdjustmentView *out_view)
{
    CmHirCanonicalInstance caller;
    CmMirLowerLegacyInstanceQuery legacy;
    CmSemanticResultsStatus status;

    if (plan->semantic_evidence == CM_MIR_SEMANTIC_EVIDENCE_BODY) {
        return cm_semantic_results_expression_adjustment(
            plan->semantic_results, plan->admission,
            plan->item->data.function_item.body, expression, adjustment,
            out_view);
    }
    if (plan->semantic_evidence
            != CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    if (!cm_mir_lower_instance_is_canonical(plan->instance)) {
        if (!cm_mir_lower_legacy_instance_query_init(&legacy, plan->hir,
                plan->instance)) return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
        status = cm_semantic_results_instance_expression_adjustment(
            plan->semantic_results, plan->admission, &legacy.spec,
            expression, adjustment, out_view);
        cm_mir_lower_legacy_instance_query_destroy(&legacy);
        return status;
    }
    cm_hir_canonical_instance_init(&caller);
    caller.definition = plan->instance->definition;
    caller.body_definition = plan->instance->body_definition;
    caller.body = plan->instance->body;
    caller.bytes = plan->instance->identity_bytes;
    caller.size = plan->instance->identity_size;
    return cm_semantic_results_canonical_instance_expression_adjustment(
        plan->semantic_results, plan->admission, &caller, expression,
        adjustment, out_view);
}

static CmSemanticResultsStatus cm_mir_flow_semantic_primitive_query(
    const CmMirFlowPlan *plan, CmHirExprId expression,
    CmSemanticPrimitiveBinaryView *out_view)
{
    CmHirCanonicalInstance caller;
    CmMirLowerLegacyInstanceQuery legacy;
    CmSemanticResultsStatus status;

    if (plan->semantic_evidence == CM_MIR_SEMANTIC_EVIDENCE_BODY) {
        return cm_semantic_results_primitive_binary(plan->semantic_results,
            plan->admission, plan->item->data.function_item.body,
            expression, out_view);
    }
    if (plan->semantic_evidence
            != CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    if (!cm_mir_lower_instance_is_canonical(plan->instance)) {
        if (!cm_mir_lower_legacy_instance_query_init(&legacy, plan->hir,
                plan->instance)) return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
        status = cm_semantic_results_instance_primitive_binary(
            plan->semantic_results, plan->admission, &legacy.spec,
            expression, out_view);
        cm_mir_lower_legacy_instance_query_destroy(&legacy);
        return status;
    }
    cm_hir_canonical_instance_init(&caller);
    caller.definition = plan->instance->definition;
    caller.body_definition = plan->instance->body_definition;
    caller.body = plan->instance->body;
    caller.bytes = plan->instance->identity_bytes;
    caller.size = plan->instance->identity_size;
    return cm_semantic_results_canonical_instance_primitive_binary(
        plan->semantic_results, plan->admission, &caller, expression,
        out_view);
}

static CmSemanticResultsStatus cm_mir_flow_semantic_field_query(
    const CmMirFlowPlan *plan, CmHirExprId expression,
    CmSemanticFieldSelectionView *out_view)
{
    CmHirCanonicalInstance caller;
    CmMirLowerLegacyInstanceQuery legacy;
    CmSemanticResultsStatus status;

    if (plan->semantic_evidence == CM_MIR_SEMANTIC_EVIDENCE_BODY) {
        return cm_semantic_results_field_selection(plan->semantic_results,
            plan->admission, plan->item->data.function_item.body,
            expression, out_view);
    }
    if (plan->semantic_evidence
            != CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    if (!cm_mir_lower_instance_is_canonical(plan->instance)) {
        if (!cm_mir_lower_legacy_instance_query_init(&legacy, plan->hir,
                plan->instance)) return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
        status = cm_semantic_results_instance_field_selection(
            plan->semantic_results, plan->admission, &legacy.spec,
            expression, out_view);
        cm_mir_lower_legacy_instance_query_destroy(&legacy);
        return status;
    }
    cm_hir_canonical_instance_init(&caller);
    caller.definition = plan->instance->definition;
    caller.body_definition = plan->instance->body_definition;
    caller.body = plan->instance->body;
    caller.bytes = plan->instance->identity_bytes;
    caller.size = plan->instance->identity_size;
    return cm_semantic_results_canonical_instance_field_selection(
        plan->semantic_results, plan->admission, &caller, expression,
        out_view);
}

static CmSemanticResultsStatus cm_mir_flow_semantic_signature_query(
    const CmMirFlowPlan *plan, const CmMirBody *callee,
    CmSemanticFunctionSignatureView *out_view)
{
    CmHirCanonicalInstance query;
    CmMirLowerLegacyInstanceQuery legacy;
    CmSemanticResultsStatus status;

    if (plan->semantic_evidence == CM_MIR_SEMANTIC_EVIDENCE_BODY) {
        return cm_semantic_results_signature(plan->semantic_results,
            plan->admission, callee->source_body, out_view);
    }
    if (plan->semantic_evidence
            != CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    if (!cm_mir_lower_instance_is_canonical(&callee->instance)) {
        if (!cm_mir_lower_legacy_instance_query_init(&legacy, plan->hir,
                &callee->instance)) {
            return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
        }
        status = cm_semantic_results_instance_signature(
            plan->semantic_results, plan->admission, &legacy.spec, out_view);
        cm_mir_lower_legacy_instance_query_destroy(&legacy);
        return status;
    }
    cm_hir_canonical_instance_init(&query);
    query.definition = callee->instance.definition;
    query.body_definition = callee->instance.body_definition;
    query.body = callee->instance.body;
    query.bytes = callee->instance.identity_bytes;
    query.size = callee->instance.identity_size;
    return cm_semantic_results_canonical_instance_signature(
        plan->semantic_results, plan->admission, &query, out_view);
}

static CmSemanticResultsStatus
cm_mir_flow_semantic_signature_parameter_query(
    const CmMirFlowPlan *plan, const CmMirBody *callee, uint32_t parameter,
    CmSemanticTypeView *out_view)
{
    CmHirCanonicalInstance query;
    CmMirLowerLegacyInstanceQuery legacy;
    CmSemanticResultsStatus status;

    if (plan->semantic_evidence == CM_MIR_SEMANTIC_EVIDENCE_BODY) {
        return cm_semantic_results_signature_parameter(
            plan->semantic_results, plan->admission, callee->source_body,
            parameter, out_view);
    }
    if (plan->semantic_evidence
            != CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    if (!cm_mir_lower_instance_is_canonical(&callee->instance)) {
        if (!cm_mir_lower_legacy_instance_query_init(&legacy, plan->hir,
                &callee->instance)) {
            return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
        }
        status = cm_semantic_results_instance_signature_parameter(
            plan->semantic_results, plan->admission, &legacy.spec, parameter,
            out_view);
        cm_mir_lower_legacy_instance_query_destroy(&legacy);
        return status;
    }
    cm_hir_canonical_instance_init(&query);
    query.definition = callee->instance.definition;
    query.body_definition = callee->instance.body_definition;
    query.body = callee->instance.body;
    query.bytes = callee->instance.identity_bytes;
    query.size = callee->instance.identity_size;
    return cm_semantic_results_canonical_instance_signature_parameter(
        plan->semantic_results, plan->admission, &query, parameter,
        out_view);
}

static CmSemanticResultsStatus cm_mir_flow_semantic_call_query(
    const CmMirFlowPlan *plan, const CmMirBody *callee,
    CmHirExprId expression, CmSemanticDirectCallView *out_view)
{
    CmHirCanonicalInstance caller;
    CmHirCanonicalInstance target;
    CmMirLowerLegacyInstanceQuery legacy_caller;
    CmMirLowerLegacyInstanceQuery legacy_target;
    CmSemanticResultsStatus status;

    if (plan->semantic_evidence == CM_MIR_SEMANTIC_EVIDENCE_BODY) {
        return cm_semantic_results_direct_call(plan->semantic_results,
            plan->admission, plan->item->data.function_item.body,
            expression, out_view);
    }
    if (plan->semantic_evidence
            != CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    if (cm_mir_lower_instance_is_canonical(plan->instance)
        != cm_mir_lower_instance_is_canonical(&callee->instance)) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    if (!cm_mir_lower_instance_is_canonical(plan->instance)) {
        memset(&legacy_caller, 0, sizeof(legacy_caller));
        memset(&legacy_target, 0, sizeof(legacy_target));
        if (!cm_mir_lower_legacy_instance_query_init(&legacy_caller,
                plan->hir, plan->instance)
            || !cm_mir_lower_legacy_instance_query_init(&legacy_target,
                plan->hir, &callee->instance)) {
            cm_mir_lower_legacy_instance_query_destroy(&legacy_caller);
            cm_mir_lower_legacy_instance_query_destroy(&legacy_target);
            return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
        }
        status = cm_semantic_results_instance_direct_call(
            plan->semantic_results, plan->admission, &legacy_caller.spec,
            expression, &legacy_target.spec, out_view);
        cm_mir_lower_legacy_instance_query_destroy(&legacy_target);
        cm_mir_lower_legacy_instance_query_destroy(&legacy_caller);
        return status;
    }
    cm_hir_canonical_instance_init(&caller);
    caller.definition = plan->instance->definition;
    caller.body_definition = plan->instance->body_definition;
    caller.body = plan->instance->body;
    caller.bytes = plan->instance->identity_bytes;
    caller.size = plan->instance->identity_size;
    cm_hir_canonical_instance_init(&target);
    target.definition = callee->instance.definition;
    target.body_definition = callee->instance.body_definition;
    target.body = callee->instance.body;
    target.bytes = callee->instance.identity_bytes;
    target.size = callee->instance.identity_size;
    return cm_semantic_results_canonical_instance_direct_call(
        plan->semantic_results, plan->admission, &caller, expression,
        &target, out_view);
}

static CmSemanticResultsStatus cm_mir_flow_semantic_call_parameter_query(
    const CmMirFlowPlan *plan, const CmMirBody *callee,
    CmHirExprId expression, uint32_t parameter,
    CmSemanticTypeView *out_view)
{
    CmHirCanonicalInstance caller;
    CmHirCanonicalInstance target;
    CmMirLowerLegacyInstanceQuery legacy_caller;
    CmMirLowerLegacyInstanceQuery legacy_target;
    CmSemanticResultsStatus status;

    if (plan->semantic_evidence == CM_MIR_SEMANTIC_EVIDENCE_BODY) {
        return cm_semantic_results_direct_call_parameter(
            plan->semantic_results, plan->admission,
            plan->item->data.function_item.body, expression, parameter,
            out_view);
    }
    if (plan->semantic_evidence
            != CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    if (cm_mir_lower_instance_is_canonical(plan->instance)
        != cm_mir_lower_instance_is_canonical(&callee->instance)) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    if (!cm_mir_lower_instance_is_canonical(plan->instance)) {
        memset(&legacy_caller, 0, sizeof(legacy_caller));
        memset(&legacy_target, 0, sizeof(legacy_target));
        if (!cm_mir_lower_legacy_instance_query_init(&legacy_caller,
                plan->hir, plan->instance)
            || !cm_mir_lower_legacy_instance_query_init(&legacy_target,
                plan->hir, &callee->instance)) {
            cm_mir_lower_legacy_instance_query_destroy(&legacy_caller);
            cm_mir_lower_legacy_instance_query_destroy(&legacy_target);
            return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
        }
        status = cm_semantic_results_instance_direct_call_parameter(
            plan->semantic_results, plan->admission, &legacy_caller.spec,
            expression, &legacy_target.spec, parameter, out_view);
        cm_mir_lower_legacy_instance_query_destroy(&legacy_target);
        cm_mir_lower_legacy_instance_query_destroy(&legacy_caller);
        return status;
    }
    cm_hir_canonical_instance_init(&caller);
    caller.definition = plan->instance->definition;
    caller.body_definition = plan->instance->body_definition;
    caller.body = plan->instance->body;
    caller.bytes = plan->instance->identity_bytes;
    caller.size = plan->instance->identity_size;
    cm_hir_canonical_instance_init(&target);
    target.definition = callee->instance.definition;
    target.body_definition = callee->instance.body_definition;
    target.body = callee->instance.body;
    target.bytes = callee->instance.identity_bytes;
    target.size = callee->instance.identity_size;
    return cm_semantic_results_canonical_instance_direct_call_parameter(
        plan->semantic_results, plan->admission, &caller, expression,
        &target, parameter, out_view);
}

static CmSemanticResultsStatus cm_mir_flow_semantic_callable_query(
    const CmMirFlowPlan *plan, const CmMirBody *callee,
    CmHirExprId expression,
    CmSemanticCallableSelectionView *out_view)
{
    CmHirCanonicalInstance caller;
    CmHirCanonicalInstance target;
    CmMirLowerLegacyInstanceQuery legacy_caller;
    CmMirLowerLegacyInstanceQuery legacy_target;
    CmSemanticResultsStatus status;

    if (plan->semantic_evidence == CM_MIR_SEMANTIC_EVIDENCE_BODY) {
        return cm_semantic_results_callable_selection(
            plan->semantic_results, plan->admission,
            plan->item->data.function_item.body, expression, out_view);
    }
    if (plan->semantic_evidence
            != CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    if (cm_mir_lower_instance_is_canonical(plan->instance)
        != cm_mir_lower_instance_is_canonical(&callee->instance)) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    if (!cm_mir_lower_instance_is_canonical(plan->instance)) {
        memset(&legacy_caller, 0, sizeof(legacy_caller));
        memset(&legacy_target, 0, sizeof(legacy_target));
        if (!cm_mir_lower_legacy_instance_query_init(&legacy_caller,
                plan->hir, plan->instance)
            || !cm_mir_lower_legacy_instance_query_init(&legacy_target,
                plan->hir, &callee->instance)) {
            cm_mir_lower_legacy_instance_query_destroy(&legacy_caller);
            cm_mir_lower_legacy_instance_query_destroy(&legacy_target);
            return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
        }
        status =
            cm_semantic_results_instance_callable_selection_for_callee(
                plan->semantic_results, plan->admission,
                &legacy_caller.spec, expression, &legacy_target.spec,
                out_view);
        cm_mir_lower_legacy_instance_query_destroy(&legacy_target);
        cm_mir_lower_legacy_instance_query_destroy(&legacy_caller);
        return status;
    }
    cm_hir_canonical_instance_init(&caller);
    caller.definition = plan->instance->definition;
    caller.body_definition = plan->instance->body_definition;
    caller.body = plan->instance->body;
    caller.bytes = plan->instance->identity_bytes;
    caller.size = plan->instance->identity_size;
    cm_hir_canonical_instance_init(&target);
    target.definition = callee->instance.definition;
    target.body_definition = callee->instance.body_definition;
    target.body = callee->instance.body;
    target.bytes = callee->instance.identity_bytes;
    target.size = callee->instance.identity_size;
    return
        cm_semantic_results_canonical_instance_callable_selection_for_callee(
            plan->semantic_results, plan->admission, &caller, expression,
            &target, out_view);
}

/* Definition lookup hint; exact-instance authority is checked after resolve. */
static CmSemanticResultsStatus cm_mir_flow_semantic_callable_hint_query(
    const CmMirFlowPlan *plan, CmHirExprId expression,
    CmSemanticCallableSelectionView *out_view)
{
    CmHirCanonicalInstance caller;
    CmMirLowerLegacyInstanceQuery legacy;
    CmSemanticResultsStatus status;

    if (plan->semantic_evidence == CM_MIR_SEMANTIC_EVIDENCE_BODY) {
        return cm_semantic_results_callable_selection(
            plan->semantic_results, plan->admission,
            plan->item->data.function_item.body, expression, out_view);
    }
    if (plan->semantic_evidence
            != CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    if (!cm_mir_lower_instance_is_canonical(plan->instance)) {
        if (!cm_mir_lower_legacy_instance_query_init(&legacy, plan->hir,
                plan->instance)) return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
        status = cm_semantic_results_instance_callable_selection(
            plan->semantic_results, plan->admission, &legacy.spec,
            expression, out_view);
        cm_mir_lower_legacy_instance_query_destroy(&legacy);
        return status;
    }
    cm_hir_canonical_instance_init(&caller);
    caller.definition = plan->instance->definition;
    caller.body_definition = plan->instance->body_definition;
    caller.body = plan->instance->body;
    caller.bytes = plan->instance->identity_bytes;
    caller.size = plan->instance->identity_size;
    return cm_semantic_results_canonical_instance_callable_selection(
        plan->semantic_results, plan->admission, &caller, expression,
        out_view);
}

static CmSemanticResultsStatus cm_mir_flow_semantic_callable_argument_query(
    const CmMirFlowPlan *plan, CmHirExprId expression, uint32_t argument,
    CmHirExprId *out_expression)
{
    CmHirCanonicalInstance caller;
    CmMirLowerLegacyInstanceQuery legacy;
    CmSemanticResultsStatus status;

    if (plan->semantic_evidence == CM_MIR_SEMANTIC_EVIDENCE_BODY) {
        return cm_semantic_results_callable_argument(plan->semantic_results,
            plan->admission, plan->item->data.function_item.body,
            expression, argument, out_expression);
    }
    if (plan->semantic_evidence
            != CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    if (!cm_mir_lower_instance_is_canonical(plan->instance)) {
        if (!cm_mir_lower_legacy_instance_query_init(&legacy, plan->hir,
                plan->instance)) return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
        status = cm_semantic_results_instance_callable_argument(
            plan->semantic_results, plan->admission, &legacy.spec,
            expression, argument, out_expression);
        cm_mir_lower_legacy_instance_query_destroy(&legacy);
        return status;
    }
    cm_hir_canonical_instance_init(&caller);
    caller.definition = plan->instance->definition;
    caller.body_definition = plan->instance->body_definition;
    caller.body = plan->instance->body;
    caller.bytes = plan->instance->identity_bytes;
    caller.size = plan->instance->identity_size;
    return cm_semantic_results_canonical_instance_callable_argument(
        plan->semantic_results, plan->admission, &caller, expression,
        argument, out_expression);
}

static CmSemanticResultsStatus
cm_mir_flow_semantic_callable_generic_argument_query(
    const CmMirFlowPlan *plan, CmHirExprId expression,
    CmSemanticCallableGenericArgumentDomain domain, uint32_t argument,
    CmSemanticGenericArgumentView *out_view)
{
    CmHirCanonicalInstance caller;
    CmMirLowerLegacyInstanceQuery legacy;
    CmSemanticResultsStatus status;

    if (plan->semantic_evidence == CM_MIR_SEMANTIC_EVIDENCE_BODY) {
        return cm_semantic_results_callable_generic_argument(
            plan->semantic_results, plan->admission,
            plan->item->data.function_item.body, expression, domain,
            argument, out_view);
    }
    if (plan->semantic_evidence
            != CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    if (!cm_mir_lower_instance_is_canonical(plan->instance)) {
        if (!cm_mir_lower_legacy_instance_query_init(&legacy, plan->hir,
                plan->instance)) return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
        status = cm_semantic_results_instance_callable_generic_argument(
            plan->semantic_results, plan->admission, &legacy.spec,
            expression, domain, argument, out_view);
        cm_mir_lower_legacy_instance_query_destroy(&legacy);
        return status;
    }
    cm_hir_canonical_instance_init(&caller);
    caller.definition = plan->instance->definition;
    caller.body_definition = plan->instance->body_definition;
    caller.body = plan->instance->body;
    caller.bytes = plan->instance->identity_bytes;
    caller.size = plan->instance->identity_size;
    return cm_semantic_results_canonical_instance_callable_generic_argument(
        plan->semantic_results, plan->admission, &caller, expression,
        domain, argument, out_view);
}

static CmSemanticResultsStatus cm_mir_flow_semantic_callable_parameter_query(
    const CmMirFlowPlan *plan, const CmMirBody *callee,
    CmHirExprId expression, uint32_t parameter,
    CmSemanticTypeView *out_view)
{
    CmHirCanonicalInstance caller;
    CmHirCanonicalInstance target;
    CmMirLowerLegacyInstanceQuery legacy_caller;
    CmMirLowerLegacyInstanceQuery legacy_target;
    CmSemanticResultsStatus status;

    if (plan->semantic_evidence == CM_MIR_SEMANTIC_EVIDENCE_BODY) {
        return cm_semantic_results_callable_parameter(plan->semantic_results,
            plan->admission, plan->item->data.function_item.body,
            expression, parameter, out_view);
    }
    if (plan->semantic_evidence
            != CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    if (cm_mir_lower_instance_is_canonical(plan->instance)
        != cm_mir_lower_instance_is_canonical(&callee->instance)) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    if (!cm_mir_lower_instance_is_canonical(plan->instance)) {
        memset(&legacy_caller, 0, sizeof(legacy_caller));
        memset(&legacy_target, 0, sizeof(legacy_target));
        if (!cm_mir_lower_legacy_instance_query_init(&legacy_caller,
                plan->hir, plan->instance)
            || !cm_mir_lower_legacy_instance_query_init(&legacy_target,
                plan->hir, &callee->instance)) {
            cm_mir_lower_legacy_instance_query_destroy(&legacy_caller);
            cm_mir_lower_legacy_instance_query_destroy(&legacy_target);
            return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
        }
        status =
            cm_semantic_results_instance_callable_parameter_for_callee(
                plan->semantic_results, plan->admission,
                &legacy_caller.spec, expression, &legacy_target.spec,
                parameter, out_view);
        cm_mir_lower_legacy_instance_query_destroy(&legacy_target);
        cm_mir_lower_legacy_instance_query_destroy(&legacy_caller);
        return status;
    }
    cm_hir_canonical_instance_init(&caller);
    caller.definition = plan->instance->definition;
    caller.body_definition = plan->instance->body_definition;
    caller.body = plan->instance->body;
    caller.bytes = plan->instance->identity_bytes;
    caller.size = plan->instance->identity_size;
    cm_hir_canonical_instance_init(&target);
    target.definition = callee->instance.definition;
    target.body_definition = callee->instance.body_definition;
    target.body = callee->instance.body;
    target.bytes = callee->instance.identity_bytes;
    target.size = callee->instance.identity_size;
    return
        cm_semantic_results_canonical_instance_callable_parameter_for_callee(
            plan->semantic_results, plan->admission, &caller, expression,
            &target, parameter, out_view);
}

typedef struct CmMirFlowOutput {
    const CmMirFlowPlan *plan;
    CmVec *locals;
    CmVec *statements;
    CmVec *blocks;
    CmVec *block_starts;
    CmVec *arguments;
    CmVec *aggregate_fields;
    CmVec *projections;
    uint32_t call_index;
    CmMirBasicBlockId current_block;
} CmMirFlowOutput;

static int cm_mir_semantic_types_equal(const CmSemanticTypeView *left,
    const CmSemanticTypeView *right)
{
    int equal;

    equal = 0;
    return cm_semantic_type_view_equal(left, right, &equal)
            == CM_SEMANTIC_RESULTS_OK
        && equal;
}

typedef struct CmMirReceiverAdjustmentPlan {
    int present;
    CmMirBorrowKind borrow_kind;
    CmHirTypeId source_type;
    CmHirTypeId target_type;
    CmHirExprId expression;
    CmSpan span;
} CmMirReceiverAdjustmentPlan;

static int cm_mir_flow_receiver_adjustment(
    const CmMirFlowPlan *plan, const CmHirExpr *call,
    CmHirExprId call_id,
    const CmSemanticCallableSelectionView *selection,
    CmMirReceiverAdjustmentPlan *out_adjustment)
{
    const CmHirExpr *receiver;
    const CmHirBody *body;
    const CmHirItem *declared;
    const CmHirItem *selected;
    const CmHirType *target;
    CmSemanticExpressionView expression_view;
    CmSemanticAdjustmentView adjustment;
    CmHirReceiverKind receiver_kind;
    CmHirMutability mutability;
    int self_matches;

    if (plan == NULL || call == NULL || selection == NULL
        || out_adjustment == NULL || call->kind != CM_HIR_EXPR_METHOD_CALL
        || selection->body != call->owner_body
        || selection->expression != call_id
        || selection->syntax != CM_HIR_CALLABLE_DOT_METHOD
        || selection->receiver_argument != 0u
        || selection->receiver_expression != call->data.method_call.receiver) {
        return 0;
    }
    memset(out_adjustment, 0, sizeof(*out_adjustment));
    receiver = cm_hir_get_expr(plan->hir,
        call->data.method_call.receiver);
    body = cm_hir_get_body(plan->hir, call->owner_body);
    declared = cm_mir_flow_definition_item(plan->hir,
        selection->declared_trait_callable);
    selected = cm_mir_flow_definition_item(plan->hir,
        selection->selected_callable);
    memset(&expression_view, 0, sizeof(expression_view));
    self_matches = 0;
    if (receiver == NULL || declared == NULL || selected == NULL
        || declared->kind != CM_HIR_ITEM_FUNCTION
        || selected->kind != CM_HIR_ITEM_FUNCTION
        || receiver->owner_body != call->owner_body
        || cm_mir_flow_semantic_expression_query(plan,
            call->data.method_call.receiver, &expression_view)
                != CM_SEMANTIC_RESULTS_OK
        || expression_view.body != call->owner_body
        || expression_view.expression != call->data.method_call.receiver
        || cm_semantic_type_view_matches_monomorphic_hir(
            plan->semantic_results, plan->admission,
            &expression_view.unadjusted_type, receiver->type,
            &self_matches) != CM_SEMANTIC_RESULTS_OK
        || !self_matches
        || !cm_mir_semantic_types_equal(&selection->requested_self_type,
            &expression_view.unadjusted_type)) {
        return 0;
    }
    receiver_kind = declared->data.function_item.signature.receiver;
    if (selected->data.function_item.signature.receiver != receiver_kind
        || declared->data.function_item.signature.parameter_count
            != selection->argument_count
        || selected->data.function_item.signature.parameter_count
            != selection->argument_count
        || selection->argument_count == 0u
        || declared->data.function_item.signature.parameters == NULL
        || selected->data.function_item.signature.parameters == NULL) {
        return 0;
    }
    if (receiver_kind == CM_HIR_RECEIVER_VALUE) {
        return expression_view.adjustment_count == 0u
            && cm_mir_semantic_types_equal(
                &expression_view.unadjusted_type,
                &expression_view.adjusted_type);
    }
    if ((receiver_kind != CM_HIR_RECEIVER_REF_SHARED
            && receiver_kind != CM_HIR_RECEIVER_REF_MUTABLE)
        || expression_view.adjustment_count != 1u || body == NULL
        || receiver->kind != CM_HIR_EXPR_LOCAL
        || receiver->data.local.local_index >= body->local_count) {
        return 0;
    }
    memset(&adjustment, 0, sizeof(adjustment));
    if (cm_mir_flow_semantic_adjustment_query(plan,
            call->data.method_call.receiver, 0u, &adjustment)
                != CM_SEMANTIC_RESULTS_OK
        || adjustment.body != call->owner_body
        || adjustment.expression != call->data.method_call.receiver
        || adjustment.index != 0u
        || adjustment.has_selected_trait
        || !cm_hir_def_id_is_none(adjustment.selected_trait)
        || !cm_hir_def_id_is_none(adjustment.selected_method)
        || !cm_hir_def_id_is_none(adjustment.selected_impl)
        || !cm_mir_semantic_types_equal(&adjustment.source_type,
            &expression_view.unadjusted_type)
        || !cm_mir_semantic_types_equal(&adjustment.target_type,
            &expression_view.adjusted_type)) {
        return 0;
    }
    if (receiver_kind == CM_HIR_RECEIVER_REF_SHARED) {
        if (adjustment.kind != CM_SEMANTIC_ADJUSTMENT_BORROW_SHARED) {
            return 0;
        }
        out_adjustment->borrow_kind = CM_MIR_BORROW_SHARED;
        mutability = CM_HIR_IMMUTABLE;
    } else {
        if (adjustment.kind != CM_SEMANTIC_ADJUSTMENT_BORROW_MUTABLE
            || body->locals[receiver->data.local.local_index].mutability
                != CM_HIR_MUTABLE) {
            return 0;
        }
        out_adjustment->borrow_kind = CM_MIR_BORROW_MUTABLE;
        mutability = CM_HIR_MUTABLE;
    }
    out_adjustment->source_type = receiver->type;
    out_adjustment->target_type =
        selected->data.function_item.signature.parameters[0].type;
    out_adjustment->expression = call->data.method_call.receiver;
    out_adjustment->span = receiver->span;
    target = cm_hir_get_type(plan->hir, out_adjustment->target_type);
    if (target == NULL || target->kind != CM_HIR_TYPE_REFERENCE_KIND
        || target->data.reference_type.region.kind != CM_HIR_REGION_ERASED
        || target->data.reference_type.mutability != mutability
        || !cm_mir_hir_type_equal(plan->hir,
            target->data.reference_type.pointee, receiver->type)
        || !cm_mir_lower_type_is_call_scalar(plan->hir,
            out_adjustment->target_type)) {
        return 0;
    }
    out_adjustment->present = 1;
    return 1;
}

static int cm_mir_flow_fail(CmMirFlowPlan *plan, CmMirFlowError error,
    CmHirExprId expression, CmMirStatus status)
{
    plan->error = error;
    plan->error_expression = expression;
    plan->error_status = status;
    return 0;
}

static int cm_mir_flow_expression_type(const CmMirFlowPlan *plan,
    CmHirExprId expression_id, const CmHirExpr **out_expression,
    CmHirTypeId *out_type)
{
    const CmHirExpr *expression;

    expression = cm_hir_get_expr(plan->hir, expression_id);
    if (expression == NULL
        || expression->owner_body != plan->item->data.function_item.body
        || !cm_mir_lower_type(plan->hir, plan->item,
            plan->instance->substitutions,
            plan->instance->substitution_count, expression->type,
            out_type)
        || !cm_mir_lower_type_target_valid(plan->context, plan->hir,
            plan->item, *out_type, 0u)) {
        return 0;
    }
    if (out_expression != NULL) *out_expression = expression;
    return 1;
}

static int cm_mir_flow_span_within(CmSpan inner, CmSpan outer)
{
    return inner.source != 0u && inner.source == outer.source
        && inner.start <= inner.end && inner.start >= outer.start
        && inner.end <= outer.end;
}

static int cm_mir_flow_preflight(CmMirFlowPlan *plan,
    CmHirExprId expression_id, uint32_t visible_local_count,
    int has_destination, size_t depth, uint32_t *out_projection_count)
{
    const CmHirExpr *expression;
    CmHirTypeId type;
    CmHirTypeId local_type;
    uint32_t expression_projection_count;
    int ok;

    if (expression_id == CM_HIR_EXPR_NONE
        || depth >= plan->hir->expressions.len
        || depth >= CM_MIR_FLOW_RECURSION_LIMIT
        || cm_mir_seen_expression(&plan->seen, expression_id)) {
        return cm_mir_flow_fail(plan, CM_MIR_FLOW_INVALID, expression_id,
            CM_MIR_OK);
    }
    if (!cm_mir_flow_expression_type(plan, expression_id, &expression,
            &type)) {
        return cm_mir_flow_fail(plan, CM_MIR_FLOW_INVALID, expression_id,
            CM_MIR_OK);
    }
    (void)cm_vec_push(&plan->seen, &expression_id);
    expression_projection_count = 0u;
    ok = 0;
    if (expression->kind == CM_HIR_EXPR_BLOCK) {
        const CmHirExpr *tail;
        CmHirTypeId tail_type;

        if (expression->data.block.statement_count != 0u
            || expression->data.block.statements != NULL
            || expression->data.block.tail_expression == CM_HIR_EXPR_NONE) {
            return cm_mir_flow_fail(plan, CM_MIR_FLOW_UNSUPPORTED,
                expression_id, CM_MIR_OK);
        }
        if (!cm_mir_flow_expression_type(plan,
                expression->data.block.tail_expression, &tail, &tail_type)
            || !cm_mir_hir_type_equal(plan->hir, type, tail_type)
            || !cm_mir_flow_span_within(tail->span, expression->span)) {
            return cm_mir_flow_fail(plan, CM_MIR_FLOW_INVALID,
                expression_id, CM_MIR_OK);
        }
        ok = cm_mir_flow_preflight(plan,
            expression->data.block.tail_expression, visible_local_count,
            has_destination, depth + 1u, &expression_projection_count);
    } else if (expression->kind == CM_HIR_EXPR_LOCAL) {
        if (expression->data.local.local_index == UINT32_MAX
            || expression->data.local.local_index >= visible_local_count
            || expression->data.local.local_index >= plan->body->local_count
            || !cm_mir_lower_type(plan->hir, plan->item,
                plan->instance->substitutions,
                plan->instance->substitution_count,
                plan->body->locals[expression->data.local.local_index].type,
                &local_type)
            || !cm_mir_hir_type_equal(plan->hir, type, local_type)) {
            return cm_mir_flow_fail(plan, CM_MIR_FLOW_INVALID,
                expression_id, CM_MIR_OK);
        }
        if (has_destination) {
            if (plan->statement_count == UINT32_MAX) {
                return cm_mir_flow_fail(plan, CM_MIR_FLOW_INVALID,
                    expression_id, CM_MIR_OK);
            }
            plan->statement_count += 1u;
        }
        ok = 1;
    } else if (expression->kind == CM_HIR_EXPR_INTEGER) {
        if (expression->data.integer.high_bits != 0u
            || (cm_mir_lower_type_is_u32(plan->hir, type)
                && expression->data.integer.low_bits > (uint64_t)UINT32_MAX)
            || (cm_mir_lower_type_is_usize(plan->hir, type)
                && !cm_mir_lower_usize_value_valid(plan->context,
                    expression->data.integer.low_bits))
            || (!cm_mir_lower_type_is_u32(plan->hir, type)
                && !cm_mir_lower_type_is_usize(plan->hir, type)
                && expression->data.integer.low_bits > (uint64_t)INT32_MAX)
            || (!cm_mir_lower_type_is_i32(plan->hir, type)
                && !cm_mir_lower_type_is_u32(plan->hir, type)
                && !cm_mir_lower_type_is_usize(plan->hir, type))) {
            return cm_mir_flow_fail(plan, CM_MIR_FLOW_CONSTANT_RANGE,
                expression_id, CM_MIR_OK);
        }
        if (has_destination) {
            if (plan->statement_count == UINT32_MAX) {
                return cm_mir_flow_fail(plan, CM_MIR_FLOW_INVALID,
                    expression_id, CM_MIR_OK);
            }
            plan->statement_count += 1u;
        }
        ok = 1;
    } else if (expression->kind == CM_HIR_EXPR_FIELD) {
        const CmHirExpr *base_expression;
        const CmHirType *base_hir_type;
        const CmHirItem *aggregate;
        CmHirTypeId base_type;
        CmHirTypeId field_type;
        uint32_t base_projection_count;
        uint32_t projection_count;
        CmSemanticFieldSelectionView semantic_field;
        CmSemanticExpressionView semantic_base;
        CmSemanticExpressionView semantic_result;

        if (!cm_mir_flow_preflight(plan, expression->data.field.base,
                visible_local_count, 0, depth + 1u,
                &base_projection_count)
            || !cm_mir_flow_expression_type(plan,
                expression->data.field.base, &base_expression, &base_type)) {
            return 0;
        }
        if (plan->semantic_results != NULL
            && (plan->admission == NULL
            || cm_mir_flow_semantic_field_query(plan, expression_id,
                &semantic_field) != CM_SEMANTIC_RESULTS_OK
            || semantic_field.base_expression != expression->data.field.base
            || !cm_hir_def_id_equal(semantic_field.aggregate_definition,
                expression->data.field.definition)
            || semantic_field.field_index != expression->data.field.field_index
            || cm_mir_flow_semantic_expression_query(plan,
                expression->data.field.base, &semantic_base)
                != CM_SEMANTIC_RESULTS_OK
            || cm_mir_flow_semantic_expression_query(plan, expression_id,
                &semantic_result) != CM_SEMANTIC_RESULTS_OK
            || semantic_base.adjustment_count != 0u
            || semantic_result.adjustment_count != 0u
            || !cm_mir_semantic_types_equal(&semantic_field.base_type,
                &semantic_base.adjusted_type)
            || !cm_mir_semantic_types_equal(&semantic_field.field_type,
                &semantic_result.adjusted_type))) {
            return cm_mir_flow_fail(plan, CM_MIR_FLOW_ADMISSION,
                expression_id, CM_MIR_INVALID_ADMISSION);
        }
        base_hir_type = cm_hir_get_type(plan->hir, base_type);
        aggregate = base_hir_type == NULL
                || base_hir_type->kind != CM_HIR_TYPE_ADT_KIND
                || base_hir_type->data.named_type.argument_count != 0u
                || base_hir_type->data.named_type.arguments != NULL
                || !cm_hir_def_id_equal(
                    base_hir_type->data.named_type.definition,
                    expression->data.field.definition)
            ? NULL : cm_mir_lower_named_struct(plan->hir, plan->item,
                expression->data.field.definition);
        if (aggregate == NULL
            || expression->data.field.field_index
                >= aggregate->data.aggregate_item.field_count
            || !cm_mir_lower_type(plan->hir, plan->item,
                plan->instance->substitutions,
                plan->instance->substitution_count,
                aggregate->data.aggregate_item
                    .fields[expression->data.field.field_index].type,
                &field_type)
            || !cm_mir_hir_type_equal(plan->hir, type, field_type)
            || !cm_mir_flow_span_within(base_expression->span,
                expression->span)
            || base_projection_count >= CM_MIR_MAX_PLACE_PROJECTIONS) {
            return cm_mir_flow_fail(plan, CM_MIR_FLOW_INVALID,
                expression_id, CM_MIR_OK);
        }
        projection_count = base_projection_count + 1u;
        if (plan->projection_count > UINT32_MAX - projection_count
            || (has_destination && plan->statement_count == UINT32_MAX)) {
            return cm_mir_flow_fail(plan, CM_MIR_FLOW_INVALID,
                expression_id, CM_MIR_ID_EXHAUSTED);
        }
        plan->projection_count += projection_count;
        if (has_destination) plan->statement_count += 1u;
        expression_projection_count = projection_count;
        ok = 1;
    } else if (expression->kind == CM_HIR_EXPR_AGGREGATE) {
        const CmHirType *aggregate_type;
        const CmHirItem *aggregate;
        int seen[CM_MIR_MAX_AGGREGATE_FIELDS];
        uint32_t index;

        aggregate_type = cm_hir_get_type(plan->hir, type);
        aggregate = aggregate_type == NULL
                || aggregate_type->kind != CM_HIR_TYPE_ADT_KIND
                || aggregate_type->data.named_type.argument_count != 0u
                || aggregate_type->data.named_type.arguments != NULL
                || !cm_hir_def_id_equal(
                    aggregate_type->data.named_type.definition,
                    expression->data.aggregate.definition)
            ? NULL : cm_mir_lower_named_struct(plan->hir, plan->item,
                expression->data.aggregate.definition);
        if (aggregate == NULL
            || expression->data.aggregate.field_count
                != aggregate->data.aggregate_item.field_count
            || expression->data.aggregate.field_count
                > CM_MIR_MAX_AGGREGATE_FIELDS
            || (expression->data.aggregate.field_count == 0u)
                != (expression->data.aggregate.fields == NULL)
            || plan->aggregate_field_count
                > UINT32_MAX - expression->data.aggregate.field_count
            || plan->statement_count == UINT32_MAX
            || (!has_destination && plan->temporary_count == UINT32_MAX)) {
            return cm_mir_flow_fail(plan, CM_MIR_FLOW_INVALID,
                expression_id, CM_MIR_OK);
        }
        plan->aggregate_field_count +=
            expression->data.aggregate.field_count;
        plan->statement_count += 1u;
        if (!has_destination) {
            if (plan->temporary_count == UINT32_MAX) {
                return cm_mir_flow_fail(plan, CM_MIR_FLOW_INVALID,
                    expression_id, CM_MIR_ID_EXHAUSTED);
            }
            plan->temporary_count += 1u;
        }
        memset(seen, 0, sizeof(seen));
        ok = 1;
        for (index = 0u; index < expression->data.aggregate.field_count;
             ++index) {
            const CmHirAggregateFieldValue *field;
            const CmHirExpr *value_expression;
            CmHirTypeId value_type;
            CmHirTypeId declared_type;

            field = &expression->data.aggregate.fields[index];
            if (field->field_index
                    >= aggregate->data.aggregate_item.field_count
                || seen[field->field_index]
                || !cm_mir_flow_span_within(field->span, expression->span)
                || !cm_mir_flow_expression_type(plan, field->value,
                    &value_expression, &value_type)
                || !cm_mir_flow_span_within(value_expression->span,
                    field->span)
                || !cm_mir_lower_type(plan->hir, plan->item,
                    plan->instance->substitutions,
                    plan->instance->substitution_count,
                    aggregate->data.aggregate_item
                        .fields[field->field_index].type,
                    &declared_type)
                || !cm_mir_hir_type_equal(plan->hir, value_type,
                    declared_type)
                || !cm_mir_flow_preflight(plan, field->value,
                    visible_local_count, 0, depth + 1u, NULL)) {
                ok = 0;
                break;
            }
            seen[field->field_index] = 1;
        }
        if (!ok) {
            return plan->error == CM_MIR_FLOW_OK
                ? cm_mir_flow_fail(plan, CM_MIR_FLOW_INVALID,
                    expression_id, CM_MIR_OK)
                : 0;
        }
    } else if (expression->kind == CM_HIR_EXPR_BINARY) {
        CmHirTypeId left_type;
        CmHirTypeId right_type;
        CmSemanticPrimitiveBinaryView semantic_binary;
        CmSemanticExpressionView semantic_left;
        CmSemanticExpressionView semantic_right;
        CmSemanticExpressionView semantic_result;

        if (plan->semantic_results != NULL
            && (plan->admission == NULL
            || cm_mir_flow_semantic_primitive_query(plan, expression_id,
                &semantic_binary) != CM_SEMANTIC_RESULTS_OK
            || semantic_binary.operator_kind
                != expression->data.binary.operator_kind
            || semantic_binary.left_expression
                != expression->data.binary.left
            || semantic_binary.right_expression
                != expression->data.binary.right
            || cm_mir_flow_semantic_expression_query(plan,
                semantic_binary.left_expression, &semantic_left)
                != CM_SEMANTIC_RESULTS_OK
            || cm_mir_flow_semantic_expression_query(plan,
                semantic_binary.right_expression, &semantic_right)
                != CM_SEMANTIC_RESULTS_OK
            || cm_mir_flow_semantic_expression_query(plan, expression_id,
                &semantic_result) != CM_SEMANTIC_RESULTS_OK
            || semantic_left.adjustment_count != 0u
            || semantic_right.adjustment_count != 0u
            || semantic_result.adjustment_count != 0u
            || !cm_mir_semantic_types_equal(&semantic_binary.left_type,
                &semantic_left.adjusted_type)
            || !cm_mir_semantic_types_equal(&semantic_binary.right_type,
                &semantic_right.adjusted_type)
            || !cm_mir_semantic_types_equal(&semantic_binary.result_type,
                &semantic_result.adjusted_type))) {
            return cm_mir_flow_fail(plan, CM_MIR_FLOW_ADMISSION,
                expression_id, CM_MIR_INVALID_ADMISSION);
        }
        if ((expression->data.binary.operator_kind != CM_HIR_BINARY_ADD
                && expression->data.binary.operator_kind
                    != CM_HIR_BINARY_SUBTRACT
                && expression->data.binary.operator_kind
                    != CM_HIR_BINARY_EQUAL
                && expression->data.binary.operator_kind
                    != CM_HIR_BINARY_LESS)
            || ((expression->data.binary.operator_kind
                    == CM_HIR_BINARY_EQUAL
                    || expression->data.binary.operator_kind
                        == CM_HIR_BINARY_LESS)
                ? !cm_mir_lower_type_is_bool(plan->hir, type)
                : (!cm_mir_lower_type_is_u32(plan->hir, type)
                    && !cm_mir_lower_type_is_usize(plan->hir, type)))
            || !cm_mir_flow_expression_type(plan,
                expression->data.binary.left, NULL, &left_type)
            || !cm_mir_flow_expression_type(plan,
                expression->data.binary.right, NULL, &right_type)
            || (expression->data.binary.operator_kind
                    == CM_HIR_BINARY_EQUAL
                ? (!cm_mir_lower_type_is_u32(plan->hir, left_type)
                    || !cm_mir_lower_type_is_u32(plan->hir, right_type))
                : expression->data.binary.operator_kind
                        == CM_HIR_BINARY_LESS
                    ? (!cm_mir_lower_type_is_usize(plan->hir, left_type)
                        || !cm_mir_lower_type_is_usize(plan->hir,
                            right_type))
                    : (!cm_mir_hir_type_equal(plan->hir, type, left_type)
                        || !cm_mir_hir_type_equal(plan->hir, type,
                            right_type)))
            || plan->binary_count == UINT32_MAX
            || plan->statement_count == UINT32_MAX
            || (!has_destination && plan->temporary_count == UINT32_MAX)) {
            return cm_mir_flow_fail(plan,
                (expression->data.binary.operator_kind != CM_HIR_BINARY_ADD
                    && expression->data.binary.operator_kind
                        != CM_HIR_BINARY_SUBTRACT
                    && expression->data.binary.operator_kind
                        != CM_HIR_BINARY_EQUAL
                    && expression->data.binary.operator_kind
                        != CM_HIR_BINARY_LESS)
                    ? CM_MIR_FLOW_UNSUPPORTED : CM_MIR_FLOW_INVALID,
                expression_id, CM_MIR_OK);
        }
        plan->binary_count += 1u;
        plan->statement_count += 1u;
        if (!has_destination) plan->temporary_count += 1u;
        ok = cm_mir_flow_preflight(plan, expression->data.binary.left,
                visible_local_count, 0, depth + 1u, NULL)
            && cm_mir_flow_preflight(plan, expression->data.binary.right,
                visible_local_count, 0, depth + 1u, NULL);
    } else if (expression->kind == CM_HIR_EXPR_IF) {
        CmHirTypeId condition_type;
        CmHirTypeId then_type;
        CmHirTypeId else_type;

        if (expression_id != plan->allowed_if_expression
            || (!cm_mir_lower_type_is_u32(plan->hir, type)
                && !cm_mir_lower_type_is_usize(plan->hir, type))
            || !cm_mir_flow_expression_type(plan,
                expression->data.if_expr.condition, NULL, &condition_type)
            || !cm_mir_flow_expression_type(plan,
                expression->data.if_expr.then_expression, NULL, &then_type)
            || !cm_mir_flow_expression_type(plan,
                expression->data.if_expr.else_expression, NULL, &else_type)
            || !cm_mir_lower_type_is_bool(plan->hir, condition_type)
            || !cm_mir_hir_type_equal(plan->hir, type, then_type)
            || !cm_mir_hir_type_equal(plan->hir, type, else_type)
            || plan->conditional_count == UINT32_MAX
            || (!has_destination && plan->temporary_count == UINT32_MAX)) {
            return cm_mir_flow_fail(plan, CM_MIR_FLOW_INVALID,
                expression_id, CM_MIR_OK);
        }
        plan->conditional_count += 1u;
        if (!has_destination) plan->temporary_count += 1u;
        ok = cm_mir_flow_preflight(plan,
                expression->data.if_expr.condition, visible_local_count,
                0, depth + 1u, NULL)
            && cm_mir_flow_preflight(plan,
                expression->data.if_expr.then_expression,
                visible_local_count, 1, depth + 1u, NULL)
            && cm_mir_flow_preflight(plan,
                expression->data.if_expr.else_expression,
                visible_local_count, 1, depth + 1u, NULL);
    } else if (expression->kind == CM_HIR_EXPR_CALL
            || expression->kind == CM_HIR_EXPR_QUALIFIED_CALL
            || expression->kind == CM_HIR_EXPR_METHOD_CALL) {
        CmHirTypeId callee_substitution;
        CmHirTypeId *callee_substitutions;
        const CmHirExprId *call_arguments;
        CmHirExprId call_argument_storage[2];
        uint32_t call_argument_count;
        uint32_t call_substitution_count;
        CmMirBodyId callee_id;
        const CmMirBody *callee_body;
        CmMirBody reserved_callee;
        CmMirLocal reserved_locals[3];
        CmMirInstance reserved_instance;
        CmHirBodyId reserved_source_body;
        const CmHirBody *callee_hir_body;
        const CmHirItem *callee_item;
        CmHirDefId callee_definition;
        CmSemanticDirectCallView semantic_call;
        CmSemanticCallableSelectionView semantic_callable;
        CmSemanticFunctionSignatureView semantic_callee_signature;
        CmSemanticExpressionView semantic_expression;
        CmSemanticGenericArgumentView semantic_impl_argument;
        CmMirReceiverAdjustmentPlan receiver_adjustment;
        int has_aggregate_argument;
        CmMirStatus status;
        uint32_t index;
        int selected_call;

        call_arguments = NULL;
        call_argument_count = 0u;
        selected_call = expression->kind != CM_HIR_EXPR_CALL;
        if (expression->kind == CM_HIR_EXPR_CALL) {
            call_arguments = expression->data.call.arguments;
            call_argument_count = expression->data.call.argument_count;
        } else if (!cm_mir_flow_callable_arguments(expression,
                call_argument_storage, &call_arguments,
                &call_argument_count)) {
            return cm_mir_flow_fail(plan, CM_MIR_FLOW_UNSUPPORTED,
                expression_id, CM_MIR_OK);
        }
        call_substitution_count = expression->kind == CM_HIR_EXPR_CALL
            ? expression->data.call.type_substitution_count : 0u;

        if (!cm_mir_lower_type_is_call_scalar(plan->hir, type)
            || call_argument_count < 1u
            || call_argument_count > 2u
            || call_arguments == NULL
            || (call_substitution_count != 0u
                && (call_substitution_count != 1u
                    || call_argument_count != 1u
                    || expression->data.call.type_substitutions == NULL))
            || plan->call_count == UINT32_MAX
            || (!has_destination && plan->temporary_count == UINT32_MAX)
            || plan->call_argument_count
                > UINT32_MAX - call_argument_count) {
            return cm_mir_flow_fail(plan, CM_MIR_FLOW_UNSUPPORTED,
                expression_id, CM_MIR_OK);
        }
        callee_substitutions = NULL;
        callee_definition = expression->kind == CM_HIR_EXPR_CALL
            ? expression->data.call.callee : cm_hir_def_id_none();
        memset(&semantic_call, 0, sizeof(semantic_call));
        memset(&semantic_callable, 0, sizeof(semantic_callable));
        memset(&semantic_callee_signature, 0,
            sizeof(semantic_callee_signature));
        memset(&semantic_impl_argument, 0,
            sizeof(semantic_impl_argument));
        memset(&receiver_adjustment, 0, sizeof(receiver_adjustment));
        if (selected_call) {
            const CmHirItem *declared;
            const CmHirExpr *receiver;
            int self_matches;

            declared = NULL;
            receiver = NULL;
            self_matches = 0;
            if (plan->semantic_results == NULL || plan->admission == NULL
                || cm_mir_flow_semantic_callable_hint_query(plan,
                    expression_id,
                    &semantic_callable) != CM_SEMANTIC_RESULTS_OK
                || semantic_callable.body != expression->owner_body
                || semantic_callable.expression != expression_id
                || cm_hir_def_id_is_none(semantic_callable.selected_impl)
                || cm_hir_def_id_is_none(
                    semantic_callable.selected_callable)
                || semantic_callable.argument_count != call_argument_count
                || (expression->kind == CM_HIR_EXPR_QUALIFIED_CALL
                    && (semantic_callable.syntax
                            != expression->data.qualified_call.syntax
                        || semantic_callable.syntax
                            != CM_HIR_CALLABLE_QUALIFIED_TRAIT_METHOD
                        || !cm_hir_def_id_equal(
                            semantic_callable.requested_trait,
                            expression->data.qualified_call.requested_trait)
                        || !cm_hir_def_id_equal(
                            semantic_callable.declared_trait_callable,
                            expression->data.qualified_call
                                .declared_trait_callable)
                        || semantic_callable.receiver_argument
                            != expression->data.qualified_call
                                .receiver_argument
                        || cm_semantic_type_view_matches_monomorphic_hir(
                            plan->semantic_results, plan->admission,
                            &semantic_callable.requested_self_type,
                            expression->data.qualified_call.requested_self_type,
                            &self_matches) != CM_SEMANTIC_RESULTS_OK
                        || !self_matches))) {
                return cm_mir_flow_fail(plan, CM_MIR_FLOW_ADMISSION,
                    expression_id, CM_MIR_INVALID_ADMISSION);
            }
            if (expression->kind == CM_HIR_EXPR_METHOD_CALL) {
                declared = cm_mir_flow_definition_item(plan->hir,
                    semantic_callable.declared_trait_callable);
                receiver = cm_hir_get_expr(plan->hir,
                    expression->data.method_call.receiver);
                if (semantic_callable.syntax != CM_HIR_CALLABLE_DOT_METHOD
                    || semantic_callable.syntax
                        != expression->data.method_call.syntax
                    || semantic_callable.receiver_argument != 0u
                    || semantic_callable.receiver_expression
                        != expression->data.method_call.receiver
                    || !cm_mir_flow_method_trait_in_scope(expression,
                        semantic_callable.requested_trait)
                    || declared == NULL
                    || declared->kind != CM_HIR_ITEM_FUNCTION
                    || declared->name
                        != expression->data.method_call.method_name
                    || !cm_hir_def_id_equal(declared->parent_definition,
                        semantic_callable.requested_trait)
                    || receiver == NULL
                    || receiver->owner_body != expression->owner_body
                    || cm_semantic_type_view_matches_monomorphic_hir(
                        plan->semantic_results, plan->admission,
                        &semantic_callable.requested_self_type,
                        receiver->type, &self_matches)
                            != CM_SEMANTIC_RESULTS_OK
                    || !self_matches
                    || !cm_mir_flow_receiver_adjustment(plan, expression,
                        expression_id, &semantic_callable,
                        &receiver_adjustment)) {
                    return cm_mir_flow_fail(plan, CM_MIR_FLOW_ADMISSION,
                        expression_id, CM_MIR_INVALID_ADMISSION);
                }
            }
            if (semantic_callable.receiver_argument
                    == CM_HIR_CALLABLE_RECEIVER_NONE) {
                if (semantic_callable.receiver_expression
                        != CM_HIR_EXPR_NONE) {
                    return cm_mir_flow_fail(plan, CM_MIR_FLOW_ADMISSION,
                        expression_id, CM_MIR_INVALID_ADMISSION);
                }
            } else if (semantic_callable.receiver_argument
                    >= call_argument_count
                || semantic_callable.receiver_expression
                    != call_arguments[semantic_callable.receiver_argument]) {
                return cm_mir_flow_fail(plan, CM_MIR_FLOW_ADMISSION,
                    expression_id, CM_MIR_INVALID_ADMISSION);
            }
            callee_definition = semantic_callable.selected_callable;
            if (semantic_callable.item_argument_count != 0u
                || semantic_callable.method_argument_count != 0u
                || semantic_callable.implemented_trait_argument_count != 0u
                || semantic_callable.enclosing_impl_argument_count > 1u) {
                return cm_mir_flow_fail(plan, CM_MIR_FLOW_UNSUPPORTED,
                    expression_id, CM_MIR_OK);
            }
            if (semantic_callable.enclosing_impl_argument_count == 1u
                && !cm_hir_def_id_equal(
                    semantic_callable.selected_callable,
                    semantic_callable.declared_trait_callable)) {
                if (cm_mir_flow_semantic_callable_generic_argument_query(
                        plan, expression_id,
                        CM_SEMANTIC_CALLABLE_GENERIC_ARGUMENT_ENCLOSING_IMPL,
                        0u, &semantic_impl_argument)
                            != CM_SEMANTIC_RESULTS_OK
                    || semantic_impl_argument.kind != CM_HIR_GENERIC_ARG_TYPE
                    || cm_semantic_type_view_materialize_existing_hir(
                        plan->semantic_results, plan->admission,
                        &semantic_impl_argument.normalized,
                        &callee_substitution) != CM_SEMANTIC_RESULTS_OK
                    || !cm_mir_lower_type_is_executable_substitution(plan->hir,
                        callee_substitution)) {
                    return cm_mir_flow_fail(plan, CM_MIR_FLOW_ADMISSION,
                        expression_id, CM_MIR_INVALID_ADMISSION);
                }
                callee_substitutions = &callee_substitution;
                call_substitution_count = 1u;
            }
        } else if (plan->semantic_results != NULL
            && plan->semantic_evidence == CM_MIR_SEMANTIC_EVIDENCE_BODY) {
            if (plan->admission == NULL
                || plan->instance->substitution_count != 0u
                || call_substitution_count != 0u
                || cm_semantic_results_direct_call(
                    plan->semantic_results, plan->admission,
                    plan->item->data.function_item.body,
                    expression_id, &semantic_call)
                        != CM_SEMANTIC_RESULTS_OK
                || semantic_call.parameter_count
                    != call_argument_count
                || !cm_hir_def_id_equal(semantic_call.callee,
                    expression->data.call.callee)
                || cm_semantic_results_expression(plan->semantic_results,
                    plan->admission, plan->item->data.function_item.body,
                    expression_id, &semantic_expression)
                        != CM_SEMANTIC_RESULTS_OK
                || !cm_mir_semantic_types_equal(
                    &semantic_call.return_type,
                    &semantic_expression.adjusted_type)) {
                return cm_mir_flow_fail(plan, CM_MIR_FLOW_ADMISSION,
                    expression_id, CM_MIR_INVALID_ADMISSION);
            }
            callee_definition = semantic_call.callee;
        }
        if (!selected_call && call_substitution_count == 1u) {
            if (!cm_mir_lower_type(plan->hir, plan->item,
                    plan->instance->substitutions,
                    plan->instance->substitution_count,
                    expression->data.call.type_substitutions[0],
                    &callee_substitution)) {
                return cm_mir_flow_fail(plan, CM_MIR_FLOW_UNSUPPORTED,
                    expression_id, CM_MIR_OK);
            }
            callee_substitutions = &callee_substitution;
        }
        status = cm_mir_flow_find_callee(plan, expression_id,
            callee_definition, callee_substitutions,
            call_substitution_count, &callee_id);
        if (status != CM_MIR_OK) {
            return cm_mir_flow_fail(plan,
                plan->publication != NULL
                        && (status == CM_MIR_INVALID_ARGUMENT
                            || status == CM_MIR_INVALID_ADMISSION)
                    ? CM_MIR_FLOW_ADMISSION : CM_MIR_FLOW_CALLEE,
                expression_id, status);
        }
        memset(&reserved_callee, 0, sizeof(reserved_callee));
        memset(reserved_locals, 0, sizeof(reserved_locals));
        memset(&reserved_instance, 0, sizeof(reserved_instance));
        reserved_source_body = CM_HIR_BODY_NONE;
        callee_body = plan->publication == NULL
            ? cm_mir_get_body(plan->context, callee_id)
            : cm_mir_publication_get_body(plan->publication, callee_id);
        if (callee_body == NULL && plan->publication != NULL
            && cm_mir_publication_get_instance(plan->publication,
                callee_id, &reserved_instance, &reserved_source_body)
                == CM_MIR_OK) {
            reserved_callee.instance = reserved_instance;
            reserved_callee.owner = reserved_instance.body_definition;
            reserved_callee.source_body = reserved_source_body;
            reserved_callee.semantic_evidence =
                CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE;
            reserved_callee.locals = reserved_locals;
            callee_body = &reserved_callee;
        }
        callee_hir_body = callee_body == NULL ? NULL
            : cm_hir_get_body(plan->hir, callee_body->source_body);
        callee_item = callee_hir_body == NULL ? NULL
            : cm_mir_lower_function(plan->hir, callee_hir_body);
        if (callee_body == &reserved_callee && callee_item != NULL) {
            const CmHirFunctionSignature *callee_signature;

            callee_signature = &callee_item->data.function_item.signature;
            if (callee_signature->parameter_count > 2u) {
                return cm_mir_flow_fail(plan, CM_MIR_FLOW_UNSUPPORTED,
                    expression_id, CM_MIR_OK);
            }
            reserved_callee.local_count =
                callee_signature->parameter_count + 1u;
            reserved_locals[0].kind = CM_MIR_LOCAL_RETURN;
            if (reserved_callee.local_count > 3u
                || !cm_mir_lower_type(plan->hir, callee_item,
                    reserved_instance.substitutions,
                    reserved_instance.substitution_count,
                    callee_signature->return_type,
                    &reserved_locals[0].type)) {
                return cm_mir_flow_fail(plan, CM_MIR_FLOW_INVALID,
                    expression_id, CM_MIR_OK);
            }
            for (index = 0u; index < callee_signature->parameter_count;
                 ++index) {
                reserved_locals[index + 1u].kind = CM_MIR_LOCAL_ARGUMENT;
                if (!cm_mir_lower_type(plan->hir, callee_item,
                        reserved_instance.substitutions,
                        reserved_instance.substitution_count,
                        callee_signature->parameters[index].type,
                        &reserved_locals[index + 1u].type)) {
                    return cm_mir_flow_fail(plan, CM_MIR_FLOW_INVALID,
                        expression_id, CM_MIR_OK);
                }
            }
        }
        if (callee_body == NULL || callee_body->locals == NULL
            || callee_body->local_count
                < call_argument_count + 1u
            || callee_item == NULL
            || callee_item->data.function_item.signature.parameter_count
                != call_argument_count
            || !cm_hir_def_id_equal(callee_body->instance.definition,
                callee_definition)) {
            return cm_mir_flow_fail(plan, CM_MIR_FLOW_INVALID,
                expression_id, CM_MIR_OK);
        }
        if (plan->semantic_results != NULL
            && (plan->admission == NULL
                || (expression->kind == CM_HIR_EXPR_CALL
                    && (cm_mir_flow_semantic_call_query(plan, callee_body,
                            expression_id, &semantic_call)
                            != CM_SEMANTIC_RESULTS_OK
                        || semantic_call.parameter_count
                            != call_argument_count
                        || !cm_hir_def_id_equal(semantic_call.callee,
                            expression->data.call.callee)))
                || cm_mir_flow_semantic_expression_query(plan,
                    expression_id, &semantic_expression)
                    != CM_SEMANTIC_RESULTS_OK
                || (selected_call
                    && (semantic_expression.adjustment_count != 0u
                        || !cm_mir_semantic_types_equal(
                            &semantic_expression.unadjusted_type,
                            &semantic_expression.adjusted_type)))
                || cm_mir_flow_semantic_signature_query(plan, callee_body,
                    &semantic_callee_signature)
                    != CM_SEMANTIC_RESULTS_OK
                || (selected_call
                    && cm_mir_flow_semantic_callable_query(plan, callee_body,
                        expression_id, &semantic_callable)
                        != CM_SEMANTIC_RESULTS_OK)
                || !cm_hir_def_id_equal(
                    semantic_callee_signature.definition,
                    selected_call ? semantic_callable.body_definition
                        : callee_definition)
                || (selected_call
                    ? (semantic_callee_signature.parameter_count
                            != semantic_callable.argument_count
                        || !cm_mir_semantic_types_equal(
                            &semantic_callable.return_type,
                            &semantic_expression.adjusted_type)
                        || !cm_mir_semantic_types_equal(
                            &semantic_callee_signature.return_type,
                            &semantic_callable.return_type))
                    : (semantic_callee_signature.parameter_count
                            != semantic_call.parameter_count
                        || !cm_mir_semantic_types_equal(
                            &semantic_call.return_type,
                            &semantic_expression.adjusted_type)
                        || !cm_mir_semantic_types_equal(
                            &semantic_callee_signature.return_type,
                            &semantic_call.return_type))))) {
            return cm_mir_flow_fail(plan, CM_MIR_FLOW_ADMISSION,
                expression_id, CM_MIR_INVALID_ADMISSION);
        }
        if (!cm_mir_hir_type_equal(plan->hir, type,
                callee_body->locals[CM_MIR_RETURN_LOCAL].type)
            || !cm_mir_lower_type_is_call_scalar(plan->hir, type)) {
            return cm_mir_flow_fail(plan, CM_MIR_FLOW_UNSUPPORTED,
                expression_id, CM_MIR_OK);
        }
        has_aggregate_argument = 0;
        for (index = 0u; index < call_argument_count;
             ++index) {
            const CmHirExpr *argument;
            CmHirTypeId parameter_type;
            CmSemanticTypeView semantic_parameter;
            CmSemanticTypeView semantic_callee_parameter;
            CmSemanticExpressionView semantic_argument;
            CmHirExprId argument_id;

            argument = cm_hir_get_expr(plan->hir,
                call_arguments[index]);
            if (argument == NULL) {
                return cm_mir_flow_fail(plan, CM_MIR_FLOW_INVALID,
                    call_arguments[index], CM_MIR_OK);
            }
            if (plan->semantic_results != NULL
                && ((expression->kind == CM_HIR_EXPR_CALL
                        ? cm_mir_flow_semantic_call_parameter_query(plan,
                        callee_body, expression_id, index,
                        &semantic_parameter)
                        : cm_mir_flow_semantic_callable_parameter_query(plan,
                            callee_body, expression_id, index,
                            &semantic_parameter))
                        != CM_SEMANTIC_RESULTS_OK
                    || (selected_call
                        && (cm_mir_flow_semantic_callable_argument_query(plan,
                                expression_id, index, &argument_id)
                                != CM_SEMANTIC_RESULTS_OK
                            || argument_id != call_arguments[index]))
                    || cm_mir_flow_semantic_signature_parameter_query(plan,
                        callee_body, index,
                        &semantic_callee_parameter)
                        != CM_SEMANTIC_RESULTS_OK
                    || cm_mir_flow_semantic_expression_query(plan,
                        call_arguments[index],
                        &semantic_argument) != CM_SEMANTIC_RESULTS_OK
                    || (selected_call
                        && (index == 0u && receiver_adjustment.present
                            ? (semantic_argument.adjustment_count != 1u
                                || !cm_mir_semantic_types_equal(
                                    &semantic_parameter,
                                    &semantic_argument.adjusted_type))
                            : (semantic_argument.adjustment_count != 0u
                                || !cm_mir_semantic_types_equal(
                                    &semantic_argument.unadjusted_type,
                                    &semantic_argument.adjusted_type))))
                    || !cm_mir_semantic_types_equal(&semantic_parameter,
                        &semantic_callee_parameter)
                    || !cm_mir_semantic_types_equal(&semantic_parameter,
                        &semantic_argument.adjusted_type))) {
                return cm_mir_flow_fail(plan, CM_MIR_FLOW_ADMISSION,
                    call_arguments[index],
                    CM_MIR_INVALID_ADMISSION);
            }
            if (!cm_mir_flow_expression_type(plan,
                    call_arguments[index], NULL,
                    &local_type)
                || !cm_mir_lower_type(plan->hir, callee_item,
                    callee_body->instance.substitutions,
                    callee_body->instance.substitution_count,
                    callee_item->data.function_item.signature
                        .parameters[index].type,
                    &parameter_type)
                || (index == 0u && receiver_adjustment.present
                    ? (!cm_mir_hir_type_equal(plan->hir, local_type,
                            receiver_adjustment.source_type)
                        || !cm_mir_hir_type_equal(plan->hir, parameter_type,
                            receiver_adjustment.target_type))
                    : !cm_mir_hir_type_equal(plan->hir, local_type,
                        parameter_type))
                || (!cm_mir_lower_type_is_call_scalar(plan->hir,
                        parameter_type)
                    && !cm_mir_lower_type_is_aggregate(plan->hir,
                        callee_item, parameter_type))) {
                return cm_mir_flow_fail(plan, CM_MIR_FLOW_UNSUPPORTED,
                    call_arguments[index], CM_MIR_OK);
            }
            if (argument->kind == CM_HIR_EXPR_INTEGER
                && !cm_mir_lower_type_is_usize(plan->hir, local_type)) {
                return cm_mir_flow_fail(plan, CM_MIR_FLOW_UNSUPPORTED,
                    call_arguments[index], CM_MIR_OK);
            }
            if (cm_mir_lower_type_is_aggregate(plan->hir,
                    callee_item, parameter_type)) {
                has_aggregate_argument = 1;
            }
            if (!cm_mir_flow_preflight(plan,
                    call_arguments[index],
                    visible_local_count, 0, depth + 1u, NULL)) {
                return 0;
            }
        }
        if (has_aggregate_argument
            && (call_substitution_count != 0u
                || callee_body->instance.substitution_count != 0u
                || callee_item->generic_parameter_count != 0u)) {
            return cm_mir_flow_fail(plan, CM_MIR_FLOW_UNSUPPORTED,
                expression_id, CM_MIR_OK);
        }
        {
            CmMirFlowCall call;

            call.expression = expression_id;
            call.callee = callee_id;
            (void)cm_vec_push(&plan->calls, &call);
        }
        plan->call_count += 1u;
        plan->call_argument_count += call_argument_count;
        if (receiver_adjustment.present) {
            if (plan->temporary_count == UINT32_MAX
                || plan->statement_count == UINT32_MAX) {
                return cm_mir_flow_fail(plan, CM_MIR_FLOW_INVALID,
                    expression_id, CM_MIR_ID_EXHAUSTED);
            }
            plan->temporary_count += 1u;
            plan->statement_count += 1u;
        }
        if (!has_destination) {
            if (plan->temporary_count == UINT32_MAX) {
                return cm_mir_flow_fail(plan, CM_MIR_FLOW_INVALID,
                    expression_id, CM_MIR_ID_EXHAUSTED);
            }
            plan->temporary_count += 1u;
        }
        ok = 1;
    } else {
        return cm_mir_flow_fail(plan, CM_MIR_FLOW_UNSUPPORTED,
            expression_id, CM_MIR_OK);
    }
    if (ok && out_projection_count != NULL) {
        *out_projection_count = expression_projection_count;
    }
    return ok;
}

static int cm_mir_flow_append_binary(CmMirFlowOutput *output,
    const CmHirExpr *expression, CmHirBinaryOperator operator_kind,
    CmMirLocalId destination,
    const CmMirOperand *left, const CmMirOperand *right)
{
    CmMirStatement statement;
    const CmMirLocal *destination_local;
    CmHirTypeId type;

    if (!cm_mir_lower_type(output->plan->hir, output->plan->item,
            output->plan->instance->substitutions,
            output->plan->instance->substitution_count,
            expression->type, &type)) {
        return 0;
    }
    destination_local = (const CmMirLocal *)cm_vec_at_const(output->locals,
        destination);
    if (destination_local == NULL
        || !cm_mir_hir_type_equal(output->plan->hir,
            destination_local->type, type)) {
        return 0;
    }
    type = destination_local->type;
    memset(&statement, 0, sizeof(statement));
    statement.kind = CM_MIR_STATEMENT_ASSIGN;
    statement.data.assign.destination = destination;
    statement.data.assign.value.type = type;
    statement.data.assign.value.span = expression->span;
    if (operator_kind == CM_HIR_BINARY_EQUAL) {
        statement.data.assign.value.kind = CM_MIR_RVALUE_EQUAL;
        statement.data.assign.value.data.equal.left = *left;
        statement.data.assign.value.data.equal.right = *right;
    } else if (operator_kind == CM_HIR_BINARY_LESS) {
        statement.data.assign.value.kind = CM_MIR_RVALUE_LESS;
        statement.data.assign.value.data.less.left = *left;
        statement.data.assign.value.data.less.right = *right;
    } else {
        statement.data.assign.value.kind = CM_MIR_RVALUE_BINARY;
        statement.data.assign.value.data.binary.operator_kind =
            operator_kind == CM_HIR_BINARY_ADD
                ? CM_MIR_BINARY_ADD : CM_MIR_BINARY_SUBTRACT;
        statement.data.assign.value.data.binary.left = *left;
        statement.data.assign.value.data.binary.right = *right;
    }
    (void)cm_vec_push(output->statements, &statement);
    return 1;
}

static int cm_mir_flow_temporary(CmMirFlowOutput *output,
    CmHirTypeId type, CmMirLocalId *out_local);

static int cm_mir_flow_append_use(CmMirFlowOutput *output,
    CmMirLocalId destination, const CmMirOperand *operand)
{
    CmMirStatement statement;

    if (operand == NULL) return 0;
    memset(&statement, 0, sizeof(statement));
    statement.kind = CM_MIR_STATEMENT_ASSIGN;
    statement.data.assign.destination = destination;
    statement.data.assign.value.kind = CM_MIR_RVALUE_USE;
    statement.data.assign.value.type = operand->type;
    statement.data.assign.value.data.use = *operand;
    (void)cm_vec_push(output->statements, &statement);
    return 1;
}

static int cm_mir_flow_append_receiver_borrow(CmMirFlowOutput *output,
    const CmMirReceiverAdjustmentPlan *adjustment,
    const CmMirOperand *source, CmMirOperand *out_operand)
{
    CmMirStatement statement;
    CmMirLocalId destination;

    if (output == NULL || adjustment == NULL || !adjustment->present
        || source == NULL || out_operand == NULL
        || source->kind != CM_MIR_OPERAND_MOVE
        || !cm_mir_hir_type_equal(output->plan->hir, source->type,
            adjustment->source_type)
        || !cm_mir_flow_temporary(output, adjustment->target_type,
            &destination)) {
        return 0;
    }
    memset(&statement, 0, sizeof(statement));
    statement.kind = CM_MIR_STATEMENT_ASSIGN;
    statement.data.assign.destination = destination;
    statement.data.assign.value.kind = CM_MIR_RVALUE_BORROW;
    statement.data.assign.value.type = adjustment->target_type;
    statement.data.assign.value.span = adjustment->span;
    statement.data.assign.value.data.borrow.kind = adjustment->borrow_kind;
    statement.data.assign.value.data.borrow.source.base = source->data.local;
    statement.data.assign.value.data.borrow.source.type =
        adjustment->source_type;
    statement.data.assign.value.data.borrow.source.span = adjustment->span;
    (void)cm_vec_push(output->statements, &statement);
    memset(out_operand, 0, sizeof(*out_operand));
    out_operand->kind = CM_MIR_OPERAND_MOVE;
    out_operand->type = adjustment->target_type;
    out_operand->data.local = destination;
    return 1;
}

static int cm_mir_flow_append_tuple_parameter_prologue(
    CmMirFlowOutput *output)
{
    const CmHirFunctionSignature *signature;
    uint32_t hir_local_index;
    uint32_t parameter_index;

    if (output == NULL || output->plan == NULL) return 0;
    signature = &output->plan->item->data.function_item.signature;
    hir_local_index = 0u;
    for (parameter_index = 0u;
         parameter_index < signature->parameter_count; ++parameter_index) {
        const CmHirFunctionParameter *parameter;

        parameter = &signature->parameters[parameter_index];
        if (parameter->binding_kind == CM_HIR_BINDING_NAMED) {
            hir_local_index += 1u;
            continue;
        }
        if (parameter->binding_kind == CM_HIR_BINDING_DISCARD) continue;
        if (parameter->binding_kind == CM_HIR_BINDING_NEWTYPE_PATTERN) {
            const CmHirType *parameter_type;
            CmHirTypeId declared_field_type;
            CmHirTypeId field_type;
            CmMirPlaceProjection projection;
            CmMirOperand operand;
            CmMirLocalId destination;
            size_t projection_index;

            parameter_type = cm_hir_get_type(output->plan->hir,
                parameter->type);
            if (parameter_type == NULL
                || cm_mir_lower_applied_newtype(output->plan->hir,
                    output->plan->item, parameter->type,
                    &declared_field_type) == NULL
                || !cm_mir_lower_type(output->plan->hir,
                    output->plan->item,
                    output->plan->instance->substitutions,
                    output->plan->instance->substitution_count,
                    declared_field_type, &field_type)
                || !cm_mir_lower_hir_local_id(output->plan->hir, signature,
                    output->plan->body,
                    &output->plan->parameter_layout,
                    hir_local_index, &destination)
                || output->projections->len >= output->projections->cap) {
                return 0;
            }
            memset(&projection, 0, sizeof(projection));
            projection.kind = CM_MIR_PROJECTION_FIELD;
            projection.definition =
                parameter_type->data.named_type.definition;
            projection.field_index = 0u;
            projection_index = output->projections->len;
            (void)cm_vec_push(output->projections, &projection);

            memset(&operand, 0, sizeof(operand));
            operand.kind = CM_MIR_OPERAND_MOVE_PLACE;
            operand.type = field_type;
            operand.data.place.base = parameter_index + 1u;
            operand.data.place.type = field_type;
            operand.data.place.projections =
                (CmMirPlaceProjection *)output->projections->data
                    + projection_index;
            operand.data.place.projection_count = 1u;
            operand.data.place.span = parameter->newtype_binding.span;
            if (!cm_mir_flow_append_use(output, destination, &operand)) {
                return 0;
            }
            hir_local_index += 1u;
            continue;
        }
        if (parameter->binding_kind != CM_HIR_BINDING_TUPLE_PATTERN) {
            return 0;
        }
        {
            const CmHirType *tuple_type;
            uint32_t binding_count;
            uint32_t field_index;

            tuple_type = cm_hir_get_type(output->plan->hir,
                parameter->type);
            if (tuple_type == NULL
                || tuple_type->kind != CM_HIR_TYPE_TUPLE_KIND
                || tuple_type->data.tuple_type.element_count == 0u
                || tuple_type->data.tuple_type.element_count
                    > CM_HIR_TUPLE_PARAMETER_BINDING_COUNT
                || tuple_type->data.tuple_type.elements == NULL) {
                return 0;
            }
            binding_count = tuple_type->data.tuple_type.element_count;
            for (field_index = 0u;
                 field_index < binding_count;
                 ++field_index) {
                CmMirPlaceProjection projection;
                CmMirOperand operand;
                CmMirLocalId destination;
                size_t projection_index;

                if (!cm_mir_lower_hir_local_id(output->plan->hir, signature,
                        output->plan->body,
                        &output->plan->parameter_layout,
                        hir_local_index, &destination)
                    || output->projections->len
                        >= output->projections->cap) {
                    return 0;
                }
                memset(&projection, 0, sizeof(projection));
                projection.kind = CM_MIR_PROJECTION_FIELD;
                projection.definition = cm_hir_def_id_none();
                projection.field_index = field_index;
                projection_index = output->projections->len;
                (void)cm_vec_push(output->projections, &projection);

                memset(&operand, 0, sizeof(operand));
                operand.kind = binding_count == 1u
                    ? CM_MIR_OPERAND_MOVE_PLACE
                    : CM_MIR_OPERAND_COPY_PLACE;
                if (!cm_mir_lower_type(output->plan->hir,
                        output->plan->item,
                        output->plan->instance->substitutions,
                        output->plan->instance->substitution_count,
                        tuple_type->data.tuple_type.elements[field_index],
                        &operand.type)
                    || (binding_count == 2u
                        && (operand.type
                            != tuple_type->data.tuple_type
                                .elements[field_index]
                            || !cm_mir_lower_type_is_scalar(
                                output->plan->hir, operand.type)))
                    || (binding_count == 1u
                        && !cm_mir_lower_type_is_tuple_element(
                            output->plan->hir, operand.type))) {
                    return 0;
                }
                operand.data.place.base = parameter_index + 1u;
                operand.data.place.type = operand.type;
                operand.data.place.projections =
                    (CmMirPlaceProjection *)output->projections->data
                        + projection_index;
                operand.data.place.projection_count = 1u;
                operand.data.place.span =
                    parameter->tuple_bindings[field_index].span;
                if (!cm_mir_flow_append_use(output, destination, &operand)) {
                    return 0;
                }
                hir_local_index += 1u;
            }
        }
    }
    return 1;
}

static int cm_mir_flow_temporary(CmMirFlowOutput *output,
    CmHirTypeId type, CmMirLocalId *out_local)
{
    CmMirLocal local;

    if (output->locals->len > (size_t)UINT32_MAX) return 0;
    *out_local = (CmMirLocalId)output->locals->len;
    memset(&local, 0, sizeof(local));
    local.kind = CM_MIR_LOCAL_TEMPORARY;
    local.type = type;
    (void)cm_vec_push(output->locals, &local);
    return 1;
}

static CmHirTypeId cm_mir_flow_temporary_type(
    const CmMirFlowOutput *output, CmHirTypeId type)
{
    return cm_mir_lower_type_is_scalar(output->plan->hir, type)
            && cm_mir_lower_type_is_scalar(output->plan->hir,
                output->plan->expected_type)
            && cm_mir_hir_type_equal(output->plan->hir, type,
                output->plan->expected_type)
        ? output->plan->expected_type : type;
}

static int cm_mir_flow_next_block(CmMirFlowOutput *output)
{
    CmMirBasicBlock block;
    size_t statement_start;

    if (output->blocks->len > (size_t)UINT32_MAX) return 0;
    memset(&block, 0, sizeof(block));
    statement_start = output->statements->len;
    (void)cm_vec_push(output->blocks, &block);
    (void)cm_vec_push(output->block_starts, &statement_start);
    output->current_block = (CmMirBasicBlockId)(output->blocks->len - 1u);
    return 1;
}

static int cm_mir_flow_expression(CmMirFlowOutput *output,
    CmHirExprId expression_id, int has_destination,
    CmMirLocalId requested_destination, CmMirOperand *out_operand)
{
    const CmHirExpr *expression;
    CmHirTypeId type;

    expression = cm_hir_get_expr(output->plan->hir, expression_id);
    if (expression == NULL
        || !cm_mir_lower_type(output->plan->hir, output->plan->item,
            output->plan->instance->substitutions,
            output->plan->instance->substitution_count,
            expression->type, &type)) {
        return 0;
    }
    memset(out_operand, 0, sizeof(*out_operand));
    out_operand->type = type;
    if (expression->kind == CM_HIR_EXPR_BLOCK) {
        return expression->data.block.statement_count == 0u
            && expression->data.block.statements == NULL
            && cm_mir_flow_expression(output,
                expression->data.block.tail_expression, has_destination,
                requested_destination, out_operand);
    }
    if (expression->kind == CM_HIR_EXPR_LOCAL) {
        CmMirLocalId source_local;

        if (!cm_mir_lower_type(output->plan->hir, output->plan->item,
                output->plan->instance->substitutions,
                output->plan->instance->substitution_count,
                output->plan->body->locals[
                    expression->data.local.local_index].type, &type)
            || !cm_mir_lower_hir_local_id(output->plan->hir,
                &output->plan->item->data.function_item.signature,
                output->plan->body, &output->plan->parameter_layout,
                expression->data.local.local_index, &source_local)) {
            return 0;
        }
        out_operand->kind = CM_MIR_OPERAND_MOVE;
        out_operand->type = type;
        out_operand->data.local = source_local;
        if (has_destination) {
            CmMirOperand source;

            source = *out_operand;
            if (!cm_mir_flow_append_use(output, requested_destination,
                    &source)) {
                return 0;
            }
            out_operand->data.local = requested_destination;
        }
        return 1;
    }
    if (expression->kind == CM_HIR_EXPR_INTEGER) {
        if (cm_mir_lower_type_is_u32(output->plan->hir, type)) {
            out_operand->kind = CM_MIR_CONSTANT_U32;
            out_operand->data.u32_value =
                (uint32_t)expression->data.integer.low_bits;
        } else if (cm_mir_lower_type_is_usize(output->plan->hir, type)) {
            out_operand->kind = CM_MIR_CONSTANT_USIZE;
            out_operand->data.usize_value =
                expression->data.integer.low_bits;
        } else {
            out_operand->kind = CM_MIR_CONSTANT_I32;
            out_operand->data.i32_value =
                (int32_t)expression->data.integer.low_bits;
        }
        if (has_destination) {
            CmMirOperand source;

            source = *out_operand;
            if (!cm_mir_flow_append_use(output, requested_destination,
                    &source)) {
                return 0;
            }
            out_operand->kind = CM_MIR_OPERAND_MOVE;
            out_operand->data.local = requested_destination;
        }
        return 1;
    }
    if (expression->kind == CM_HIR_EXPR_FIELD) {
        CmMirOperand base;
        CmMirPlaceProjection projection_buffer[
            CM_MIR_MAX_PLACE_PROJECTIONS];
        CmMirPlace place;
        size_t projection_start;
        uint32_t base_projection_count;
        CmSemanticFieldSelectionView semantic_field;

        memset(&semantic_field, 0, sizeof(semantic_field));
        semantic_field.base_expression = expression->data.field.base;
        semantic_field.aggregate_definition = expression->data.field.definition;
        semantic_field.field_index = expression->data.field.field_index;
        if ((output->plan->semantic_results != NULL
                && cm_mir_flow_semantic_field_query(output->plan,
                    expression_id, &semantic_field)
                    != CM_SEMANTIC_RESULTS_OK)
            || semantic_field.base_expression != expression->data.field.base
            || !cm_hir_def_id_equal(semantic_field.aggregate_definition,
                expression->data.field.definition)
            || semantic_field.field_index != expression->data.field.field_index
            || !cm_mir_flow_expression(output, semantic_field.base_expression,
                0, CM_MIR_RETURN_LOCAL, &base)) {
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
            if (base_projection_count != 0u) {
                memcpy(projection_buffer, base.data.place.projections,
                    (size_t)base_projection_count
                        * sizeof(CmMirPlaceProjection));
            }
        } else {
            return 0;
        }
        if (base_projection_count >= CM_MIR_MAX_PLACE_PROJECTIONS) return 0;
        projection_buffer[base_projection_count].kind =
            CM_MIR_PROJECTION_FIELD;
        projection_buffer[base_projection_count].definition =
            semantic_field.aggregate_definition;
        projection_buffer[base_projection_count].field_index =
            semantic_field.field_index;
        place.projection_count = base_projection_count + 1u;
        projection_start = output->projections->len;
        cm_vec_append(output->projections, projection_buffer,
            place.projection_count);
        place.projections = (CmMirPlaceProjection *)output->projections->data
            + projection_start;
        place.type = type;
        place.span = expression->span;
        out_operand->kind = cm_mir_lower_type_is_scalar(
                output->plan->hir, type)
            ? CM_MIR_OPERAND_COPY_PLACE : CM_MIR_OPERAND_MOVE_PLACE;
        out_operand->data.place = place;
        if (has_destination) {
            CmMirOperand source;

            source = *out_operand;
            if (!cm_mir_flow_append_use(output, requested_destination,
                    &source)) {
                return 0;
            }
            out_operand->kind = CM_MIR_OPERAND_MOVE;
            out_operand->data.local = requested_destination;
        }
        return 1;
    }
    if (expression->kind == CM_HIR_EXPR_AGGREGATE) {
        CmMirOperand field_values[CM_MIR_MAX_AGGREGATE_FIELDS];
        int seen[CM_MIR_MAX_AGGREGATE_FIELDS];
        CmMirStatement statement;
        CmMirLocalId destination;
        size_t field_start;
        uint32_t index;

        memset(field_values, 0, sizeof(field_values));
        memset(seen, 0, sizeof(seen));
        for (index = 0u; index < expression->data.aggregate.field_count;
             ++index) {
            const CmHirAggregateFieldValue *field;

            field = &expression->data.aggregate.fields[index];
            if (field->field_index >= CM_MIR_MAX_AGGREGATE_FIELDS
                || seen[field->field_index]
                || !cm_mir_flow_expression(output, field->value, 0,
                    CM_MIR_RETURN_LOCAL,
                    &field_values[field->field_index])) {
                return 0;
            }
            seen[field->field_index] = 1;
        }
        if (!has_destination
            && !cm_mir_flow_temporary(output, type, &destination)) {
            return 0;
        }
        if (has_destination) destination = requested_destination;
        field_start = output->aggregate_fields->len;
        for (index = 0u; index < expression->data.aggregate.field_count;
             ++index) {
            CmMirAggregateField field;

            if (!seen[index]) return 0;
            memset(&field, 0, sizeof(field));
            field.field_index = index;
            field.value = field_values[index];
            (void)cm_vec_push(output->aggregate_fields, &field);
        }
        memset(&statement, 0, sizeof(statement));
        statement.kind = CM_MIR_STATEMENT_ASSIGN;
        statement.data.assign.destination = destination;
        statement.data.assign.value.kind = CM_MIR_RVALUE_AGGREGATE;
        statement.data.assign.value.type = type;
        statement.data.assign.value.span = expression->span;
        statement.data.assign.value.data.aggregate.definition =
            expression->data.aggregate.definition;
        statement.data.assign.value.data.aggregate.fields =
            (CmMirAggregateField *)output->aggregate_fields->data
                + field_start;
        statement.data.assign.value.data.aggregate.field_count =
            expression->data.aggregate.field_count;
        (void)cm_vec_push(output->statements, &statement);
        out_operand->kind = CM_MIR_OPERAND_MOVE;
        out_operand->data.local = destination;
        return 1;
    }
    if (expression->kind == CM_HIR_EXPR_BINARY) {
        CmMirOperand left;
        CmMirOperand right;
        CmMirLocalId destination;
        CmSemanticPrimitiveBinaryView semantic_binary;

        memset(&semantic_binary, 0, sizeof(semantic_binary));
        semantic_binary.operator_kind =
            expression->data.binary.operator_kind;
        semantic_binary.left_expression = expression->data.binary.left;
        semantic_binary.right_expression = expression->data.binary.right;
        if ((output->plan->semantic_results != NULL
                && cm_mir_flow_semantic_primitive_query(output->plan,
                    expression_id, &semantic_binary)
                    != CM_SEMANTIC_RESULTS_OK)
            || semantic_binary.operator_kind
                != expression->data.binary.operator_kind
            || semantic_binary.left_expression
                != expression->data.binary.left
            || semantic_binary.right_expression
                != expression->data.binary.right
            || !cm_mir_flow_expression(output, semantic_binary.left_expression,
                0, CM_MIR_RETURN_LOCAL, &left)
            || !cm_mir_flow_expression(output,
                semantic_binary.right_expression, 0,
                CM_MIR_RETURN_LOCAL, &right)
            || (!has_destination
                && !cm_mir_flow_temporary(output,
                    cm_mir_flow_temporary_type(output, type),
                    &destination))) {
            return 0;
        }
        if (has_destination) destination = requested_destination;
        if (!cm_mir_flow_append_binary(output, expression,
                semantic_binary.operator_kind, destination, &left, &right)) {
            return 0;
        }
        out_operand->kind = CM_MIR_OPERAND_MOVE;
        out_operand->type = ((const CmMirLocal *)cm_vec_at_const(
            output->locals, destination))->type;
        out_operand->data.local = destination;
        return 1;
    }
    if (expression->kind == CM_HIR_EXPR_IF) {
        CmMirOperand condition;
        CmMirOperand branch_result;
        CmMirLocalId destination;
        CmMirBasicBlockId switch_block;
        CmMirBasicBlockId true_target;
        CmMirBasicBlockId then_end;
        CmMirBasicBlockId false_target;
        CmMirBasicBlockId else_end;
        CmMirBasicBlockId join;
        CmMirBasicBlock *block;

        if (!cm_mir_flow_expression(output,
                expression->data.if_expr.condition, 0,
                CM_MIR_RETURN_LOCAL, &condition)
            || condition.kind != CM_MIR_OPERAND_MOVE
            || !cm_mir_lower_type_is_bool(output->plan->hir,
                condition.type)
            || (!has_destination
                && !cm_mir_flow_temporary(output, type, &destination))) {
            return 0;
        }
        if (has_destination) destination = requested_destination;
        switch_block = output->current_block;
        if (!cm_mir_flow_next_block(output)) return 0;
        true_target = output->current_block;
        if (!cm_mir_flow_expression(output,
                expression->data.if_expr.then_expression, 1, destination,
                &branch_result)
            || branch_result.kind != CM_MIR_OPERAND_MOVE
            || branch_result.data.local != destination) {
            return 0;
        }
        then_end = output->current_block;
        if (!cm_mir_flow_next_block(output)) return 0;
        false_target = output->current_block;
        if (!cm_mir_flow_expression(output,
                expression->data.if_expr.else_expression, 1, destination,
                &branch_result)
            || branch_result.kind != CM_MIR_OPERAND_MOVE
            || branch_result.data.local != destination) {
            return 0;
        }
        else_end = output->current_block;
        if (!cm_mir_flow_next_block(output)) return 0;
        join = output->current_block;

        block = (CmMirBasicBlock *)cm_vec_at(output->blocks, switch_block);
        if (block == NULL) return 0;
        block->terminator.kind = CM_MIR_TERMINATOR_SWITCH_BOOL;
        block->terminator.data.switch_bool.condition = condition;
        block->terminator.data.switch_bool.true_target = true_target;
        block->terminator.data.switch_bool.false_target = false_target;
        block = (CmMirBasicBlock *)cm_vec_at(output->blocks, then_end);
        if (block == NULL) return 0;
        block->terminator.kind = CM_MIR_TERMINATOR_GOTO;
        block->terminator.data.goto_block.target = join;
        block = (CmMirBasicBlock *)cm_vec_at(output->blocks, else_end);
        if (block == NULL) return 0;
        block->terminator.kind = CM_MIR_TERMINATOR_GOTO;
        block->terminator.data.goto_block.target = join;

        out_operand->kind = CM_MIR_OPERAND_MOVE;
        out_operand->type = ((const CmMirLocal *)cm_vec_at_const(
            output->locals, destination))->type;
        out_operand->data.local = destination;
        return 1;
    }
    if (expression->kind == CM_HIR_EXPR_CALL
        || expression->kind == CM_HIR_EXPR_QUALIFIED_CALL
        || expression->kind == CM_HIR_EXPR_METHOD_CALL) {
        const CmMirFlowCall *planned_call;
        const CmMirBody *planned_callee_body;
        CmMirInstance callee_instance;
        CmHirBodyId planned_source_body;
        CmMirOperand call_arguments[2];
        const CmHirExprId *argument_expressions;
        CmHirExprId argument_storage[2];
        uint32_t argument_count;
        CmSemanticCallableSelectionView semantic_callable;
        CmMirReceiverAdjustmentPlan receiver_adjustment;
        CmMirOperand raw_receiver;
        CmMirLocalId destination;
        CmMirBasicBlock *block;
        size_t argument_start;
        uint32_t index;

        argument_expressions = NULL;
        argument_count = 0u;
        memset(&semantic_callable, 0, sizeof(semantic_callable));
        memset(&receiver_adjustment, 0, sizeof(receiver_adjustment));
        memset(&raw_receiver, 0, sizeof(raw_receiver));
        if (expression->kind == CM_HIR_EXPR_CALL) {
            argument_expressions = expression->data.call.arguments;
            argument_count = expression->data.call.argument_count;
        } else if (!cm_mir_flow_callable_arguments(expression,
                argument_storage, &argument_expressions, &argument_count)) {
            return 0;
        }
        if (expression->kind == CM_HIR_EXPR_METHOD_CALL
            && (cm_mir_flow_semantic_callable_hint_query(output->plan,
                    expression_id, &semantic_callable)
                    != CM_SEMANTIC_RESULTS_OK
                || !cm_mir_flow_receiver_adjustment(output->plan,
                    expression, expression_id, &semantic_callable,
                    &receiver_adjustment))) {
            return 0;
        }

        for (index = 0u; index < argument_count;
             ++index) {
            if (!cm_mir_flow_expression(output,
                    argument_expressions[index], 0,
                    CM_MIR_RETURN_LOCAL, &call_arguments[index])
                || (call_arguments[index].kind != CM_MIR_OPERAND_MOVE
                    && call_arguments[index].kind
                        != CM_MIR_OPERAND_MOVE_PLACE
                    && call_arguments[index].kind
                        != CM_MIR_OPERAND_COPY_PLACE
                    && call_arguments[index].kind
                        != CM_MIR_CONSTANT_U32
                    && call_arguments[index].kind
                        != CM_MIR_CONSTANT_USIZE)) {
                return 0;
            }
            if (index == 0u && receiver_adjustment.present) {
                raw_receiver = call_arguments[index];
                if (!cm_mir_flow_append_receiver_borrow(output,
                        &receiver_adjustment, &raw_receiver,
                        &call_arguments[0])) {
                    return 0;
                }
            }
        }
        planned_call = (const CmMirFlowCall *)cm_vec_at_const(
            &output->plan->calls, output->call_index);
        if (planned_call == NULL
            || planned_call->expression != expression_id
            || (!has_destination
                && !cm_mir_flow_temporary(output,
                    cm_mir_flow_temporary_type(output, type),
                    &destination))) {
            return 0;
        }
        if (has_destination) destination = requested_destination;
        memset(&callee_instance, 0, sizeof(callee_instance));
        planned_source_body = CM_HIR_BODY_NONE;
        planned_callee_body = output->plan->publication == NULL
            ? cm_mir_get_body(output->plan->context, planned_call->callee)
            : cm_mir_publication_get_body(output->plan->publication,
                planned_call->callee);
        if (planned_callee_body != NULL) {
            callee_instance = planned_callee_body->instance;
        } else if (output->plan->publication == NULL
            || cm_mir_publication_get_instance(output->plan->publication,
                planned_call->callee, &callee_instance,
                &planned_source_body) != CM_MIR_OK) {
            return 0;
        }
        block = (CmMirBasicBlock *)cm_vec_at(output->blocks,
            output->current_block);
        if (block == NULL
            || output->blocks->len >= (size_t)UINT32_MAX) {
            return 0;
        }
        argument_start = output->arguments->len;
        cm_vec_append(output->arguments, call_arguments,
            argument_count);
        block->terminator.kind = CM_MIR_TERMINATOR_CALL;
        block->terminator.data.call.destination = destination;
        block->terminator.data.call.arguments =
            (CmMirOperand *)output->arguments->data + argument_start;
        block->terminator.data.call.argument_count =
            argument_count;
        block->terminator.data.call.callee_instance = planned_call->callee;
        block->terminator.data.call.callee = callee_instance;
        block->terminator.data.call.target =
            (CmMirBasicBlockId)output->blocks->len;
        output->call_index += 1u;
        if (!cm_mir_flow_next_block(output)) return 0;
        out_operand->kind = CM_MIR_OPERAND_MOVE;
        out_operand->type = ((const CmMirLocal *)cm_vec_at_const(
            output->locals, destination))->type;
        out_operand->data.local = destination;
        return 1;
    }
    return 0;
}

static CmMirLowerResult cm_mir_lower_instance_impl(CmMirContext *context,
    CmMirPublication *publication, CmMirBodyId reserved_body,
    const CmHirContext *hir, CmHirBodyId body_id,
    const CmMirInstance *reserved_instance,
    const CmHirTypeId *substitutions, uint32_t substitution_count,
    const CmSemanticAdmission *admission,
    const CmSemanticResults *semantic_results,
    CmMirSemanticEvidenceKind semantic_evidence)
{
    CmMirLowerResult result;
    const CmHirBody *hir_body;
    const CmHirItem *item;
    const CmHirFunctionSignature *signature;
    const CmHirExpr *root;
    const CmHirStatement *hir_statements;
    uint32_t hir_statement_count;
    CmHirExprId terminal_id;
    CmMirBody body;
    CmHirTypeId return_type;
    CmHirTypeId argument_types[2];
    CmMirLowerParameterLayout parameter_layout;
    CmMirFlowPlan plan;
    CmMirFlowOutput output;
    CmVec flow_locals;
    CmVec flow_statements;
    CmVec flow_blocks;
    CmVec flow_block_starts;
    CmVec flow_arguments;
    CmVec flow_aggregate_fields;
    CmVec flow_projections;
    CmMirOperand root_operand;
    CmMirLocal local;
    CmMirStatus status;
    size_t local_count;
    size_t planned_block_count;
    size_t block_index;
    uint32_t parameter_index;
    uint32_t local_index;
    uint32_t statement_index;
    int lowering_ok;

    memset(&result, 0, sizeof(result));
    if (context == NULL || hir == NULL || body_id == CM_HIR_BODY_NONE
        || (substitution_count == 0u) != (substitutions == NULL)
        || (reserved_instance != NULL
            && (reserved_instance->body != CM_HIR_BODY_NONE
                && reserved_instance->body != body_id))) {
        cm_mir_lower_fail(&result, CM_MIR_LOWER_INVALID_ARGUMENT, body_id,
            CM_HIR_EXPR_NONE, CM_MIR_INVALID_ARGUMENT,
            "invalid exact HIR-to-MIR lowering arguments");
        return result;
    }
    hir_body = cm_hir_get_body(hir, body_id);
    item = hir_body == NULL ? NULL : cm_mir_lower_function(hir, hir_body);
    if (hir_body == NULL || item == NULL
        || (item->generic_parameter_count != substitution_count
            && !cm_mir_lower_transitional_impl_instance(hir, item,
                reserved_instance, substitutions, substitution_count))) {
        cm_mir_lower_fail(&result, CM_MIR_LOWER_INVALID_HIR, body_id,
            CM_HIR_EXPR_NONE, CM_MIR_OK,
            "exact MIR instance does not match a source function");
        return result;
    }
    if (hir_body->state != CM_HIR_BODY_TYPED
        || hir_body->root_expression == CM_HIR_EXPR_NONE) {
        cm_mir_lower_fail(&result, CM_MIR_LOWER_UNSUPPORTED_BODY_STATE,
            body_id, hir_body->root_expression, CM_MIR_OK,
            "exact MIR lowering requires a fully typed HIR body");
        return result;
    }
    signature = &item->data.function_item.signature;
    if (signature->parameter_count > 2u
        || (signature->parameter_count != 0u
            && signature->parameters == NULL)
        || signature->is_variadic
        || hir_body->parameter_count != signature->parameter_count
        || (hir_body->local_count != 0u && hir_body->locals == NULL)
        || !cm_mir_lower_type(hir, item, substitutions,
            substitution_count, signature->return_type, &return_type)
        || !cm_mir_lower_type_target_valid(context, hir, item,
            return_type, 0u)
        || cm_mir_lower_type_is_unary_tuple(hir,
            signature->return_type)
        || cm_mir_lower_type_is_bool(hir, return_type)) {
        cm_mir_lower_fail(&result, CM_MIR_LOWER_UNSUPPORTED_TYPE, body_id,
            hir_body->root_expression, CM_MIR_OK,
            "exact MIR lowering supports up to two checked arguments "
            "and result");
        return result;
    }
    if (!cm_mir_lower_parameter_layout(context, hir, item, hir_body,
            substitutions, substitution_count, argument_types, NULL,
            &parameter_layout)) {
        cm_mir_lower_fail(&result, CM_MIR_LOWER_UNSUPPORTED_TYPE,
            body_id, hir_body->root_expression, CM_MIR_OK,
            "exact MIR parameter locals do not match the supported ABI "
            "binding layout");
        return result;
    }
    for (local_index = parameter_layout.hir_parameter_local_count;
         local_index < hir_body->local_count; ++local_index) {
        CmHirTypeId user_type;

        if (hir_body->locals[local_index].parameter_index
                != CM_HIR_PARAMETER_INDEX_NONE
            || hir_body->locals[local_index].mutability != CM_HIR_IMMUTABLE
            || !cm_mir_lower_type(hir, item, substitutions,
                substitution_count, hir_body->locals[local_index].type,
                &user_type)
            || !cm_mir_lower_type_target_valid(context, hir, item,
                user_type, 0u)) {
            cm_mir_lower_fail(&result, CM_MIR_LOWER_UNSUPPORTED_TYPE,
                body_id, hir_body->root_expression, CM_MIR_OK,
                "exact MIR user locals have an unsupported concrete type");
            return result;
        }
    }

    root = cm_hir_get_expr(hir, hir_body->root_expression);
    if (root == NULL || root->owner_body != body_id
        || !cm_mir_hir_type_equal(hir, root->type,
            hir_body->expected_type)) {
        cm_mir_lower_fail(&result, CM_MIR_LOWER_INVALID_HIR, body_id,
            hir_body->root_expression, CM_MIR_OK,
            "typed HIR root expression is absent or has the wrong owner");
        return result;
    }
    hir_statements = NULL;
    hir_statement_count = 0u;
    terminal_id = hir_body->root_expression;
    if (root->kind == CM_HIR_EXPR_BLOCK) {
        hir_statements = root->data.block.statements;
        hir_statement_count = root->data.block.statement_count;
        terminal_id = root->data.block.tail_expression;
    }
    if ((hir_statement_count == 0u) != (hir_statements == NULL)
        || hir_statement_count
            != hir_body->local_count
                - parameter_layout.hir_parameter_local_count
        || terminal_id == CM_HIR_EXPR_NONE) {
        cm_mir_lower_fail(&result, CM_MIR_LOWER_INVALID_HIR, body_id,
            hir_body->root_expression, CM_MIR_OK,
            "typed HIR block locals and statements do not correspond");
        return result;
    }

    memset(&body, 0, sizeof(body));
    if (reserved_instance != NULL) {
        body.instance = *reserved_instance;
    } else {
        body.instance.definition = item->definition;
        body.instance.body_definition = item->definition;
        body.instance.substitutions = (CmHirTypeId *)substitutions;
        body.instance.substitution_count = substitution_count;
    }
    body.owner = body.instance.body_definition;
    body.source_body = body_id;
    body.semantic_evidence = semantic_evidence;

    memset(&plan, 0, sizeof(plan));
    plan.context = context;
    plan.publication = publication;
    plan.hir = hir;
    plan.body = hir_body;
    plan.item = item;
    plan.instance = &body.instance;
    plan.admission = admission;
    plan.semantic_results = semantic_results;
    plan.semantic_evidence = semantic_evidence;
    plan.expected_type = return_type;
    plan.allowed_if_expression = terminal_id;
    plan.parameter_layout = parameter_layout;
    plan.statement_count = parameter_layout.tuple_binding_local_count;
    plan.projection_count = parameter_layout.tuple_binding_local_count;
    cm_vec_init(&plan.seen, sizeof(CmHirExprId));
    cm_vec_init(&plan.calls, sizeof(CmMirFlowCall));
    for (statement_index = 0u; statement_index < hir_statement_count;
         ++statement_index) {
        const CmHirStatement *hir_statement;

        hir_statement = &hir_statements[statement_index];
        local_index = parameter_layout.hir_parameter_local_count
            + statement_index;
        if (hir_statement->kind != CM_HIR_STATEMENT_LET
            || hir_statement->data.let_statement.local_index != local_index
            || hir_statement->data.let_statement.initializer
                == CM_HIR_EXPR_NONE
            || !cm_mir_flow_preflight(&plan,
                hir_statement->data.let_statement.initializer,
                local_index, 1, 0u, NULL)) {
            break;
        }
    }
    if (statement_index == hir_statement_count) {
        (void)cm_mir_flow_preflight(&plan, terminal_id,
            hir_body->local_count, 1, 0u, NULL);
    }
    if (plan.error != CM_MIR_FLOW_OK
        || statement_index != hir_statement_count) {
        CmMirLowerErrorKind error_kind;
        const char *message;

        error_kind = CM_MIR_LOWER_INVALID_HIR;
        message = "u32 statement and expression flow is malformed";
        if (plan.error == CM_MIR_FLOW_UNSUPPORTED) {
            error_kind = CM_MIR_LOWER_UNSUPPORTED_EXPRESSION;
            message = "u32 flow contains an unsupported expression";
        } else if (plan.error == CM_MIR_FLOW_CONSTANT_RANGE) {
            error_kind = CM_MIR_LOWER_CONSTANT_OUT_OF_RANGE;
            message = "integer expression does not fit u32 MIR storage";
        } else if (plan.error == CM_MIR_FLOW_CALLEE) {
            error_kind = CM_MIR_LOWER_MODEL_FAILURE;
            message = "reachable nested callee instance is not published";
        } else if (plan.error == CM_MIR_FLOW_ADMISSION) {
            error_kind = CM_MIR_LOWER_INVALID_ADMISSION;
            message = "semantic call facts are missing or inconsistent";
        }
        cm_vec_destroy(&plan.calls);
        cm_vec_destroy(&plan.seen);
        cm_mir_lower_fail(&result, error_kind, body_id,
            plan.error_expression == CM_HIR_EXPR_NONE
                ? hir_body->root_expression : plan.error_expression,
            plan.error_status, message);
        return result;
    }
    cm_vec_destroy(&plan.seen);

    if ((size_t)plan.temporary_count > (size_t)UINT32_MAX
            - (size_t)parameter_layout.non_temporary_local_count
        || plan.call_count == UINT32_MAX
        || plan.conditional_count
            > (UINT32_MAX - plan.call_count - 1u) / 3u) {
        cm_vec_destroy(&plan.calls);
        cm_mir_lower_fail(&result, CM_MIR_LOWER_INVALID_HIR, body_id,
            terminal_id, CM_MIR_ID_EXHAUSTED,
            "u32 statement and expression flow exceeds MIR storage");
        return result;
    }
    local_count = (size_t)parameter_layout.non_temporary_local_count
        + (size_t)plan.temporary_count;
    planned_block_count = (size_t)plan.call_count + 1u
        + (size_t)plan.conditional_count * 3u;

    cm_vec_init(&flow_locals, sizeof(CmMirLocal));
    cm_vec_init(&flow_statements, sizeof(CmMirStatement));
    cm_vec_init(&flow_blocks, sizeof(CmMirBasicBlock));
    cm_vec_init(&flow_block_starts, sizeof(size_t));
    cm_vec_init(&flow_arguments, sizeof(CmMirOperand));
    cm_vec_init(&flow_aggregate_fields, sizeof(CmMirAggregateField));
    cm_vec_init(&flow_projections, sizeof(CmMirPlaceProjection));
    cm_vec_reserve(&flow_locals, local_count);
    cm_vec_reserve(&flow_statements, (size_t)plan.statement_count);
    cm_vec_reserve(&flow_blocks, planned_block_count);
    cm_vec_reserve(&flow_block_starts, planned_block_count);
    cm_vec_reserve(&flow_arguments, (size_t)plan.call_argument_count);
    cm_vec_reserve(&flow_aggregate_fields,
        (size_t)plan.aggregate_field_count);
    cm_vec_reserve(&flow_projections, (size_t)plan.projection_count);

    memset(&local, 0, sizeof(local));
    local.kind = CM_MIR_LOCAL_RETURN;
    local.type = return_type;
    (void)cm_vec_push(&flow_locals, &local);
    for (parameter_index = 0u;
         parameter_index < signature->parameter_count; ++parameter_index) {
        memset(&local, 0, sizeof(local));
        local.kind = CM_MIR_LOCAL_ARGUMENT;
        local.type = argument_types[parameter_index];
        (void)cm_vec_push(&flow_locals, &local);
    }
    for (local_index = 0u; local_index < hir_body->local_count;
         ++local_index) {
        CmMirLocalId mapped_local;

        if (!cm_mir_lower_hir_local_id(hir, signature, hir_body,
                &parameter_layout, local_index, &mapped_local)) {
            break;
        }
        if (mapped_local <= signature->parameter_count) {
            const CmMirLocal *argument_local;
            CmHirTypeId instantiated_local_type;

            argument_local = (const CmMirLocal *)cm_vec_at_const(
                &flow_locals, mapped_local);
            if (argument_local == NULL
                || !cm_mir_lower_type(hir, item, substitutions,
                    substitution_count,
                    hir_body->locals[local_index].type,
                    &instantiated_local_type)
                || !cm_mir_hir_type_equal(hir, argument_local->type,
                    instantiated_local_type)) {
                break;
            }
            continue;
        }
        if (mapped_local != (CmMirLocalId)flow_locals.len) break;
        memset(&local, 0, sizeof(local));
        local.kind = CM_MIR_LOCAL_USER;
        if (!cm_mir_lower_type(hir, item, substitutions,
                substitution_count, hir_body->locals[local_index].type,
                &local.type)) {
            break;
        }
        (void)cm_vec_push(&flow_locals, &local);
    }
    if (local_index != hir_body->local_count
        || flow_locals.len
            != (size_t)parameter_layout.non_temporary_local_count) {
        cm_vec_destroy(&flow_projections);
        cm_vec_destroy(&flow_aggregate_fields);
        cm_vec_destroy(&flow_arguments);
        cm_vec_destroy(&flow_block_starts);
        cm_vec_destroy(&flow_blocks);
        cm_vec_destroy(&flow_statements);
        cm_vec_destroy(&flow_locals);
        cm_vec_destroy(&plan.calls);
        cm_mir_lower_fail(&result, CM_MIR_LOWER_INVALID_HIR, body_id,
            terminal_id, CM_MIR_OK,
            "MIR local ABI layout changed during lowering");
        return result;
    }

    memset(&output, 0, sizeof(output));
    output.plan = &plan;
    output.locals = &flow_locals;
    output.statements = &flow_statements;
    output.blocks = &flow_blocks;
    output.block_starts = &flow_block_starts;
    output.arguments = &flow_arguments;
    output.aggregate_fields = &flow_aggregate_fields;
    output.projections = &flow_projections;
    lowering_ok = cm_mir_flow_next_block(&output)
        && cm_mir_flow_append_tuple_parameter_prologue(&output);
    statement_index = 0u;
    if (lowering_ok) {
        for (statement_index = 0u;
             statement_index < hir_statement_count; ++statement_index) {
            const CmHirStatement *hir_statement;
            CmMirLocalId destination;

            hir_statement = &hir_statements[statement_index];
            if (!cm_mir_lower_hir_local_id(hir, signature, hir_body,
                    &parameter_layout,
                    hir_statement->data.let_statement.local_index,
                    &destination)) {
                lowering_ok = 0;
                break;
            }
            if (!cm_mir_flow_expression(&output,
                    hir_statement->data.let_statement.initializer, 1,
                    destination, &root_operand)) {
                lowering_ok = 0;
                break;
            }
        }
    }
    if (lowering_ok && statement_index == hir_statement_count
        && !cm_mir_flow_expression(&output, terminal_id, 1,
            CM_MIR_RETURN_LOCAL, &root_operand)) {
        lowering_ok = 0;
    }
    if (!lowering_ok || statement_index != hir_statement_count
        || root_operand.kind != CM_MIR_OPERAND_MOVE
        || root_operand.data.local != CM_MIR_RETURN_LOCAL
        || flow_locals.len != local_count
        || flow_statements.len != (size_t)plan.statement_count
        || flow_blocks.len != planned_block_count
        || flow_block_starts.len != flow_blocks.len
        || flow_arguments.len != (size_t)plan.call_argument_count
        || flow_aggregate_fields.len
            != (size_t)plan.aggregate_field_count
        || flow_projections.len != (size_t)plan.projection_count
        || output.call_index != plan.call_count) {
        cm_vec_destroy(&flow_projections);
        cm_vec_destroy(&flow_aggregate_fields);
        cm_vec_destroy(&flow_arguments);
        cm_vec_destroy(&flow_block_starts);
        cm_vec_destroy(&flow_blocks);
        cm_vec_destroy(&flow_statements);
        cm_vec_destroy(&flow_locals);
        cm_vec_destroy(&plan.calls);
        cm_mir_lower_fail(&result, CM_MIR_LOWER_INVALID_HIR, body_id,
            terminal_id, CM_MIR_OK,
            "u32 statement and expression flow changed during MIR lowering");
        return result;
    }
    ((CmMirBasicBlock *)flow_blocks.data)[output.current_block]
        .terminator.kind = CM_MIR_TERMINATOR_RETURN;
    for (block_index = 0u; block_index < flow_blocks.len;
         ++block_index) {
        CmMirBasicBlock *flow_block;
        const size_t *start;
        const size_t *next_start;
        size_t end;

        flow_block = (CmMirBasicBlock *)cm_vec_at(&flow_blocks,
            block_index);
        start = (const size_t *)cm_vec_at_const(&flow_block_starts,
            block_index);
        next_start = (const size_t *)cm_vec_at_const(
            &flow_block_starts, block_index + 1u);
        end = next_start == NULL ? flow_statements.len : *next_start;
        if (flow_block == NULL || start == NULL || end < *start
            || end - *start > (size_t)UINT32_MAX) {
            cm_vec_destroy(&flow_projections);
            cm_vec_destroy(&flow_aggregate_fields);
            cm_vec_destroy(&flow_arguments);
            cm_vec_destroy(&flow_block_starts);
            cm_vec_destroy(&flow_blocks);
            cm_vec_destroy(&flow_statements);
            cm_vec_destroy(&flow_locals);
            cm_vec_destroy(&plan.calls);
            cm_mir_lower_fail(&result, CM_MIR_LOWER_INVALID_HIR,
                body_id, terminal_id, CM_MIR_OK,
                "u32 expression block boundaries are malformed");
            return result;
        }
        flow_block->statement_count = (uint32_t)(end - *start);
        flow_block->statements = end == *start ? NULL
            : (CmMirStatement *)flow_statements.data + *start;
    }
    body.locals = (CmMirLocal *)flow_locals.data;
    body.local_count = (uint32_t)flow_locals.len;
    body.basic_blocks = (CmMirBasicBlock *)flow_blocks.data;
    body.basic_block_count = (uint32_t)flow_blocks.len;
    if (publication != NULL) {
        status = cm_mir_publication_define(publication, reserved_body,
            &body);
        result.body = status == CM_MIR_OK ? reserved_body
            : CM_MIR_BODY_NONE;
    } else {
        status = admission == NULL
            ? cm_mir_add_monomorphized_body(context, hir, &body,
                &result.body)
            : cm_mir_add_admitted_monomorphized_body(context, admission,
                &body, &result.body);
    }
    cm_vec_destroy(&flow_projections);
    cm_vec_destroy(&flow_aggregate_fields);
    cm_vec_destroy(&flow_arguments);
    cm_vec_destroy(&flow_block_starts);
    cm_vec_destroy(&flow_blocks);
    cm_vec_destroy(&flow_statements);
    cm_vec_destroy(&flow_locals);
    cm_vec_destroy(&plan.calls);
    if (status != CM_MIR_OK) {
        cm_mir_lower_fail(&result, CM_MIR_LOWER_MODEL_FAILURE,
            body_id, terminal_id, status,
            "MIR model rejected the exact lowered expression flow");
        return result;
    }
    result.lowered_body_count = 1u;
    return result;
}

CmMirLowerResult cm_mir_lower_instance(CmMirContext *context,
    const CmHirContext *hir, CmHirBodyId body_id,
    const CmHirTypeId *substitutions, uint32_t substitution_count)
{
    return cm_mir_lower_instance_impl(context, NULL, CM_MIR_BODY_NONE, hir,
        body_id, NULL, substitutions, substitution_count, NULL, NULL,
        CM_MIR_SEMANTIC_EVIDENCE_NONE);
}

static const CmHirContext *cm_mir_lower_admitted_hir(
    const CmMirContext *context, const CmSemanticAdmission *admission,
    CmHirBodyId body_id, CmHirCrateId *out_crate)
{
    const CmHirContext *hir;
    const CmHirBody *body;
    CmHirCrateId crate_id;

    if (!cm_semantic_admission_is_current(admission)) return NULL;
    hir = cm_semantic_admission_hir(admission);
    crate_id = cm_semantic_admission_crate(admission);
    body = hir == NULL ? NULL : cm_hir_get_body(hir, body_id);
    if (body == NULL || crate_id == CM_HIR_CRATE_NONE
        || body->owner.crate_id != crate_id
        || cm_semantic_admission_generation(admission)
            != hir->semantic_generation) return NULL;
    if (context->admitted_crate == CM_HIR_CRATE_NONE) {
        if (context->bodies.len != 0u || context->hir_owner != NULL
            || context->admitted_storage_lifetime_id != UINT64_C(0)
            || context->admitted_semantic_generation != UINT64_C(0)
            || context->admitted_rewind_generation != UINT64_C(0)
            || context->admitted_admission_capability_id
                != UINT64_C(0)
            || context->admitted_barrier_capability_id
                != UINT64_C(0)
            || context->admitted_parent_capability_id
                != UINT64_C(0)) {
            return NULL;
        }
    } else if (context->hir_owner != hir
        || context->admitted_crate != crate_id
        || context->admitted_storage_lifetime_id != hir->storage.lifetime_id
        || context->admitted_semantic_generation != hir->semantic_generation
        || context->admitted_rewind_generation != hir->rewind_generation
        || context->admitted_admission_capability_id
            != cm_semantic_admission_capability_id(admission)
        || context->admitted_barrier_capability_id
            != cm_semantic_admission_barrier_capability_id(admission)
        || context->admitted_parent_capability_id
            != cm_semantic_admission_parent_capability_id(admission)) {
        return NULL;
    }
    *out_crate = crate_id;
    return hir;
}

static void cm_mir_lower_latch_admission(CmMirContext *context,
    const CmSemanticAdmission *admission, const CmHirContext *hir,
    CmHirCrateId crate_id)
{
    context->hir_owner = hir;
    context->admitted_crate = crate_id;
    context->admitted_storage_lifetime_id = hir->storage.lifetime_id;
    context->admitted_semantic_generation = hir->semantic_generation;
    context->admitted_rewind_generation = hir->rewind_generation;
    context->admitted_admission_capability_id =
        cm_semantic_admission_capability_id(admission);
    context->admitted_barrier_capability_id =
        cm_semantic_admission_barrier_capability_id(admission);
    context->admitted_parent_capability_id =
        cm_semantic_admission_parent_capability_id(admission);
}

static CmMirLowerResult cm_mir_lower_admission_failure(CmHirBodyId body_id)
{
    CmMirLowerResult result;
    memset(&result, 0, sizeof(result));
    cm_mir_lower_fail(&result, CM_MIR_LOWER_INVALID_ADMISSION, body_id,
        CM_HIR_EXPR_NONE, CM_MIR_INVALID_ADMISSION,
        "MIR lowering requires current matching semantic admission");
    return result;
}

CmMirLowerResult cm_mir_lower_admitted_body(CmMirContext *context,
    const CmSemanticAdmission *admission, CmHirBodyId body_id)
{
    const CmHirContext *hir;
    const CmSemanticResults *semantic_results;
    CmSemanticBodyView semantic_body;
    CmHirCrateId crate_id;
    CmMirLowerResult result;

    if (context == NULL) {
        memset(&result, 0, sizeof(result));
        cm_mir_lower_fail(&result, CM_MIR_LOWER_INVALID_ARGUMENT, body_id,
            CM_HIR_EXPR_NONE, CM_MIR_INVALID_ARGUMENT,
            "invalid admitted HIR-to-MIR lowering destination");
        return result;
    }
    hir = cm_mir_lower_admitted_hir(context, admission, body_id, &crate_id);
    semantic_results = hir == NULL ? NULL
        : cm_semantic_admission_results(admission);
    if (semantic_results == NULL
        || cm_semantic_results_body(semantic_results, admission, body_id,
            &semantic_body) != CM_SEMANTIC_RESULTS_OK
        || cm_hir_body_function_owner_kind(hir,
            cm_mir_flow_definition_item(hir, semantic_body.owner))
            == CM_HIR_BODY_FUNCTION_OWNER_TRAIT_DEFAULT) {
        return cm_mir_lower_admission_failure(body_id);
    }
    result = cm_mir_lower_instance_impl(context, NULL, CM_MIR_BODY_NONE, hir,
        body_id, NULL, NULL, 0u, admission, semantic_results,
        CM_MIR_SEMANTIC_EVIDENCE_BODY);
    if (result.error_count == 0u
        && context->admitted_crate == CM_HIR_CRATE_NONE) {
        cm_mir_lower_latch_admission(context, admission, hir, crate_id);
    }
    return result;
}

CmMirLowerResult cm_mir_lower_admitted_instance(CmMirContext *context,
    const CmSemanticAdmission *admission, CmHirBodyId body_id,
    const CmHirTypeId *substitutions, uint32_t substitution_count)
{
    const CmHirContext *hir;
    const CmSemanticResults *semantic_results;
    CmSemanticBodyView semantic_body;
    CmHirInstanceSpec spec;
    CmHirGenericArg *arguments;
    CmHirCrateId crate_id;
    CmMirLowerResult result;
    uint32_t index;

    if (context == NULL) {
        memset(&result, 0, sizeof(result));
        cm_mir_lower_fail(&result, CM_MIR_LOWER_INVALID_ARGUMENT, body_id,
            CM_HIR_EXPR_NONE, CM_MIR_INVALID_ARGUMENT,
            "invalid admitted HIR-to-MIR lowering destination");
        return result;
    }
    if ((substitution_count == 0u) != (substitutions == NULL)) {
        memset(&result, 0, sizeof(result));
        cm_mir_lower_fail(&result, CM_MIR_LOWER_INVALID_ARGUMENT, body_id,
            CM_HIR_EXPR_NONE, CM_MIR_INVALID_ARGUMENT,
            "missing exact MIR instance substitutions");
        return result;
    }
    hir = cm_mir_lower_admitted_hir(context, admission, body_id, &crate_id);
    if (hir == NULL) return cm_mir_lower_admission_failure(body_id);
    semantic_results = cm_semantic_admission_results(admission);
    arguments = NULL;
    if (substitution_count != 0u) {
        arguments = (CmHirGenericArg *)cm_alloc_zeroed(substitution_count,
            sizeof(CmHirGenericArg));
        for (index = 0u; index < substitution_count; ++index) {
            arguments[index].kind = CM_HIR_GENERIC_ARG_TYPE;
            arguments[index].data.type = substitutions[index];
        }
    }
    cm_hir_instance_spec_init(&spec);
    spec.item_arguments = arguments;
    spec.item_argument_count = substitution_count;
    if (semantic_results == NULL) {
        cm_free(arguments);
        return cm_mir_lower_admission_failure(body_id);
    }
    {
        const CmHirBody *body;

        body = cm_hir_get_body(hir, body_id);
        if (body == NULL) {
            cm_free(arguments);
            return cm_mir_lower_admission_failure(body_id);
        }
        spec.selected_callable = body->owner;
        spec.body_definition = body->owner;
        if (cm_semantic_results_instance_body(semantic_results, admission,
                &spec, &semantic_body) != CM_SEMANTIC_RESULTS_OK
            || semantic_body.body != body_id
            || !cm_hir_def_id_equal(semantic_body.owner, body->owner)) {
            cm_free(arguments);
            return cm_mir_lower_admission_failure(body_id);
        }
    }
    cm_free(arguments);
    result = cm_mir_lower_instance_impl(context, NULL, CM_MIR_BODY_NONE, hir,
        body_id, NULL, substitutions, substitution_count, admission,
        semantic_results, CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE);
    if (result.error_count == 0u
        && context->admitted_crate == CM_HIR_CRATE_NONE) {
        cm_mir_lower_latch_admission(context, admission, hir, crate_id);
    }
    return result;
}

CmMirLowerResult cm_mir_lower_admitted_publication_instance(
    CmMirContext *context, CmMirPublication *publication,
    const CmSemanticAdmission *admission, CmMirBodyId reserved_body,
    CmHirBodyId body_id, const CmHirTypeId *substitutions,
    uint32_t substitution_count)
{
    const CmHirContext *hir;
    const CmSemanticResults *semantic_results;
    CmMirInstance reserved_instance;
    CmHirBodyId reserved_source_body;
    const CmHirBody *reserved_hir_body;
    CmMirLowerResult result;
    CmHirCrateId crate_id;

    memset(&result, 0, sizeof(result));
    memset(&reserved_instance, 0, sizeof(reserved_instance));
    reserved_source_body = CM_HIR_BODY_NONE;
    reserved_hir_body = admission == NULL ? NULL
        : cm_hir_get_body(cm_semantic_admission_hir(admission), body_id);
    if (context == NULL || publication == NULL
        || (substitution_count == 0u) != (substitutions == NULL)
        || cm_mir_publication_get_instance(publication, reserved_body,
            &reserved_instance, &reserved_source_body) != CM_MIR_OK
        || reserved_source_body != body_id
        || reserved_hir_body == NULL
        || !cm_hir_def_id_equal(reserved_instance.body_definition,
            reserved_hir_body->owner)
        || reserved_instance.substitution_count != substitution_count
        || (substitution_count != 0u
            && memcmp(reserved_instance.substitutions, substitutions,
                (size_t)substitution_count * sizeof(*substitutions))
                    != 0)) {
        cm_mir_lower_fail(&result, CM_MIR_LOWER_INVALID_ARGUMENT, body_id,
            CM_HIR_EXPR_NONE, CM_MIR_INVALID_ARGUMENT,
            "invalid reserved exact MIR instance");
        return result;
    }
    hir = cm_mir_lower_admitted_hir(context, admission, body_id, &crate_id);
    semantic_results = cm_semantic_admission_results(admission);
    if (hir == NULL || semantic_results == NULL) {
        return cm_mir_lower_admission_failure(body_id);
    }
    return cm_mir_lower_instance_impl(context, publication, reserved_body,
        hir, body_id, &reserved_instance, substitutions, substitution_count,
        admission,
        semantic_results, CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE);
}

CmMirLowerResult cm_mir_lower_admitted_publication_canonical(
    CmMirContext *context, CmMirPublication *publication,
    const CmSemanticAdmission *admission, CmMirBodyId reserved_body)
{
    const CmHirContext *hir;
    const CmSemanticResults *semantic_results;
    const CmHirBody *reserved_hir_body;
    CmMirInstance reserved_instance;
    CmHirBodyId source_body;
    CmMirLowerResult result;
    CmHirCrateId crate_id;

    memset(&result, 0, sizeof(result));
    memset(&reserved_instance, 0, sizeof(reserved_instance));
    source_body = CM_HIR_BODY_NONE;
    if (context == NULL || publication == NULL
        || cm_mir_publication_get_instance(publication, reserved_body,
            &reserved_instance, &source_body) != CM_MIR_OK
        || reserved_instance.body != source_body
        || reserved_instance.identity_bytes == NULL
        || reserved_instance.identity_size == 0u) {
        cm_mir_lower_fail(&result, CM_MIR_LOWER_INVALID_ARGUMENT,
            source_body, CM_HIR_EXPR_NONE, CM_MIR_INVALID_ARGUMENT,
            "invalid reserved canonical MIR instance");
        return result;
    }
    hir = cm_mir_lower_admitted_hir(context, admission, source_body,
        &crate_id);
    semantic_results = admission == NULL ? NULL
        : cm_semantic_admission_results(admission);
    reserved_hir_body = hir == NULL ? NULL
        : cm_hir_get_body(hir, source_body);
    if (hir == NULL || semantic_results == NULL
        || reserved_hir_body == NULL
        || !cm_hir_def_id_equal(reserved_hir_body->owner,
            reserved_instance.body_definition)) {
        return cm_mir_lower_admission_failure(source_body);
    }
    return cm_mir_lower_instance_impl(context, publication, reserved_body,
        hir, source_body, &reserved_instance, reserved_instance.substitutions,
        reserved_instance.substitution_count, admission, semantic_results,
        CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE);
}

const char *cm_mir_lower_error_kind_name(CmMirLowerErrorKind kind)
{
    switch (kind) {
    case CM_MIR_LOWER_INVALID_ARGUMENT:
        return "invalid argument";
    case CM_MIR_LOWER_INVALID_ADMISSION:
        return "invalid admission";
    case CM_MIR_LOWER_INVALID_HIR:
        return "invalid HIR";
    case CM_MIR_LOWER_UNSUPPORTED_BODY_STATE:
        return "unsupported body state";
    case CM_MIR_LOWER_UNSUPPORTED_TYPE:
        return "unsupported type";
    case CM_MIR_LOWER_UNSUPPORTED_EXPRESSION:
        return "unsupported expression";
    case CM_MIR_LOWER_CONSTANT_OUT_OF_RANGE:
        return "constant out of range";
    case CM_MIR_LOWER_MODEL_FAILURE:
        return "MIR model failure";
    }
    return "unknown MIR lowering error";
}
