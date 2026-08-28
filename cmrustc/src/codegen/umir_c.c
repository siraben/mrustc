#include "cm/codegen/umir_c.h"

#include <string.h>
#include "cm/hir/model.h"

/*
 * M9-06 v1: dry C-emission over u-MIR — no output buffer yet.  Every
 * statement is classified: renderable as C with the current type and
 * rvalue vocabulary, or counted as a `cemit-*` gap class.  The census
 * names the emitter's next family, exactly like the tyck and ulower
 * loops before it.
 */

static void cm_umir_c_count(CmUMirCEmitResult *result, const char *reason)
{
    size_t index;
    for (index = 0u; index < result->class_count; ++index)
        if (strcmp(result->classes[index].reason, reason) == 0) {
            result->classes[index].count += 1u;
            return;
        }
    if (result->class_count < CM_UMIR_CEMIT_CLASSES) {
        result->classes[result->class_count].reason = reason;
        result->classes[result->class_count].count = 1u;
        result->class_count += 1u;
    }
}

/* v1 C type vocabulary: scalars, unit, and pointers/references render;
 * everything else is a named gap. */
static const char *cm_umir_c_type_gap(const CmTyArena *arena, CmTyId type,
    unsigned int depth)
{
    const CmTy *ty;
    if (depth > 8u) return "ctype-depth";
    if (type == CM_TY_NONE) return "ctype-none";
    ty = cm_ty_get((CmTyArena *)arena, cm_ty_resolve((CmTyArena *)arena,
        type));
    if (ty == NULL) return "ctype-none";
    switch (ty->kind) {
    case CM_TY_BOOL:
    case CM_TY_CHAR:
    case CM_TY_INT:
    case CM_TY_FLOAT:
    case CM_TY_NEVER:
        return NULL;
    case CM_TY_TUPLE:
    case CM_TY_REF:
    case CM_TY_PTR:
    case CM_TY_ADT:
    case CM_TY_ARRAY:
    case CM_TY_FN_DEF:
    case CM_TY_FN_PTR:
    case CM_TY_CLOSURE:
        /* Every concrete nominal/structural type is C-nameable through
         * a mangled typedef; slices and str are ptr+len pairs. */
        return NULL;
    case CM_TY_SLICE:
    case CM_TY_STR:
        return NULL;
    case CM_TY_PARAM:
    case CM_TY_SELF:
        /* Disappears after instance collection; informational. */
        return "needs-mono-generic";
    case CM_TY_PROJECTION:
        return "needs-mono-projection";
    case CM_TY_DYN:
        return "ctype-dyn";
    case CM_TY_INFER:
        return "ctype-infer";
    case CM_TY_ERROR:
        return "ctype-error";
    default:
        return "ctype-other";
    }
}

CmUMirCEmitResult cm_umir_c_emit_dry(const CmUMirSet *umir,
    const CmTyckSet *tyck)
{
    CmUMirCEmitResult result;
    size_t body_index;
    memset(&result, 0, sizeof(result));
    if (umir == NULL || tyck == NULL) return result;
    for (body_index = 0u; body_index < umir->bodies.len; ++body_index) {
        const CmUMirBody *body = (const CmUMirBody *)cm_vec_at_const(
            &umir->bodies, body_index);
        size_t block_index;
        int clean = 1;
        if (body == NULL || !body->complete) continue;
        result.bodies += 1u;
        for (block_index = 0u; block_index < body->blocks.len;
                ++block_index) {
            const CmUMirBlock *block = (const CmUMirBlock *)
                cm_vec_at_const(&body->blocks, block_index);
            size_t statement_index;
            if (block == NULL) continue;
            for (statement_index = 0u;
                    statement_index < block->statements.len;
                    ++statement_index) {
                const CmUMirStatement *statement =
                    (const CmUMirStatement *)cm_vec_at_const(
                        &block->statements, statement_index);
                const char *gap;
                if (statement == NULL) continue;
                result.statements += 1u;
                gap = cm_umir_c_type_gap(&tyck->arena, statement->type,
                    0u);
                if (gap == NULL) {
                    switch (statement->kind) {
                    case CM_UMIR_RVALUE_LITERAL:
                    case CM_UMIR_RVALUE_LOCAL:
                    case CM_UMIR_RVALUE_BINARY:
                    case CM_UMIR_RVALUE_UNARY:
                    case CM_UMIR_RVALUE_REF:
                    case CM_UMIR_RVALUE_CAST:
                    case CM_UMIR_RVALUE_ASSIGN:
                        break;
                    case CM_UMIR_RVALUE_CALL:
                    case CM_UMIR_RVALUE_METHOD_CALL:
                        /* Callee symbols are mangled instance names. */
                        break;
                    case CM_UMIR_RVALUE_FIELD:
                    case CM_UMIR_RVALUE_INDEX:
                    case CM_UMIR_RVALUE_AGGREGATE:
                        /* Renders through the layout engine's member
                         * names. */
                        break;
                    case CM_UMIR_RVALUE_TRY_UNWRAP:
                    case CM_UMIR_RVALUE_ITER_NEXT:
                        /* Discriminant reads + payload extraction. */
                        break;
                    case CM_UMIR_RVALUE_CLOSURE:
                        gap = "cemit-closure";
                        break;
                    case CM_UMIR_RVALUE_ASM:
                        gap = "cemit-asm";
                        break;
                    case CM_UMIR_RVALUE_OFFSET_OF:
                        gap = "cemit-offset-of";
                        break;
                    case CM_UMIR_RVALUE_OPAQUE:
                    default:
                        gap = "cemit-opaque";
                        break;
                    }
                }
                if (gap != NULL) {
                    clean = 0;
                    cm_umir_c_count(&result, gap);
                } else {
                    result.rendered += 1u;
                }
            }
        }
        if (clean) result.emitted += 1u;
    }
    return result;
}

