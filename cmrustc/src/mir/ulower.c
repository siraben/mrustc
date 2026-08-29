#include "cm/mir/ulower.h"

#include <string.h>
#include "cm/hir/model.h"
#include "cm/alloc.h"

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
    const CmHirContext *hir;
    const CmUBodySet *ubodies;
    const CmTyckSet *tyck;
    CmUMirBody *body;
    const CmUBody *ub;
    const CmTyckBody *tb;
    CmUMirBlockId current;
    const char *blocked;
    unsigned int depth;
    CmMirULowerResult *census;
    /* Innermost-first loop context for break/continue targets. */
    CmUMirBlockId loop_headers[64];
    CmUMirBlockId loop_exits[64];
    unsigned int loop_depth;
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
    block.terminator = CM_UMIR_TERMINATOR_NONE;
    (void)cm_vec_push(&builder->body->blocks, &block);
    return (CmUMirBlockId)(builder->body->blocks.len - 1u);
}

/* Seal the current block with a goto unless a break/continue/return
 * already terminated it. */
static void cm_umir_seal_goto(CmUMirBuilder *builder, CmUMirBlockId target)
{
    CmUMirBlock *current = (CmUMirBlock *)cm_vec_at(&builder->body->blocks,
        builder->current);
    if (current == NULL || current->terminator != CM_UMIR_TERMINATOR_NONE)
        return;
    current->terminator = CM_UMIR_TERMINATOR_GOTO;
    current->goto_target = target;
}

static CmUMirLocalId cm_umir_new_local(CmUMirBuilder *builder, CmTyId type)
{
    (void)cm_vec_push(&builder->body->locals, &type);
    return (CmUMirLocalId)(builder->body->locals.len - 1u);
}

/* Whether a by-value `self` method really takes a reference: its impl's
 * Self is `&T` / `&mut T` (core's SpecWriteFmt for &mut W), or, for a
 * trait declaration, some impl of the trait has such a Self. */
static int cm_umir_self_is_reference(const CmHirContext *hir,
    const CmHirItem *callee)
{
    const CmHirDefinition *record;
    const CmHirItem *parent;
    size_t index;
    if (hir == NULL || callee == NULL
        || cm_hir_def_id_is_none(callee->parent_definition)) return 0;
    record = cm_hir_lookup_definition(hir, callee->parent_definition);
    parent = record == NULL || record->kind != CM_HIR_DEFINITION_ITEM ? NULL
        : cm_hir_get_item(hir, record->entity.item_id);
    if (parent == NULL) return 0;
    if (parent->kind == CM_HIR_ITEM_IMPL) {
        const CmHirType *self = cm_hir_get_type(hir,
            parent->data.impl_item.self_type);
        return self != NULL && (self->kind == CM_HIR_TYPE_REFERENCE_KIND
            || self->kind == CM_HIR_TYPE_RAW_POINTER_KIND);
    }
    if (parent->kind != CM_HIR_ITEM_TRAIT) return 0;
    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *impl = (const CmHirItem *)cm_vec_at_const(
            &hir->items, index);
        const CmHirType *self;
        if (impl == NULL || impl->kind != CM_HIR_ITEM_IMPL
            || !cm_hir_def_id_equal(impl->data.impl_item.trait_type.definition,
                parent->definition)) continue;
        self = cm_hir_get_type(hir, impl->data.impl_item.self_type);
        if (self != NULL && (self->kind == CM_HIR_TYPE_REFERENCE_KIND
                || self->kind == CM_HIR_TYPE_RAW_POINTER_KIND))
            return 1;
    }
    return 0;
}

static void cm_umir_push_operands(CmUMirBuilder *builder,
    CmUMirLocalId destination, CmUMirRvalueKind kind, CmUExprId expr,
    CmTyId type, const CmUMirLocalId *operands, uint32_t operand_count)
{
    CmUMirBlock *block = (CmUMirBlock *)cm_vec_at(&builder->body->blocks,
        builder->current);
    CmUMirStatement statement;
    uint32_t index;
    uint32_t stored = operand_count > CM_UMIR_STATEMENT_OPERANDS
        ? CM_UMIR_STATEMENT_OPERANDS : operand_count;
    if (block == NULL) return;
    memset(&statement, 0, sizeof(statement));
    statement.destination = destination;
    statement.kind = kind;
    statement.expr = expr;
    statement.type = type;
    for (index = 0u; index < stored; ++index)
        statement.operands[index] = operands[index];
    statement.operand_count = stored;
    statement.operand_overflow = operand_count - stored;
    (void)cm_vec_push(&block->statements, &statement);
}

static void cm_umir_push(CmUMirBuilder *builder, CmUMirLocalId destination,
    CmUMirRvalueKind kind, CmUExprId expr, CmTyId type)
{
    cm_umir_push_operands(builder, destination, kind, expr, type, NULL,
        0u);
}

static void cm_umir_push_immediate(CmUMirBuilder *builder,
    CmUMirLocalId destination, CmUMirRvalueKind kind, CmUExprId expr,
    CmTyId type, const CmUMirLocalId *operands, uint32_t operand_count,
    uint32_t immediate)
{
    CmUMirBlock *block;
    CmUMirStatement *statement;
    cm_umir_push_operands(builder, destination, kind, expr, type, operands,
        operand_count);
    block = (CmUMirBlock *)cm_vec_at(&builder->body->blocks,
        builder->current);
    if (block == NULL || block->statements.len == 0u) return;
    statement = (CmUMirStatement *)cm_vec_at(&block->statements,
        block->statements.len - 1u);
    if (statement != NULL) statement->immediate = immediate;
}

/* Variant index of a VARIANT-resolved path, or -1. */
static long cm_umir_variant_index(const CmHirContext *hir,
    const CmUResolution *res)
{
    const CmHirDefinition *record;
    if (hir == NULL || res->kind != CM_U_RESOLVED_VARIANT) return -1;
    record = cm_hir_lookup_definition(hir, res->definition);
    if (record == NULL || record->kind != CM_HIR_DEFINITION_ENUM_VARIANT)
        return -1;
    return (long)record->entity.enum_variant.variant_index;
}

/* Declared field index of `name` within a VARIANT-resolved struct-like
 * variant, or -1. */
static long cm_umir_variant_field_index(const CmHirContext *hir,
    const CmUBodySet *ubodies, const CmUResolution *res, CmInternId name)
{
    const CmHirDefinition *record;
    const CmHirItem *item;
    const CmHirVariant *variant;
    const CmInternedString *wanted;
    uint32_t index;
    if (hir == NULL || ubodies == NULL
        || res->kind != CM_U_RESOLVED_VARIANT) return -1;
    wanted = cm_interner_get(&ubodies->strings, name);
    if (wanted == NULL) return -1;
    record = cm_hir_lookup_definition(hir, res->definition);
    if (record == NULL || record->kind != CM_HIR_DEFINITION_ENUM_VARIANT)
        return -1;
    item = cm_hir_get_item(hir, record->entity.enum_variant.enum_item_id);
    if (item == NULL || item->kind != CM_HIR_ITEM_ENUM
        || record->entity.enum_variant.variant_index
            >= item->data.enum_item.variant_count) return -1;
    variant = &item->data.enum_item.variants[
        record->entity.enum_variant.variant_index];
    for (index = 0u; index < variant->field_count; ++index) {
        const CmInternedString *have = cm_interner_get(&hir->strings,
            variant->fields[index].name);
        if (have != NULL && have->len == wanted->len
            && memcmp(have->bytes, wanted->bytes, have->len) == 0)
            return (long)index;
    }
    return -1;
}

