#include "cm/hir/semantic_mark.h"

#include "cm/alloc.h"
#include "cm/hir/admission.h"
#include "cm/hir/semantic_results.h"

#include <string.h>

typedef struct CmSemanticMarkClosureScratch {
    CmVec captures;
    CmHirClosureCapture *published_captures;
    CmHirClosureClass callable_class;
    int is_copy;
    int visited;
} CmSemanticMarkClosureScratch;

typedef struct CmSemanticMarkScratch {
    CmHirContext *hir;
    unsigned char *visit;
    CmHirValueUsage *usage;
    CmSemanticMarkClosureScratch *closures;
    size_t closure_count;
    const CmSemanticAdmission *admission;
    const CmSemanticResults *results;
    size_t body_index;
    CmHirBodyId body;
    CmSemanticMarkResult result;
} CmSemanticMarkScratch;

typedef enum CmSemanticMarkCopyEvidence {
    CM_SEMANTIC_MARK_COPY_NO = 0,
    CM_SEMANTIC_MARK_COPY_YES,
    CM_SEMANTIC_MARK_COPY_UNKNOWN
} CmSemanticMarkCopyEvidence;

static int cm_semantic_mark_visit(CmSemanticMarkScratch *scratch,
    CmHirExprId expression_id, CmHirValueUsage usage,
    CmHirClosureId active_closure, size_t depth);

