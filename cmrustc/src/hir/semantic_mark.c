#include "cm/hir/semantic_mark.h"

#include "cm/alloc.h"
#include "cm/hir/admission.h"
#include "cm/hir/semantic_results.h"

#include <string.h>

typedef struct CmSemanticMarkScratch {
    CmHirContext *hir;
    unsigned char *visit;
    CmHirValueUsage *usage;
    const CmSemanticAdmission *admission;
    const CmSemanticResults *results;
    size_t body_index;
    CmHirBodyId body;
    CmSemanticMarkResult result;
} CmSemanticMarkScratch;

static int cm_semantic_mark_visit(CmSemanticMarkScratch *scratch,
    CmHirExprId expression_id, CmHirValueUsage usage, size_t depth);

static int cm_semantic_mark_selected_call(CmSemanticMarkScratch *scratch,
    const CmHirExpr *expression, CmHirExprId expression_id, size_t depth)
{
    CmSemanticCallableSelectionView selection;
    CmHirExprId argument_storage[2];
    const CmHirExprId *arguments;
    uint32_t argument_count;
    uint32_t index;

    memset(&selection, 0, sizeof(selection));
    arguments = NULL;
    argument_count = 0u;
    if (scratch->results == NULL || scratch->admission == NULL) return 0;
    if (expression->kind == CM_HIR_EXPR_QUALIFIED_CALL) {
        arguments = expression->data.qualified_call.arguments;
        argument_count = expression->data.qualified_call.argument_count;
        if ((argument_count == 0u) != (arguments == NULL)
            || expression->data.qualified_call.syntax
                != CM_HIR_CALLABLE_QUALIFIED_TRAIT_METHOD) return 0;
    } else if (expression->kind == CM_HIR_EXPR_METHOD_CALL) {
        if (expression->data.method_call.receiver == CM_HIR_EXPR_NONE
            || expression->data.method_call.argument_count > 1u
            || (expression->data.method_call.argument_count != 0u
                && expression->data.method_call.arguments == NULL)
            || expression->data.method_call.syntax
                != CM_HIR_CALLABLE_DOT_METHOD) return 0;
        argument_storage[0] = expression->data.method_call.receiver;
        for (index = 0u;
             index < expression->data.method_call.argument_count; ++index) {
            argument_storage[index + 1u] =
                expression->data.method_call.arguments[index];
        }
        arguments = argument_storage;
        argument_count = expression->data.method_call.argument_count + 1u;
    } else {
        return 0;
    }
    if (cm_semantic_results_callable_selection(scratch->results,
            scratch->admission, scratch->body, expression_id, &selection)
            != CM_SEMANTIC_RESULTS_OK
        || selection.syntax != (expression->kind
                == CM_HIR_EXPR_QUALIFIED_CALL
            ? expression->data.qualified_call.syntax
            : expression->data.method_call.syntax)
        || selection.argument_count != argument_count
        || cm_hir_def_id_is_none(selection.selected_impl)
        || cm_hir_def_id_is_none(selection.selected_callable)
        || cm_hir_def_id_is_none(selection.body_definition)) return 0;
    if (expression->kind == CM_HIR_EXPR_QUALIFIED_CALL
        && (!cm_hir_def_id_equal(selection.requested_trait,
                expression->data.qualified_call.requested_trait)
            || !cm_hir_def_id_equal(selection.declared_trait_callable,
                expression->data.qualified_call.declared_trait_callable)
            || selection.receiver_argument
                != expression->data.qualified_call.receiver_argument)) {
        return 0;
    }
    for (index = 0u; index < argument_count; ++index) {
        CmHirExprId retained;

        retained = CM_HIR_EXPR_NONE;
        if (cm_semantic_results_callable_argument(scratch->results,
                scratch->admission, scratch->body, expression_id, index,
                &retained) != CM_SEMANTIC_RESULTS_OK
            || retained != arguments[index]
            || !cm_semantic_mark_visit(scratch, arguments[index],
                CM_HIR_USAGE_MOVE, depth + 1u)) return 0;
    }
    return 1;
}

static CmSemanticMarkResult cm_semantic_mark_result(
    CmSemanticMarkStatus status)
{
    CmSemanticMarkResult result;

    memset(&result, 0, sizeof(result));
    result.status = status;
    result.body_index = CM_SEMANTIC_MARK_BODY_INDEX_NONE;
    result.body = CM_HIR_BODY_NONE;
    result.expression = CM_HIR_EXPR_NONE;
    return result;
}