/* ------------------------------------------------------------------ */
/* v1 text rendering                                                    */

static void cm_umir_c_render_type(CmStrBuf *output, const CmTyArena *arena,
    CmTyId type)
{
    const CmTy *ty = type == CM_TY_NONE ? NULL
        : cm_ty_get((CmTyArena *)arena, cm_ty_resolve((CmTyArena *)arena,
            type));
    /* v1: every local is one uniform integer slot so copies between
     * locals never need casts; the layout engine refines widths and
     * pointer-ness later. */
    (void)ty;
    cm_str_buf_append(output, "long long");
}

static void cm_umir_c_render_number(CmStrBuf *output, unsigned long value)
{
    char digits[24];
    int length = 0;
    if (value == 0u) { cm_str_buf_push(output, '0'); return; }
    while (value != 0u && length < 24) {
        digits[length++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (length != 0) cm_str_buf_push(output, digits[--length]);
}

static void cm_umir_c_render_local(CmStrBuf *output, CmUMirLocalId local)
{
    cm_str_buf_append(output, "_l");
    cm_umir_c_render_number(output, (unsigned long)local);
}

static void cm_umir_c_render_symbol(CmStrBuf *output, CmHirDefId def)
{
    cm_str_buf_append(output, "cm_u_");
    cm_umir_c_render_number(output, (unsigned long)def.crate_id);
    cm_str_buf_push(output, '_');
    cm_umir_c_render_number(output, (unsigned long)def.index);
}

/* `{ long long sym(); _dest = sym(args...); }` — the block-scope
 * prototype keeps every call site self-contained for the syntax check;
 * definitions are linked once instances are collected. */
static int cm_umir_c_render_call(CmStrBuf *output,
    const CmUMirStatement *statement, CmHirDefId def, uint32_t first_arg)
{
    uint32_t index;
    if (cm_hir_def_id_is_none(def) || statement->operand_overflow != 0u)
        return 0;
    cm_str_buf_append(output, "0; { long long ");
    cm_umir_c_render_symbol(output, def);
    cm_str_buf_append(output, "(); ");
    cm_umir_c_render_local(output, statement->destination);
    cm_str_buf_append(output, " = ");
    cm_umir_c_render_symbol(output, def);
    cm_str_buf_push(output, '(');
    for (index = first_arg; index < statement->operand_count; ++index) {
        if (index != first_arg) cm_str_buf_append(output, ", ");
        cm_umir_c_render_local(output, statement->operands[index]);
    }
    cm_str_buf_append(output, "); }");
    return 1;
}

/* ABI type at an exported boundary: exact C integer widths. */
static const char *cm_umir_c_abi_type(const CmTyArena *arena, CmTyId type)
{
    const CmTy *ty = type == CM_TY_NONE ? NULL
        : cm_ty_get((CmTyArena *)arena, cm_ty_resolve((CmTyArena *)arena,
            type));
    if (ty == NULL) return "long long";
    switch (ty->kind) {
    case CM_TY_BOOL: return "uint8_t";
    case CM_TY_CHAR: return "uint32_t";
    case CM_TY_TUPLE: return ty->count == 0u ? "void" : "long long";
    case CM_TY_INT:
        switch ((CmHirIntType)ty->a) {
        case CM_HIR_INT_I8: return "int8_t";
        case CM_HIR_INT_I16: return "int16_t";
        case CM_HIR_INT_I32: return "int32_t";
        case CM_HIR_INT_I64: return "int64_t";
        case CM_HIR_INT_ISIZE: return "intptr_t";
        case CM_HIR_INT_U8: return "uint8_t";
        case CM_HIR_INT_U16: return "uint16_t";
        case CM_HIR_INT_U32: return "uint32_t";
        case CM_HIR_INT_U64: return "uint64_t";
        case CM_HIR_INT_USIZE: return "uintptr_t";
        default: return "long long";
        }
    default: return "long long";
    }
}

static int cm_umir_c_item_has_attribute(const CmHirContext *hir,
    const CmHirItem *item, const char *name)
{
    uint32_t index;
    size_t length = strlen(name);
    for (index = 0u; index < item->attribute_count; ++index) {
        const CmInternedString *text = cm_interner_get(&hir->strings,
            item->attributes[index].metadata);
        if (text != NULL && text->len == length
            && memcmp(text->bytes, name, length) == 0) return 1;
    }
    return 0;
}

static const char *cm_umir_c_binary_operator(CmUBinaryOp op)
{
    switch (op) {
    case CM_U_BINARY_ADD: return "+";
    case CM_U_BINARY_SUB: return "-";
    case CM_U_BINARY_MUL: return "*";
    case CM_U_BINARY_DIV: return "/";
    case CM_U_BINARY_REM: return "%";
    case CM_U_BINARY_AND: return "&&";
    case CM_U_BINARY_OR: return "||";
    case CM_U_BINARY_BIT_AND: return "&";
    case CM_U_BINARY_BIT_OR: return "|";
    case CM_U_BINARY_BIT_XOR: return "^";
    case CM_U_BINARY_SHL: return "<<";
    case CM_U_BINARY_SHR: return ">>";
    case CM_U_BINARY_EQ: return "==";
    case CM_U_BINARY_NE: return "!=";
    case CM_U_BINARY_LT: return "<";
    case CM_U_BINARY_LE: return "<=";
    case CM_U_BINARY_GT: return ">";
    case CM_U_BINARY_GE: return ">=";
    default: return "+";
    }
}

static CmTyId cm_umir_c_local_type(const CmUMirBody *body,
    CmUMirLocalId local)
{
    const CmTyId *type = (const CmTyId *)cm_vec_at_const(&body->locals,
        local);
    return type == NULL ? CM_TY_NONE : *type;
}

/* Declared field index of `name` in the ADT behind `type`, or -1. */
static long cm_umir_c_field_index(const CmHirContext *hir,
    const CmTyckSet *tyck, const CmUBodySet *ubodies, CmTyId type,
    CmInternId name)
{
    const CmTy *ty = type == CM_TY_NONE ? NULL
        : cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena, type));
    const CmHirDefinition *record;
    const CmHirItem *item;
    const CmInternedString *wanted;
    uint32_t index;
    while (ty != NULL && (ty->kind == CM_TY_REF || ty->kind == CM_TY_PTR))
        ty = cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena, ty->children[0]));
    if (ty == NULL || ty->kind != CM_TY_ADT) return -1;
    record = cm_hir_lookup_definition(hir, ty->def);
    if (record == NULL || record->kind != CM_HIR_DEFINITION_ITEM) return -1;
    item = cm_hir_get_item(hir, record->entity.item_id);
    if (item == NULL || (item->kind != CM_HIR_ITEM_STRUCT
            && item->kind != CM_HIR_ITEM_UNION)) return -1;
    wanted = cm_interner_get(&ubodies->strings, name);
    if (wanted == NULL) return -1;
    for (index = 0u; index < item->data.aggregate_item.field_count;
            ++index) {
        const CmInternedString *have = cm_interner_get(&hir->strings,
            item->data.aggregate_item.fields[index].name);
        if (have != NULL && have->len == wanted->len
            && memcmp(have->bytes, wanted->bytes, have->len) == 0)
            return (long)index;
    }
    return -1;
}