static int cm_semantic_mark_selected_call(CmSemanticMarkScratch *scratch,
    const CmHirExpr *expression, CmHirExprId expression_id,
    CmHirClosureId active_closure, size_t depth)
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
            || (expression->data.method_call.argument_count == 0u)
                != (expression->data.method_call.arguments == NULL)
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
    if (expression->kind == CM_HIR_EXPR_METHOD_CALL
        && (selection.receiver_argument != 0u
            || selection.receiver_expression
                != expression->data.method_call.receiver)) return 0;
    for (index = 0u; index < argument_count; ++index) {
        CmHirExprId retained;
        CmHirValueUsage argument_usage;

        retained = CM_HIR_EXPR_NONE;
        argument_usage = CM_HIR_USAGE_MOVE;
        if (expression->kind == CM_HIR_EXPR_METHOD_CALL && index == 0u) {
            CmSemanticExpressionView receiver;

            memset(&receiver, 0, sizeof(receiver));
            if (cm_semantic_results_expression(scratch->results,
                    scratch->admission, scratch->body, arguments[index],
                    &receiver) != CM_SEMANTIC_RESULTS_OK
                || receiver.expression != arguments[index]
                || receiver.body != scratch->body
                || receiver.adjustment_count > 1u) return 0;
            if (receiver.adjustment_count == 1u) {
                CmSemanticAdjustmentView adjustment;

                memset(&adjustment, 0, sizeof(adjustment));
                if (cm_semantic_results_expression_adjustment(
                        scratch->results, scratch->admission, scratch->body,
                        arguments[index], 0u, &adjustment)
                        != CM_SEMANTIC_RESULTS_OK
                    || adjustment.expression != arguments[index]
                    || adjustment.body != scratch->body
                    || adjustment.index != 0u
                    || (adjustment.kind
                            != CM_SEMANTIC_ADJUSTMENT_BORROW_SHARED
                        && adjustment.kind
                            != CM_SEMANTIC_ADJUSTMENT_BORROW_MUTABLE)
                    || adjustment.has_selected_trait
                    || !cm_hir_def_id_is_none(adjustment.selected_trait)
                    || !cm_hir_def_id_is_none(adjustment.selected_method)
                    || !cm_hir_def_id_is_none(adjustment.selected_impl)) {
                    return 0;
                }
                if (adjustment.kind
                        == CM_SEMANTIC_ADJUSTMENT_BORROW_MUTABLE) {
                    const CmHirBody *body;
                    const CmHirExpr *receiver_expression;

                    body = cm_hir_get_body(scratch->hir, scratch->body);
                    receiver_expression = cm_hir_get_expr(scratch->hir,
                        arguments[index]);
                    if (body == NULL || receiver_expression == NULL
                        || receiver_expression->kind != CM_HIR_EXPR_LOCAL
                        || receiver_expression->data.local.local_index
                            >= body->local_count
                        || body->locals[receiver_expression->data.local
                                .local_index].mutability
                            != CM_HIR_MUTABLE) return 0;
                }
                argument_usage = CM_HIR_USAGE_BORROW;
            }
        }
        if (cm_semantic_results_callable_argument(scratch->results,
                scratch->admission, scratch->body, expression_id, index,
                &retained) != CM_SEMANTIC_RESULTS_OK
            || retained != arguments[index]
            || !cm_semantic_mark_visit(scratch, arguments[index],
                argument_usage, active_closure, depth + 1u)) return 0;
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

/*
 * Closure classification cannot equate "not a builtin" with "not Copy".
 * This structural subset returns UNKNOWN whenever Copy would require trait
 * or parameter-environment evidence which MARKED does not yet authenticate.
 */
static CmSemanticMarkCopyEvidence cm_semantic_mark_copy_evidence(
    const CmHirContext *hir, CmHirTypeId type_id, size_t depth)
{
    const CmHirType *type;
    uint32_t index;

    if (hir == NULL || depth > hir->types.len) {
        return CM_SEMANTIC_MARK_COPY_UNKNOWN;
    }
    type = cm_hir_get_type(hir, type_id);
    if (type == NULL) return CM_SEMANTIC_MARK_COPY_UNKNOWN;
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
        return CM_SEMANTIC_MARK_COPY_YES;
    case CM_HIR_TYPE_REFERENCE_KIND:
        return type->data.reference_type.mutability == CM_HIR_IMMUTABLE
            ? CM_SEMANTIC_MARK_COPY_YES : CM_SEMANTIC_MARK_COPY_NO;
    case CM_HIR_TYPE_TUPLE_KIND:
        for (index = 0u; index < type->data.tuple_type.element_count;
             ++index) {
            CmSemanticMarkCopyEvidence element;

            element = cm_semantic_mark_copy_evidence(hir,
                type->data.tuple_type.elements[index], depth + 1u);
            if (element != CM_SEMANTIC_MARK_COPY_YES) return element;
        }
        return CM_SEMANTIC_MARK_COPY_YES;
    case CM_HIR_TYPE_ARRAY_KIND:
        if (type->data.array_type.length.kind == CM_HIR_CONST_VALUE
            && type->data.array_type.length.data.value.low_bits == 0u
            && type->data.array_type.length.data.value.high_bits == 0u) {
            return CM_SEMANTIC_MARK_COPY_YES;
        }
        {
            CmSemanticMarkCopyEvidence element;

            element = cm_semantic_mark_copy_evidence(hir,
                type->data.array_type.element, depth + 1u);
            if (element == CM_SEMANTIC_MARK_COPY_YES) return element;
            if (type->data.array_type.length.kind != CM_HIR_CONST_VALUE) {
                return CM_SEMANTIC_MARK_COPY_UNKNOWN;
            }
            return element;
        }
    case CM_HIR_TYPE_STR_KIND:
    case CM_HIR_TYPE_SLICE_KIND:
    case CM_HIR_TYPE_DYN_TRAIT_KIND:
        return CM_SEMANTIC_MARK_COPY_NO;
    case CM_HIR_TYPE_ERROR_KIND:
    case CM_HIR_TYPE_INFER_KIND:
    case CM_HIR_TYPE_ADT_KIND:
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
    case CM_HIR_TYPE_SELF_KIND:
    case CM_HIR_TYPE_PARAMETER_KIND:
    case CM_HIR_TYPE_PROJECTION_KIND:
    case CM_HIR_TYPE_OPAQUE_KIND:
    case CM_HIR_TYPE_CLOSURE_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
        return CM_SEMANTIC_MARK_COPY_UNKNOWN;
    }
    return CM_SEMANTIC_MARK_COPY_UNKNOWN;
}

static CmHirClosureClass cm_semantic_mark_capture_class(
    CmHirValueUsage usage)
{
    if (usage == CM_HIR_USAGE_BORROW) return CM_HIR_CLOSURE_CLASS_SHARED;
    if (usage == CM_HIR_USAGE_MUTATE) return CM_HIR_CLOSURE_CLASS_MUT;
    if (usage == CM_HIR_USAGE_MOVE) return CM_HIR_CLOSURE_CLASS_ONCE;
    return CM_HIR_CLOSURE_CLASS_UNKNOWN;
}