static CmTyId cm_umir_expr_type(const CmUMirBuilder *builder, CmUExprId id)
{
    if (builder->tb->expr_types == NULL || id == CM_U_EXPR_NONE)
        return CM_TY_NONE;
    return builder->tb->expr_types[id];
}

/* Emit one expression as a fresh local; opaque kinds keep their type so
 * the emitter can grow class-by-class. */
static CmUMirLocalId cm_umir_emit_expr(CmUMirBuilder *builder, CmUExprId id);

/* Whether `type` is core's `Option` (by item name), else Result-like. */
static int cm_umir_type_is_option(const CmUMirBuilder *builder, CmTyId type)
{
    const CmTy *ty;
    const CmHirDefinition *record;
    const CmHirItem *item;
    const CmInternedString *name;
    if (builder->tyck == NULL || builder->hir == NULL || type == CM_TY_NONE)
        return 0;
    ty = cm_ty_get((CmTyArena *)&builder->tyck->arena,
        cm_ty_resolve((CmTyArena *)&builder->tyck->arena, type));
    if (ty == NULL || ty->kind != CM_TY_ADT) return 0;
    record = cm_hir_lookup_definition(builder->hir, ty->def);
    item = record == NULL || record->kind != CM_HIR_DEFINITION_ITEM ? NULL
        : cm_hir_get_item(builder->hir, record->entity.item_id);
    name = item == NULL ? NULL
        : cm_interner_get(&builder->hir->strings, item->name);
    return name != NULL && name->len == 6u
        && memcmp(name->bytes, "Option", 6u) == 0;
}

/* Discriminant a pattern selects (variant patterns), else -1. */
static long cm_umir_pattern_discriminant(const CmUMirBuilder *builder,
    const CmUPat *pat)
{
    const CmUResolution *res = NULL;
    if (pat == NULL) return -1;
    if (pat->kind == CM_U_PAT_PATH) res = &pat->data.path.resolution;
    else if (pat->kind == CM_U_PAT_TUPLE_STRUCT
        || pat->kind == CM_U_PAT_STRUCT)
        res = &pat->data.struct_pat.resolution;
    if (res == NULL) return -1;
    return cm_umir_variant_index(builder->hir, res);
}

/* Field index of `name` in the struct behind a DEFINITION resolution. */
static long cm_umir_struct_field_index(const CmUMirBuilder *builder,
    const CmUResolution *res, CmInternId name)
{
    const CmHirDefinition *record;
    const CmHirItem *item;
    const CmInternedString *wanted;
    uint32_t index;
    if (res->kind != CM_U_RESOLVED_DEFINITION || builder->hir == NULL)
        return -1;
    record = cm_hir_lookup_definition(builder->hir, res->definition);
    if (record == NULL || record->kind != CM_HIR_DEFINITION_ITEM) return -1;
    item = cm_hir_get_item(builder->hir, record->entity.item_id);
    if (item == NULL || (item->kind != CM_HIR_ITEM_STRUCT
            && item->kind != CM_HIR_ITEM_UNION)) return -1;
    wanted = cm_interner_get(&builder->ubodies->strings, name);
    if (wanted == NULL) return -1;
    for (index = 0u; index < item->data.aggregate_item.field_count; ++index) {
        const CmInternedString *have = cm_interner_get(&builder->hir->strings,
            item->data.aggregate_item.fields[index].name);
        if (have != NULL && have->len == wanted->len
            && memcmp(have->bytes, wanted->bytes, have->len) == 0)
            return (long)index;
    }
    return -1;
}

/* Bind the locals of `pat_id` from `value` (a scrutinee known to match):
 * bindings copy, tuple/variant/struct patterns read their slots
 * (variants: slot 0 is the discriminant), `&p` loads through. */
static void cm_umir_bind_pattern(CmUMirBuilder *builder, CmUPatId pat_id,
    CmUMirLocalId value, CmUExprId id)
{
    const CmUPat *pat = cm_ubody_get_pat(builder->ub, pat_id);
    CmTyId pat_type = builder->tb != NULL && builder->tb->pat_types != NULL
        ? builder->tb->pat_types[pat_id] : CM_TY_NONE;
    uint32_t index;
    if (pat == NULL) return;
    switch (pat->kind) {
    case CM_U_PAT_BINDING:
        if (pat->data.binding.local != CM_U_LOCAL_NONE)
            cm_umir_push_operands(builder,
                (CmUMirLocalId)(1u + pat->data.binding.local),
                CM_UMIR_RVALUE_LOCAL, id, pat_type, &value, 1u);
        if (pat->data.binding.subpattern != CM_U_PAT_NONE)
            cm_umir_bind_pattern(builder, pat->data.binding.subpattern,
                value, id);
        break;
    case CM_U_PAT_TUPLE:
        for (index = 0u; index < pat->data.list.pattern_count; ++index) {
            const CmUPat *sub = cm_ubody_get_pat(builder->ub,
                pat->data.list.patterns[index]);
            CmUMirLocalId slot;
            if (sub == NULL || sub->kind == CM_U_PAT_WILD
                || sub->kind == CM_U_PAT_REST) continue;
            slot = cm_umir_new_local(builder, builder->tb != NULL
                && builder->tb->pat_types != NULL
                ? builder->tb->pat_types[pat->data.list.patterns[index]]
                : CM_TY_NONE);
            cm_umir_push_immediate(builder, slot, CM_UMIR_RVALUE_SLOT, id,
                CM_TY_NONE, &value, 1u, index);
            cm_umir_bind_pattern(builder, pat->data.list.patterns[index],
                slot, id);
        }
        break;
    case CM_U_PAT_TUPLE_STRUCT: {
        int variant = cm_umir_variant_index(builder->hir,
            &pat->data.struct_pat.resolution) >= 0;
        for (index = 0u; index < pat->data.struct_pat.pattern_count;
                ++index) {
            const CmUPat *sub = cm_ubody_get_pat(builder->ub,
                pat->data.struct_pat.patterns[index]);
            CmUMirLocalId slot;
            if (sub == NULL || sub->kind == CM_U_PAT_WILD
                || sub->kind == CM_U_PAT_REST) continue;
            slot = cm_umir_new_local(builder, builder->tb != NULL
                && builder->tb->pat_types != NULL
                ? builder->tb->pat_types[pat->data.struct_pat.patterns[index]]
                : CM_TY_NONE);
            cm_umir_push_immediate(builder, slot, CM_UMIR_RVALUE_SLOT, id,
                CM_TY_NONE, &value, 1u, (variant ? 1u : 0u) + index);
            cm_umir_bind_pattern(builder,
                pat->data.struct_pat.patterns[index], slot, id);
        }
        break;
    }
    case CM_U_PAT_STRUCT: {
        int variant = cm_umir_variant_index(builder->hir,
            &pat->data.struct_pat.resolution) >= 0;
        for (index = 0u; index < pat->data.struct_pat.field_count; ++index) {
            const CmUPat *sub = cm_ubody_get_pat(builder->ub,
                pat->data.struct_pat.fields[index].pattern);
            long slot_index = variant
                ? cm_umir_variant_field_index(builder->hir, builder->ubodies,
                    &pat->data.struct_pat.resolution,
                    pat->data.struct_pat.fields[index].name)
                : cm_umir_struct_field_index(builder,
                    &pat->data.struct_pat.resolution,
                    pat->data.struct_pat.fields[index].name);
            CmUMirLocalId slot;
            if (sub == NULL || sub->kind == CM_U_PAT_WILD || slot_index < 0)
                continue;
            slot = cm_umir_new_local(builder, builder->tb != NULL
                && builder->tb->pat_types != NULL
                ? builder->tb->pat_types[
                    pat->data.struct_pat.fields[index].pattern]
                : CM_TY_NONE);
            cm_umir_push_immediate(builder, slot, CM_UMIR_RVALUE_SLOT, id,
                CM_TY_NONE, &value, 1u,
                (variant ? 1u : 0u) + (uint32_t)slot_index);
            cm_umir_bind_pattern(builder,
                pat->data.struct_pat.fields[index].pattern, slot, id);
        }
        break;
    }
    case CM_U_PAT_REF: {
        const CmUPat *sub = cm_ubody_get_pat(builder->ub,
            pat->data.ref.pattern);
        CmUMirLocalId loaded;
        if (sub == NULL || sub->kind == CM_U_PAT_WILD) break;
        loaded = cm_umir_new_local(builder, builder->tb != NULL
            && builder->tb->pat_types != NULL
            ? builder->tb->pat_types[pat->data.ref.pattern] : CM_TY_NONE);
        cm_umir_push_operands(builder, loaded, CM_UMIR_RVALUE_LOAD, id,
            builder->tb != NULL && builder->tb->pat_types != NULL
            ? builder->tb->pat_types[pat->data.ref.pattern] : CM_TY_NONE,
            &value, 1u);
        cm_umir_bind_pattern(builder, pat->data.ref.pattern, loaded, id);
        break;
    }
    default:
        break;
    }
}