static int cm_semantic_mark_builtin_copy(const CmHirContext *hir,
    CmHirTypeId type_id)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, type_id);
    if (type == NULL) return 0;
    switch (type->kind) {
    case CM_HIR_TYPE_NEVER_KIND:
    case CM_HIR_TYPE_UNIT_KIND:
    case CM_HIR_TYPE_BOOL_KIND:
    case CM_HIR_TYPE_CHAR_KIND:
    case CM_HIR_TYPE_INTEGER_KIND:
    case CM_HIR_TYPE_FLOAT_KIND:
    case CM_HIR_TYPE_RAW_POINTER_KIND:
    case CM_HIR_TYPE_FN_POINTER_KIND:
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
        return 1;
    case CM_HIR_TYPE_REFERENCE_KIND:
        return type->data.reference_type.mutability == CM_HIR_IMMUTABLE;
    default:
        return 0;
    }
}

static int cm_semantic_mark_visit(CmSemanticMarkScratch *scratch,
    CmHirExprId expression_id, CmHirValueUsage usage, size_t depth)
{
    const CmHirExpr *expression;
    size_t slot;
    uint32_t index;

    if (expression_id == CM_HIR_EXPR_NONE
        || (size_t)expression_id > scratch->hir->expressions.len
        || depth > scratch->hir->expressions.len) {
        scratch->result.status = CM_SEMANTIC_MARK_INVALID_HIR;
        scratch->result.expression = expression_id;
        return 0;
    }
    slot = (size_t)expression_id - 1u;
    expression = cm_hir_get_expr(scratch->hir, expression_id);
    if (expression == NULL || expression->owner_body != scratch->body
        || cm_hir_get_type(scratch->hir, expression->type) == NULL
        || scratch->visit[slot] != 0u
        || expression->usage != CM_HIR_USAGE_UNKNOWN
        || expression->static_borrow_state
            != CM_HIR_STATIC_BORROW_UNKNOWN) {
        scratch->result.status = CM_SEMANTIC_MARK_INVALID_HIR;
        scratch->result.expression = expression_id;
        return 0;
    }
    scratch->visit[slot] = 1u;
    scratch->usage[slot] = usage;
    switch (expression->kind) {
    case CM_HIR_EXPR_INTEGER:
    case CM_HIR_EXPR_LOCAL:
        break;
    case CM_HIR_EXPR_BLOCK:
        if ((expression->data.block.statement_count == 0u)
                != (expression->data.block.statements == NULL)) {
            scratch->result.status = CM_SEMANTIC_MARK_INVALID_HIR;
            scratch->result.expression = expression_id;
            return 0;
        }
        for (index = 0u; index < expression->data.block.statement_count;
             ++index) {
            const CmHirStatement *statement;
            const CmHirBody *body;
            uint32_t local_index;
            CmHirValueUsage initializer_usage;

            statement = &expression->data.block.statements[index];
            body = cm_hir_get_body(scratch->hir, scratch->body);
            local_index = statement->data.let_statement.local_index;
            if (statement->kind != CM_HIR_STATEMENT_LET || body == NULL
                || local_index >= body->local_count) {
                scratch->result.status = CM_SEMANTIC_MARK_INVALID_HIR;
                scratch->result.expression = expression_id;
                return 0;
            }
            initializer_usage = cm_semantic_mark_builtin_copy(scratch->hir,
                    body->locals[local_index].type)
                ? CM_HIR_USAGE_BORROW : CM_HIR_USAGE_MOVE;
            if (!cm_semantic_mark_visit(scratch,
                    statement->data.let_statement.initializer,
                    initializer_usage, depth + 1u)) return 0;
        }
        if (!cm_semantic_mark_visit(scratch,
                expression->data.block.tail_expression,
                CM_HIR_USAGE_MOVE, depth + 1u)) return 0;
        break;
    case CM_HIR_EXPR_CALL:
        if ((expression->data.call.type_substitution_count != 0u
                && expression->data.call.type_substitutions == NULL)
            || (expression->data.call.argument_count == 0u)
                != (expression->data.call.arguments == NULL)) {
            scratch->result.status = CM_SEMANTIC_MARK_INVALID_HIR;
            scratch->result.expression = expression_id;
            return 0;
        }
        for (index = 0u;
             index < expression->data.call.type_substitution_count;
             ++index) {
            if (cm_hir_get_type(scratch->hir,
                    expression->data.call.type_substitutions[index])
                    == NULL) {
                scratch->result.status = CM_SEMANTIC_MARK_INVALID_HIR;
                scratch->result.expression = expression_id;
                return 0;
            }
        }
        for (index = 0u; index < expression->data.call.argument_count;
             ++index) {
            if (!cm_semantic_mark_visit(scratch,
                    expression->data.call.arguments[index],
                    CM_HIR_USAGE_MOVE, depth + 1u)) return 0;
        }
        break;
    case CM_HIR_EXPR_BINARY:
    {
        CmHirValueUsage operand_usage;

        if (expression->data.binary.operator_kind == CM_HIR_BINARY_ADD
            || expression->data.binary.operator_kind
                == CM_HIR_BINARY_SUBTRACT) {
            operand_usage = CM_HIR_USAGE_MOVE;
        } else if (expression->data.binary.operator_kind
                == CM_HIR_BINARY_EQUAL
            || expression->data.binary.operator_kind
                == CM_HIR_BINARY_LESS) {
            operand_usage = CM_HIR_USAGE_BORROW;
        } else {
            scratch->result.status = CM_SEMANTIC_MARK_INVALID_HIR;
            scratch->result.expression = expression_id;
            return 0;
        }
        if (!cm_semantic_mark_visit(scratch,
                expression->data.binary.left, operand_usage, depth + 1u)
            || !cm_semantic_mark_visit(scratch,
                expression->data.binary.right, operand_usage,
                depth + 1u)) return 0;
        break;
    }
    case CM_HIR_EXPR_AGGREGATE:
        if ((expression->data.aggregate.field_count == 0u)
                != (expression->data.aggregate.fields == NULL)) {
            scratch->result.status = CM_SEMANTIC_MARK_INVALID_HIR;
            scratch->result.expression = expression_id;
            return 0;
        }
        for (index = 0u; index < expression->data.aggregate.field_count;
             ++index) {
            if (!cm_semantic_mark_visit(scratch,
                    expression->data.aggregate.fields[index].value,
                    CM_HIR_USAGE_MOVE, depth + 1u)) return 0;
        }
        break;
    case CM_HIR_EXPR_FIELD:
        if (!cm_semantic_mark_visit(scratch, expression->data.field.base,
                usage == CM_HIR_USAGE_MOVE
                        && cm_semantic_mark_builtin_copy(scratch->hir,
                            expression->type)
                    ? CM_HIR_USAGE_BORROW : usage,
                depth + 1u)) return 0;
        break;
    case CM_HIR_EXPR_IF:
        if (!cm_semantic_mark_visit(scratch,
                expression->data.if_expr.condition, CM_HIR_USAGE_BORROW,
                depth + 1u)
            || !cm_semantic_mark_visit(scratch,
                expression->data.if_expr.then_expression,
                CM_HIR_USAGE_MOVE, depth + 1u)
            || !cm_semantic_mark_visit(scratch,
                expression->data.if_expr.else_expression,
                CM_HIR_USAGE_MOVE, depth + 1u)) return 0;
        break;
    case CM_HIR_EXPR_METHOD_CALL:
    case CM_HIR_EXPR_QUALIFIED_CALL:
        if (!cm_semantic_mark_selected_call(scratch, expression,
                expression_id, depth)) {
            scratch->result.status = scratch->results == NULL
                ? CM_SEMANTIC_MARK_UNSUPPORTED_EXPRESSION
                : CM_SEMANTIC_MARK_INVALID_HIR;
            scratch->result.expression = expression_id;
            return 0;
        }
        break;
    case CM_HIR_EXPR_BORROW_SHARED:
    case CM_HIR_EXPR_DEREFERENCE:
        scratch->result.status =
            CM_SEMANTIC_MARK_UNSUPPORTED_EXPRESSION;
        scratch->result.expression = expression_id;
        return 0;
    default:
        scratch->result.status = CM_SEMANTIC_MARK_INVALID_HIR;
        scratch->result.expression = expression_id;
        return 0;
    }
    scratch->visit[slot] = 2u;
    return 1;
}