static int cm_semantic_mark_capture(CmSemanticMarkScratch *scratch,
    CmHirClosureId closure_id, uint32_t local_index,
    CmHirValueUsage usage)
{
    const CmHirClosure *closure;
    const CmHirBody *body;
    const CmHirType *type;
    CmSemanticMarkClosureScratch *closure_scratch;
    CmHirClosureCapture capture;
    CmHirClosureClass callable_class;
    CmSemanticMarkCopyEvidence copy_evidence;
    size_t index;

    if (closure_id == CM_HIR_CLOSURE_NONE
        || (size_t)closure_id > scratch->closure_count
        || usage <= CM_HIR_USAGE_UNKNOWN || usage > CM_HIR_USAGE_MOVE) {
        return 0;
    }
    closure = cm_hir_get_closure(scratch->hir, closure_id);
    body = closure == NULL ? NULL
        : cm_hir_get_body(scratch->hir, closure->owner_body);
    if (closure == NULL || body == NULL || closure->owner_body != scratch->body
        || local_index >= body->local_count) return 0;
    /* Locals created inside this closure are not captures.  For a nested
     * closure, propagation below naturally stops at the parent closure which
     * owns such a local. */
    if (local_index >= closure->visible_local_count) return 1;
    type = cm_hir_get_type(scratch->hir, body->locals[local_index].type);
    if (type == NULL) return 0;
    if (usage == CM_HIR_USAGE_MOVE) {
        copy_evidence = cm_semantic_mark_copy_evidence(scratch->hir,
            body->locals[local_index].type, 0u);
        if (copy_evidence == CM_SEMANTIC_MARK_COPY_YES) {
            usage = CM_HIR_USAGE_BORROW;
        } else if (type->kind == CM_HIR_TYPE_REFERENCE_KIND
            && type->data.reference_type.mutability == CM_HIR_MUTABLE) {
            usage = CM_HIR_USAGE_MUTATE;
        } else if (copy_evidence == CM_SEMANTIC_MARK_COPY_UNKNOWN) {
            scratch->result.status =
                CM_SEMANTIC_MARK_UNSUPPORTED_EXPRESSION;
            return 0;
        }
    }
    closure_scratch = &scratch->closures[(size_t)closure_id - 1u];
    callable_class = cm_semantic_mark_capture_class(usage);
    if (callable_class == CM_HIR_CLOSURE_CLASS_UNKNOWN) return 0;
    if (closure_scratch->callable_class < callable_class)
        closure_scratch->callable_class = callable_class;
    for (index = 0u; index < closure_scratch->captures.len; ++index) {
        CmHirClosureCapture *stored;

        stored = (CmHirClosureCapture *)cm_vec_at(
            &closure_scratch->captures, index);
        if (stored->local_index == local_index) {
            if (stored->type != body->locals[local_index].type) return 0;
            if (stored->usage < usage) stored->usage = usage;
            return 1;
        }
        if (stored->local_index > local_index) break;
    }
    capture.local_index = local_index;
    capture.type = body->locals[local_index].type;
    capture.usage = usage;
    (void)cm_vec_push(&closure_scratch->captures, &capture);
    index = closure_scratch->captures.len - 1u;
    while (index != 0u) {
        CmHirClosureCapture *left;
        CmHirClosureCapture *right;
        CmHirClosureCapture swap;

        left = (CmHirClosureCapture *)cm_vec_at(&closure_scratch->captures,
            index - 1u);
        right = (CmHirClosureCapture *)cm_vec_at(&closure_scratch->captures,
            index);
        if (left->local_index < right->local_index) break;
        swap = *left;
        *left = *right;
        *right = swap;
        --index;
    }
    return 1;
}