static long cm_umir_c_variant_field_index(const CmHirContext *hir,
    const CmUResolution *res, CmInternId name, const CmUBodySet *ubodies)
{
    const CmHirDefinition *record;
    const CmHirItem *item;
    const CmHirVariant *variant;
    const CmInternedString *wanted;
    uint32_t index;
    if (res->kind != CM_U_RESOLVED_VARIANT) return -1;
    record = cm_hir_lookup_definition(hir, res->definition);
    if (record == NULL || record->kind != CM_HIR_DEFINITION_ENUM_VARIANT)
        return -1;
    item = cm_hir_get_item(hir, record->entity.enum_variant.enum_item_id);
    if (item == NULL || item->kind != CM_HIR_ITEM_ENUM
        || record->entity.enum_variant.variant_index
            >= item->data.enum_item.variant_count) return -1;
    variant = &item->data.enum_item.variants[
        record->entity.enum_variant.variant_index];
    wanted = cm_interner_get(&ubodies->strings, name);
    if (wanted == NULL) return -1;
    for (index = 0u; index < variant->field_count; ++index) {
        const CmInternedString *have = cm_interner_get(&hir->strings,
            variant->fields[index].name);
        if (have != NULL && have->len == wanted->len
            && memcmp(have->bytes, wanted->bytes, have->len) == 0)
            return (long)index;
    }
    return -1;
}

