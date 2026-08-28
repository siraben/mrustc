#include "cm/mir/ulower.h"

#include <string.h>

/*
 * v1 vocabulary: block/let(wild,binding)/literal/path/return/if/binary/
 * unary/call/method-call/field/tuple-field/ref/assign/cast/tuple/struct/
 * array/loop/while/break/continue/match/index/assign-op/range/try.
 * Each unsupported construct increments one census class; the walk stops
 * at a body's first blocker so counts name bodies, not nodes.
 */

typedef struct CmMirULowerState {
    const CmHirContext *hir;
    const CmUBody *ub;
    const CmTyckBody *tb;
    const char *blocked;
    unsigned int depth;
    /* Desugar-able control flow encountered: counted, not blocking. */
    int needs_match;
    int needs_try;
    int needs_range;
    int needs_for;
    int needs_let_condition;
} CmMirULowerState;

static void cm_mir_ulower_expr(CmMirULowerState *state, CmUExprId id);

static void cm_mir_ulower_block(CmMirULowerState *state, const CmUExpr *expr)
{
    uint32_t index;
    for (index = 0u; index < expr->data.block.statement_count
            && state->blocked == NULL; ++index) {
        const CmUStmt *stmt = cm_ubody_get_stmt(state->ub,
            expr->data.block.statements[index]);
        if (stmt == NULL) continue;
        switch (stmt->kind) {
        case CM_U_STMT_LET: {
            const CmUPat *pat = cm_ubody_get_pat(state->ub,
                stmt->data.let_stmt.pattern);
            if (pat != NULL && pat->kind != CM_U_PAT_WILD
                && pat->kind != CM_U_PAT_BINDING
                && pat->kind != CM_U_PAT_TUPLE
                && pat->kind != CM_U_PAT_REF
                && pat->kind != CM_U_PAT_STRUCT
                && pat->kind != CM_U_PAT_TUPLE_STRUCT) {
                state->blocked = "let-pattern";
                return;
            }
            if (stmt->data.let_stmt.initializer != CM_U_EXPR_NONE)
                cm_mir_ulower_expr(state, stmt->data.let_stmt.initializer);
            if (stmt->data.let_stmt.else_block != CM_U_EXPR_NONE)
                cm_mir_ulower_expr(state, stmt->data.let_stmt.else_block);
            break;
        }
        case CM_U_STMT_EXPR:
            cm_mir_ulower_expr(state, stmt->data.expr_stmt.expression);
            break;
        case CM_U_STMT_ITEM:
        default:
            break;
        }
    }
    if (state->blocked == NULL && expr->data.block.tail != CM_U_EXPR_NONE)
        cm_mir_ulower_expr(state, expr->data.block.tail);
}