static int cm_semantic_mark_finalize_closure(
    CmSemanticMarkScratch *scratch, CmHirClosureId closure_id,
    CmHirClosureId enclosing_closure)
{
    const CmHirClosure *closure;
    CmSemanticMarkClosureScratch *closure_scratch;
    size_t index;

    closure = cm_hir_get_closure(scratch->hir, closure_id);
    if (closure == NULL || (size_t)closure_id > scratch->closure_count) {
        return 0;
    }
    closure_scratch = &scratch->closures[(size_t)closure_id - 1u];
    if (closure_scratch->callable_class == CM_HIR_CLOSURE_CLASS_UNKNOWN) {
        closure_scratch->callable_class = CM_HIR_CLOSURE_CLASS_NO_CAPTURE;
    }
    /* Original mrustc first derives Fn/FnMut/FnOnce from use, then applies
     * the move-capture override without changing that callable class. */
    if (closure->is_move) {
        for (index = 0u; index < closure_scratch->captures.len; ++index) {
            CmHirClosureCapture *capture;

            capture = (CmHirClosureCapture *)cm_vec_at(
                &closure_scratch->captures, index);
            capture->usage = CM_HIR_USAGE_MOVE;
        }
    } else {
        for (index = 0u; index < closure_scratch->captures.len; ++index) {
            CmHirClosureCapture *capture;
            const CmHirType *type;

            capture = (CmHirClosureCapture *)cm_vec_at(
                &closure_scratch->captures, index);
            type = cm_hir_get_type(scratch->hir, capture->type);
            if (capture->usage == CM_HIR_USAGE_BORROW && type != NULL
                && type->kind == CM_HIR_TYPE_REFERENCE_KIND
                && type->data.reference_type.mutability
                    == CM_HIR_IMMUTABLE) {
                capture->usage = CM_HIR_USAGE_MOVE;
            }
        }
    }
    closure_scratch->is_copy = 1;
    for (index = 0u; index < closure_scratch->captures.len; ++index) {
        const CmHirClosureCapture *capture;
        CmSemanticMarkCopyEvidence copy_evidence;

        capture = (const CmHirClosureCapture *)cm_vec_at_const(
            &closure_scratch->captures, index);
        copy_evidence = capture->usage == CM_HIR_USAGE_MOVE
            ? cm_semantic_mark_copy_evidence(scratch->hir,
                capture->type, 0u)
            : CM_SEMANTIC_MARK_COPY_YES;
        if (copy_evidence == CM_SEMANTIC_MARK_COPY_UNKNOWN) {
            scratch->result.status =
                CM_SEMANTIC_MARK_UNSUPPORTED_EXPRESSION;
            return 0;
        }
        if (capture->usage == CM_HIR_USAGE_MUTATE
            || (capture->usage == CM_HIR_USAGE_MOVE
                && copy_evidence == CM_SEMANTIC_MARK_COPY_NO)) {
            closure_scratch->is_copy = 0;
        }
        if (enclosing_closure != CM_HIR_CLOSURE_NONE
            && !cm_semantic_mark_capture(scratch, enclosing_closure,
                capture->local_index, capture->usage)) return 0;
    }
    return 1;
}