/* Branch on whether `value` matches `pat`: a variant pattern switches on
 * the discriminant, anything else is taken as irrefutable.  Leaves the
 * builder in the success block with the pattern bound; `*out_fail` is the
 * failure block. */
static void cm_umir_emit_pattern_test(CmUMirBuilder *builder,
    CmUPatId pat_id, CmUMirLocalId value, CmUExprId id,
    CmUMirBlockId *out_fail)
{
    const CmUPat *pat = cm_ubody_get_pat(builder->ub, pat_id);
    long disc = cm_umir_pattern_discriminant(builder, pat);
    CmUMirBlockId success = cm_umir_new_block(builder);
    CmUMirBlockId fail = cm_umir_new_block(builder);
    CmUMirBlock *current = (CmUMirBlock *)cm_vec_at(&builder->body->blocks,
        builder->current);
    if (current != NULL && disc >= 0) {
        CmUMirBlockId *targets = (CmUMirBlockId *)cm_alloc_zeroed(1u,
            sizeof(*targets));
        long *discriminants = (long *)cm_alloc_zeroed(1u,
            sizeof(*discriminants));
        targets[0] = success;
        discriminants[0] = disc;
        current->terminator = CM_UMIR_TERMINATOR_SWITCH;
        current->condition = value;
        current->goto_target = fail;
        current->arm_targets = targets;
        current->arm_discriminants = discriminants;
        current->arm_count = 1u;
    } else if (current != NULL) {
        current->terminator = CM_UMIR_TERMINATOR_GOTO;
        current->goto_target = success;
    }
    builder->current = success;
    cm_umir_bind_pattern(builder, pat_id, value, id);
    *out_fail = fail;
}

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
        if (expr->data.path.resolution.kind == CM_U_RESOLVED_VARIANT
            && cm_umir_variant_index(builder->hir,
                &expr->data.path.resolution) >= 0) {
            cm_umir_push_immediate(builder, destination,
                CM_UMIR_RVALUE_VARIANT, id, type, NULL, 0u,
                (uint32_t)cm_umir_variant_index(builder->hir,
                    &expr->data.path.resolution));
        } else if (expr->data.path.resolution.kind == CM_U_RESOLVED_LOCAL
            && expr->data.path.resolution.local != CM_U_LOCAL_NONE) {
            /* Body locals occupy slots 1..n after the return slot. */
            CmUMirLocalId bound = (CmUMirLocalId)(1u
                + expr->data.path.resolution.local);
            cm_umir_push_operands(builder, destination,
                CM_UMIR_RVALUE_LOCAL, id, type, &bound, 1u);
        } else {
            cm_umir_push(builder, destination, CM_UMIR_RVALUE_LOCAL, id,
                type);
        }
        break;
    case CM_U_EXPR_BINARY: {
        CmUMirLocalId operands[2];
        operands[0] = cm_umir_emit_expr(builder, expr->data.binary.left);
        operands[1] = cm_umir_emit_expr(builder, expr->data.binary.right);
        cm_umir_push_operands(builder, destination, CM_UMIR_RVALUE_BINARY,
            id, type, operands, 2u);
        break;
    }
    case CM_U_EXPR_UNARY: {
        CmUMirLocalId operand = cm_umir_emit_expr(builder,
            expr->data.unary.operand);
        cm_umir_push_operands(builder, destination, CM_UMIR_RVALUE_UNARY,
            id, type, &operand, 1u);
        break;
    }
    case CM_U_EXPR_CALL: {
        uint32_t index;
        CmUMirLocalId operands[CM_UMIR_STATEMENT_OPERANDS];
        uint32_t recorded = 0u;
        const CmUExpr *callee_expr = cm_ubody_get_expr(builder->ub,
            expr->data.call.callee);
        CmUMirLocalId callee;
        if (callee_expr != NULL && callee_expr->kind == CM_U_EXPR_PATH
            && callee_expr->data.path.resolution.kind
                == CM_U_RESOLVED_DEFINITION
            && cm_umir_variant_index(builder->hir,
                &callee_expr->data.path.resolution) < 0
            && builder->hir != NULL) {
            /* Tuple-struct constructor called as a function (`Bytes(..)`):
             * an aggregate of its arguments. */
            const CmHirDefinition *record = cm_hir_lookup_definition(
                builder->hir, callee_expr->data.path.resolution.definition);
            const CmHirItem *item = record == NULL
                    || record->kind != CM_HIR_DEFINITION_ITEM ? NULL
                : cm_hir_get_item(builder->hir, record->entity.item_id);
            if (item != NULL && item->kind == CM_HIR_ITEM_STRUCT) {
                for (index = 0u; index < expr->data.call.argument_count;
                        ++index) {
                    CmUMirLocalId argument = cm_umir_emit_expr(builder,
                        expr->data.call.arguments[index]);
                    if (recorded < CM_UMIR_STATEMENT_OPERANDS)
                        operands[recorded++] = argument;
                }
                cm_umir_push_operands(builder, destination,
                    CM_UMIR_RVALUE_AGGREGATE, id, type, operands,
                    expr->data.call.argument_count);
                break;
            }
        }
        if (callee_expr != NULL && callee_expr->kind == CM_U_EXPR_PATH
            && cm_umir_variant_index(builder->hir,
                &callee_expr->data.path.resolution) >= 0) {
            /* Tuple-variant constructor: slot[0] = discriminant. */
            for (index = 0u; index < expr->data.call.argument_count;
                    ++index) {
                CmUMirLocalId argument = cm_umir_emit_expr(builder,
                    expr->data.call.arguments[index]);
                if (recorded < CM_UMIR_STATEMENT_OPERANDS)
                    operands[recorded++] = argument;
            }
            cm_umir_push_immediate(builder, destination,
                CM_UMIR_RVALUE_VARIANT, id, type, operands,
                expr->data.call.argument_count,
                (uint32_t)cm_umir_variant_index(builder->hir,
                    &callee_expr->data.path.resolution));
            break;
        }
        callee = cm_umir_emit_expr(builder, expr->data.call.callee);
        if (recorded < CM_UMIR_STATEMENT_OPERANDS)
            operands[recorded++] = callee;
        for (index = 0u; index < expr->data.call.argument_count; ++index) {
            CmUMirLocalId argument = cm_umir_emit_expr(builder,
                expr->data.call.arguments[index]);
            if (recorded < CM_UMIR_STATEMENT_OPERANDS)
                operands[recorded++] = argument;
        }
        cm_umir_push_operands(builder, destination, CM_UMIR_RVALUE_CALL,
            id, type, operands,
            1u + expr->data.call.argument_count);
        break;
    }
    case CM_U_EXPR_METHOD_CALL: {
        uint32_t index;
        CmUMirLocalId operands[CM_UMIR_STATEMENT_OPERANDS];
        uint32_t recorded = 0u;
        CmUMirLocalId receiver = cm_umir_emit_expr(builder,
            expr->data.method_call.receiver);
        /* Receiver adjustment: `&self` callees take the receiver's
         * address, by-value callees load through a reference. */
        if (builder->tb->method_targets != NULL && builder->tyck != NULL
            && builder->hir != NULL) {
            const CmHirDefinition *record = cm_hir_lookup_definition(
                builder->hir, builder->tb->method_targets[id]);
            const CmHirItem *callee = record == NULL
                    || record->kind != CM_HIR_DEFINITION_ITEM ? NULL
                : cm_hir_get_item(builder->hir, record->entity.item_id);
            CmTyId receiver_type = cm_umir_expr_type(builder,
                expr->data.method_call.receiver);
            const CmTy *rt = receiver_type == CM_TY_NONE ? NULL
                : cm_ty_get((CmTyArena *)&builder->tyck->arena,
                    cm_ty_resolve((CmTyArena *)&builder->tyck->arena,
                        receiver_type));
            int receiver_is_ref = rt != NULL
                && (rt->kind == CM_TY_REF || rt->kind == CM_TY_PTR);
            if (callee != NULL && callee->kind == CM_HIR_ITEM_FUNCTION) {
                CmHirReceiverKind kind =
                    callee->data.function_item.signature.receiver;
                if ((kind == CM_HIR_RECEIVER_REF_SHARED
                        || kind == CM_HIR_RECEIVER_REF_MUTABLE)
                    && !receiver_is_ref) {
                    CmUMirLocalId address = cm_umir_new_local(builder,
                        receiver_type);
                    cm_umir_push_operands(builder, address,
                        CM_UMIR_RVALUE_REF, expr->data.method_call.receiver,
                        receiver_type, &receiver, 1u);
                    receiver = address;
                } else if (kind == CM_HIR_RECEIVER_VALUE
                    && receiver_is_ref
                    && !cm_umir_self_is_reference(builder->hir, callee)) {
                    CmUMirLocalId loaded = cm_umir_new_local(builder,
                        rt->children[0]);
                    cm_umir_push_operands(builder, loaded,
                        CM_UMIR_RVALUE_LOAD, expr->data.method_call.receiver,
                        rt->children[0], &receiver, 1u);
                    receiver = loaded;
                }
            }
        }
        if (recorded < CM_UMIR_STATEMENT_OPERANDS)
            operands[recorded++] = receiver;
        for (index = 0u; index < expr->data.method_call.argument_count;
                ++index) {
            CmUMirLocalId argument = cm_umir_emit_expr(builder,
                expr->data.method_call.arguments[index]);
            if (recorded < CM_UMIR_STATEMENT_OPERANDS)
                operands[recorded++] = argument;
        }
        cm_umir_push_operands(builder, destination,
            CM_UMIR_RVALUE_METHOD_CALL, id, type, operands,
            1u + expr->data.method_call.argument_count);
        break;
    }
    case CM_U_EXPR_REF: {
        CmUMirLocalId operand = cm_umir_emit_expr(builder,
            expr->data.ref.operand);
        cm_umir_push_operands(builder, destination, CM_UMIR_RVALUE_REF,
            id, type, &operand, 1u);
        break;
    }
    case CM_U_EXPR_CAST: {
        CmUMirLocalId operand = cm_umir_emit_expr(builder,
            expr->data.cast.value);
        cm_umir_push_operands(builder, destination, CM_UMIR_RVALUE_CAST,
            id, type, &operand, 1u);
        break;
    }
    case CM_U_EXPR_ASSIGN:
    case CM_U_EXPR_ASSIGN_OP: {
        const CmUExpr *target = cm_ubody_get_expr(builder->ub,
            expr->data.assign.target);
        CmUMirLocalId value = cm_umir_emit_expr(builder,
            expr->data.assign.value);
        if (expr->kind == CM_U_EXPR_ASSIGN_OP) {
            /* `t op= v` : read the place, combine, then store. */
            CmUMirLocalId current = cm_umir_emit_expr(builder,
                expr->data.assign.target);
            CmUMirLocalId combined = cm_umir_new_local(builder,
                cm_umir_expr_type(builder, expr->data.assign.target));
            CmUMirLocalId pair[2];
            pair[0] = current;
            pair[1] = value;
            cm_umir_push_operands(builder, combined, CM_UMIR_RVALUE_BINARY,
                id, cm_umir_expr_type(builder, expr->data.assign.target),
                pair, 2u);
            value = combined;
        }
        if (target != NULL && target->kind == CM_U_EXPR_PATH
            && target->data.path.resolution.kind == CM_U_RESOLVED_LOCAL
            && target->data.path.resolution.local != CM_U_LOCAL_NONE) {
            CmUMirLocalId bound = (CmUMirLocalId)(1u
                + target->data.path.resolution.local);
            cm_umir_push_operands(builder, bound, CM_UMIR_RVALUE_LOCAL,
                expr->data.assign.value,
                cm_umir_expr_type(builder, expr->data.assign.value),
                &value, 1u);
        } else if (target != NULL && (target->kind == CM_U_EXPR_FIELD
                || target->kind == CM_U_EXPR_TUPLE_FIELD)) {
            CmUMirLocalId operands[2];
            operands[0] = cm_umir_emit_expr(builder,
                target->kind == CM_U_EXPR_FIELD ? target->data.field.base
                    : target->data.tuple_field.base);
            operands[1] = value;
            cm_umir_push_operands(builder, destination,
                CM_UMIR_RVALUE_STORE_FIELD, expr->data.assign.target,
                type, operands, 2u);
        } else if (target != NULL && target->kind == CM_U_EXPR_INDEX) {
            CmUMirLocalId operands[3];
            operands[0] = cm_umir_emit_expr(builder,
                target->data.index.base);
            operands[1] = cm_umir_emit_expr(builder,
                target->data.index.index);
            operands[2] = value;
            cm_umir_push_operands(builder, destination,
                CM_UMIR_RVALUE_STORE_INDEX, expr->data.assign.target,
                type, operands, 3u);
        } else if (target != NULL && target->kind == CM_U_EXPR_UNARY
                && target->data.unary.op == CM_U_UNARY_DEREF) {
            CmUMirLocalId operands[2];
            operands[0] = cm_umir_emit_expr(builder,
                target->data.unary.operand);
            operands[1] = value;
            cm_umir_push_operands(builder, destination,
                CM_UMIR_RVALUE_STORE_DEREF, expr->data.assign.target,
                type, operands, 2u);
        } else {
            CmUMirLocalId operands[2];
            operands[0] = value;
            operands[1] = cm_umir_emit_expr(builder,
                expr->data.assign.target);
            cm_umir_push_operands(builder, destination,
                CM_UMIR_RVALUE_ASSIGN, id, type, operands, 2u);
        }
        break;
    }
    case CM_U_EXPR_FIELD: {
        CmUMirLocalId base = cm_umir_emit_expr(builder,
            expr->data.field.base);
        cm_umir_push_operands(builder, destination, CM_UMIR_RVALUE_FIELD,
            id, type, &base, 1u);
        break;
    }
    case CM_U_EXPR_TUPLE_FIELD: {
        CmUMirLocalId base = cm_umir_emit_expr(builder,
            expr->data.tuple_field.base);
        cm_umir_push_operands(builder, destination, CM_UMIR_RVALUE_FIELD,
            id, type, &base, 1u);
        break;
    }
    case CM_U_EXPR_TUPLE:
    case CM_U_EXPR_ARRAY: {
        uint32_t index;
        CmUMirLocalId operands[CM_UMIR_STATEMENT_OPERANDS];
        uint32_t recorded = 0u;
        for (index = 0u; index < expr->data.list.element_count; ++index) {
            CmUMirLocalId element = cm_umir_emit_expr(builder,
                expr->data.list.elements[index]);
            if (recorded < CM_UMIR_STATEMENT_OPERANDS)
                operands[recorded++] = element;
        }
        cm_umir_push_operands(builder, destination,
            CM_UMIR_RVALUE_AGGREGATE, id, type, operands,
            expr->data.list.element_count);
        break;
    }
    case CM_U_EXPR_STRUCT: {
        uint32_t index;
        CmUMirLocalId operands[CM_UMIR_STATEMENT_OPERANDS];
        uint32_t recorded = 0u;
        for (index = 0u; index < expr->data.struct_expr.field_count;
                ++index) {
            CmUMirLocalId value = cm_umir_emit_expr(builder,
                expr->data.struct_expr.fields[index].value);
            if (recorded < CM_UMIR_STATEMENT_OPERANDS)
                operands[recorded++] = value;
        }
        if (expr->data.struct_expr.base != CM_U_EXPR_NONE)
            (void)cm_umir_emit_expr(builder,
                expr->data.struct_expr.base);
        if (cm_umir_variant_index(builder->hir,
                &expr->data.struct_expr.resolution) >= 0)
            cm_umir_push_immediate(builder, destination,
                CM_UMIR_RVALUE_VARIANT, id, type, operands,
                expr->data.struct_expr.field_count,
                (uint32_t)cm_umir_variant_index(builder->hir,
                    &expr->data.struct_expr.resolution));
        else
            cm_umir_push_operands(builder, destination,
                CM_UMIR_RVALUE_AGGREGATE, id, type, operands,
                expr->data.struct_expr.field_count);
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
                CmUMirLocalId init_local = 0u;
                const CmUPat *pat = cm_ubody_get_pat(builder->ub,
                    stmt->data.let_stmt.pattern);
                if (stmt->data.let_stmt.initializer != CM_U_EXPR_NONE)
                    init_local = cm_umir_emit_expr(builder,
                        stmt->data.let_stmt.initializer);
                if (pat != NULL
                    && stmt->data.let_stmt.initializer != CM_U_EXPR_NONE) {
                    if (stmt->data.let_stmt.else_block != CM_U_EXPR_NONE) {
                        /* `let PAT = e else { diverge }`: the failure
                         * block runs the else body. */
                        CmUMirBlockId fail;
                        CmUMirBlockId join = cm_umir_new_block(builder);
                        cm_umir_emit_pattern_test(builder,
                            stmt->data.let_stmt.pattern, init_local,
                            stmt->data.let_stmt.initializer, &fail);
                        cm_umir_seal_goto(builder, join);
                        builder->current = fail;
                        (void)cm_umir_emit_expr(builder,
                            stmt->data.let_stmt.else_block);
                        cm_umir_seal_goto(builder, join);
                        builder->current = join;
                    } else {
                        cm_umir_bind_pattern(builder,
                            stmt->data.let_stmt.pattern, init_local,
                            stmt->data.let_stmt.initializer);
                    }
                }
            } else if (stmt->kind == CM_U_STMT_EXPR) {
                (void)cm_umir_emit_expr(builder,
                    stmt->data.expr_stmt.expression);
            }
        }
        if (builder->blocked == NULL
            && expr->data.block.tail != CM_U_EXPR_NONE) {
            CmUMirLocalId tail = cm_umir_emit_expr(builder,
                expr->data.block.tail);
            /* The block's value is its tail's value. */
            cm_umir_push_operands(builder, destination,
                CM_UMIR_RVALUE_LOCAL, expr->data.block.tail, type, &tail,
                1u);
        }
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
        if (expr->data.if_expr.pattern != CM_U_PAT_NONE) {
            /* `if let PAT = e`: match test with the pattern bound in the
             * then-block; failure runs the else. */
            CmUMirBlockId fail;
            cm_umir_emit_pattern_test(builder, expr->data.if_expr.pattern,
                condition, id, &fail);
            cm_umir_seal_goto(builder, then_block);
            builder->current = fail;
            cm_umir_seal_goto(builder, else_block);
        } else if (current != NULL) {
            current->terminator = CM_UMIR_TERMINATOR_SWITCH_BOOL;
            current->condition = condition;
            current->true_target = then_block;
            current->false_target = else_block;
        }
        builder->current = then_block;
        {
            CmUMirLocalId then_value = cm_umir_emit_expr(builder,
                expr->data.if_expr.then_expr);
            cm_umir_push_operands(builder, destination,
                CM_UMIR_RVALUE_LOCAL, expr->data.if_expr.then_expr, type,
                &then_value, 1u);
        }
        cm_umir_seal_goto(builder, join);
        builder->current = else_block;
        if (expr->data.if_expr.else_expr != CM_U_EXPR_NONE) {
            CmUMirLocalId else_value = cm_umir_emit_expr(builder,
                expr->data.if_expr.else_expr);
            cm_umir_push_operands(builder, destination,
                CM_UMIR_RVALUE_LOCAL, expr->data.if_expr.else_expr, type,
                &else_value, 1u);
        }
        cm_umir_seal_goto(builder, join);
        builder->current = join;
        break;
    }
    case CM_U_EXPR_INDEX: {
        CmUMirLocalId operands[2];
        operands[0] = cm_umir_emit_expr(builder, expr->data.index.base);
        operands[1] = cm_umir_emit_expr(builder, expr->data.index.index);
        cm_umir_push_operands(builder, destination, CM_UMIR_RVALUE_INDEX,
            id, type, operands, 2u);
        break;
    }
    case CM_U_EXPR_QUALIFIED_PATH:
        cm_umir_push(builder, destination, CM_UMIR_RVALUE_LOCAL, id, type);
        break;
    case CM_U_EXPR_RANGE:
        if (expr->data.range.start != CM_U_EXPR_NONE)
            (void)cm_umir_emit_expr(builder, expr->data.range.start);
        if (expr->data.range.end != CM_U_EXPR_NONE)
            (void)cm_umir_emit_expr(builder, expr->data.range.end);
        cm_umir_push(builder, destination, CM_UMIR_RVALUE_AGGREGATE, id,
            type);
        break;
    case CM_U_EXPR_ARRAY_REPEAT: {
        /* `[v; N]`: the value is the single operand; N comes from the
         * array type at emission. */
        CmUMirLocalId operands[2];
        operands[0] = cm_umir_emit_expr(builder, expr->data.repeat.value);
        operands[1] = cm_umir_emit_expr(builder, expr->data.repeat.length);
        cm_umir_push_operands(builder, destination,
            CM_UMIR_RVALUE_AGGREGATE, id, type, operands, 2u);
        break;
    }
    case CM_U_EXPR_LOOP: {
        CmUMirBlockId header = cm_umir_new_block(builder);
        CmUMirBlockId exit_block = cm_umir_new_block(builder);
        CmUMirBlock *current = (CmUMirBlock *)cm_vec_at(
            &builder->body->blocks, builder->current);
        if (current != NULL) {
            current->terminator = CM_UMIR_TERMINATOR_GOTO;
            current->goto_target = header;
        }
        if (builder->loop_depth < 64u) {
            builder->loop_headers[builder->loop_depth] = header;
            builder->loop_exits[builder->loop_depth] = exit_block;
            builder->loop_depth += 1u;
        }
        builder->current = header;
        (void)cm_umir_emit_expr(builder, expr->data.loop_expr.body);
        if (builder->loop_depth != 0u) builder->loop_depth -= 1u;
        cm_umir_seal_goto(builder, header);
        builder->current = exit_block;
        break;
    }
    case CM_U_EXPR_WHILE: {
        CmUMirBlockId header = cm_umir_new_block(builder);
        CmUMirBlockId body_block = cm_umir_new_block(builder);
        CmUMirBlockId exit_block = cm_umir_new_block(builder);
        CmUMirLocalId condition;
        CmUMirBlock *current = (CmUMirBlock *)cm_vec_at(
            &builder->body->blocks, builder->current);
        if (current != NULL) {
            current->terminator = CM_UMIR_TERMINATOR_GOTO;
            current->goto_target = header;
        }
        builder->current = header;
        condition = cm_umir_emit_expr(builder,
            expr->data.while_expr.condition);
        if (expr->data.while_expr.pattern != CM_U_PAT_NONE) {
            /* `while let PAT = e`: match test; failure exits. */
            CmUMirBlockId fail;
            cm_umir_emit_pattern_test(builder, expr->data.while_expr.pattern,
                condition, id, &fail);
            cm_umir_seal_goto(builder, body_block);
            builder->current = fail;
            cm_umir_seal_goto(builder, exit_block);
        } else {
            current = (CmUMirBlock *)cm_vec_at(&builder->body->blocks,
                builder->current);
            if (current != NULL) {
                current->terminator = CM_UMIR_TERMINATOR_SWITCH_BOOL;
                current->condition = condition;
                current->true_target = body_block;
                current->false_target = exit_block;
            }
        }
        if (builder->loop_depth < 64u) {
            builder->loop_headers[builder->loop_depth] = header;
            builder->loop_exits[builder->loop_depth] = exit_block;
            builder->loop_depth += 1u;
        }
        builder->current = body_block;
        (void)cm_umir_emit_expr(builder, expr->data.while_expr.body);
        if (builder->loop_depth != 0u) builder->loop_depth -= 1u;
        cm_umir_seal_goto(builder, header);
        builder->current = exit_block;
        break;
    }
    case CM_U_EXPR_MATCH: {
        uint32_t arm;
        uint32_t arm_count = expr->data.match_expr.arm_count;
        CmUMirLocalId scrutinee = cm_umir_emit_expr(builder,
            expr->data.match_expr.scrutinee);
        CmUMirBlockId dispatch = builder->current;
        CmUMirBlockId join = cm_umir_new_block(builder);
        /* Dispatch entries: one per (pattern alternative); a guarded arm
         * gets a test block that falls through to the next entry. */
        uint32_t capacity = arm_count == 0u ? 1u : arm_count * 4u;
        CmUMirBlockId *targets = (CmUMirBlockId *)cm_alloc_zeroed(capacity,
            sizeof(CmUMirBlockId));
        long *discriminants = (long *)cm_alloc_zeroed(capacity,
            sizeof(long));
        uint32_t entries = 0u;
        CmUMirBlockId previous_test = 0u;
        int have_previous_test = 0;
        for (arm = 0u; arm < arm_count && builder->blocked == NULL; ++arm) {
            const CmUPat *pat = cm_ubody_get_pat(builder->ub,
                expr->data.match_expr.arms[arm].pattern);
            CmUMirBlockId arm_block = cm_umir_new_block(builder);
            CmUMirBlock *arm_current;
            CmUMirLocalId arm_value;
            long disc = -1;
            CmUExprId guard = expr->data.match_expr.arms[arm].guard;
            /* A previous guarded arm falls through here on failure. */
            if (have_previous_test) {
                CmUMirBlock *test = (CmUMirBlock *)cm_vec_at(
                    &builder->body->blocks, previous_test);
                if (test != NULL) test->false_target = arm_block;
                have_previous_test = 0;
            }
            builder->current = arm_block;
            if (pat != NULL && pat->kind == CM_U_PAT_OR) {
                /* Each alternative dispatches to this arm. */
                uint32_t alt;
                for (alt = 0u; alt < pat->data.list.pattern_count
                        && entries < capacity; ++alt) {
                    const CmUPat *sub = cm_ubody_get_pat(builder->ub,
                        pat->data.list.patterns[alt]);
                    const CmUResolution *sub_res = NULL;
                    if (sub == NULL) continue;
                    if (sub->kind == CM_U_PAT_PATH)
                        sub_res = &sub->data.path.resolution;
                    else if (sub->kind == CM_U_PAT_TUPLE_STRUCT
                        || sub->kind == CM_U_PAT_STRUCT)
                        sub_res = &sub->data.struct_pat.resolution;
                    targets[entries] = arm_block;
                    discriminants[entries] = sub_res == NULL ? -1
                        : cm_umir_variant_index(builder->hir, sub_res);
                    entries += 1u;
                }
                /* Or-pattern payload bindings are not extracted (v1). */
                pat = NULL;
            }
            if (pat != NULL) {
                const CmUResolution *res = NULL;
                if (pat->kind == CM_U_PAT_PATH) res = &pat->data.path.resolution;
                else if (pat->kind == CM_U_PAT_TUPLE_STRUCT
                    || pat->kind == CM_U_PAT_STRUCT)
                    res = &pat->data.struct_pat.resolution;
                if (res != NULL)
                    disc = cm_umir_variant_index(builder->hir, res);
                if (pat->kind == CM_U_PAT_TUPLE_STRUCT) {
                    uint32_t position;
                    for (position = 0u;
                            position < pat->data.struct_pat.pattern_count;
                            ++position) {
                        const CmUPat *sub = cm_ubody_get_pat(builder->ub,
                            pat->data.struct_pat.patterns[position]);
                        if (sub != NULL && sub->kind == CM_U_PAT_BINDING
                            && sub->data.binding.local != CM_U_LOCAL_NONE)
                            cm_umir_push_immediate(builder,
                                (CmUMirLocalId)(1u + sub->data.binding.local),
                                CM_UMIR_RVALUE_SLOT, id, CM_TY_NONE,
                                &scrutinee, 1u, 1u + position);
                    }
                } else if (pat->kind == CM_U_PAT_STRUCT) {
                    uint32_t field;
                    for (field = 0u; field < pat->data.struct_pat.field_count;
                            ++field) {
                        const CmUPat *sub = cm_ubody_get_pat(builder->ub,
                            pat->data.struct_pat.fields[field].pattern);
                        long slot = cm_umir_variant_field_index(builder->hir,
                            builder->ubodies, res,
                            pat->data.struct_pat.fields[field].name);
                        if (sub != NULL && sub->kind == CM_U_PAT_BINDING
                            && sub->data.binding.local != CM_U_LOCAL_NONE
                            && slot >= 0)
                            cm_umir_push_immediate(builder,
                                (CmUMirLocalId)(1u + sub->data.binding.local),
                                CM_UMIR_RVALUE_SLOT, id, CM_TY_NONE,
                                &scrutinee, 1u, 1u + (uint32_t)slot);
                    }
                } else if (pat->kind == CM_U_PAT_BINDING
                    && pat->data.binding.local != CM_U_LOCAL_NONE) {
                    cm_umir_push_operands(builder,
                        (CmUMirLocalId)(1u + pat->data.binding.local),
                        CM_UMIR_RVALUE_LOCAL, expr->data.match_expr.scrutinee,
                        CM_TY_NONE, &scrutinee, 1u);
                }
            }
            if (pat != NULL && entries < capacity) {
                targets[entries] = arm_block;
                discriminants[entries] = disc;
                entries += 1u;
            } else if (pat == NULL && expr->data.match_expr.arms[arm].pattern
                    != CM_U_PAT_NONE && entries < capacity
                    && cm_ubody_get_pat(builder->ub,
                        expr->data.match_expr.arms[arm].pattern) == NULL) {
                targets[entries] = arm_block;
                discriminants[entries] = -1;
                entries += 1u;
            }
            if (guard != CM_U_EXPR_NONE) {
                /* Bindings are in place; test the guard, else fall through
                 * to the next arm (patched when that arm is created). */
                CmUMirLocalId guard_value = cm_umir_emit_expr(builder,
                    guard);
                CmUMirBlockId guard_body = cm_umir_new_block(builder);
                CmUMirBlock *test = (CmUMirBlock *)cm_vec_at(
                    &builder->body->blocks, builder->current);
                if (test != NULL) {
                    test->terminator = CM_UMIR_TERMINATOR_SWITCH_BOOL;
                    test->condition = guard_value;
                    test->true_target = guard_body;
                    test->false_target = join; /* patched by next arm */
                }
                previous_test = builder->current;
                have_previous_test = 1;
                builder->current = guard_body;
            }
            arm_value = cm_umir_emit_expr(builder,
                expr->data.match_expr.arms[arm].body);
            cm_umir_push_operands(builder, destination, CM_UMIR_RVALUE_LOCAL,
                expr->data.match_expr.arms[arm].body, type, &arm_value, 1u);
            (void)arm_current;
            cm_umir_seal_goto(builder, join);
        }
        {
            CmUMirBlock *dispatch_block = (CmUMirBlock *)cm_vec_at(
                &builder->body->blocks, dispatch);
            if (dispatch_block != NULL) {
                dispatch_block->terminator = CM_UMIR_TERMINATOR_SWITCH;
                dispatch_block->condition = scrutinee;
                dispatch_block->goto_target = join;
                dispatch_block->arm_targets = targets;
                dispatch_block->arm_discriminants = discriminants;
                dispatch_block->arm_count = entries;
            } else {
                cm_free(targets);
                cm_free(discriminants);
            }
        }
        builder->current = join;
        break;
    }
    case CM_U_EXPR_BREAK:
    case CM_U_EXPR_CONTINUE: {
        CmUMirBlock *current;
        CmUMirBlockId target;
        if (expr->kind == CM_U_EXPR_BREAK
            && expr->data.flow.value != CM_U_EXPR_NONE)
            (void)cm_umir_emit_expr(builder, expr->data.flow.value);
        if (builder->loop_depth == 0u) break;
        target = expr->kind == CM_U_EXPR_BREAK
            ? builder->loop_exits[builder->loop_depth - 1u]
            : builder->loop_headers[builder->loop_depth - 1u];
        current = (CmUMirBlock *)cm_vec_at(&builder->body->blocks,
            builder->current);
        if (current != NULL) {
            current->terminator = CM_UMIR_TERMINATOR_GOTO;
            current->goto_target = target;
        }
        break;
    }
    case CM_U_EXPR_TRY: {
        CmUMirLocalId operand = cm_umir_emit_expr(builder,
            expr->data.try_expr.operand);
        CmUMirBlockId return_block = cm_umir_new_block(builder);
        CmUMirBlockId continue_block = cm_umir_new_block(builder);
        CmUMirBlock *current = (CmUMirBlock *)cm_vec_at(
            &builder->body->blocks, builder->current);
        if (current != NULL) {
            /* `Option`: None (0) returns; `Result`: Err (1) returns. */
            CmUMirBlockId *targets = (CmUMirBlockId *)cm_alloc_zeroed(1u,
                sizeof(*targets));
            long *discriminants = (long *)cm_alloc_zeroed(1u,
                sizeof(*discriminants));
            targets[0] = return_block;
            discriminants[0] = cm_umir_type_is_option(builder,
                cm_umir_expr_type(builder, expr->data.try_expr.operand))
                ? 0 : 1;
            current->terminator = CM_UMIR_TERMINATOR_SWITCH;
            current->condition = operand;
            current->goto_target = continue_block;
            current->arm_targets = targets;
            current->arm_discriminants = discriminants;
            current->arm_count = 1u;
        }
        {
            CmUMirBlock *ret = (CmUMirBlock *)cm_vec_at(
                &builder->body->blocks, return_block);
            builder->current = return_block;
            /* The propagated value: the operand itself (From conversion
             * of the error is identity in the slot model). */
            cm_umir_push_operands(builder, 0u, CM_UMIR_RVALUE_LOCAL, id,
                CM_TY_NONE, &operand, 1u);
            ret = (CmUMirBlock *)cm_vec_at(&builder->body->blocks,
                return_block);
            if (ret != NULL)
                ret->terminator = CM_UMIR_TERMINATOR_RETURN;
        }
        builder->current = continue_block;
        cm_umir_push_operands(builder, destination,
            CM_UMIR_RVALUE_TRY_UNWRAP, id, type, &operand, 1u);
        break;
    }
    case CM_U_EXPR_LET: {
        /* `let PAT = e` as a condition: 1 with the pattern bound when the
         * value matches, else 0. */
        CmUMirLocalId scrutinee = cm_umir_emit_expr(builder,
            expr->data.let_expr.initializer);
        CmUMirBlockId fail;
        CmUMirBlockId join = cm_umir_new_block(builder);
        cm_umir_emit_pattern_test(builder, expr->data.let_expr.pattern,
            scrutinee, id, &fail);
        cm_umir_push_immediate(builder, destination, CM_UMIR_RVALUE_LITERAL,
            id, type, NULL, 0u, 1u);
        cm_umir_seal_goto(builder, join);
        builder->current = fail;
        cm_umir_push_immediate(builder, destination, CM_UMIR_RVALUE_LITERAL,
            id, type, NULL, 0u, 0u);
        cm_umir_seal_goto(builder, join);
        builder->current = join;
        break;
    }
    case CM_U_EXPR_FOR: {
        CmUMirBlockId header;
        CmUMirBlockId body_block;
        CmUMirBlockId exit_block;
        CmUMirLocalId element;
        CmUMirLocalId item;
        CmUMirLocalId iterable = cm_umir_emit_expr(builder,
            expr->data.for_expr.iterable);
        header = cm_umir_new_block(builder);
        body_block = cm_umir_new_block(builder);
        exit_block = cm_umir_new_block(builder);
        cm_umir_seal_goto(builder, header);
        builder->current = header;
        /* `Iterator::next(&mut iterable)` yields an Option: `Some` (slot 0
         * = 1) continues with the payload bound to the loop pattern. */
        element = cm_umir_new_local(builder, CM_TY_NONE);
        cm_umir_push_operands(builder, element, CM_UMIR_RVALUE_ITER_NEXT,
            id, type, &iterable, 1u);
        {
            CmUMirBlock *current = (CmUMirBlock *)cm_vec_at(
                &builder->body->blocks, builder->current);
            if (current != NULL) {
                CmUMirBlockId *targets = (CmUMirBlockId *)cm_alloc_zeroed(1u,
                    sizeof(*targets));
                long *discriminants = (long *)cm_alloc_zeroed(1u,
                    sizeof(*discriminants));
                targets[0] = body_block;
                discriminants[0] = 1;
                current->terminator = CM_UMIR_TERMINATOR_SWITCH;
                current->condition = element;
                current->goto_target = exit_block;
                current->arm_targets = targets;
                current->arm_discriminants = discriminants;
                current->arm_count = 1u;
            }
        }
        builder->current = body_block;
        item = cm_umir_new_local(builder, builder->tb != NULL
            && builder->tb->pat_types != NULL
            ? builder->tb->pat_types[expr->data.for_expr.pattern]
            : CM_TY_NONE);
        cm_umir_push_immediate(builder, item, CM_UMIR_RVALUE_SLOT, id,
            CM_TY_NONE, &element, 1u, 1u);
        cm_umir_bind_pattern(builder, expr->data.for_expr.pattern, item, id);
        if (builder->loop_depth < 64u) {
            builder->loop_headers[builder->loop_depth] = header;
            builder->loop_exits[builder->loop_depth] = exit_block;
            builder->loop_depth += 1u;
        }
        builder->current = body_block;
        (void)cm_umir_emit_expr(builder, expr->data.for_expr.body);
        if (builder->loop_depth != 0u) builder->loop_depth -= 1u;
        cm_umir_seal_goto(builder, header);
        builder->current = exit_block;
        break;
    }
    case CM_U_EXPR_CLOSURE:
        cm_umir_push(builder, destination, CM_UMIR_RVALUE_CLOSURE, id,
            type);
        break;
    case CM_U_EXPR_ASM:
        cm_umir_push(builder, destination, CM_UMIR_RVALUE_ASM, id, type);
        break;
    case CM_U_EXPR_OFFSET_OF:
        cm_umir_push(builder, destination, CM_UMIR_RVALUE_OFFSET_OF, id,
            type);
        break;
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
    if (builder->tb != NULL && builder->tb->unsize_targets != NULL
        && builder->tb->unsize_targets[id] != CM_TY_NONE
        && builder->blocked == NULL) {
        /* The value coerces to a trait object here: build the pair. */
        CmTyId target = builder->tb->unsize_targets[id];
        CmUMirLocalId fat = cm_umir_new_local(builder, target);
        cm_umir_push_operands(builder, fat, CM_UMIR_RVALUE_UNSIZE, id,
            target, &destination, 1u);
        destination = fat;
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
            if (block != NULL) {
                cm_vec_destroy(&block->statements);
                cm_free(block->arm_targets);
                cm_free(block->arm_discriminants);
            }
        }
        cm_vec_destroy(&body->blocks);
        cm_vec_destroy(&body->locals);
    }
    cm_vec_destroy(&set->bodies);
}