static void cm_mir_ulower_expr(CmMirULowerState *state, CmUExprId id)
{
    const CmUExpr *expr;
    uint32_t index;
    if (state->blocked != NULL || id == CM_U_EXPR_NONE) return;
    if (state->depth > 512u) {
        state->blocked = "depth";
        return;
    }
    expr = cm_ubody_get_expr(state->ub, id);
    if (expr == NULL) return;
    state->depth += 1u;
    switch (expr->kind) {
    case CM_U_EXPR_LITERAL:
    case CM_U_EXPR_PATH:
    case CM_U_EXPR_CONTINUE:
        break;
    case CM_U_EXPR_BLOCK:
        cm_mir_ulower_block(state, expr);
        break;
    case CM_U_EXPR_CALL:
        cm_mir_ulower_expr(state, expr->data.call.callee);
        for (index = 0u; index < expr->data.call.argument_count; ++index)
            cm_mir_ulower_expr(state, expr->data.call.arguments[index]);
        break;
    case CM_U_EXPR_METHOD_CALL:
        cm_mir_ulower_expr(state, expr->data.method_call.receiver);
        for (index = 0u; index < expr->data.method_call.argument_count;
                ++index)
            cm_mir_ulower_expr(state,
                expr->data.method_call.arguments[index]);
        break;
    case CM_U_EXPR_FIELD:
        cm_mir_ulower_expr(state, expr->data.field.base);
        break;
    case CM_U_EXPR_TUPLE_FIELD:
        cm_mir_ulower_expr(state, expr->data.tuple_field.base);
        break;
    case CM_U_EXPR_INDEX:
        cm_mir_ulower_expr(state, expr->data.index.base);
        cm_mir_ulower_expr(state, expr->data.index.index);
        break;
    case CM_U_EXPR_UNARY:
        cm_mir_ulower_expr(state, expr->data.unary.operand);
        break;
    case CM_U_EXPR_REF:
        cm_mir_ulower_expr(state, expr->data.ref.operand);
        break;
    case CM_U_EXPR_BINARY:
        cm_mir_ulower_expr(state, expr->data.binary.left);
        cm_mir_ulower_expr(state, expr->data.binary.right);
        break;
    case CM_U_EXPR_ASSIGN:
    case CM_U_EXPR_ASSIGN_OP:
        cm_mir_ulower_expr(state, expr->data.assign.target);
        cm_mir_ulower_expr(state, expr->data.assign.value);
        break;
    case CM_U_EXPR_CAST:
        cm_mir_ulower_expr(state, expr->data.cast.value);
        break;
    case CM_U_EXPR_RETURN:
    case CM_U_EXPR_BREAK:
        cm_mir_ulower_expr(state, expr->data.flow.value);
        break;
    case CM_U_EXPR_IF:
        cm_mir_ulower_expr(state, expr->data.if_expr.condition);
        cm_mir_ulower_expr(state, expr->data.if_expr.then_expr);
        cm_mir_ulower_expr(state, expr->data.if_expr.else_expr);
        break;
    case CM_U_EXPR_LOOP:
        cm_mir_ulower_expr(state, expr->data.loop_expr.body);
        break;
    case CM_U_EXPR_WHILE:
        cm_mir_ulower_expr(state, expr->data.while_expr.condition);
        cm_mir_ulower_expr(state, expr->data.while_expr.body);
        break;
    case CM_U_EXPR_TUPLE:
    case CM_U_EXPR_ARRAY:
        for (index = 0u; index < expr->data.list.element_count; ++index)
            cm_mir_ulower_expr(state, expr->data.list.elements[index]);
        break;
    case CM_U_EXPR_ARRAY_REPEAT:
        cm_mir_ulower_expr(state, expr->data.repeat.value);
        cm_mir_ulower_expr(state, expr->data.repeat.length);
        break;
    case CM_U_EXPR_STRUCT:
        for (index = 0u; index < expr->data.struct_expr.field_count;
                ++index)
            cm_mir_ulower_expr(state,
                expr->data.struct_expr.fields[index].value);
        if (expr->data.struct_expr.base != CM_U_EXPR_NONE)
            cm_mir_ulower_expr(state, expr->data.struct_expr.base);
        break;
    case CM_U_EXPR_QUALIFIED_PATH:
        break;
    case CM_U_EXPR_MATCH: {
        uint32_t arm;
        state->needs_match = 1;
        cm_mir_ulower_expr(state, expr->data.match_expr.scrutinee);
        for (arm = 0u; arm < expr->data.match_expr.arm_count
                && state->blocked == NULL; ++arm) {
            cm_mir_ulower_expr(state,
                expr->data.match_expr.arms[arm].guard);
            cm_mir_ulower_expr(state,
                expr->data.match_expr.arms[arm].body);
        }
        break;
    }
    case CM_U_EXPR_TRY:
        state->needs_try = 1;
        cm_mir_ulower_expr(state, expr->data.try_expr.operand);
        break;
    case CM_U_EXPR_RANGE:
        state->needs_range = 1;
        cm_mir_ulower_expr(state, expr->data.range.start);
        cm_mir_ulower_expr(state, expr->data.range.end);
        break;
    case CM_U_EXPR_LET:
        state->needs_let_condition = 1;
        cm_mir_ulower_expr(state, expr->data.let_expr.initializer);
        break;
    case CM_U_EXPR_FOR:
        state->needs_for = 1;
        cm_mir_ulower_expr(state, expr->data.for_expr.iterable);
        cm_mir_ulower_expr(state, expr->data.for_expr.body);
        break;
    case CM_U_EXPR_CLOSURE:
        state->blocked = "closure";
        break;
    case CM_U_EXPR_ASM:
        state->blocked = "asm";
        break;
    case CM_U_EXPR_OFFSET_OF:
        state->blocked = "offset-of";
        break;
    case CM_U_EXPR_UNSUPPORTED:
    default:
        state->blocked = "unsupported-expr";
        break;
    }
    state->depth -= 1u;
}

static void cm_mir_ulower_count(CmMirULowerResult *result,
    const char *reason)
{
    size_t index;
    for (index = 0u; index < result->class_count; ++index)
        if (strcmp(result->classes[index].reason, reason) == 0) {
            result->classes[index].count += 1u;
            return;
        }
    if (result->class_count < CM_MIR_ULOWER_CLASSES) {
        result->classes[result->class_count].reason = reason;
        result->classes[result->class_count].count = 1u;
        result->class_count += 1u;
    }
}

CmMirULowerResult cm_mir_ulower_all(const CmHirContext *hir,
    const CmUBodySet *bodies, const CmTyckSet *tyck)
{
    CmMirULowerResult result;
    size_t body_index;
    memset(&result, 0, sizeof(result));
    if (hir == NULL || bodies == NULL || tyck == NULL) return result;
    for (body_index = 1u; body_index <= tyck->bodies.len; ++body_index) {
        const CmTyckBody *tb = cm_tyck_get(tyck, (CmHirBodyId)body_index);
        const CmUBody *ub = cm_ubody_get(bodies, (CmHirBodyId)body_index);
        CmMirULowerState state;
        if (tb == NULL || ub == NULL
            || tb->status != CM_TYCK_BODY_TYPED) continue;
        result.bodies += 1u;
        memset(&state, 0, sizeof(state));
        state.hir = hir;
        state.ub = ub;
        state.tb = tb;
        cm_mir_ulower_expr(&state, ub->root);
        if (state.blocked == NULL) {
            result.lowered += 1u;
            if (state.needs_match)
                cm_mir_ulower_count(&result, "needs-match");
            if (state.needs_try)
                cm_mir_ulower_count(&result, "needs-try");
            if (state.needs_range)
                cm_mir_ulower_count(&result, "needs-range");
            if (state.needs_for)
                cm_mir_ulower_count(&result, "needs-for");
            if (state.needs_let_condition)
                cm_mir_ulower_count(&result, "needs-let-condition");
        } else {
            result.blocked += 1u;
            cm_mir_ulower_count(&result, state.blocked);
        }
    }
    return result;
}
