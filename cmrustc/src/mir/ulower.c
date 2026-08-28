#include "cm/mir/ulower.h"

#include <string.h>

/*
 * v1 vocabulary: block/let(wild,binding)/literal/path/return/if/binary/
 * unary/call/method-call/field/tuple-field/ref/assign/cast/tuple/struct/
 * array/loop/while/break/continue/match/index/assign-op/range/try.
 * Each unsupported construct increments one census class; the walk stops
 * at a body's first blocker so counts name bodies, not nodes.
 */

/*
 * Construction classes (v1): each expression position must map to an
 * operand, rvalue, place, or terminator.  Kinds without a mapping yet are
 * counted as construct-<kind> so the builder grows by measured family.
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
    /* v1 construction metrics. */
    size_t statements;
    size_t blocks;
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
        state->statements += 1u;
        break;
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
        state->blocks += 2u;
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
        result.statements += state.statements;
        result.blocks += state.blocks;
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

/* ------------------------------------------------------------------ */
/* u-MIR construction (v1)                                              */

typedef struct CmUMirBuilder {
    CmUMirBody *body;
    const CmUBody *ub;
    const CmTyckBody *tb;
    CmUMirBlockId current;
    const char *blocked;
    unsigned int depth;
    CmMirULowerResult *census;
} CmUMirBuilder;

/* Census: opaque statements tallied by originating expression kind, so
 * real emission lands by measured family. */
static const char *const cm_umir_opaque_names[34] = {
    "opaque-literal", "opaque-path", "opaque-qualified-path",
    "opaque-block", "opaque-call", "opaque-method-call", "opaque-field",
    "opaque-tuple-field", "opaque-index", "opaque-unary", "opaque-ref",
    "opaque-binary", "opaque-assign", "opaque-assign-op", "opaque-cast",
    "opaque-try", "opaque-range", "opaque-let", "opaque-return",
    "opaque-break", "opaque-continue", "opaque-if", "opaque-match",
    "opaque-loop", "opaque-while", "opaque-for", "opaque-closure",
    "opaque-tuple", "opaque-array", "opaque-array-repeat",
    "opaque-struct", "opaque-asm", "opaque-offset-of",
    "opaque-unsupported"
};

static void cm_mir_ulower_count(CmMirULowerResult *result,
    const char *reason);

static CmUMirBlockId cm_umir_new_block(CmUMirBuilder *builder)
{
    CmUMirBlock block;
    memset(&block, 0, sizeof(block));
    cm_vec_init(&block.statements, sizeof(CmUMirStatement));
    block.terminator = CM_UMIR_TERMINATOR_RETURN;
    (void)cm_vec_push(&builder->body->blocks, &block);
    return (CmUMirBlockId)(builder->body->blocks.len - 1u);
}

static CmUMirLocalId cm_umir_new_local(CmUMirBuilder *builder, CmTyId type)
{
    (void)cm_vec_push(&builder->body->locals, &type);
    return (CmUMirLocalId)(builder->body->locals.len - 1u);
}

static void cm_umir_push(CmUMirBuilder *builder, CmUMirLocalId destination,
    CmUMirRvalueKind kind, CmUExprId expr, CmTyId type)
{
    CmUMirBlock *block = (CmUMirBlock *)cm_vec_at(&builder->body->blocks,
        builder->current);
    CmUMirStatement statement;
    if (block == NULL) return;
    statement.destination = destination;
    statement.kind = kind;
    statement.expr = expr;
    statement.type = type;
    (void)cm_vec_push(&block->statements, &statement);
}

static CmTyId cm_umir_expr_type(const CmUMirBuilder *builder, CmUExprId id)
{
    if (builder->tb->expr_types == NULL || id == CM_U_EXPR_NONE)
        return CM_TY_NONE;
    return builder->tb->expr_types[id];
}

/* Emit one expression as a fresh local; opaque kinds keep their type so
 * the emitter can grow class-by-class. */