/* Build the u-MIR body of one closure expression: the enclosing body's
 * locals are aliased as the environment, then a return slot and the
 * closure's own temporaries follow. */
static void cm_mir_ulower_closure(CmUMirSet *out, const CmHirContext *hir,
    const CmUBodySet *bodies, const CmTyckSet *tyck, const CmUBody *ub,
    const CmTyckBody *tb, CmHirBodyId body_id, CmUExprId closure_id,
    CmMirULowerResult *result)
{
    const CmUExpr *closure = cm_ubody_get_expr(ub, closure_id);
    CmUMirBody body;
    CmUMirBuilder builder;
    CmTyId return_type;
    size_t local_index;
    if (closure == NULL || closure->kind != CM_U_EXPR_CLOSURE) return;
    memset(&body, 0, sizeof(body));
    body.source = body_id;
    body.closure_expr = closure_id;
    body.env_count = (uint32_t)(1u + ub->locals.len);
    cm_vec_init(&body.locals, sizeof(CmTyId));
    cm_vec_init(&body.blocks, sizeof(CmUMirBlock));
    /* Environment slots mirror the enclosing frame exactly. */
    return_type = tb->return_type;
    (void)cm_vec_push(&body.locals, &return_type);
    for (local_index = 0u; local_index < ub->locals.len; ++local_index) {
        CmTyId local_type = tb->local_types == NULL ? CM_TY_NONE
            : tb->local_types[local_index];
        (void)cm_vec_push(&body.locals, &local_type);
    }
    /* Closure return slot. */
    return_type = tb->expr_types == NULL ? CM_TY_NONE
        : tb->expr_types[closure->data.closure.body];
    (void)cm_vec_push(&body.locals, &return_type);
    memset(&builder, 0, sizeof(builder));
    builder.hir = hir;
    builder.ubodies = bodies;
    builder.tyck = tyck;
    builder.body = &body;
    builder.ub = ub;
    builder.tb = tb;
    builder.census = result;
    builder.current = cm_umir_new_block(&builder);
    {
        CmUMirLocalId root_local = cm_umir_emit_expr(&builder,
            closure->data.closure.body);
        if (builder.blocked == NULL)
            cm_umir_push_operands(&builder, body.env_count,
                CM_UMIR_RVALUE_LOCAL, closure->data.closure.body,
                return_type, &root_local, 1u);
    }
    {
        size_t seal_index;
        for (seal_index = 0u; seal_index < body.blocks.len; ++seal_index) {
            CmUMirBlock *block = (CmUMirBlock *)cm_vec_at(&body.blocks,
                seal_index);
            if (block != NULL && block->terminator == CM_UMIR_TERMINATOR_NONE)
                block->terminator = CM_UMIR_TERMINATOR_RETURN;
        }
    }
    body.complete = builder.blocked == NULL;
    (void)cm_vec_push(&out->bodies, &body);
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
        builder.hir = hir;
        builder.ubodies = bodies;
        builder.tyck = tyck;
        builder.body = &body;
        builder.ub = ub;
        builder.tb = tb;
        builder.census = &result;
        return_type = tb->return_type;
        (void)cm_vec_push(&body.locals, &return_type);
        {
            size_t local_index;
            for (local_index = 0u; local_index < ub->locals.len;
                    ++local_index) {
                CmTyId local_type = tb->local_types == NULL ? CM_TY_NONE
                    : tb->local_types[local_index];
                (void)cm_vec_push(&body.locals, &local_type);
            }
        }
        builder.current = cm_umir_new_block(&builder);
        {
            CmUMirLocalId root_local = cm_umir_emit_expr(&builder,
                ub->root);
            if (builder.blocked == NULL && root_local != 0u)
                cm_umir_push_operands(&builder, 0u, CM_UMIR_RVALUE_LOCAL,
                    ub->root, return_type, &root_local, 1u);
        }
        {
            size_t seal_index;
            for (seal_index = 0u; seal_index < body.blocks.len;
                    ++seal_index) {
                CmUMirBlock *block = (CmUMirBlock *)cm_vec_at(
                    &body.blocks, seal_index);
                if (block != NULL
                    && block->terminator == CM_UMIR_TERMINATOR_NONE)
                    block->terminator = CM_UMIR_TERMINATOR_RETURN;
            }
        }
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
        /* One extra body per closure expression in this body. */
        {
            size_t expr_index;
            for (expr_index = 1u; expr_index <= ub->expressions.len;
                    ++expr_index) {
                const CmUExpr *candidate = cm_ubody_get_expr(ub,
                    (CmUExprId)expr_index);
                if (candidate != NULL
                    && candidate->kind == CM_U_EXPR_CLOSURE)
                    cm_mir_ulower_closure(out, hir, bodies, tyck, ub, tb,
                        (CmHirBodyId)body_index, (CmUExprId)expr_index,
                        &result);
            }
        }
    }
    return result;
}
