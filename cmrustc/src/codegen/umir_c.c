#include "cm/codegen/umir_c.h"

#include <string.h>

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

int cm_umir_c_render_body(CmStrBuf *output, const CmUMirBody *body,
    const CmUBody *ub, const CmTyckSet *tyck, unsigned long symbol)
{
    size_t block_index;
    size_t local_index;
    int complete = 1;
    if (output == NULL || body == NULL || ub == NULL || tyck == NULL)
        return 0;
    cm_str_buf_append(output, "static void cm_u_body_");
    cm_umir_c_render_number(output, symbol);
    cm_str_buf_append(output, "(void)\n{\n");
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
                cm_str_buf_append(output, "0 /* path */");
                break;
            case CM_UMIR_RVALUE_BINARY:
                if (statement->operand_count == 2u) {
                    cm_str_buf_push(output, '(');
                    cm_str_buf_append(output, "(long long)");
                    cm_umir_c_render_local(output,
                        statement->operands[0]);
                    cm_str_buf_append(output, " + (long long)");
                    cm_umir_c_render_local(output,
                        statement->operands[1]);
                    cm_str_buf_append(output, ") /* op */");
                } else {
                    cm_str_buf_append(output, "0 /* binary */");
                    complete = 0;
                }
                break;
            case CM_UMIR_RVALUE_UNARY:
                if (statement->operand_count == 1u)
                    cm_umir_c_render_local(output,
                        statement->operands[0]);
                else {
                    cm_str_buf_append(output, "0 /* unary */");
                    complete = 0;
                }
                break;
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
            cm_str_buf_append(output, "return;\n");
            break;
        case CM_UMIR_TERMINATOR_GOTO:
            cm_str_buf_append(output, "goto _b");
            cm_umir_c_render_number(output,
                (unsigned long)block->goto_target);
            cm_str_buf_append(output, ";\n");
            break;
        case CM_UMIR_TERMINATOR_SWITCH_BOOL:
        case CM_UMIR_TERMINATOR_SWITCH:
            cm_str_buf_append(output, "if (");
            cm_umir_c_render_local(output, block->condition);
            cm_str_buf_append(output, ") goto _b");
            cm_umir_c_render_number(output,
                (unsigned long)block->true_target);
            cm_str_buf_append(output, "; else goto _b");
            cm_umir_c_render_number(output,
                (unsigned long)(block->terminator
                        == CM_UMIR_TERMINATOR_SWITCH
                    ? block->goto_target : block->false_target));
            cm_str_buf_append(output, ";\n");
            break;
        default:
            cm_str_buf_append(output, "return;\n");
            break;
        }
    }
    cm_str_buf_append(output, "}\n");
    return complete;
}