static CmUMirLocalId cm_umir_emit_expr(CmUMirBuilder *builder, CmUExprId id)
{
    const CmUExpr *expr;
    CmTyId type = cm_umir_expr_type(builder, id);
    CmUMirLocalId destination;
    if (builder->blocked != NULL || id == CM_U_EXPR_NONE)
        return 0u;
    if (builder->depth > 512u) {
        builder->blocked = "construct-depth";
        return 0u;
    }
    expr = cm_ubody_get_expr(builder->ub, id);
    destination = cm_umir_new_local(builder, type);
    if (expr == NULL) return destination;
    builder->depth += 1u;
    switch (expr->kind) {
    case CM_U_EXPR_LITERAL:
        cm_umir_push(builder, destination, CM_UMIR_RVALUE_LITERAL, id,
            type);
        break;
    case CM_U_EXPR_PATH:
        cm_umir_push(builder, destination, CM_UMIR_RVALUE_LOCAL, id, type);
        break;
    case CM_U_EXPR_BINARY:
        (void)cm_umir_emit_expr(builder, expr->data.binary.left);
        (void)cm_umir_emit_expr(builder, expr->data.binary.right);
        cm_umir_push(builder, destination, CM_UMIR_RVALUE_BINARY, id,
            type);
        break;
    case CM_U_EXPR_UNARY:
        (void)cm_umir_emit_expr(builder, expr->data.unary.operand);
        cm_umir_push(builder, destination, CM_UMIR_RVALUE_UNARY, id, type);
        break;
    case CM_U_EXPR_CALL: {
        uint32_t index;
        (void)cm_umir_emit_expr(builder, expr->data.call.callee);
        for (index = 0u; index < expr->data.call.argument_count; ++index)
            (void)cm_umir_emit_expr(builder,
                expr->data.call.arguments[index]);
        cm_umir_push(builder, destination, CM_UMIR_RVALUE_CALL, id, type);
        break;
    }
    case CM_U_EXPR_METHOD_CALL: {
        uint32_t index;
        (void)cm_umir_emit_expr(builder, expr->data.method_call.receiver);
        for (index = 0u; index < expr->data.method_call.argument_count;
                ++index)
            (void)cm_umir_emit_expr(builder,
                expr->data.method_call.arguments[index]);
        cm_umir_push(builder, destination, CM_UMIR_RVALUE_METHOD_CALL, id,
            type);
        break;
    }
    case CM_U_EXPR_REF:
        (void)cm_umir_emit_expr(builder, expr->data.ref.operand);
        cm_umir_push(builder, destination, CM_UMIR_RVALUE_REF, id, type);
        break;
    case CM_U_EXPR_CAST:
        (void)cm_umir_emit_expr(builder, expr->data.cast.value);
        cm_umir_push(builder, destination, CM_UMIR_RVALUE_CAST, id, type);
        break;
    case CM_U_EXPR_ASSIGN:
    case CM_U_EXPR_ASSIGN_OP:
        (void)cm_umir_emit_expr(builder, expr->data.assign.value);
        (void)cm_umir_emit_expr(builder, expr->data.assign.target);
        cm_umir_push(builder, destination, CM_UMIR_RVALUE_ASSIGN, id,
            type);
        break;
    case CM_U_EXPR_FIELD:
        (void)cm_umir_emit_expr(builder, expr->data.field.base);
        cm_umir_push(builder, destination, CM_UMIR_RVALUE_FIELD, id, type);
        break;
    case CM_U_EXPR_TUPLE_FIELD:
        (void)cm_umir_emit_expr(builder, expr->data.tuple_field.base);
        cm_umir_push(builder, destination, CM_UMIR_RVALUE_FIELD, id, type);
        break;
    case CM_U_EXPR_TUPLE:
    case CM_U_EXPR_ARRAY: {
        uint32_t index;
        for (index = 0u; index < expr->data.list.element_count; ++index)
            (void)cm_umir_emit_expr(builder,
                expr->data.list.elements[index]);
        cm_umir_push(builder, destination, CM_UMIR_RVALUE_AGGREGATE, id,
            type);
        break;
    }
    case CM_U_EXPR_STRUCT: {
        uint32_t index;
        for (index = 0u; index < expr->data.struct_expr.field_count;
                ++index)
            (void)cm_umir_emit_expr(builder,
                expr->data.struct_expr.fields[index].value);
        if (expr->data.struct_expr.base != CM_U_EXPR_NONE)
            (void)cm_umir_emit_expr(builder,
                expr->data.struct_expr.base);
        cm_umir_push(builder, destination, CM_UMIR_RVALUE_AGGREGATE, id,
            type);
        break;
    }
    case CM_U_EXPR_RETURN: {
        CmUMirBlock *current;
        (void)cm_umir_emit_expr(builder, expr->data.flow.value);
        current = (CmUMirBlock *)cm_vec_at(&builder->body->blocks,
            builder->current);
        if (current != NULL)
            current->terminator = CM_UMIR_TERMINATOR_RETURN;
        break;
    }
    case CM_U_EXPR_BLOCK: {
        uint32_t index;
        for (index = 0u; index < expr->data.block.statement_count
                && builder->blocked == NULL; ++index) {
            const CmUStmt *stmt = cm_ubody_get_stmt(builder->ub,
                expr->data.block.statements[index]);
            if (stmt == NULL) continue;
            if (stmt->kind == CM_U_STMT_LET) {
                if (stmt->data.let_stmt.initializer != CM_U_EXPR_NONE)
                    (void)cm_umir_emit_expr(builder,
                        stmt->data.let_stmt.initializer);
                if (stmt->data.let_stmt.else_block != CM_U_EXPR_NONE)
                    (void)cm_umir_emit_expr(builder,
                        stmt->data.let_stmt.else_block);
            } else if (stmt->kind == CM_U_STMT_EXPR) {
                (void)cm_umir_emit_expr(builder,
                    stmt->data.expr_stmt.expression);
            }
        }
        if (builder->blocked == NULL
            && expr->data.block.tail != CM_U_EXPR_NONE)
            (void)cm_umir_emit_expr(builder, expr->data.block.tail);
        break;
    }
    case CM_U_EXPR_IF: {
        CmUMirLocalId condition = cm_umir_emit_expr(builder,
            expr->data.if_expr.condition);
        CmUMirBlockId then_block = cm_umir_new_block(builder);
        CmUMirBlockId else_block = cm_umir_new_block(builder);
        CmUMirBlockId join = cm_umir_new_block(builder);
        CmUMirBlock *current = (CmUMirBlock *)cm_vec_at(
            &builder->body->blocks, builder->current);
        if (current != NULL) {
            current->terminator = CM_UMIR_TERMINATOR_SWITCH_BOOL;
            current->condition = condition;
            current->true_target = then_block;
            current->false_target = else_block;
        }
        builder->current = then_block;
        (void)cm_umir_emit_expr(builder, expr->data.if_expr.then_expr);
        current = (CmUMirBlock *)cm_vec_at(&builder->body->blocks,
            builder->current);
        if (current != NULL) {
            current->terminator = CM_UMIR_TERMINATOR_GOTO;
            current->goto_target = join;
        }
        builder->current = else_block;
        if (expr->data.if_expr.else_expr != CM_U_EXPR_NONE)
            (void)cm_umir_emit_expr(builder, expr->data.if_expr.else_expr);
        current = (CmUMirBlock *)cm_vec_at(&builder->body->blocks,
            builder->current);
        if (current != NULL) {
            current->terminator = CM_UMIR_TERMINATOR_GOTO;
            current->goto_target = join;
        }
        builder->current = join;
        break;
    }
    default:
        /* Every other kind is representable later: emit an opaque
         * assignment that keeps the type and source expression. */
        cm_umir_push(builder, destination, CM_UMIR_RVALUE_OPAQUE, id,
            type);
        if (builder->census != NULL && (size_t)expr->kind < 34u)
            cm_mir_ulower_count(builder->census,
                cm_umir_opaque_names[(size_t)expr->kind]);
        break;
    }
    builder->depth -= 1u;
    return destination;
}