int cm_umir_c_render_body(CmStrBuf *output, const CmHirContext *hir,
    const CmUMirBody *body, const CmUBodySet *ubodies, const CmUBody *ub,
    const CmTyckSet *tyck)
{
    size_t block_index;
    size_t local_index;
    uint32_t param;
    int complete = 1;
    const CmHirBody *hir_body;
    const CmHirItem *owner = NULL;
    const CmTyckBody *tb;
    CmHirDefId def;
    if (output == NULL || hir == NULL || body == NULL || ub == NULL
        || tyck == NULL) return 0;
    hir_body = cm_hir_get_body(hir, body->source);
    if (hir_body == NULL) return 0;
    def = hir_body->origin.definition;
    {
        const CmHirDefinition *record = cm_hir_lookup_definition(hir, def);
        if (record != NULL && record->kind == CM_HIR_DEFINITION_ITEM)
            owner = cm_hir_get_item(hir, record->entity.item_id);
    }
    tb = cm_tyck_get(tyck, body->source);
    /* Definition: uniform long long slots; parameters arrive as p<i>. */
    cm_str_buf_append(output, "long long ");
    cm_umir_c_render_symbol(output, def);
    cm_str_buf_push(output, '(');
    if (ub->parameter_count == 0u) cm_str_buf_append(output, "void");
    for (param = 0u; param < ub->parameter_count; ++param) {
        if (param != 0u) cm_str_buf_append(output, ", ");
        cm_str_buf_append(output, "long long p");
        cm_umir_c_render_number(output, (unsigned long)param);
    }
    cm_str_buf_append(output, ")\n{\n");
    for (local_index = 0u; local_index < body->locals.len;
            ++local_index) {
        const CmTyId *type = (const CmTyId *)cm_vec_at_const(
            &body->locals, local_index);
        cm_str_buf_append(output, "    ");
        cm_umir_c_render_type(output, &tyck->arena,
            type == NULL ? CM_TY_NONE : *type);
        cm_str_buf_push(output, ' ');
        cm_umir_c_render_local(output, (CmUMirLocalId)local_index);
        cm_str_buf_append(output, " = 0;\n");
    }
    /* Aggregates live as function-scope slot arrays (M9 leniency: an
     * aggregate value is a pointer to its declared-order field slots). */
    for (block_index = 0u; block_index < body->blocks.len;
            ++block_index) {
        const CmUMirBlock *block = (const CmUMirBlock *)cm_vec_at_const(
            &body->blocks, block_index);
        size_t statement_index;
        if (block == NULL) continue;
        for (statement_index = 0u;
                statement_index < block->statements.len;
                ++statement_index) {
            const CmUMirStatement *statement =
                (const CmUMirStatement *)cm_vec_at_const(
                    &block->statements, statement_index);
            unsigned long slots;
            if (statement == NULL
                || (statement->kind != CM_UMIR_RVALUE_AGGREGATE
                    && statement->kind != CM_UMIR_RVALUE_VARIANT)) continue;
            slots = (unsigned long)statement->operand_count
                + (unsigned long)statement->operand_overflow
                + (statement->kind == CM_UMIR_RVALUE_VARIANT ? 1u : 0u);
            cm_str_buf_append(output, "    long long _agg");
            cm_umir_c_render_number(output,
                (unsigned long)statement->destination);
            cm_str_buf_append(output, "[");
            cm_umir_c_render_number(output, slots == 0u ? 1u : slots);
            cm_str_buf_append(output, "];\n");
        }
    }
    /* Prologue: bind parameter patterns to their local slots. */
    for (param = 0u; param < ub->parameter_count; ++param) {
        const CmUPat *pat = cm_ubody_get_pat(ub, ub->parameters[param]);
        if (pat == NULL || pat->kind != CM_U_PAT_BINDING
            || pat->data.binding.local == CM_U_LOCAL_NONE) continue;
        cm_str_buf_append(output, "    ");
        cm_umir_c_render_local(output,
            (CmUMirLocalId)(1u + pat->data.binding.local));
        cm_str_buf_append(output, " = p");
        cm_umir_c_render_number(output, (unsigned long)param);
        cm_str_buf_append(output, ";\n");
    }
    for (block_index = 0u; block_index < body->blocks.len;
            ++block_index) {
        const CmUMirBlock *block = (const CmUMirBlock *)cm_vec_at_const(
            &body->blocks, block_index);
        size_t statement_index;
        if (block == NULL) continue;
        cm_str_buf_append(output, "_b");
        cm_umir_c_render_number(output, (unsigned long)block_index);
        cm_str_buf_append(output, ": ;\n");
        for (statement_index = 0u;
                statement_index < block->statements.len;
                ++statement_index) {
            const CmUMirStatement *statement =
                (const CmUMirStatement *)cm_vec_at_const(
                    &block->statements, statement_index);
            const CmUExpr *expr;
            if (statement == NULL) continue;
            cm_str_buf_append(output, "    ");
            cm_umir_c_render_local(output, statement->destination);
            cm_str_buf_append(output, " = ");
            expr = cm_ubody_get_expr(ub, statement->expr);
            switch (statement->kind) {
            case CM_UMIR_RVALUE_LITERAL:
                if (expr != NULL
                    && expr->kind == CM_U_EXPR_LITERAL
                    && (expr->data.literal.kind == CM_U_LITERAL_INTEGER
                        || expr->data.literal.kind == CM_U_LITERAL_BOOL))
                    cm_umir_c_render_number(output, (unsigned long)
                        expr->data.literal.value_low);
                else {
                    cm_str_buf_append(output, "0 /* literal */");
                    complete = 0;
                }
                break;
            case CM_UMIR_RVALUE_LOCAL:
                if (statement->operand_count == 1u)
                    cm_umir_c_render_local(output,
                        statement->operands[0]);
                else {
                    cm_str_buf_append(output, "0 /* item path */");
                    complete = 0;
                }
                break;
            case CM_UMIR_RVALUE_BINARY:
                if (statement->operand_count == 2u && expr != NULL
                    && (expr->kind == CM_U_EXPR_BINARY
                        || expr->kind == CM_U_EXPR_ASSIGN_OP)) {
                    /* Operands compute at their own ABI width so
                     * wrapping, signedness, and comparisons are exact. */
                    const char *operand_abi = cm_umir_c_abi_type(
                        &tyck->arena, cm_umir_c_local_type(body,
                            statement->operands[0]));
                    CmUBinaryOp op = expr->kind == CM_U_EXPR_BINARY
                        ? expr->data.binary.op : expr->data.assign.op;
                    if (strcmp(operand_abi, "void") == 0)
                        operand_abi = "long long";
                    cm_str_buf_append(output, "(long long)((");
                    cm_str_buf_append(output, operand_abi);
                    cm_str_buf_push(output, ')');
                    cm_umir_c_render_local(output,
                        statement->operands[0]);
                    cm_str_buf_push(output, ' ');
                    cm_str_buf_append(output,
                        cm_umir_c_binary_operator(op));
                    cm_str_buf_append(output, " (");
                    cm_str_buf_append(output, operand_abi);
                    cm_str_buf_push(output, ')');
                    cm_umir_c_render_local(output,
                        statement->operands[1]);
                    cm_str_buf_push(output, ')');
                } else {
                    cm_str_buf_append(output, "0 /* binary */");
                    complete = 0;
                }
                break;
            case CM_UMIR_RVALUE_UNARY:
                if (statement->operand_count == 1u && expr != NULL
                    && expr->kind == CM_U_EXPR_UNARY) {
                    const char *operand_abi = cm_umir_c_abi_type(
                        &tyck->arena, cm_umir_c_local_type(body,
                            statement->operands[0]));
                    if (strcmp(operand_abi, "void") == 0)
                        operand_abi = "long long";
                    /* `(long long)(OP (abi)local)` — balanced by
                     * construction. */
                    cm_str_buf_append(output, "(long long)(");
                    if (expr->data.unary.op == CM_U_UNARY_NEG)
                        cm_str_buf_push(output, '-');
                    else if (expr->data.unary.op == CM_U_UNARY_NOT)
                        cm_str_buf_push(output,
                            strcmp(operand_abi, "uint8_t") == 0
                                ? '!' : '~');
                    cm_str_buf_push(output, '(');
                    cm_str_buf_append(output, operand_abi);
                    cm_str_buf_push(output, ')');
                    cm_umir_c_render_local(output,
                        statement->operands[0]);
                    cm_str_buf_push(output, ')');
                } else {
                    cm_str_buf_append(output, "0 /* unary */");
                    complete = 0;
                }
                break;
            case CM_UMIR_RVALUE_AGGREGATE: {
                uint32_t field;
                uint32_t slot_count = statement->operand_count;
                int mapped = statement->operand_overflow == 0u;
                cm_str_buf_append(output, "0; ");
                for (field = 0u; field < slot_count && mapped; ++field) {
                    long slot = (long)field;
                    if (expr != NULL && expr->kind == CM_U_EXPR_STRUCT
                        && field < expr->data.struct_expr.field_count) {
                        slot = cm_umir_c_field_index(hir, tyck, ubodies,
                            statement->type,
                            expr->data.struct_expr.fields[field].name);
                        if (slot < 0) { mapped = 0; break; }
                    }
                    cm_str_buf_append(output, "_agg");
                    cm_umir_c_render_number(output,
                        (unsigned long)statement->destination);
                    cm_str_buf_push(output, '[');
                    cm_umir_c_render_number(output, (unsigned long)slot);
                    cm_str_buf_append(output, "] = ");
                    cm_umir_c_render_local(output,
                        statement->operands[field]);
                    cm_str_buf_append(output, "; ");
                }
                if (!mapped) {
                    cm_str_buf_append(output, "/* aggregate */");
                    complete = 0;
                }
                cm_umir_c_render_local(output, statement->destination);
                cm_str_buf_append(output, " = (long long)(intptr_t)_agg");
                cm_umir_c_render_number(output,
                    (unsigned long)statement->destination);
                break;
            }
            case CM_UMIR_RVALUE_VARIANT: {
                uint32_t field;
                int mapped = statement->operand_overflow == 0u;
                cm_str_buf_append(output, "0; _agg");
                cm_umir_c_render_number(output,
                    (unsigned long)statement->destination);
                cm_str_buf_append(output, "[0] = ");
                cm_umir_c_render_number(output,
                    (unsigned long)statement->immediate);
                cm_str_buf_append(output, "; ");
                for (field = 0u; field < statement->operand_count && mapped;
                        ++field) {
                    long slot = (long)field;
                    if (expr != NULL && expr->kind == CM_U_EXPR_STRUCT
                        && field < expr->data.struct_expr.field_count) {
                        slot = cm_umir_c_variant_field_index(hir,
                            &expr->data.struct_expr.resolution,
                            expr->data.struct_expr.fields[field].name,
                            ubodies);
                        if (slot < 0) { mapped = 0; break; }
                    }
                    cm_str_buf_append(output, "_agg");
                    cm_umir_c_render_number(output,
                        (unsigned long)statement->destination);
                    cm_str_buf_push(output, '[');
                    cm_umir_c_render_number(output, 1u + (unsigned long)slot);
                    cm_str_buf_append(output, "] = ");
                    cm_umir_c_render_local(output,
                        statement->operands[field]);
                    cm_str_buf_append(output, "; ");
                }
                if (!mapped) {
                    cm_str_buf_append(output, "/* variant */");
                    complete = 0;
                }
                cm_umir_c_render_local(output, statement->destination);
                cm_str_buf_append(output, " = (long long)(intptr_t)_agg");
                cm_umir_c_render_number(output,
                    (unsigned long)statement->destination);
                break;
            }
            case CM_UMIR_RVALUE_SLOT:
                cm_str_buf_append(output, "((long long *)(intptr_t)");
                cm_umir_c_render_local(output, statement->operands[0]);
                cm_str_buf_append(output, ")[");
                cm_umir_c_render_number(output,
                    (unsigned long)statement->immediate);
                cm_str_buf_push(output, ']');
                break;
            case CM_UMIR_RVALUE_FIELD: {
                long slot = -1;
                if (statement->operand_count == 1u && expr != NULL) {
                    if (expr->kind == CM_U_EXPR_TUPLE_FIELD)
                        slot = (long)expr->data.tuple_field.index;
                    else if (expr->kind == CM_U_EXPR_FIELD)
                        slot = cm_umir_c_field_index(hir, tyck, ubodies,
                            cm_umir_c_local_type(body,
                                statement->operands[0]),
                            expr->data.field.name);
                }
                if (slot >= 0) {
                    cm_str_buf_append(output, "((long long *)(intptr_t)");
                    cm_umir_c_render_local(output,
                        statement->operands[0]);
                    cm_str_buf_append(output, ")[");
                    cm_umir_c_render_number(output, (unsigned long)slot);
                    cm_str_buf_push(output, ']');
                } else {
                    cm_str_buf_append(output, "0 /* field */");
                    complete = 0;
                }
                break;
            }
            case CM_UMIR_RVALUE_CALL: {
                /* Callee is the first operand's defining PATH. */
                CmHirDefId callee_def = cm_hir_def_id_none();
                const CmUExpr *callee = expr == NULL
                        || expr->kind != CM_U_EXPR_CALL ? NULL
                    : cm_ubody_get_expr(ub, expr->data.call.callee);
                {
                    const CmTyckBody *ctb = cm_tyck_get(tyck, body->source);
                    if (ctb != NULL && ctb->method_targets != NULL
                        && expr != NULL && expr->kind == CM_U_EXPR_CALL)
                        callee_def = ctb->method_targets[expr->data.call.callee];
                }
                if (cm_hir_def_id_is_none(callee_def) && callee != NULL
                    && callee->kind == CM_U_EXPR_PATH
                    && callee->data.path.resolution.kind
                        == CM_U_RESOLVED_DEFINITION)
                    callee_def = callee->data.path.resolution.definition;
                if (cm_hir_def_id_is_none(callee_def) && expr != NULL
                    && expr->kind == CM_U_EXPR_CALL) {
                    /* Any callee shape: its tyck type is a FN_DEF that
                     * names the definition. */
                    const CmTyckBody *ctb = cm_tyck_get(tyck,
                        body->source);
                    if (ctb != NULL && ctb->expr_types != NULL) {
                        const CmTy *callee_ty = cm_ty_get(
                            (CmTyArena *)&tyck->arena,
                            cm_ty_resolve((CmTyArena *)&tyck->arena,
                                ctb->expr_types[expr->data.call.callee]));
                        if (callee_ty != NULL
                            && callee_ty->kind == CM_TY_FN_DEF)
                            callee_def = callee_ty->def;
                    }
                }
                if (!cm_umir_c_render_call(output, statement, callee_def, 1u)) {
                    cm_str_buf_append(output, "0 /* call */");
                    complete = 0;
                }
                break;
            }
            case CM_UMIR_RVALUE_METHOD_CALL: {
                const CmTyckBody *mtb = cm_tyck_get(tyck, body->source);
                CmHirDefId method_def = mtb == NULL
                        || mtb->method_targets == NULL
                    ? cm_hir_def_id_none()
                    : mtb->method_targets[statement->expr];
                if (!cm_umir_c_render_call(output, statement, method_def,
                        0u)) {
                    cm_str_buf_append(output, "0 /* method */");
                    complete = 0;
                }
                break;
            }
            default:
                cm_str_buf_append(output, "0 /* todo */");
                complete = 0;
                break;
            }
            cm_str_buf_append(output, ";\n");
        }
        cm_str_buf_append(output, "    ");
        switch (block->terminator) {
        case CM_UMIR_TERMINATOR_RETURN:
            cm_str_buf_append(output, "return _l0;\n");
            break;
        case CM_UMIR_TERMINATOR_GOTO:
            cm_str_buf_append(output, "goto _b");
            cm_umir_c_render_number(output,
                (unsigned long)block->goto_target);
            cm_str_buf_append(output, ";\n");
            break;
        case CM_UMIR_TERMINATOR_SWITCH: {
            uint32_t arm;
            CmUMirBlockId fallback = block->goto_target;
            cm_str_buf_append(output, "switch ((int)((long long *)(intptr_t)");
            cm_umir_c_render_local(output, block->condition);
            cm_str_buf_append(output, ")[0]) {");
            for (arm = 0u; arm < block->arm_count; ++arm) {
                if (block->arm_discriminants[arm] < 0) {
                    fallback = block->arm_targets[arm];
                    continue;
                }
                cm_str_buf_append(output, " case ");
                cm_umir_c_render_number(output,
                    (unsigned long)block->arm_discriminants[arm]);
                cm_str_buf_append(output, ": goto _b");
                cm_umir_c_render_number(output,
                    (unsigned long)block->arm_targets[arm]);
                cm_str_buf_push(output, ';');
            }
            cm_str_buf_append(output, " default: goto _b");
            cm_umir_c_render_number(output, (unsigned long)fallback);
            cm_str_buf_append(output, "; }\n");
            break;
        }
        case CM_UMIR_TERMINATOR_SWITCH_BOOL:
            cm_str_buf_append(output, "if (");
            cm_umir_c_render_local(output, block->condition);
            cm_str_buf_append(output, ") goto _b");
            cm_umir_c_render_number(output,
                (unsigned long)block->true_target);
            cm_str_buf_append(output, "; else goto _b");
            cm_umir_c_render_number(output,
                (unsigned long)block->false_target);
            cm_str_buf_append(output, ";\n");
            break;
        default:
            cm_str_buf_append(output, "return _l0;\n");
            break;
        }
    }
    cm_str_buf_append(output, "}\n");
    /* `#[no_mangle]` exports: an ABI-typed wrapper with the item name. */
    if (owner != NULL && owner->kind == CM_HIR_ITEM_FUNCTION
        && cm_umir_c_item_has_attribute(hir, owner, "no_mangle")) {
        const CmInternedString *name = cm_interner_get(&hir->strings,
            owner->name);
        const char *ret_abi = tb == NULL ? "long long"
            : cm_umir_c_abi_type(&tyck->arena, tb->return_type);
        if (name != NULL) {
            cm_str_buf_append(output, ret_abi);
            cm_str_buf_push(output, ' ');
            cm_str_buf_append_n(output, (const char *)name->bytes,
                name->len);
            cm_str_buf_push(output, '(');
            if (ub->parameter_count == 0u) cm_str_buf_append(output, "void");
            for (param = 0u; param < ub->parameter_count; ++param) {
                const CmUPat *pat = cm_ubody_get_pat(ub,
                    ub->parameters[param]);
                CmTyId ptype = pat != NULL
                        && pat->kind == CM_U_PAT_BINDING
                        && tb != NULL && tb->local_types != NULL
                        && pat->data.binding.local != CM_U_LOCAL_NONE
                    ? tb->local_types[pat->data.binding.local]
                    : CM_TY_NONE;
                if (param != 0u) cm_str_buf_append(output, ", ");
                cm_str_buf_append(output,
                    cm_umir_c_abi_type(&tyck->arena, ptype));
                cm_str_buf_append(output, " a");
                cm_umir_c_render_number(output, (unsigned long)param);
            }
            cm_str_buf_append(output, ")\n{\n    ");
            if (strcmp(ret_abi, "void") != 0) {
                cm_str_buf_append(output, "return (");
                cm_str_buf_append(output, ret_abi);
                cm_str_buf_push(output, ')');
            }
            cm_umir_c_render_symbol(output, def);
            cm_str_buf_push(output, '(');
            for (param = 0u; param < ub->parameter_count; ++param) {
                if (param != 0u) cm_str_buf_append(output, ", ");
                cm_str_buf_append(output, "(long long)a");
                cm_umir_c_render_number(output, (unsigned long)param);
            }
            cm_str_buf_append(output, ");\n}\n");
        }
    }
    return complete;
}