static int cm_semantic_mark_visit(CmSemanticMarkScratch *scratch,
    CmHirExprId expression_id, CmHirValueUsage usage,
    CmHirClosureId active_closure, size_t depth)
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
        break;
    case CM_HIR_EXPR_LOCAL:
    {
        const CmHirBody *body;

        body = cm_hir_get_body(scratch->hir, scratch->body);
        if (body == NULL
            || expression->data.local.local_index >= body->local_count) {
            scratch->result.status = CM_SEMANTIC_MARK_INVALID_HIR;
            scratch->result.expression = expression_id;
            return 0;
        }
        if (active_closure != CM_HIR_CLOSURE_NONE
            && !cm_semantic_mark_capture(scratch, active_closure,
                expression->data.local.local_index, usage)) {
            if (scratch->result.status == CM_SEMANTIC_MARK_OK) {
                scratch->result.status = CM_SEMANTIC_MARK_INVALID_HIR;
            }
            scratch->result.expression = expression_id;
            return 0;
        }
        break;
    }
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
                    initializer_usage, active_closure,
                    depth + 1u)) return 0;
        }
        if (!cm_semantic_mark_visit(scratch,
                expression->data.block.tail_expression,
                CM_HIR_USAGE_MOVE, active_closure, depth + 1u)) return 0;
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
                    CM_HIR_USAGE_MOVE, active_closure,
                    depth + 1u)) return 0;
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
                expression->data.binary.left, operand_usage,
                active_closure, depth + 1u)
            || !cm_semantic_mark_visit(scratch,
                expression->data.binary.right, operand_usage,
                active_closure, depth + 1u)) return 0;
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
                    CM_HIR_USAGE_MOVE, active_closure,
                    depth + 1u)) return 0;
        }
        break;
    case CM_HIR_EXPR_FIELD:
        if (active_closure != CM_HIR_CLOSURE_NONE) {
            scratch->result.status =
                CM_SEMANTIC_MARK_UNSUPPORTED_EXPRESSION;
            scratch->result.expression = expression_id;
            return 0;
        }
        if (!cm_semantic_mark_visit(scratch, expression->data.field.base,
                usage == CM_HIR_USAGE_MOVE
                        && cm_semantic_mark_builtin_copy(scratch->hir,
                            expression->type)
                    ? CM_HIR_USAGE_BORROW : usage,
                active_closure, depth + 1u)) return 0;
        break;
    case CM_HIR_EXPR_IF:
        if (!cm_semantic_mark_visit(scratch,
                expression->data.if_expr.condition, CM_HIR_USAGE_BORROW,
                active_closure, depth + 1u)
            || !cm_semantic_mark_visit(scratch,
                expression->data.if_expr.then_expression,
                CM_HIR_USAGE_MOVE, active_closure, depth + 1u)
            || !cm_semantic_mark_visit(scratch,
                expression->data.if_expr.else_expression,
                CM_HIR_USAGE_MOVE, active_closure,
                depth + 1u)) return 0;
        break;
    case CM_HIR_EXPR_METHOD_CALL:
    case CM_HIR_EXPR_QUALIFIED_CALL:
        if (!cm_semantic_mark_selected_call(scratch, expression,
                expression_id, active_closure, depth)) {
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
    case CM_HIR_EXPR_CLOSURE_PARAMETER:
    {
        const CmHirClosure *closure;
        const CmHirClosureParam *parameter;

        closure = cm_hir_get_closure(scratch->hir, active_closure);
        parameter = closure != NULL
                && expression->data.closure_parameter.parameter_index
                    < closure->parameter_count
            ? &closure->parameters[
                expression->data.closure_parameter.parameter_index]
            : NULL;
        if (active_closure == CM_HIR_CLOSURE_NONE || closure == NULL
            || parameter == NULL
            || expression->data.closure_parameter.closure
                != active_closure
            || closure->owner_body != scratch->body
            || closure->state != CM_HIR_CLOSURE_BODY_BOUND
            || parameter->binding_kind != CM_HIR_BINDING_NAMED
            || parameter->type != expression->type) {
            scratch->result.status = CM_SEMANTIC_MARK_INVALID_HIR;
            scratch->result.expression = expression_id;
            return 0;
        }
        break;
    }
    case CM_HIR_EXPR_CLOSURE:
    {
        const CmHirClosure *closure;
        const CmHirType *type;
        CmHirClosureId closure_id;
        CmSemanticMarkClosureScratch *closure_scratch;

        closure_id = expression->data.closure.closure;
        closure = cm_hir_get_closure(scratch->hir, closure_id);
        type = cm_hir_get_type(scratch->hir, expression->type);
        closure_scratch = closure_id == CM_HIR_CLOSURE_NONE
                || (size_t)closure_id > scratch->closure_count
            ? NULL : &scratch->closures[(size_t)closure_id - 1u];
        if (closure == NULL || closure_scratch == NULL
            || closure_scratch->visited
            || closure->owner_body != scratch->body
            || closure->state != CM_HIR_CLOSURE_BODY_BOUND
            || closure->body_expression == CM_HIR_EXPR_NONE
            || closure->capture_state
                != CM_HIR_CLOSURE_CAPTURES_UNMARKED
            || closure->captures != NULL || closure->capture_count != 0u
            || closure->callable_class != CM_HIR_CLOSURE_CLASS_UNKNOWN
            || closure->is_copy != 0
            || type == NULL || type->kind != CM_HIR_TYPE_CLOSURE_KIND
            || type->data.closure_type.closure != closure_id) {
            scratch->result.status = CM_SEMANTIC_MARK_INVALID_HIR;
            scratch->result.expression = expression_id;
            return 0;
        }
        closure_scratch->visited = 1;
        if (!cm_semantic_mark_visit(scratch, closure->body_expression,
                CM_HIR_USAGE_MOVE, closure_id, depth + 1u)
            || !cm_semantic_mark_finalize_closure(scratch, closure_id,
                active_closure)) return 0;
        break;
    }
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
    size_t closure_count;
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
    closure_count = hir->closures.len;
    scratch.closure_count = closure_count;
    scratch.visit = (unsigned char *)cm_alloc_zeroed(
        expression_count == 0u ? 1u : expression_count,
        sizeof(*scratch.visit));
    scratch.usage = (CmHirValueUsage *)cm_alloc_zeroed(
        expression_count == 0u ? 1u : expression_count,
        sizeof(*scratch.usage));
    scratch.closures = (CmSemanticMarkClosureScratch *)cm_alloc_zeroed(
        closure_count == 0u ? 1u : closure_count,
        sizeof(*scratch.closures));
    for (index = 0u; index < closure_count; ++index) {
        cm_vec_init(&scratch.closures[index].captures,
            sizeof(CmHirClosureCapture));
    }
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
                CM_HIR_USAGE_MOVE, CM_HIR_CLOSURE_NONE, 0u)) {
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
    for (index = 0u; index < closure_count; ++index) {
        const CmHirClosure *closure;
        int belongs_to_manifest;
        size_t capture_bytes;

        closure = (const CmHirClosure *)cm_vec_at_const(
            &hir->closures, index);
        belongs_to_manifest = 0;
        for (body_index = 0u; body_index < body_count; ++body_index) {
            if (closure != NULL
                && closure->owner_body == bodies[body_index]) {
                belongs_to_manifest = 1;
                break;
            }
        }
        if (closure == NULL
            || (belongs_to_manifest
                && (closure->state != CM_HIR_CLOSURE_BODY_BOUND
                    || closure->capture_state
                        != CM_HIR_CLOSURE_CAPTURES_UNMARKED
                    || closure->captures != NULL
                    || closure->capture_count != 0u
                    || closure->callable_class
                        != CM_HIR_CLOSURE_CLASS_UNKNOWN
                    || closure->is_copy != 0))
            || belongs_to_manifest != scratch.closures[index].visited) {
            scratch.result.status = CM_SEMANTIC_MARK_INVALID_HIR;
            scratch.result.body_index = belongs_to_manifest
                ? body_index : CM_SEMANTIC_MARK_BODY_INDEX_NONE;
            scratch.result.body = belongs_to_manifest
                ? closure->owner_body : CM_HIR_BODY_NONE;
            scratch.result.expression = CM_HIR_EXPR_NONE;
            goto done;
        }
        capture_bytes = 0u;
        if (belongs_to_manifest
            && (scratch.closures[index].captures.len > (size_t)UINT32_MAX
                || !cm_size_mul(scratch.closures[index].captures.len,
                    sizeof(CmHirClosureCapture), &capture_bytes))) {
            scratch.result.status = CM_SEMANTIC_MARK_INVALID_HIR;
            scratch.result.body_index = body_index;
            scratch.result.body = closure->owner_body;
            scratch.result.expression = CM_HIR_EXPR_NONE;
            goto done;
        }
    }
    /* No logical failure is possible beyond this point.  Allocate every
     * deep capture array first, then publish closure and expression evidence
     * as one semantic-generation mutation. */
    for (index = 0u; index < closure_count; ++index) {
        CmSemanticMarkClosureScratch *closure_scratch;
        size_t byte_count;

        closure_scratch = &scratch.closures[index];
        if (!closure_scratch->visited || closure_scratch->captures.len == 0u)
            continue;
        (void)cm_size_mul(closure_scratch->captures.len,
            sizeof(CmHirClosureCapture), &byte_count);
        closure_scratch->published_captures =
            (CmHirClosureCapture *)cm_arena_alloc(&hir->storage,
                byte_count, 16u);
        memcpy(closure_scratch->published_captures,
            closure_scratch->captures.data, byte_count);
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
    for (index = 0u; index < closure_count; ++index) {
        CmHirClosure *closure;
        CmSemanticMarkClosureScratch *closure_scratch;

        closure_scratch = &scratch.closures[index];
        if (!closure_scratch->visited) continue;
        closure = (CmHirClosure *)cm_vec_at(&hir->closures, index);
        closure->capture_state = CM_HIR_CLOSURE_CAPTURES_MARKED;
        closure->captures = closure_scratch->published_captures;
        closure->capture_count =
            (uint32_t)closure_scratch->captures.len;
        closure->callable_class = closure_scratch->callable_class;
        closure->is_copy = closure_scratch->is_copy;
    }
    cm_hir_context_record_semantic_mutation(hir);
    scratch.result.body_index = CM_SEMANTIC_MARK_BODY_INDEX_NONE;
    scratch.result.body = CM_HIR_BODY_NONE;
    scratch.result.expression = CM_HIR_EXPR_NONE;

done:
    for (index = 0u; index < closure_count; ++index) {
        cm_vec_destroy(&scratch.closures[index].captures);
    }
    cm_free(scratch.closures);
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