void cm_umir_set_init(CmUMirSet *set)
{
    cm_vec_init(&set->bodies, sizeof(CmUMirBody));
}

void cm_umir_set_destroy(CmUMirSet *set)
{
    size_t index;
    for (index = 0u; index < set->bodies.len; ++index) {
        CmUMirBody *body = (CmUMirBody *)cm_vec_at(&set->bodies, index);
        size_t block_index;
        if (body == NULL) continue;
        for (block_index = 0u; block_index < body->blocks.len;
                ++block_index) {
            CmUMirBlock *block = (CmUMirBlock *)cm_vec_at(&body->blocks,
                block_index);
            if (block != NULL) cm_vec_destroy(&block->statements);
        }
        cm_vec_destroy(&body->blocks);
        cm_vec_destroy(&body->locals);
    }
    cm_vec_destroy(&set->bodies);
}

CmMirULowerResult cm_mir_ulower_build(CmUMirSet *out,
    const CmHirContext *hir, const CmUBodySet *bodies,
    const CmTyckSet *tyck)
{
    CmMirULowerResult result;
    size_t body_index;
    memset(&result, 0, sizeof(result));
    if (out == NULL || hir == NULL || bodies == NULL || tyck == NULL)
        return result;
    for (body_index = 1u; body_index <= tyck->bodies.len; ++body_index) {
        const CmTyckBody *tb = cm_tyck_get(tyck, (CmHirBodyId)body_index);
        const CmUBody *ub = cm_ubody_get(bodies, (CmHirBodyId)body_index);
        CmUMirBody body;
        CmUMirBuilder builder;
        CmTyId return_type;
        if (tb == NULL || ub == NULL
            || tb->status != CM_TYCK_BODY_TYPED) continue;
        result.bodies += 1u;
        memset(&body, 0, sizeof(body));
        body.source = (CmHirBodyId)body_index;
        cm_vec_init(&body.locals, sizeof(CmTyId));
        cm_vec_init(&body.blocks, sizeof(CmUMirBlock));
        memset(&builder, 0, sizeof(builder));
        builder.body = &body;
        builder.ub = ub;
        builder.tb = tb;
        builder.census = &result;
        return_type = tb->return_type;
        (void)cm_vec_push(&body.locals, &return_type);
        builder.current = cm_umir_new_block(&builder);
        (void)cm_umir_emit_expr(&builder, ub->root);
        if (builder.blocked == NULL) {
            body.complete = 1;
            result.lowered += 1u;
        } else {
            result.blocked += 1u;
            cm_mir_ulower_count(&result, builder.blocked);
        }
        {
            size_t block_index;
            for (block_index = 0u; block_index < body.blocks.len;
                    ++block_index) {
                const CmUMirBlock *block = (const CmUMirBlock *)
                    cm_vec_at_const(&body.blocks, block_index);
                if (block != NULL)
                    result.statements += block->statements.len;
            }
            result.blocks += body.blocks.len;
        }
        (void)cm_vec_push(&out->bodies, &body);
    }
    return result;
}