static CmSemanticMarkResult cm_hir_semantic_mark_bodies_impl(
    CmHirContext *hir, const CmHirBodyId *bodies, size_t body_count,
    const CmSemanticAdmission *admission)
{
    CmSemanticMarkScratch scratch;
    size_t expression_count;
    size_t index;
    size_t body_index;

    if (hir == NULL || (body_count != 0u && bodies == NULL))
        return cm_semantic_mark_result(CM_SEMANTIC_MARK_INVALID_ARGUMENT);
    memset(&scratch, 0, sizeof(scratch));
    scratch.hir = hir;
    scratch.admission = admission;
    scratch.results = admission == NULL ? NULL
        : cm_semantic_admission_results(admission);
    if (admission != NULL && (scratch.results == NULL
            || cm_semantic_admission_hir(admission) != hir)) {
        return cm_semantic_mark_result(CM_SEMANTIC_MARK_INVALID_ARGUMENT);
    }
    scratch.result = cm_semantic_mark_result(CM_SEMANTIC_MARK_OK);
    expression_count = hir->expressions.len;
    scratch.visit = (unsigned char *)cm_alloc_zeroed(
        expression_count == 0u ? 1u : expression_count,
        sizeof(*scratch.visit));
    scratch.usage = (CmHirValueUsage *)cm_alloc_zeroed(
        expression_count == 0u ? 1u : expression_count,
        sizeof(*scratch.usage));
    for (body_index = 0u; body_index < body_count; ++body_index) {
        const CmHirBody *body;
        size_t prior;

        scratch.body_index = body_index;
        scratch.body = bodies[body_index];
        scratch.result.body_index = body_index;
        scratch.result.body = scratch.body;
        for (prior = 0u; prior < body_index; ++prior) {
            if (bodies[prior] == scratch.body) {
                scratch.result.status = CM_SEMANTIC_MARK_INVALID_HIR;
                goto done;
            }
        }
        body = cm_hir_get_body(hir, scratch.body);
        if (body == NULL || body->state != CM_HIR_BODY_TYPED
            || body->root_expression == CM_HIR_EXPR_NONE
            || !cm_semantic_mark_visit(&scratch, body->root_expression,
                CM_HIR_USAGE_MOVE, 0u)) {
            if (scratch.result.status == CM_SEMANTIC_MARK_OK)
                scratch.result.status = CM_SEMANTIC_MARK_INVALID_HIR;
            goto done;
        }
    }
    for (index = 0u; index < expression_count; ++index) {
        const CmHirExpr *expression;
        int belongs_to_manifest;

        expression = (const CmHirExpr *)cm_vec_at_const(
            &hir->expressions, index);
        belongs_to_manifest = 0;
        for (body_index = 0u; body_index < body_count; ++body_index) {
            if (expression->owner_body == bodies[body_index]) {
                belongs_to_manifest = 1;
                break;
            }
        }
        if (belongs_to_manifest != (scratch.visit[index] == 2u)) {
            scratch.result.status = CM_SEMANTIC_MARK_INVALID_HIR;
            scratch.result.body_index = belongs_to_manifest
                ? body_index : CM_SEMANTIC_MARK_BODY_INDEX_NONE;
            scratch.result.body = belongs_to_manifest
                ? expression->owner_body : CM_HIR_BODY_NONE;
            scratch.result.expression = (CmHirExprId)(index + 1u);
            goto done;
        }
    }
    for (index = 0u; index < expression_count; ++index) {
        if (scratch.visit[index] == 2u) {
            CmHirExpr *expression;

            expression = (CmHirExpr *)cm_vec_at(&hir->expressions, index);
            expression->usage = scratch.usage[index];
            expression->static_borrow_state =
                CM_HIR_STATIC_BORROW_NOT_PROMOTED;
        }
    }
    cm_hir_context_record_semantic_mutation(hir);
    scratch.result.body_index = CM_SEMANTIC_MARK_BODY_INDEX_NONE;
    scratch.result.body = CM_HIR_BODY_NONE;
    scratch.result.expression = CM_HIR_EXPR_NONE;

done:
    cm_free(scratch.usage);
    cm_free(scratch.visit);
    return scratch.result;
}

CmSemanticMarkResult cm_hir_semantic_mark_bodies(CmHirContext *hir,
    const CmHirBodyId *bodies, size_t body_count)
{
    return cm_hir_semantic_mark_bodies_impl(hir, bodies, body_count, NULL);
}

CmSemanticMarkResult cm_hir_semantic_mark_admitted_bodies(
    CmHirContext *hir, const CmHirBodyId *bodies, size_t body_count,
    const CmSemanticAdmission *admission)
{
    if (admission == NULL) {
        return cm_semantic_mark_result(CM_SEMANTIC_MARK_INVALID_ARGUMENT);
    }
    return cm_hir_semantic_mark_bodies_impl(hir, bodies, body_count,
        admission);
}

const char *cm_semantic_mark_status_name(CmSemanticMarkStatus status)
{
    switch (status) {
    case CM_SEMANTIC_MARK_OK: return "ok";
    case CM_SEMANTIC_MARK_INVALID_ARGUMENT: return "invalid argument";
    case CM_SEMANTIC_MARK_INVALID_HIR: return "invalid HIR";
    case CM_SEMANTIC_MARK_UNSUPPORTED_EXPRESSION:
        return "unsupported expression";
    }
    return "unknown";
}
