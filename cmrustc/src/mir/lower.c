#include "cm/mir/lower.h"
#include "cm/alloc.h"
#include "cm/hir/instance.h"
#include "cm/hir/semantic_results.h"

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

static int cm_mir_hir_type_equal(const CmHirContext *hir,
    CmHirTypeId left_id, CmHirTypeId right_id)
{
    const CmHirType *left;
    const CmHirType *right;

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
    if (left->kind == CM_HIR_TYPE_ADT_KIND) {
        return left->data.named_type.argument_count == 0u
            && left->data.named_type.arguments == NULL
            && right->data.named_type.argument_count == 0u
            && right->data.named_type.arguments == NULL
            && cm_hir_def_id_equal(left->data.named_type.definition,
                right->data.named_type.definition);
    }
    return 0;
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

static int cm_mir_lower_type_is_scalar(const CmHirContext *hir,
    CmHirTypeId type_id)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, type_id);
    return type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && (type->data.integer_type.kind == CM_HIR_INT_I32
            || type->data.integer_type.kind == CM_HIR_INT_U32
            || type->data.integer_type.kind == CM_HIR_INT_USIZE);
}

static int cm_mir_lower_type_is_u32(const CmHirContext *hir,
    CmHirTypeId type_id)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, type_id);
    return type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_U32;
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
    if (type->kind != CM_HIR_TYPE_ADT_KIND) return 1;
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
        && cm_mir_lower_named_struct(hir, item,
            type->data.named_type.definition) != NULL) {
        *out_type = declared;
        return 1;
    }
    if (type->kind != CM_HIR_TYPE_PARAMETER_KIND) return 0;
    parameter = cm_hir_get_generic_param(hir,
        type->data.parameter_type.parameter);
    if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_TYPE
        || !cm_hir_def_id_equal(parameter->owner, item->definition)
        || parameter->index >= substitution_count) {
        return 0;
    }
    index = parameter->index;
    type = cm_hir_get_type(hir, substitutions[index]);
    if (type == NULL || type->kind != CM_HIR_TYPE_INTEGER_KIND
        || type->data.integer_type.kind != CM_HIR_INT_U32) {
        return 0;
    }
    *out_type = substitutions[index];
    return 1;
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
    const CmHirContext *hir;
    const CmHirBody *body;
    const CmHirItem *item;
    const CmMirInstance *instance;
    const CmSemanticAdmission *admission;
    const CmSemanticResults *semantic_results;
    CmHirTypeId expected_type;
    CmHirExprId allowed_if_expression;
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
            || !cm_mir_lower_type_is_scalar(plan->hir, type)) {
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

        if (!cm_mir_flow_preflight(plan, expression->data.field.base,
                visible_local_count, 0, depth + 1u,
                &base_projection_count)
            || !cm_mir_flow_expression_type(plan,
                expression->data.field.base, &base_expression, &base_type)) {
            return 0;
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
        if (!has_destination) plan->temporary_count += 1u;
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
    } else if (expression->kind == CM_HIR_EXPR_CALL) {
        CmHirTypeId callee_substitution;
        CmHirTypeId *callee_substitutions;
        CmMirBodyId callee_id;
        const CmMirBody *callee_body;
        const CmHirBody *callee_hir_body;
        const CmHirItem *callee_item;
        CmHirDefId callee_definition;
        CmSemanticDirectCallView semantic_call;
        CmSemanticFunctionSignatureView semantic_callee_signature;
        CmSemanticExpressionView semantic_expression;
        int has_aggregate_argument;
        CmMirStatus status;
        uint32_t index;

        if ((!cm_mir_lower_type_is_u32(plan->hir, type)
                && !cm_mir_lower_type_is_usize(plan->hir, type))
            || expression->data.call.argument_count < 1u
            || expression->data.call.argument_count > 2u
            || expression->data.call.arguments == NULL
            || (expression->data.call.type_substitution_count != 0u
                && (expression->data.call.type_substitution_count != 1u
                    || expression->data.call.argument_count != 1u
                    || expression->data.call.type_substitutions == NULL))
            || plan->call_count == UINT32_MAX
            || (!has_destination && plan->temporary_count == UINT32_MAX)
            || plan->call_argument_count
                > UINT32_MAX - expression->data.call.argument_count) {
            return cm_mir_flow_fail(plan, CM_MIR_FLOW_UNSUPPORTED,
                expression_id, CM_MIR_OK);
        }
        callee_substitutions = NULL;
        callee_definition = expression->data.call.callee;
        memset(&semantic_call, 0, sizeof(semantic_call));
        memset(&semantic_callee_signature, 0,
            sizeof(semantic_callee_signature));
        if (plan->semantic_results != NULL) {
            if (plan->admission == NULL
                || plan->instance->substitution_count != 0u
                || expression->data.call.type_substitution_count != 0u
                || cm_semantic_results_direct_call(
                    plan->semantic_results, plan->admission,
                    plan->item->data.function_item.body,
                    expression_id, &semantic_call)
                        != CM_SEMANTIC_RESULTS_OK
                || semantic_call.parameter_count
                    != expression->data.call.argument_count
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
        if (expression->data.call.type_substitution_count == 1u) {
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
        status = cm_mir_find_instance(plan->context,
            callee_definition, callee_substitutions,
            expression->data.call.type_substitution_count, &callee_id);
        if (status != CM_MIR_OK) {
            return cm_mir_flow_fail(plan, CM_MIR_FLOW_CALLEE,
                expression_id, status);
        }
        callee_body = cm_mir_get_body(plan->context, callee_id);
        callee_hir_body = callee_body == NULL ? NULL
            : cm_hir_get_body(plan->hir, callee_body->source_body);
        callee_item = callee_hir_body == NULL ? NULL
            : cm_mir_lower_function(plan->hir, callee_hir_body);
        if (callee_body == NULL || callee_body->locals == NULL
            || callee_body->local_count
                < expression->data.call.argument_count + 1u
            || callee_item == NULL
            || callee_item->data.function_item.signature.parameter_count
                != expression->data.call.argument_count
            || !cm_hir_def_id_equal(callee_body->instance.definition,
                callee_definition)) {
            return cm_mir_flow_fail(plan, CM_MIR_FLOW_INVALID,
                expression_id, CM_MIR_OK);
        }
        if (plan->semantic_results != NULL
            && (cm_semantic_results_signature(plan->semantic_results,
                    plan->admission, callee_body->source_body,
                    &semantic_callee_signature)
                    != CM_SEMANTIC_RESULTS_OK
                || !cm_hir_def_id_equal(
                    semantic_callee_signature.definition,
                    callee_definition)
                || semantic_callee_signature.parameter_count
                    != semantic_call.parameter_count
                || !cm_mir_semantic_types_equal(
                    &semantic_callee_signature.return_type,
                    &semantic_call.return_type))) {
            return cm_mir_flow_fail(plan, CM_MIR_FLOW_ADMISSION,
                expression_id, CM_MIR_INVALID_ADMISSION);
        }
        if (!cm_mir_hir_type_equal(plan->hir, type,
                callee_body->locals[CM_MIR_RETURN_LOCAL].type)
            || (!cm_mir_lower_type_is_u32(plan->hir, type)
                && !cm_mir_lower_type_is_usize(plan->hir, type))) {
            return cm_mir_flow_fail(plan, CM_MIR_FLOW_UNSUPPORTED,
                expression_id, CM_MIR_OK);
        }
        has_aggregate_argument = 0;
        for (index = 0u; index < expression->data.call.argument_count;
             ++index) {
            const CmHirExpr *argument;
            CmHirTypeId parameter_type;
            CmSemanticTypeView semantic_parameter;
            CmSemanticTypeView semantic_callee_parameter;
            CmSemanticExpressionView semantic_argument;

            argument = cm_hir_get_expr(plan->hir,
                expression->data.call.arguments[index]);
            if (argument == NULL) {
                return cm_mir_flow_fail(plan, CM_MIR_FLOW_INVALID,
                    expression->data.call.arguments[index], CM_MIR_OK);
            }
            if (plan->semantic_results != NULL
                && (cm_semantic_results_direct_call_parameter(
                        plan->semantic_results, plan->admission,
                        plan->item->data.function_item.body, expression_id,
                        index, &semantic_parameter)
                        != CM_SEMANTIC_RESULTS_OK
                    || cm_semantic_results_signature_parameter(
                        plan->semantic_results, plan->admission,
                        callee_body->source_body, index,
                        &semantic_callee_parameter)
                        != CM_SEMANTIC_RESULTS_OK
                    || cm_semantic_results_expression(
                        plan->semantic_results, plan->admission,
                        plan->item->data.function_item.body,
                        expression->data.call.arguments[index],
                        &semantic_argument) != CM_SEMANTIC_RESULTS_OK
                    || !cm_mir_semantic_types_equal(&semantic_parameter,
                        &semantic_callee_parameter)
                    || !cm_mir_semantic_types_equal(&semantic_parameter,
                        &semantic_argument.adjusted_type))) {
                return cm_mir_flow_fail(plan, CM_MIR_FLOW_ADMISSION,
                    expression->data.call.arguments[index],
                    CM_MIR_INVALID_ADMISSION);
            }
            if (!cm_mir_flow_expression_type(plan,
                    expression->data.call.arguments[index], NULL,
                    &local_type)
                || !cm_mir_lower_type(plan->hir, callee_item,
                    callee_body->instance.substitutions,
                    callee_body->instance.substitution_count,
                    callee_item->data.function_item.signature
                        .parameters[index].type,
                    &parameter_type)
                || !cm_mir_hir_type_equal(plan->hir, local_type,
                    parameter_type)
                || ((!cm_mir_lower_type_is_u32(plan->hir, parameter_type)
                        && !cm_mir_lower_type_is_usize(plan->hir,
                            parameter_type))
                    && !cm_mir_lower_type_is_aggregate(plan->hir,
                        callee_item, parameter_type))) {
                return cm_mir_flow_fail(plan, CM_MIR_FLOW_UNSUPPORTED,
                    expression->data.call.arguments[index], CM_MIR_OK);
            }
            if (argument->kind == CM_HIR_EXPR_INTEGER
                && !cm_mir_lower_type_is_usize(plan->hir, local_type)) {
                return cm_mir_flow_fail(plan, CM_MIR_FLOW_UNSUPPORTED,
                    expression->data.call.arguments[index], CM_MIR_OK);
            }
            if (cm_mir_lower_type_is_aggregate(plan->hir,
                    callee_item, parameter_type)) {
                has_aggregate_argument = 1;
            }
            if (!cm_mir_flow_preflight(plan,
                    expression->data.call.arguments[index],
                    visible_local_count, 0, depth + 1u, NULL)) {
                return 0;
            }
        }
        if (has_aggregate_argument
            && (expression->data.call.type_substitution_count != 0u
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
        plan->call_argument_count += expression->data.call.argument_count;
        if (!has_destination) plan->temporary_count += 1u;
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
    const CmHirExpr *expression, CmMirLocalId destination,
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
    if (expression->data.binary.operator_kind == CM_HIR_BINARY_EQUAL) {
        statement.data.assign.value.kind = CM_MIR_RVALUE_EQUAL;
        statement.data.assign.value.data.equal.left = *left;
        statement.data.assign.value.data.equal.right = *right;
    } else if (expression->data.binary.operator_kind
            == CM_HIR_BINARY_LESS) {
        statement.data.assign.value.kind = CM_MIR_RVALUE_LESS;
        statement.data.assign.value.data.less.left = *left;
        statement.data.assign.value.data.less.right = *right;
    } else {
        statement.data.assign.value.kind = CM_MIR_RVALUE_BINARY;
        statement.data.assign.value.data.binary.operator_kind =
            expression->data.binary.operator_kind == CM_HIR_BINARY_ADD
                ? CM_MIR_BINARY_ADD : CM_MIR_BINARY_SUBTRACT;
        statement.data.assign.value.data.binary.left = *left;
        statement.data.assign.value.data.binary.right = *right;
    }
    (void)cm_vec_push(output->statements, &statement);
    return 1;
}

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
        if (!cm_mir_lower_type(output->plan->hir, output->plan->item,
                output->plan->instance->substitutions,
                output->plan->instance->substitution_count,
                output->plan->body->locals[
                    expression->data.local.local_index].type, &type)) {
            return 0;
        }
        out_operand->kind = CM_MIR_OPERAND_MOVE;
        out_operand->type = type;
        out_operand->data.local = expression->data.local.local_index + 1u;
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
        CmMirFieldProjection projection_buffer[
            CM_MIR_MAX_PLACE_PROJECTIONS];
        CmMirPlace place;
        size_t projection_start;
        uint32_t base_projection_count;

        if (!cm_mir_flow_expression(output, expression->data.field.base,
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
                        * sizeof(CmMirFieldProjection));
            }
        } else {
            return 0;
        }
        if (base_projection_count >= CM_MIR_MAX_PLACE_PROJECTIONS) return 0;
        projection_buffer[base_projection_count].definition =
            expression->data.field.definition;
        projection_buffer[base_projection_count].field_index =
            expression->data.field.field_index;
        place.projection_count = base_projection_count + 1u;
        projection_start = output->projections->len;
        cm_vec_append(output->projections, projection_buffer,
            place.projection_count);
        place.projections = (CmMirFieldProjection *)output->projections->data
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

        if (!cm_mir_flow_expression(output, expression->data.binary.left,
                0, CM_MIR_RETURN_LOCAL, &left)
            || !cm_mir_flow_expression(output,
                expression->data.binary.right, 0,
                CM_MIR_RETURN_LOCAL, &right)
            || (!has_destination
                && !cm_mir_flow_temporary(output,
                    cm_mir_flow_temporary_type(output, type),
                    &destination))) {
            return 0;
        }
        if (has_destination) destination = requested_destination;
        if (!cm_mir_flow_append_binary(output, expression, destination,
                &left, &right)) {
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
    if (expression->kind == CM_HIR_EXPR_CALL) {
        const CmMirFlowCall *planned_call;
        const CmMirBody *callee;
        CmMirOperand call_arguments[2];
        CmMirLocalId destination;
        CmMirBasicBlock *block;
        size_t argument_start;
        uint32_t index;

        for (index = 0u; index < expression->data.call.argument_count;
             ++index) {
            if (!cm_mir_flow_expression(output,
                    expression->data.call.arguments[index], 0,
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
        callee = cm_mir_get_body(output->plan->context,
            planned_call->callee);
        block = (CmMirBasicBlock *)cm_vec_at(output->blocks,
            output->current_block);
        if (callee == NULL || block == NULL
            || output->blocks->len >= (size_t)UINT32_MAX) {
            return 0;
        }
        argument_start = output->arguments->len;
        cm_vec_append(output->arguments, call_arguments,
            expression->data.call.argument_count);
        block->terminator.kind = CM_MIR_TERMINATOR_CALL;
        block->terminator.data.call.destination = destination;
        block->terminator.data.call.arguments =
            (CmMirOperand *)output->arguments->data + argument_start;
        block->terminator.data.call.argument_count =
            expression->data.call.argument_count;
        block->terminator.data.call.callee_instance = planned_call->callee;
        block->terminator.data.call.callee = callee->instance;
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
    const CmHirContext *hir, CmHirBodyId body_id,
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
        || (substitution_count == 0u) != (substitutions == NULL)) {
        cm_mir_lower_fail(&result, CM_MIR_LOWER_INVALID_ARGUMENT, body_id,
            CM_HIR_EXPR_NONE, CM_MIR_INVALID_ARGUMENT,
            "invalid exact HIR-to-MIR lowering arguments");
        return result;
    }
    hir_body = cm_hir_get_body(hir, body_id);
    item = hir_body == NULL ? NULL : cm_mir_lower_function(hir, hir_body);
    if (hir_body == NULL || item == NULL
        || item->generic_parameter_count != substitution_count) {
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
    if (signature->parameter_count == 0u
        || signature->parameter_count > 2u || signature->parameters == NULL
        || signature->is_variadic
        || hir_body->parameter_count != signature->parameter_count
        || hir_body->local_count < signature->parameter_count
        || hir_body->locals == NULL
        || !cm_mir_lower_type(hir, item, substitutions,
            substitution_count, signature->return_type, &return_type)
        || !cm_mir_lower_type_target_valid(context, hir, item,
            return_type, 0u)
        || cm_mir_lower_type_is_bool(hir, return_type)) {
        cm_mir_lower_fail(&result, CM_MIR_LOWER_UNSUPPORTED_TYPE, body_id,
            hir_body->root_expression, CM_MIR_OK,
            "exact MIR lowering supports one or two checked arguments "
            "and result");
        return result;
    }
    for (parameter_index = 0u;
         parameter_index < signature->parameter_count; ++parameter_index) {
        if (hir_body->locals[parameter_index].parameter_index
                != parameter_index
            || !cm_mir_lower_type(hir, item, substitutions,
                substitution_count,
                signature->parameters[parameter_index].type,
                &argument_types[parameter_index])
            || !cm_mir_lower_type_target_valid(context, hir, item,
                argument_types[parameter_index], 0u)
            || cm_mir_lower_type_is_bool(hir,
                argument_types[parameter_index])
            || !cm_mir_hir_type_equal(hir,
                hir_body->locals[parameter_index].type,
                signature->parameters[parameter_index].type)) {
            cm_mir_lower_fail(&result, CM_MIR_LOWER_UNSUPPORTED_TYPE,
                body_id, hir_body->root_expression, CM_MIR_OK,
                "exact MIR parameter locals do not match the u32 signature");
            return result;
        }
    }
    for (local_index = signature->parameter_count;
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
            != hir_body->local_count - signature->parameter_count
        || terminal_id == CM_HIR_EXPR_NONE) {
        cm_mir_lower_fail(&result, CM_MIR_LOWER_INVALID_HIR, body_id,
            hir_body->root_expression, CM_MIR_OK,
            "typed HIR block locals and statements do not correspond");
        return result;
    }

    memset(&body, 0, sizeof(body));
    body.instance.definition = item->definition;
    body.instance.substitutions = (CmHirTypeId *)substitutions;
    body.instance.substitution_count = substitution_count;
    body.owner = item->definition;
    body.source_body = body_id;
    body.semantic_evidence = semantic_evidence;

    memset(&plan, 0, sizeof(plan));
    plan.context = context;
    plan.hir = hir;
    plan.body = hir_body;
    plan.item = item;
    plan.instance = &body.instance;
    plan.admission = admission;
    plan.semantic_results = semantic_results;
    plan.expected_type = return_type;
    plan.allowed_if_expression = terminal_id;
    cm_vec_init(&plan.seen, sizeof(CmHirExprId));
    cm_vec_init(&plan.calls, sizeof(CmMirFlowCall));
    for (statement_index = 0u; statement_index < hir_statement_count;
         ++statement_index) {
        const CmHirStatement *hir_statement;

        hir_statement = &hir_statements[statement_index];
        local_index = signature->parameter_count + statement_index;
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

    if ((size_t)hir_body->local_count + 1u > (size_t)UINT32_MAX
        || (size_t)plan.temporary_count > (size_t)UINT32_MAX
            - ((size_t)hir_body->local_count + 1u)
        || plan.call_count == UINT32_MAX
        || plan.conditional_count
            > (UINT32_MAX - plan.call_count - 1u) / 3u) {
        cm_vec_destroy(&plan.calls);
        cm_mir_lower_fail(&result, CM_MIR_LOWER_INVALID_HIR, body_id,
            terminal_id, CM_MIR_ID_EXHAUSTED,
            "u32 statement and expression flow exceeds MIR storage");
        return result;
    }
    local_count = (size_t)hir_body->local_count + 1u
        + (size_t)plan.temporary_count;
    planned_block_count = (size_t)plan.call_count + 1u
        + (size_t)plan.conditional_count * 3u;

    cm_vec_init(&flow_locals, sizeof(CmMirLocal));
    cm_vec_init(&flow_statements, sizeof(CmMirStatement));
    cm_vec_init(&flow_blocks, sizeof(CmMirBasicBlock));
    cm_vec_init(&flow_block_starts, sizeof(size_t));
    cm_vec_init(&flow_arguments, sizeof(CmMirOperand));
    cm_vec_init(&flow_aggregate_fields, sizeof(CmMirAggregateField));
    cm_vec_init(&flow_projections, sizeof(CmMirFieldProjection));
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
    for (local_index = 0u; local_index < hir_body->local_count;
         ++local_index) {
        memset(&local, 0, sizeof(local));
        local.kind = local_index < signature->parameter_count
            ? CM_MIR_LOCAL_ARGUMENT : CM_MIR_LOCAL_USER;
        if (!cm_mir_lower_type(hir, item, substitutions,
                substitution_count, hir_body->locals[local_index].type,
                &local.type)) {
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
                "u32 local types changed during MIR lowering");
            return result;
        }
        (void)cm_vec_push(&flow_locals, &local);
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
    lowering_ok = cm_mir_flow_next_block(&output);
    statement_index = 0u;
    if (lowering_ok) {
        for (statement_index = 0u;
             statement_index < hir_statement_count; ++statement_index) {
            const CmHirStatement *hir_statement;

            hir_statement = &hir_statements[statement_index];
            if (!cm_mir_flow_expression(&output,
                    hir_statement->data.let_statement.initializer, 1,
                    hir_statement->data.let_statement.local_index + 1u,
                    &root_operand)) {
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
    status = admission == NULL
        ? cm_mir_add_monomorphized_body(context, hir, &body, &result.body)
        : cm_mir_add_admitted_monomorphized_body(context, admission, &body,
            &result.body);
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
    return cm_mir_lower_instance_impl(context, hir, body_id, substitutions,
        substitution_count, NULL, NULL, CM_MIR_SEMANTIC_EVIDENCE_NONE);
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
            || context->admitted_rewind_generation != UINT64_C(0)) {
            return NULL;
        }
    } else if (context->hir_owner != hir
        || context->admitted_crate != crate_id
        || context->admitted_storage_lifetime_id != hir->storage.lifetime_id
        || context->admitted_semantic_generation != hir->semantic_generation
        || context->admitted_rewind_generation != hir->rewind_generation) {
        return NULL;
    }
    *out_crate = crate_id;
    return hir;
}

static void cm_mir_lower_latch_admission(CmMirContext *context,
    const CmHirContext *hir, CmHirCrateId crate_id)
{
    context->hir_owner = hir;
    context->admitted_crate = crate_id;
    context->admitted_storage_lifetime_id = hir->storage.lifetime_id;
    context->admitted_semantic_generation = hir->semantic_generation;
    context->admitted_rewind_generation = hir->rewind_generation;
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
            &semantic_body) != CM_SEMANTIC_RESULTS_OK) {
        return cm_mir_lower_admission_failure(body_id);
    }
    result = cm_mir_lower_instance_impl(context, hir, body_id, NULL, 0u,
        admission, semantic_results, CM_MIR_SEMANTIC_EVIDENCE_BODY);
    if (result.error_count == 0u
        && context->admitted_crate == CM_HIR_CRATE_NONE) {
        cm_mir_lower_latch_admission(context, hir, crate_id);
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
        if (cm_semantic_results_instance_body(semantic_results, admission,
                &spec, &semantic_body) != CM_SEMANTIC_RESULTS_OK
            || semantic_body.body != body_id
            || !cm_hir_def_id_equal(semantic_body.owner, body->owner)) {
            cm_free(arguments);
            return cm_mir_lower_admission_failure(body_id);
        }
    }
    cm_free(arguments);
    result = cm_mir_lower_instance_impl(context, hir, body_id,
        substitutions, substitution_count, admission, semantic_results,
        CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE);
    if (result.error_count == 0u
        && context->admitted_crate == CM_HIR_CRATE_NONE) {
        cm_mir_lower_latch_admission(context, hir, crate_id);
    }
    return result;
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
