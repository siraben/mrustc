#include "cm/codegen/umir_c.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "cm/hir/model.h"
#include "cm/alloc.h"

static CmTyId cm_umir_c_subst(CmTyId type);
/* HIR of the unit being rendered (transparent-wrapper lookups). */
static const CmHirContext *cm_umir_c_hir = NULL;

static void cm_umir_c_render_callee_symbol(CmStrBuf *output,
    const CmHirContext *hir, const CmTyckSet *tyck, CmHirDefId def,
    CmTyId callee_type, CmTyId receiver_type,
    const CmUMirStatement *statement, uint32_t first_arg);
static CmHirDefId cm_umir_c_deref_fn(const CmHirContext *hir, int mutable);

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
                    case CM_UMIR_RVALUE_REBORROW:
                    case CM_UMIR_RVALUE_CAST:
                    case CM_UMIR_RVALUE_ASSIGN:
                        break;
                    case CM_UMIR_RVALUE_CALL:
                    case CM_UMIR_RVALUE_METHOD_CALL:
                        /* Callee symbols are mangled instance names. */
                        break;
                    case CM_UMIR_RVALUE_FIELD:
                    case CM_UMIR_RVALUE_REF_FIELD:
                    case CM_UMIR_RVALUE_INDEX:
                    case CM_UMIR_RVALUE_REF_INDEX:
                    case CM_UMIR_RVALUE_RANGE_TEST:
                    case CM_UMIR_RVALUE_AGGREGATE:
                        /* Renders through the layout engine's member
                         * names. */
                        break;
                    case CM_UMIR_RVALUE_TRY_UNWRAP:
                    case CM_UMIR_RVALUE_INTO_ITER:
                    case CM_UMIR_RVALUE_ITER_NEXT:
                    case CM_UMIR_RVALUE_DEREF_CALL:
                    case CM_UMIR_RVALUE_SLICE_LEN:
                    case CM_UMIR_RVALUE_STATIC_ADDR:
                    case CM_UMIR_RVALUE_SUBSLICE:
                    case CM_UMIR_RVALUE_DROP:
                    case CM_UMIR_RVALUE_SCOPE_DROP:
                    case CM_UMIR_RVALUE_VARIANT:
                    case CM_UMIR_RVALUE_SLOT:
                    case CM_UMIR_RVALUE_REF_SLOT:
                    case CM_UMIR_RVALUE_STORE_FIELD:
                    case CM_UMIR_RVALUE_STORE_INDEX:
                    case CM_UMIR_RVALUE_STORE_DEREF:
                    case CM_UMIR_RVALUE_LOAD:
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

static const CmUMirBody *cm_umir_c_active_body = NULL;
static const CmUBody *cm_umir_c_active_ub = NULL;
/* Set while rendering a vtable: the receiver type is the exact Self. */
static int cm_umir_c_exact_self = 0;

/* Locals live in one frame array so closures can alias it as their
 * environment: `_l[i]` for the frame, `env[i]` for an enclosing frame's
 * slots seen from a closure body. */
static void cm_umir_c_render_local(CmStrBuf *output, CmUMirLocalId local)
{
    const CmUMirBody *body = cm_umir_c_active_body;
    if (body != NULL && body->closure_expr != CM_U_EXPR_NONE
        && local < body->env_count) {
        cm_str_buf_append(output, "env[");
        cm_umir_c_render_number(output, (unsigned long)local);
        cm_str_buf_push(output, ']');
        return;
    }
    cm_str_buf_append(output, "_l[");
    cm_umir_c_render_number(output, (unsigned long)(body != NULL
        && body->closure_expr != CM_U_EXPR_NONE
            ? local - body->env_count : local));
    cm_str_buf_push(output, ']');
}

static void cm_umir_c_render_closure_symbol(CmStrBuf *output,
    CmHirBodyId body, CmUExprId expr, long instance)
{
    cm_str_buf_append(output, "cm_u_closure_");
    cm_umir_c_render_number(output, (unsigned long)body);
    cm_str_buf_push(output, '_');
    cm_umir_c_render_number(output, (unsigned long)expr);
    if (instance >= 0) {
        cm_str_buf_append(output, "_i");
        cm_umir_c_render_number(output, (unsigned long)instance);
    }
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
static const CmHirItem *cm_umir_c_item_of(const CmHirContext *hir,
    CmHirDefId definition);

static int cm_umir_c_render_call(CmStrBuf *output,
    const CmHirContext *hir, const CmTyckSet *tyck,
    const CmUMirStatement *statement, CmHirDefId def, uint32_t first_arg,
    CmTyId callee_type, CmTyId receiver_type)
{
    uint32_t index;
    CmStrBuf symbol;
    CmHirDefId deref_args[CM_UMIR_STATEMENT_OPERANDS];
    CmTyId deref_self[CM_UMIR_STATEMENT_OPERANDS];
    const CmHirItem *callee_item;
    if (cm_hir_def_id_is_none(def) || statement->operand_overflow != 0u)
        return 0;
    for (index = 0u; index < CM_UMIR_STATEMENT_OPERANDS; ++index) {
        deref_args[index] = cm_hir_def_id_none();
        deref_self[index] = CM_TY_NONE;
    }
    callee_item = cm_umir_c_item_of(hir, def);
    if (callee_item != NULL && callee_item->kind == CM_HIR_ITEM_FUNCTION
        && cm_umir_c_active_body != NULL && cm_umir_c_active_ub != NULL) {
        const CmUExpr *call = cm_ubody_get_expr(cm_umir_c_active_ub,
            statement->expr);
        const CmTyckBody *tb = cm_tyck_get(tyck,
            cm_umir_c_active_body->source);
        for (index = first_arg; call != NULL
                && call->kind == CM_U_EXPR_CALL
                && index < statement->operand_count; ++index) {
            uint32_t parameter = index - first_arg;
            const CmUExpr *argument;
            CmTyId expected;
            CmTyId source_type;
            const CmTy *et;
            const CmTy *st;
            const CmHirItem *source_item;
            const CmInternedString *source_name;
            if (parameter >= call->data.call.argument_count
                || parameter >= callee_item->data.function_item.signature
                    .parameter_count
                || tb == NULL || tb->expr_types == NULL)
                continue;
            argument = cm_ubody_get_expr(cm_umir_c_active_ub,
                call->data.call.arguments[parameter]);
            if (argument == NULL || argument->kind != CM_U_EXPR_REF)
                continue;
            expected = cm_ty_from_hir((CmTyArena *)&tyck->arena, hir,
                callee_item->data.function_item.signature
                    .parameters[parameter].type);
            expected = cm_umir_c_subst(expected);
            et = cm_ty_get((CmTyArena *)&tyck->arena,
                cm_ty_resolve((CmTyArena *)&tyck->arena, expected));
            if (et == NULL || (et->kind != CM_TY_REF
                    && et->kind != CM_TY_PTR))
                continue;
            et = cm_ty_get((CmTyArena *)&tyck->arena,
                cm_ty_resolve((CmTyArena *)&tyck->arena, et->children[0]));
            if (et == NULL || et->kind != CM_TY_SLICE) continue;
            source_type = cm_umir_c_subst(
                tb->expr_types[argument->data.ref.operand]);
            st = cm_ty_get((CmTyArena *)&tyck->arena,
                cm_ty_resolve((CmTyArena *)&tyck->arena, source_type));
            source_item = st == NULL || st->kind != CM_TY_ADT ? NULL
                : cm_umir_c_item_of(hir, st->def);
            source_name = source_item == NULL ? NULL
                : cm_interner_get(&hir->strings, source_item->name);
            if (source_name == NULL || source_name->len != 3u
                || memcmp(source_name->bytes, "Vec", 3u) != 0)
                continue;
            deref_args[index] = cm_umir_c_deref_fn(hir, 0);
            deref_self[index] = source_type;
        }
    }
    cm_str_buf_init(&symbol);
    cm_umir_c_render_callee_symbol(&symbol, hir, tyck, def, callee_type,
        receiver_type, statement, first_arg);
    {
        /* A C-variadic extern declaration cannot be forwarded through the
         * foreign shim (C99 has no way to re-pass `...`): call the host
         * symbol directly with every argument. */
        if (callee_item != NULL && callee_item->kind == CM_HIR_ITEM_FUNCTION
            && callee_item->data.function_item.body == 0u
            && callee_item->data.function_item.is_foreign
            && callee_item->data.function_item.signature.is_variadic) {
            const CmInternedString *host = cm_interner_get(&hir->strings,
                callee_item->name);
            cm_str_buf_destroy(&symbol);
            cm_str_buf_init(&symbol);
            cm_str_buf_append_n(&symbol, (const char *)host->bytes,
                host->len);
        }
    }
    cm_str_buf_append(output, "0; { long long ");
    cm_str_buf_append_n(output, symbol.data, symbol.len);
    cm_str_buf_append(output, "(); ");
    for (index = first_arg; index < statement->operand_count; ++index) {
        if (!cm_hir_def_id_is_none(deref_args[index])) {
            CmStrBuf deref_symbol;
            cm_str_buf_init(&deref_symbol);
            cm_umir_c_render_callee_symbol(&deref_symbol, hir, tyck,
                deref_args[index], CM_TY_NONE, deref_self[index], NULL, 0u);
            cm_str_buf_append(output, "long long _cv");
            cm_umir_c_render_number(output,
                (unsigned long)statement->destination);
            cm_str_buf_push(output, '_');
            cm_umir_c_render_number(output, (unsigned long)index);
            cm_str_buf_append(output, "; { long long ");
            cm_str_buf_append_n(output, deref_symbol.data,
                deref_symbol.len);
            cm_str_buf_append(output, "(); _cv");
            cm_umir_c_render_number(output,
                (unsigned long)statement->destination);
            cm_str_buf_push(output, '_');
            cm_umir_c_render_number(output, (unsigned long)index);
            cm_str_buf_append(output, " = ");
            cm_str_buf_append_n(output, deref_symbol.data,
                deref_symbol.len);
            cm_str_buf_push(output, '(');
            cm_umir_c_render_local(output, statement->operands[index]);
            cm_str_buf_append(output, "); } ");
            cm_str_buf_destroy(&deref_symbol);
        }
    }
    cm_umir_c_render_local(output, statement->destination);
    cm_str_buf_append(output, " = ");
    cm_str_buf_append_n(output, symbol.data, symbol.len);
    cm_str_buf_destroy(&symbol);
    cm_str_buf_push(output, '(');
    for (index = first_arg; index < statement->operand_count; ++index) {
        if (index != first_arg) cm_str_buf_append(output, ", ");
        if (!cm_hir_def_id_is_none(deref_args[index])) {
            cm_str_buf_append(output, "_cv");
            cm_umir_c_render_number(output,
                (unsigned long)statement->destination);
            cm_str_buf_push(output, '_');
            cm_umir_c_render_number(output, (unsigned long)index);
        } else
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

/* The symbol a foreign declaration binds to: its `#[link_name = ".."]`
 * (std's `errno_location` is `__errno_location` on Linux, through
 * `cfg_attr`), else its own name. */
static const CmInternedString *cm_umir_c_item_link_name(
    const CmHirContext *hir, const CmHirItem *item, const char **out_bytes,
    size_t *out_len)
{
    static const char key[] = "link_name";
    const CmInternedString *name = cm_interner_get(&hir->strings, item->name);
    uint32_t index;
    *out_bytes = name == NULL ? "" : (const char *)name->bytes;
    *out_len = name == NULL ? 0u : name->len;
    for (index = 0u; index < item->attribute_count; ++index) {
        const CmInternedString *text = cm_interner_get(&hir->strings,
            item->attributes[index].metadata);
        const char *cursor;
        const char *end;
        const char *open;
        if (text == NULL || text->len <= sizeof(key) - 1u
            || memcmp(text->bytes, key, sizeof(key) - 1u) != 0) continue;
        cursor = (const char *)text->bytes + sizeof(key) - 1u;
        end = (const char *)text->bytes + text->len;
        while (cursor < end && (*cursor == ' ' || *cursor == '\t')) ++cursor;
        if (cursor >= end || *cursor != '=') continue;
        ++cursor;
        while (cursor < end && (*cursor == ' ' || *cursor == '\t')) ++cursor;
        if (cursor >= end || *cursor != '"') continue;
        open = ++cursor;
        while (cursor < end && *cursor != '"') ++cursor;
        if (cursor >= end || cursor == open) continue;
        *out_bytes = open;
        *out_len = (size_t)(cursor - open);
        return text;
    }
    return name;
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

static unsigned int cm_umir_c_ref_depth(const CmTyckSet *tyck, CmTyId type)
{
    unsigned int depth = 0u;
    const CmTy *ty = type == CM_TY_NONE ? NULL
        : cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena, type));
    while (ty != NULL && (ty->kind == CM_TY_REF || ty->kind == CM_TY_PTR)
            && depth < 8u) {
        depth += 1u;
        ty = cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena, ty->children[0]));
    }
    return depth;
}

/* Scalar C type for typed-width memory access (ints, bool, char); NULL
 * for everything else, which travels in a full slot. */
static CmTyId cm_umir_c_representation(const CmHirContext *hir,
    const CmTyckSet *tyck, CmTyId type);

static const char *cm_umir_c_scalar_type(const CmTyckSet *tyck, CmTyId type)
{
    const CmTy *ty;
    if (cm_umir_c_hir != NULL && type != CM_TY_NONE)
        type = cm_umir_c_representation(cm_umir_c_hir, tyck, type);
    ty = type == CM_TY_NONE ? NULL
        : cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena, type));
    if (ty == NULL) return NULL;
    if (ty->kind == CM_TY_INT || ty->kind == CM_TY_BOOL
        || ty->kind == CM_TY_CHAR)
        return cm_umir_c_abi_type(&tyck->arena, type);
    return NULL;
}

/* Byte size of a scalar element in memory (slots otherwise). */
static unsigned long cm_umir_c_scalar_size(const CmTyckSet *tyck,
    CmTyId type)
{
    const CmTy *ty;
    if (cm_umir_c_hir != NULL && type != CM_TY_NONE)
        type = cm_umir_c_representation(cm_umir_c_hir, tyck, type);
    ty = type == CM_TY_NONE ? NULL
        : cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena, type));
    if (ty == NULL) return 8ul;
    if (ty->kind == CM_TY_BOOL) return 1ul;
    if (ty->kind == CM_TY_CHAR) return 4ul;
    if (ty->kind == CM_TY_INT) {
        switch ((CmHirIntType)ty->a) {
        case CM_HIR_INT_I8: case CM_HIR_INT_U8: return 1ul;
        case CM_HIR_INT_I16: case CM_HIR_INT_U16: return 2ul;
        case CM_HIR_INT_I32: case CM_HIR_INT_U32: return 4ul;
        default: return 8ul;
        }
    }
    return 8ul;
}

/* The pointee behind every reference/pointer layer. */
static CmTyId cm_umir_c_peel(const CmTyckSet *tyck, CmTyId type)
{
    const CmTy *ty = type == CM_TY_NONE ? NULL
        : cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena, type));
    unsigned int guard = 0u;
    while (ty != NULL && (ty->kind == CM_TY_REF || ty->kind == CM_TY_PTR)
            && guard++ < 8u) {
        type = ty->children[0];
        ty = cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena, type));
    }
    return type;
}

/* Scalar C type of an array's elements (NULL for slot elements or
 * non-arrays). */
static const char *cm_umir_c_array_elem_scalar(const CmTyckSet *tyck,
    CmTyId type)
{
    const CmTy *ty = type == CM_TY_NONE ? NULL
        : cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena, type));
    if (ty == NULL || ty->kind != CM_TY_ARRAY || ty->count == 0u) return NULL;
    return cm_umir_c_scalar_type(tyck, ty->children[0]);
}

/* Element count of an array type (0 when not a concrete constant). */
static unsigned long cm_umir_c_array_len(const CmTyckSet *tyck, CmTyId type)
{
    const CmTy *ty = type == CM_TY_NONE ? NULL
        : cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena, type));
    const CmTy *len;
    if (ty == NULL || ty->kind != CM_TY_ARRAY || ty->count < 2u) return 0ul;
    len = cm_ty_get((CmTyArena *)&tyck->arena,
        cm_ty_resolve((CmTyArena *)&tyck->arena, ty->children[1]));
    if (len == NULL || len->kind != CM_TY_CONST) return 0ul;
    return (unsigned long)len->lo;
}

/* Zero-sized: `()`, a fieldless struct (PhantomData), an empty array. */
static int cm_umir_c_is_zst(const CmHirContext *hir, const CmTyckSet *tyck,
    CmTyId type)
{
    const CmTy *ty = type == CM_TY_NONE ? NULL
        : cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena, type));
    if (ty == NULL) return 0;
    if (ty->kind == CM_TY_TUPLE && ty->count == 0u) return 1;
    if (ty->kind == CM_TY_ARRAY && cm_umir_c_array_len(tyck, type) == 0ul
        && ty->count >= 2u) {
        const CmTy *len = cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena, ty->children[1]));
        return len != NULL && len->kind == CM_TY_CONST;
    }
    if (ty->kind == CM_TY_ADT) {
        const CmHirDefinition *record = cm_hir_lookup_definition(hir, ty->def);
        const CmHirItem *item = record == NULL
                || record->kind != CM_HIR_DEFINITION_ITEM ? NULL
            : cm_hir_get_item(hir, record->entity.item_id);
        return item != NULL && item->kind == CM_HIR_ITEM_STRUCT
            && item->data.aggregate_item.field_count == 0u;
    }
    return 0;
}

static CmTyId cm_umir_c_transparent_inner(const CmHirContext *hir,
    const CmTyckSet *tyck, CmTyId type, long field);

/* A struct or union with exactly one non-zero-sized field is
 * transparent: its value is that field's value (NonNull<T> is its
 * pointer, MaybeUninit<T> / ManuallyDrop<T> are T), so the bit-casts
 * core performs between a wrapper and its field agree.  Returns the
 * representative field's index, or -1. */
static long cm_umir_c_transparent_field(const CmHirContext *hir,
    const CmTyckSet *tyck, CmTyId type)
{
    const CmTy *ty = type == CM_TY_NONE ? NULL
        : cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena, type));
    const CmHirDefinition *record;
    const CmHirItem *item;
    uint32_t index;
    long representative = -1;
    if (ty == NULL || ty->kind != CM_TY_ADT) return -1;
    record = cm_hir_lookup_definition(hir, ty->def);
    if (record == NULL || record->kind != CM_HIR_DEFINITION_ITEM) return -1;
    item = cm_hir_get_item(hir, record->entity.item_id);
    if (item == NULL || (item->kind != CM_HIR_ITEM_STRUCT
            && item->kind != CM_HIR_ITEM_UNION)) return -1;
    if (item->data.aggregate_item.field_count == 1u) return 0;
    for (index = 0u; index < item->data.aggregate_item.field_count; ++index) {
        /* The field's type under the ADT's arguments: `Box<T, A>`'s
         * `alloc: A` is zero-sized exactly when `A` is (`Global`). */
        CmTyId field_type = cm_umir_c_transparent_inner(hir, tyck, type,
            (long)index);
        int zst = cm_umir_c_is_zst(hir, tyck, field_type);
        if (getenv("CMRUSTC_UMIR_DEBUG_REP") != NULL) {
            CmStrBuf text;
            cm_str_buf_init(&text);
            cm_ty_print((CmTyArena *)&tyck->arena, hir, field_type, &text);
            fprintf(stderr, "UMIR rep field %u %.*s zst=%d\n",
                (unsigned)index, (int)text.len, text.data, zst);
            cm_str_buf_destroy(&text);
        }
        item = cm_hir_get_item(hir, record->entity.item_id);
        if (zst) continue;
        if (representative >= 0) return -1;
        representative = (long)index;
    }
    return representative;
}


/* The (substituted) type of transparent wrapper `type`'s field `field`. */
static CmTyId cm_umir_c_transparent_inner(const CmHirContext *hir,
    const CmTyckSet *tyck, CmTyId type, long field)
{
    const CmTy *ty = cm_ty_get((CmTyArena *)&tyck->arena,
        cm_ty_resolve((CmTyArena *)&tyck->arena, type));
    const CmHirDefinition *record = cm_hir_lookup_definition(hir, ty->def);
    const CmHirItem *item = cm_hir_get_item(hir, record->entity.item_id);
    CmTySubst subst;
    CmHirGenericParamId params[32];
    CmTyId arguments[32];
    uint32_t count = item->generic_parameter_count > 32u ? 32u
        : item->generic_parameter_count;
    uint32_t index;
    CmTyId raw;
    /* Copy the arguments first: creating the field type can move the
     * arena from under `ty`. */
    if (count > ty->count) count = ty->count;
    for (index = 0u; index < count; ++index) {
        params[index] = item->generic_parameter_start + index;
        arguments[index] = ty->children[index];
    }
    raw = cm_ty_from_hir((CmTyArena *)&tyck->arena, hir,
        item->data.aggregate_item.fields[field].type);
    subst.parameters = params;
    subst.types = arguments;
    subst.count = count;
    subst.self_type = CM_TY_NONE;
    return cm_ty_subst((CmTyArena *)&tyck->arena, raw, &subst);
}

/* The representation type behind transparent wrappers. */
static CmTyId cm_umir_c_representation(const CmHirContext *hir,
    const CmTyckSet *tyck, CmTyId type)
{
    unsigned int guard = 0u;
    while (guard++ < 8u) {
        long field = cm_umir_c_transparent_field(hir, tyck, type);
        if (field < 0) return type;
        type = cm_umir_c_transparent_inner(hir, tyck, type, field);
    }
    return type;
}

static long cm_umir_c_field_index(const CmHirContext *hir,
    const CmTyckSet *tyck, const CmUBodySet *ubodies, CmTyId type,
    CmInternId name);

/* Positional field count of the tuple struct behind `type`; 0 for a
 * unit or named-field struct or union (no `.N` of its own), -1 when
 * `type` is not an aggregate at all. */
static long cm_umir_c_aggregate_field_count(const CmHirContext *hir,
    const CmTyckSet *tyck, CmTyId type)
{
    const CmTy *ty = type == CM_TY_NONE ? NULL
        : cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena, type));
    const CmHirDefinition *record;
    const CmHirItem *item;
    if (ty == NULL || ty->kind != CM_TY_ADT) return -1;
    record = cm_hir_lookup_definition(hir, ty->def);
    if (record == NULL || record->kind != CM_HIR_DEFINITION_ITEM) return -1;
    item = cm_hir_get_item(hir, record->entity.item_id);
    if (item == NULL || (item->kind != CM_HIR_ITEM_STRUCT
            && item->kind != CM_HIR_ITEM_UNION)) return -1;
    if (item->data.aggregate_item.form != CM_HIR_AGGREGATE_TUPLE) return 0;
    return (long)item->data.aggregate_item.field_count;
}

/* Box<T>'s runtime value points at one slot holding T.  Field syntax applies
 * Deref before selecting T's field (`boxed.field`), unlike transparent ADT
 * wrappers whose represented field is already the value. */
static CmTyId cm_umir_c_box_pointee(const CmHirContext *hir,
    const CmTyckSet *tyck, CmTyId type)
{
    const CmTy *ty = type == CM_TY_NONE ? NULL
        : cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena, type));
    const CmHirItem *item;
    const CmInternedString *name;
    if (ty == NULL || ty->kind != CM_TY_ADT || ty->count == 0u)
        return CM_TY_NONE;
    item = cm_umir_c_item_of(hir, ty->def);
    name = item == NULL ? NULL : cm_interner_get(&hir->strings, item->name);
    return name != NULL && name->len == 3u
            && memcmp(name->bytes, "Box", 3u) == 0
        ? ty->children[0] : CM_TY_NONE;
}

/* The aggregate a field access `expr` on a value of `type` reads: the
 * peeled type itself, or -- when the field is not one of its own --
 * the transparent wrapper's field type it derefs to (`b.1` on a
 * `ManuallyDrop<Box<T, A>>` is Box's allocator: the wrapper's value is
 * the field's, so the slot is read from the same block).  Sets `*slot`
 * to the field's index, -1 when unknown. */
static CmTyId cm_umir_c_field_carrier(const CmHirContext *hir,
    const CmTyckSet *tyck, const CmUBodySet *ubodies, CmTyId type,
    const CmUExpr *expr, long *slot, unsigned int *user_deref_depth)
{
    CmTyId peeled = cm_umir_c_peel(tyck, type);
    CmTyId current = peeled;
    unsigned int guard = 0u;
    *slot = -1;
    *user_deref_depth = 0u;
    while (guard++ < 8u) {
        long found = -1;
        long representative;
        if (expr->kind == CM_U_EXPR_TUPLE_FIELD) {
            long count = cm_umir_c_aggregate_field_count(hir, tyck, current);
            found = (long)expr->data.tuple_field.index;
            if (count >= 0 && found >= count) found = -1;
        } else if (expr->kind == CM_U_EXPR_FIELD) {
            found = cm_umir_c_field_index(hir, tyck, ubodies, current,
                expr->data.field.name);
        }
        if (found >= 0) {
            *slot = found;
            return current;
        }
        {
            CmTyId pointee = cm_umir_c_box_pointee(hir, tyck, current);
            if (pointee != CM_TY_NONE) {
                current = pointee;
                *user_deref_depth += 1u;
                continue;
            }
        }
        representative = cm_umir_c_transparent_field(hir, tyck, current);
        if (representative < 0) break;
        current = cm_umir_c_transparent_inner(hir, tyck, current,
            representative);
    }
    if (expr->kind == CM_U_EXPR_TUPLE_FIELD)
        *slot = (long)expr->data.tuple_field.index;
    return peeled;
}

/* The value behind `depth` reference layers of `local`. */
static void cm_umir_c_render_loaded(CmStrBuf *output, CmUMirLocalId local,
    unsigned int depth)
{
    unsigned int index;
    for (index = 0u; index < depth; ++index)
        cm_str_buf_append(output, "*(long long *)(intptr_t)");
    cm_umir_c_render_local(output, local);
}

/* Unsized pointees travel as a `[data, len]` slot pair.  A transparent
 * unsized wrapper (`CStr([u8])`) has the same fat representation as its
 * field, so pointer casts between the field and wrapper preserve metadata. */
static int cm_umir_c_is_fat(const CmHirContext *hir,
    const CmTyckSet *tyck, CmTyId pointee)
{
    const CmTy *ty;
    if (pointee != CM_TY_NONE)
        pointee = cm_umir_c_representation(hir, tyck, pointee);
    ty = pointee == CM_TY_NONE ? NULL
        : cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena, pointee));
    return ty != NULL && (ty->kind == CM_TY_SLICE || ty->kind == CM_TY_STR);
}

/* `((long long *)(intptr_t)LOAD...(local))` — the slot-array base of an
 * aggregate reached through `depth` reference layers. */
static void cm_umir_c_render_base(CmStrBuf *output, CmUMirLocalId local,
    unsigned int depth)
{
    unsigned int index;
    cm_str_buf_append(output, "((long long *)(intptr_t)");
    for (index = 0u; index < depth; ++index)
        cm_str_buf_append(output, "*(long long *)(intptr_t)");
    cm_umir_c_render_local(output, local);
    cm_str_buf_push(output, ')');
}

static CmTyId cm_umir_c_local_type(const CmUMirBody *body,
    CmUMirLocalId local)
{
    const CmTyId *type = (const CmTyId *)cm_vec_at_const(&body->locals,
        local);
    return type == NULL ? CM_TY_NONE : cm_umir_c_subst(*type);
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


/* `Ordering as i8` / `STATX_STATE::Present as u8`: an enum value is an
 * aggregate whose slot 0 holds the variant index; casting it to an
 * integer yields the variant's declared discriminant. */
static const CmHirItem *cm_umir_c_enum_item_of(const CmHirContext *hir,
    const CmTyckSet *tyck, CmTyId type)
{
    const CmTy *ty;
    const CmHirDefinition *record;
    const CmHirItem *item;
    if (type == CM_TY_NONE) return NULL;
    ty = cm_ty_get((CmTyArena *)&tyck->arena,
        cm_ty_resolve((CmTyArena *)&tyck->arena, type));
    if (ty == NULL || ty->kind != CM_TY_ADT) return NULL;
    record = cm_hir_lookup_definition(hir, ty->def);
    item = record == NULL || record->kind != CM_HIR_DEFINITION_ITEM ? NULL
        : cm_hir_get_item(hir, record->entity.item_id);
    return item != NULL && item->kind == CM_HIR_ITEM_ENUM ? item : NULL;
}

/* Declared discriminants (implicit ones follow their predecessor);
 * returns whether every variant's discriminant is its index. */
static int cm_umir_c_enum_discriminants(const CmHirItem *item,
    long long *values, uint32_t count)
{
    uint32_t index;
    long long next = 0;
    int sequential = 1;
    for (index = 0u; index < count; ++index) {
        const CmHirVariant *variant = &item->data.enum_item.variants[index];
        if (variant->has_discriminant
            && variant->discriminant.kind == CM_HIR_CONST_VALUE)
            next = (long long)variant->discriminant.data.value.low_bits;
        values[index] = next;
        if (next != (long long)index) sequential = 0;
        next += 1;
    }
    return sequential;
}

/* Every variant fieldless: values are index blocks, castable to their
 * declared discriminant. */
static int cm_umir_c_enum_is_fieldless(const CmHirItem *item)
{
    uint32_t index;
    if (item == NULL || item->kind != CM_HIR_ITEM_ENUM
        || item->data.enum_item.variant_count == 0u) return 0;
    for (index = 0u; index < item->data.enum_item.variant_count; ++index)
        if (item->data.enum_item.variants[index].field_count != 0u) return 0;
    return 1;
}

static int cm_umir_c_render_enum_cast(CmStrBuf *output,
    const CmHirContext *hir, const CmTyckSet *tyck, CmTyId from, CmTyId to,
    CmUMirLocalId operand)
{
    const CmHirItem *item;
    const CmTy *target;
    uint32_t count;
    long long *values;
    uint32_t index;
    if (cm_umir_c_ref_depth(tyck, from) != 0u
        || cm_umir_c_ref_depth(tyck, to) != 0u) return 0;
    item = cm_umir_c_enum_item_of(hir, tyck, from);
    if (!cm_umir_c_enum_is_fieldless(item)) return 0;
    if (getenv("CMRUSTC_UMIR_DEBUG") != NULL) {
        const CmInternedString *nm = cm_interner_get(&hir->strings,
            item->name);
        fprintf(stderr, "UMIR enum-cast enum=%.*s from=%u to=%u\n",
            nm == NULL ? 1 : (int)nm->len,
            nm == NULL ? "?" : (const char *)nm->bytes,
            (unsigned)from, (unsigned)to);
    }
    target = to == CM_TY_NONE ? NULL
        : cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena, to));
    if (target == NULL || target->kind != CM_TY_INT) return 0;
    count = item->data.enum_item.variant_count;
    if (count == 0u) return 0;
    values = (long long *)cm_alloc_zeroed(count, sizeof(*values));
    cm_str_buf_append(output, "(long long)(");
    cm_str_buf_append(output, cm_umir_c_abi_type(&tyck->arena, to));
    cm_str_buf_append(output, ")(");
    if (cm_umir_c_enum_discriminants(item, values, count)) {
        cm_str_buf_append(output, "((long long *)(intptr_t)");
        cm_umir_c_render_local(output, operand);
        cm_str_buf_append(output, ")[0]");
    } else {
        for (index = 0u; index < count; ++index) {
            char text[48];
            cm_str_buf_append(output, "((long long *)(intptr_t)");
            cm_umir_c_render_local(output, operand);
            snprintf(text, sizeof(text), ")[0] == %lu ? %lldLL : ",
                (unsigned long)index, values[index]);
            cm_str_buf_append(output, text);
        }
        cm_str_buf_append(output, "0LL");
    }
    cm_str_buf_append(output, ")");
    cm_free(values);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Instances                                                            */

typedef struct CmUMirInstance {
    CmHirDefId definition;
    CmUExprId closure_expr; /* NONE for the item's own body */
    CmTyId self_type;       /* bound Self for trait default methods */
    CmHirBodyId body;
    CmTyId *types;          /* generic arguments, parameter order */
    uint32_t count;
    CmHirGenericParamId *parameters;
    unsigned long index;    /* symbol suffix */
    int rendered;
} CmUMirInstance;

/* One trait object vtable: the impl methods of `trait_def` for `type`,
 * in the trait's declaration order. */
typedef struct CmUMirVtable {
    CmHirDefId trait_def;
    CmTyId type;
} CmUMirVtable;

typedef struct CmUMirProgram {
    CmVec instances;        /* CmUMirInstance */
    CmVec vtables;          /* CmUMirVtable */
    const CmHirContext *hir;
    const CmUMirSet *umir;
    const CmUBodySet *ubodies;
    const CmTyckSet *tyck;
} CmUMirProgram;

/* Generic parameters of `item` and its impl/trait parent, in FN_DEF
 * argument order (parent first). */
static uint32_t cm_umir_c_collect_parameters(const CmHirContext *hir,
    const CmHirItem *item, CmHirGenericParamId *out, uint32_t capacity)
{
    uint32_t count = 0u;
    const CmHirItem *parent = NULL;
    uint32_t index;
    if (!cm_hir_def_id_is_none(item->parent_definition)) {
        const CmHirDefinition *record = cm_hir_lookup_definition(hir,
            item->parent_definition);
        if (record != NULL && record->kind == CM_HIR_DEFINITION_ITEM)
            parent = cm_hir_get_item(hir, record->entity.item_id);
    }
    if (parent != NULL)
        for (index = 0u; index < parent->generic_parameter_count
                && count < capacity; ++index)
            out[count++] = parent->generic_parameter_start + index;
    for (index = 0u; index < item->generic_parameter_count
            && count < capacity; ++index)
        out[count++] = item->generic_parameter_start + index;
    return count;
}

/* Intrinsics whose rendering depends on the instance's type arguments. */
static const CmHirItem *cm_umir_c_item_of(const CmHirContext *hir,
    CmHirDefId def);

/* Whether `type` is core's `Option` (by item name). */
static int cm_umir_c_is_option(const CmHirContext *hir, const CmTyckSet *tyck,
    CmTyId type)
{
    const CmTy *ty = type == CM_TY_NONE ? NULL
        : cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena, type));
    const CmHirItem *item;
    const CmInternedString *name;
    if (ty == NULL || ty->kind != CM_TY_ADT) return 0;
    item = cm_umir_c_item_of(hir, ty->def);
    name = item == NULL ? NULL : cm_interner_get(&hir->strings, item->name);
    return name != NULL && name->len == 6u
        && memcmp(name->bytes, "Option", 6u) == 0;
}

static int cm_umir_c_render_typed_shim(CmStrBuf *output,
    const CmHirContext *hir, const CmTyckSet *tyck,
    const CmInternedString *name, const CmUMirInstance *instance)
{
    CmTyId first = instance->count == 0u ? CM_TY_NONE : instance->types[0];
    const CmTy *ft = first == CM_TY_NONE ? NULL
        : cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena, first));
    const CmTy *self_ty = instance->self_type == CM_TY_NONE ? NULL
        : cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena, instance->self_type));
    if (name == NULL) return 0;
#define CM_SHIM_IS(text) (name->len == sizeof(text) - 1u \
        && memcmp(name->bytes, text, name->len) == 0)
    if (CM_SHIM_IS("const_eval_select") && ft != NULL
        && ft->kind == CM_TY_TUPLE && ft->count <= 16u) {
        /* Runtime code always selects `called_at_rt`.  The intrinsic's
         * first operand is a tuple, while that function receives the tuple
         * elements as ordinary arguments. */
        uint32_t index;
        cm_str_buf_append(output, "(long long a, long long c, long long r) { "
            "(void)c; return ((long long (*) (");
        if (ft->count == 0u) {
            cm_str_buf_append(output, "void");
        } else {
            for (index = 0u; index < ft->count; ++index) {
                if (index != 0u) cm_str_buf_append(output, ", ");
                cm_str_buf_append(output, "long long");
            }
        }
        cm_str_buf_append(output, "))(intptr_t)r)(");
        for (index = 0u; index < ft->count; ++index) {
            if (index != 0u) cm_str_buf_append(output, ", ");
            cm_str_buf_append(output, "((long long *)(intptr_t)a)[");
            cm_umir_c_render_number(output, (unsigned long)index);
            cm_str_buf_push(output, ']');
        }
        cm_str_buf_append(output, "); }");
        return 1;
    }
    if (CM_SHIM_IS("transmute") || CM_SHIM_IS("transmute_unchecked")) {
        /* Niche layouts: core transmutes an integer/pointer straight to
         * `Option<NonZero<T>>` / `Option<&T>` (0 = None) and back; our
         * Option is a block, so build or unwrap one.  Otherwise the bit
         * pattern is the value itself. */
        CmTyId to = instance->count >= 2u ? instance->types[1] : CM_TY_NONE;
        int from_option = cm_umir_c_is_option(hir, tyck, first);
        int to_option = cm_umir_c_is_option(hir, tyck, to);
        /* `transmute::<usize, Alignment>(align)` (core's ptr::Alignment
         * wraps a fieldless enum whose discriminants are 1 << n): an
         * enum value is a block whose slot 0 is the variant index, so an
         * integer becomes the block of the variant with that
         * discriminant, and back. */
        const CmHirItem *from_enum = cm_umir_c_enum_item_of(hir, tyck,
            cm_umir_c_representation(hir, tyck, first));
        const CmHirItem *to_enum = to == CM_TY_NONE ? NULL
            : cm_umir_c_enum_item_of(hir, tyck,
                cm_umir_c_representation(hir, tyck, to));
        if (!cm_umir_c_enum_is_fieldless(from_enum)) from_enum = NULL;
        if (!cm_umir_c_enum_is_fieldless(to_enum)) to_enum = NULL;
        if (to_enum != NULL && from_enum == NULL && !from_option) {
            uint32_t count = to_enum->data.enum_item.variant_count;
            long long *values = (long long *)cm_alloc_zeroed(count,
                sizeof(*values));
            uint32_t index;
            (void)cm_umir_c_enum_discriminants(to_enum, values, count);
            cm_str_buf_append(output, "(long long a) { long long *b = "
                "(long long *)calloc(2, 8); b[0] = (");
            for (index = 0u; index < count; ++index) {
                char text[48];
                snprintf(text, sizeof(text), "a == %lldLL ? %lu : ",
                    values[index], (unsigned long)index);
                cm_str_buf_append(output, text);
            }
            cm_str_buf_append(output, "0); return (long long)(intptr_t)b; }");
            cm_free(values);
            return 1;
        }
        if (from_enum != NULL && to_enum == NULL && !to_option) {
            uint32_t count = from_enum->data.enum_item.variant_count;
            long long *values = (long long *)cm_alloc_zeroed(count,
                sizeof(*values));
            uint32_t index;
            (void)cm_umir_c_enum_discriminants(from_enum, values, count);
            cm_str_buf_append(output, "(long long a) { long long i = "
                "((long long *)(intptr_t)a)[0]; return (");
            for (index = 0u; index < count; ++index) {
                char text[48];
                snprintf(text, sizeof(text), "i == %lu ? %lldLL : ",
                    (unsigned long)index, values[index]);
                cm_str_buf_append(output, text);
            }
            cm_str_buf_append(output, "0LL); }");
            cm_free(values);
            return 1;
        }
        if (to_option && !from_option)
            cm_str_buf_append(output, "(long long a) { long long *b = "
                "(long long *)malloc(16); if (a == 0) { b[0] = 0; } else "
                "{ b[0] = 1; b[1] = a; } return (long long)(intptr_t)b; }");
        else if (from_option && !to_option)
            cm_str_buf_append(output, "(long long a) { long long *b = "
                "(long long *)(intptr_t)a; return b[0] == 0 ? 0 : b[1]; }");
        else
            cm_str_buf_append(output, "(long long a) { return a; }");
        return 1;
    }
    if (CM_SHIM_IS("simd_eq")) {
        /* Slot ABI vectors occupy one packed word.  Compare its eight byte
         * lanes and return the intrinsic's all-ones/zero lane mask.  SSE's
         * logical group is 16 lanes, so tell bitmask that the unavailable
         * upper lanes came from a comparison (and therefore do not match). */
        cm_str_buf_append(output, "(long long a, long long b) { unsigned "
            "long long x = (unsigned long long)a, y = (unsigned long long)b, "
            "r = 0; unsigned i; cm_umir_simd_compare = 1; "
            "for (i = 0; i < 8; ++i) if (((x >> (i * 8)) "
            "& 255u) == ((y >> (i * 8)) & 255u)) r |= 255ull << (i * 8); "
            "return (long long)r; }");
        return 1;
    }
    if (CM_SHIM_IS("simd_lt")) {
        /* The reachable x86 path compares i8x16 against zero; the slot ABI
         * exposes its low eight signed byte lanes (and reports WIDTH=8). */
        cm_str_buf_append(output, "(long long a, long long b) { unsigned "
            "long long x = (unsigned long long)a, y = (unsigned long long)b, "
            "r = 0; unsigned i; for (i = 0; i < 8; ++i) if ((int8_t)(x >> "
            "(i * 8)) < (int8_t)(y >> (i * 8))) r |= 255ull << (i * 8); "
            "return (long long)r; }");
        return 1;
    }
    if (CM_SHIM_IS("simd_bitmask")) {
        cm_str_buf_append(output, "(long long a) { unsigned long long x = "
            "(unsigned long long)a; unsigned long long r = 0; unsigned i; "
            "for (i = 0; i < 8; ++i) if (((x >> (i * 8)) & 128u) != 0) "
            "r |= 1ull << i; if (!cm_umir_simd_compare) r |= 0xff00u; "
            "cm_umir_simd_compare = 0; return (long long)r; }");
        return 1;
    }
    if (CM_SHIM_IS("write") && ((self_ty != NULL
            && self_ty->kind == CM_TY_PTR) || instance->count == 1u)) {
        CmTyId pointee_id = self_ty != NULL && self_ty->kind == CM_TY_PTR
                && self_ty->count != 0u ? self_ty->children[0] : first;
        const CmTy *pointee = pointee_id == CM_TY_NONE ? NULL
            : cm_ty_get((CmTyArena *)&tyck->arena,
                cm_ty_resolve((CmTyArena *)&tyck->arena,
                    pointee_id));
        const char *scalar = cm_umir_c_scalar_type(tyck, pointee_id);
        /* A raw-pointer `write` can arrive here through the declaration
         * fallback when metadata has no compiler-provided inherent body.
         * Do not confuse it with body-less trait methods of the same name
         * (notably io::Write::write): bound Self identifies this case.
         * Tuples live in slot arrays, so writing `T` copies its fields into
         * the pointed-to storage instead of storing the array pointer. */
        if (pointee != NULL && pointee->kind == CM_TY_TUPLE) {
            cm_str_buf_append(output, "(long long p, long long v) { ");
            if (pointee->count != 0u) {
                cm_str_buf_append(output, "memmove((void *)(intptr_t)p, "
                    "(const void *)(intptr_t)v, ");
                cm_umir_c_render_number(output,
                    (unsigned long)pointee->count * 8u);
                cm_str_buf_append(output, "); ");
            }
            cm_str_buf_append(output, "return 0; }");
        } else if (scalar != NULL) {
            cm_str_buf_append(output, "(long long p, long long v) { *(");
            cm_str_buf_append(output, scalar);
            cm_str_buf_append(output, " *)(intptr_t)p = (");
            cm_str_buf_append(output, scalar);
            cm_str_buf_append(output, ")v; return 0; }");
        } else {
            cm_str_buf_append(output, "(long long p, long long v) { "
                "*(long long *)(intptr_t)p = v; return 0; }");
        }
        return 1;
    }
    if (CM_SHIM_IS("ptr_metadata")) {
        /* `*const [T]` / `*const str`: the pair's length; thin: unit. */
        if (cm_umir_c_is_fat(hir, tyck, first))
            cm_str_buf_append(output, "(long long a) { return "
                "((long long *)(intptr_t)*(long long *)(intptr_t)a)[1]; }");
        else
            cm_str_buf_append(output, "(long long a) { (void)a; return 0; }");
        return 1;
    }
    if (CM_SHIM_IS("offset") || CM_SHIM_IS("arith_offset")) {
        /* `*const T` moves by the scalar width of T (slots otherwise). */
        CmTyId elem = ft != NULL && (ft->kind == CM_TY_PTR
            || ft->kind == CM_TY_REF) ? ft->children[0] : CM_TY_NONE;
        if (getenv("CMRUSTC_UMIR_DEBUG") != NULL) {
            CmStrBuf text;
            cm_str_buf_init(&text);
            cm_ty_print((CmTyArena *)&tyck->arena, cm_umir_c_hir, elem,
                &text);
            fprintf(stderr, "UMIR shim offset elem=%.*s size=%lu\n",
                (int)text.len, text.data,
                cm_umir_c_scalar_size(tyck, elem));
            cm_str_buf_destroy(&text);
        }
        cm_str_buf_append(output, "(long long p, long long n) "
            "{ return p + n * ");
        cm_umir_c_render_number(output, cm_umir_c_scalar_size(tyck, elem));
        cm_str_buf_append(output, "; }");
        return 1;
    }
    if (CM_SHIM_IS("ptr_offset_from") || CM_SHIM_IS("ptr_offset_from_unsigned")) {
        cm_str_buf_append(output, "(long long a, long long b) "
            "{ return (a - b) / ");
        cm_umir_c_render_number(output, cm_umir_c_scalar_size(tyck, first));
        cm_str_buf_append(output, "; }");
        return 1;
    }
    if (CM_SHIM_IS("aggregate_raw_ptr")) {
        /* (data, metadata) -> pointer: a fat pointee gets a pair block
         * referenced like every other fat value; thin is the data. */
        const CmTy *pointee = ft != NULL && (ft->kind == CM_TY_PTR
                || ft->kind == CM_TY_REF)
            ? cm_ty_get((CmTyArena *)&tyck->arena,
                cm_ty_resolve((CmTyArena *)&tyck->arena, ft->children[0]))
            : NULL;
        if (pointee != NULL && (pointee->kind == CM_TY_SLICE
                || pointee->kind == CM_TY_STR || pointee->kind == CM_TY_DYN))
            cm_str_buf_append(output, "(long long d, long long m) "
                "{ long long *b = (long long *)malloc(24); b[1] = d; "
                "b[2] = m; b[0] = (long long)(intptr_t)&b[1]; "
                "return (long long)(intptr_t)&b[0]; }");
        else
            cm_str_buf_append(output, "(long long d, long long m) "
                "{ (void)m; return d; }");
        return 1;
    }
    if (CM_SHIM_IS("slice_get_unchecked")) {
        /* <ItemPtr, SlicePtr, T>(slice, index): element address at the
         * element's width (slots for non-scalars). */
        CmTyId elem = instance->count >= 3u ? instance->types[2] : CM_TY_NONE;
        cm_str_buf_append(output, "(long long s, long long i) { return "
            "((long long *)(intptr_t)*(long long *)(intptr_t)s)[0] + i * ");
        cm_umir_c_render_number(output, cm_umir_c_scalar_size(tyck, elem));
        cm_str_buf_append(output, "; }");
        return 1;
    }
    if (CM_SHIM_IS("unchecked_add") || CM_SHIM_IS("wrapping_add")
        || CM_SHIM_IS("saturating_add")) {
        cm_str_buf_append(output, "(long long a, long long b) "
            "{ return a + b; }");
        return 1;
    }
    if (CM_SHIM_IS("unchecked_sub") || CM_SHIM_IS("wrapping_sub")
        || CM_SHIM_IS("saturating_sub")) {
        cm_str_buf_append(output, "(long long a, long long b) "
            "{ return a - b; }");
        return 1;
    }
    if (CM_SHIM_IS("unchecked_mul") || CM_SHIM_IS("wrapping_mul")) {
        cm_str_buf_append(output, "(long long a, long long b) "
            "{ return a * b; }");
        return 1;
    }
    if (CM_SHIM_IS("unchecked_div") || CM_SHIM_IS("exact_div")) {
        cm_str_buf_append(output, "(long long a, long long b) "
            "{ return b == 0 ? 0 : a / b; }");
        return 1;
    }
    if (CM_SHIM_IS("unchecked_rem")) {
        cm_str_buf_append(output, "(long long a, long long b) "
            "{ return b == 0 ? 0 : a % b; }");
        return 1;
    }
    if (CM_SHIM_IS("unchecked_shl")) {
        cm_str_buf_append(output, "(long long a, long long b) "
            "{ return (long long)((unsigned long long)a << (b & 63)); }");
        return 1;
    }
    if (CM_SHIM_IS("unchecked_shr")) {
        cm_str_buf_append(output, "(long long a, long long b) "
            "{ return (long long)((unsigned long long)a >> (b & 63)); }");
        return 1;
    }
    if (CM_SHIM_IS("ctpop") || CM_SHIM_IS("ctlz") || CM_SHIM_IS("cttz")
        || CM_SHIM_IS("ctlz_nonzero") || CM_SHIM_IS("cttz_nonzero")
        || CM_SHIM_IS("bswap") || CM_SHIM_IS("bitreverse")
        || CM_SHIM_IS("rotate_left") || CM_SHIM_IS("rotate_right")) {
        /* Bit intrinsics at the instance's scalar width. */
        unsigned long bits = 8ul * cm_umir_c_scalar_size(tyck, first);
        char text[512];
        const char *body;
        if (CM_SHIM_IS("ctpop"))
            body = "(long long a) { unsigned long long v = (unsigned long long)a;"
                " long long n = 0; unsigned i; for (i = 0; i < %lu; ++i)"
                " { n += (long long)(v & 1u); v >>= 1; } return n; }";
        else if (CM_SHIM_IS("ctlz") || CM_SHIM_IS("ctlz_nonzero"))
            body = "(long long a) { unsigned long long v = (unsigned long long)a;"
                " long long n = 0; unsigned i; for (i = 0; i < %lu; ++i)"
                " { if ((v >> (%lu - 1 - i)) & 1u) break; n += 1; }"
                " return n; }";
        else if (CM_SHIM_IS("cttz") || CM_SHIM_IS("cttz_nonzero"))
            body = "(long long a) { unsigned long long v = (unsigned long long)a;"
                " long long n = 0; unsigned i; for (i = 0; i < %lu; ++i)"
                " { if ((v >> i) & 1u) break; n += 1; } return n; }";
        else if (CM_SHIM_IS("bswap"))
            body = "(long long a) { unsigned long long v = (unsigned long long)a,"
                " r = 0; unsigned i; for (i = 0; i < %lu / 8; ++i)"
                " { r = (r << 8) | (v & 0xffu); v >>= 8; } return (long long)r; }";
        else if (CM_SHIM_IS("bitreverse"))
            body = "(long long a) { unsigned long long v = (unsigned long long)a,"
                " r = 0; unsigned i; for (i = 0; i < %lu; ++i)"
                " { r = (r << 1) | (v & 1u); v >>= 1; } return (long long)r; }";
        else if (CM_SHIM_IS("rotate_left"))
            body = "(long long a, long long n) { unsigned long long v ="
                " (unsigned long long)a; unsigned long long m = %lu == 64 ?"
                " ~0ull : ((1ull << %lu) - 1); unsigned s = (unsigned)n %% %lu;"
                " v &= m; return (long long)(((v << s) | (v >> ((%lu - s) %% %lu)))"
                " & m); }";
        else
            body = "(long long a, long long n) { unsigned long long v ="
                " (unsigned long long)a; unsigned long long m = %lu == 64 ?"
                " ~0ull : ((1ull << %lu) - 1); unsigned s = (unsigned)n %% %lu;"
                " v &= m; return (long long)(((v >> s) | (v << ((%lu - s) %% %lu)))"
                " & m); }";
        (void)snprintf(text, sizeof text, body, bits, bits, bits, bits, bits);
        cm_str_buf_append(output, text);
        return 1;
    }
    if (CM_SHIM_IS("copy_nonoverlapping") || CM_SHIM_IS("copy")) {
        /* (src, dst, count) at the element's byte size. */
        cm_str_buf_append(output, "(long long s, long long d, long long n) "
            "{ memmove((void *)(intptr_t)d, (const void *)(intptr_t)s, "
            "(unsigned long)n * ");
        cm_umir_c_render_number(output, cm_umir_c_scalar_size(tyck, first));
        cm_str_buf_append(output, "); return 0; }");
        return 1;
    }
    if (CM_SHIM_IS("write_bytes")) {
        cm_str_buf_append(output, "(long long d, long long v, long long n) "
            "{ memset((void *)(intptr_t)d, (int)v, (unsigned long)n * ");
        cm_umir_c_render_number(output, cm_umir_c_scalar_size(tyck, first));
        cm_str_buf_append(output, "); return 0; }");
        return 1;
    }
    if (CM_SHIM_IS("read_via_copy") || CM_SHIM_IS("volatile_load")) {
        const char *scalar = cm_umir_c_scalar_type(tyck, first);
        cm_str_buf_append(output, "(long long p) { return (long long)*(");
        cm_str_buf_append(output, scalar == NULL ? "long long" : scalar);
        cm_str_buf_append(output, " *)(intptr_t)p; }");
        return 1;
    }
    if (CM_SHIM_IS("write_via_move") || CM_SHIM_IS("volatile_store")) {
        const char *scalar = cm_umir_c_scalar_type(tyck, first);
        cm_str_buf_append(output, "(long long p, long long v) { *(");
        cm_str_buf_append(output, scalar == NULL ? "long long" : scalar);
        cm_str_buf_append(output, " *)(intptr_t)p = (");
        cm_str_buf_append(output, scalar == NULL ? "long long" : scalar);
        cm_str_buf_append(output, ")v; return 0; }");
        return 1;
    }
    if (CM_SHIM_IS("needs_drop")) {
        cm_str_buf_append(output, "() { return 0; }");
        return 1;
    }
    if (CM_SHIM_IS("atomic_load") || CM_SHIM_IS("atomic_store")
        || CM_SHIM_IS("atomic_xchg") || CM_SHIM_IS("atomic_xadd")
        || CM_SHIM_IS("atomic_xsub") || CM_SHIM_IS("atomic_and")
        || CM_SHIM_IS("atomic_or") || CM_SHIM_IS("atomic_xor")
        || CM_SHIM_IS("atomic_nand") || CM_SHIM_IS("atomic_max")
        || CM_SHIM_IS("atomic_min") || CM_SHIM_IS("atomic_umax")
        || CM_SHIM_IS("atomic_umin") || CM_SHIM_IS("atomic_cxchg")
        || CM_SHIM_IS("atomic_cxchgweak") || CM_SHIM_IS("atomic_fence")
        || CM_SHIM_IS("atomic_singlethreadfence")) {
        /* Single-threaded C99: the ordering is irrelevant, every
         * read-modify-write is a plain load / store at T's width. */
        const char *scalar = cm_umir_c_scalar_type(tyck, first);
        const char *ty = scalar == NULL ? "long long" : scalar;
        if (CM_SHIM_IS("atomic_fence")
            || CM_SHIM_IS("atomic_singlethreadfence")) {
            cm_str_buf_append(output, "() { return 0; }");
            return 1;
        }
        if (CM_SHIM_IS("atomic_load")) {
            cm_str_buf_append(output, "(long long p) { return (long long)*(");
            cm_str_buf_append(output, ty);
            cm_str_buf_append(output, " *)(intptr_t)p; }");
            return 1;
        }
        if (CM_SHIM_IS("atomic_store")) {
            cm_str_buf_append(output, "(long long p, long long v) { *(");
            cm_str_buf_append(output, ty);
            cm_str_buf_append(output, " *)(intptr_t)p = (");
            cm_str_buf_append(output, ty);
            cm_str_buf_append(output, ")v; return 0; }");
            return 1;
        }
        if (CM_SHIM_IS("atomic_cxchg") || CM_SHIM_IS("atomic_cxchgweak")) {
            /* (old, swapped): a two-slot block like the overflow shims. */
            cm_str_buf_append(output, "(long long p, long long old, long long"
                " v) { long long *t = (long long *)malloc(16); ");
            cm_str_buf_append(output, ty);
            cm_str_buf_append(output, " *a = (");
            cm_str_buf_append(output, ty);
            cm_str_buf_append(output, " *)(intptr_t)p; t[0] = (long long)*a;"
                " t[1] = *a == (");
            cm_str_buf_append(output, ty);
            cm_str_buf_append(output, ")old; if (t[1]) *a = (");
            cm_str_buf_append(output, ty);
            cm_str_buf_append(output, ")v; return (long long)(intptr_t)t; }");
            return 1;
        }
        {
            const char *op = CM_SHIM_IS("atomic_xchg") ? "(T)v"
                : CM_SHIM_IS("atomic_xadd") ? "(T)(o + v)"
                : CM_SHIM_IS("atomic_xsub") ? "(T)(o - v)"
                : CM_SHIM_IS("atomic_and") ? "(T)(o & v)"
                : CM_SHIM_IS("atomic_or") ? "(T)(o | v)"
                : CM_SHIM_IS("atomic_xor") ? "(T)(o ^ v)"
                : CM_SHIM_IS("atomic_nand") ? "(T)~(o & v)"
                : CM_SHIM_IS("atomic_max") || CM_SHIM_IS("atomic_umax")
                    ? "(T)(o > (T)v ? o : (T)v)"
                : "(T)(o < (T)v ? o : (T)v)";
            const char *scan;
            cm_str_buf_append(output, "(long long p, long long v) { ");
            cm_str_buf_append(output, ty);
            cm_str_buf_append(output, " *a = (");
            cm_str_buf_append(output, ty);
            cm_str_buf_append(output, " *)(intptr_t)p; ");
            cm_str_buf_append(output, ty);
            cm_str_buf_append(output, " o = *a; *a = ");
            for (scan = op; *scan != 0; ++scan) {
                if (*scan == 'T') cm_str_buf_append(output, ty);
                else cm_str_buf_push(output, *scan);
            }
            cm_str_buf_append(output, "; return (long long)o; }");
            return 1;
        }
    }
    if (CM_SHIM_IS("box_new")) {
        /* `Box::new(x)` (1.90's intrinsic): a heap cell holding the value
         * — a scalar in the cell, an aggregate's block pointer in it — so
         * the box reads like every other pointer to a `T` slot. */
        cm_str_buf_append(output, "(long long v) { long long *b = "
            "(long long *)malloc(8); *b = v; return (long long)(intptr_t)b; }");
        return 1;
    }
    if (CM_SHIM_IS("size_of_val") || CM_SHIM_IS("min_align_of_val")) {
        cm_str_buf_append(output, "(long long p) { (void)p; return ");
        cm_umir_c_render_number(output, cm_umir_c_scalar_size(tyck, first));
        cm_str_buf_append(output, "; }");
        return 1;
    }
    if (CM_SHIM_IS("add_with_overflow") || CM_SHIM_IS("sub_with_overflow")
        || CM_SHIM_IS("mul_with_overflow")) {
        /* (T, bool) as a two-slot block; overflow judged at the scalar
         * width (unsigned when the type is unsigned). */
        const CmTy *it = ft;
        int is_signed = it != NULL && it->kind == CM_TY_INT
            && ((CmHirIntType)it->a == CM_HIR_INT_I8
                || (CmHirIntType)it->a == CM_HIR_INT_I16
                || (CmHirIntType)it->a == CM_HIR_INT_I32
                || (CmHirIntType)it->a == CM_HIR_INT_I64
                || (CmHirIntType)it->a == CM_HIR_INT_ISIZE);
        unsigned long bits = 8ul * cm_umir_c_scalar_size(tyck, first);
        const char *op = CM_SHIM_IS("add_with_overflow") ? "+"
            : CM_SHIM_IS("sub_with_overflow") ? "-" : "*";
        char text[640];
        (void)snprintf(text, sizeof text,
            "(long long a, long long b) { long long *t = (long long *)"
            "malloc(16); unsigned long long m = %lu == 64 ? ~0ull : "
            "((1ull << %lu) - 1); unsigned long long ua = (unsigned long long)"
            "a & m, ub = (unsigned long long)b & m; unsigned long long r = "
            "(ua %s ub) & m; t[0] = (long long)r; ",
            bits, bits, op);
        cm_str_buf_append(output, text);
        if (is_signed)
            (void)snprintf(text, sizeof text,
                "{ long long sa = (long long)(ua << (64 - %lu)) >> (64 - %lu);"
                " long long sb = (long long)(ub << (64 - %lu)) >> (64 - %lu);"
                " long long wide = sa %s sb; long long back = (long long)"
                "(((unsigned long long)wide & m) << (64 - %lu)) >> (64 - %lu);"
                " t[1] = wide != back; } return (long long)(intptr_t)t; }",
                bits, bits, bits, bits, op, bits, bits);
        else if (CM_SHIM_IS("sub_with_overflow"))
            (void)snprintf(text, sizeof text,
                "t[1] = ua < ub; return (long long)(intptr_t)t; }");
        else if (CM_SHIM_IS("add_with_overflow"))
            (void)snprintf(text, sizeof text,
                "t[1] = r < ua; return (long long)(intptr_t)t; }");
        else
            (void)snprintf(text, sizeof text,
                "t[1] = ua != 0 && (r / ua) != ub; "
                "return (long long)(intptr_t)t; }");
        cm_str_buf_append(output, text);
        return 1;
    }
    if (CM_SHIM_IS("size_of")) {
        cm_str_buf_append(output, "() { return ");
        cm_umir_c_render_number(output, cm_umir_c_scalar_size(tyck, first));
        cm_str_buf_append(output, "; }");
        return 1;
    }
    if (CM_SHIM_IS("min_align_of") || CM_SHIM_IS("align_of")) {
        cm_str_buf_append(output, "() { return ");
        cm_umir_c_render_number(output, cm_umir_c_scalar_size(tyck, first));
        cm_str_buf_append(output, "; }");
        return 1;
    }
#undef CM_SHIM_IS
    return 0;
}

/* std's `RandomState::new` is built around a thread_local! expansion whose
 * body is not always present in u-MIR.  A deterministic pair of keys keeps
 * the value's real aggregate shape and is sufficient for HashMap semantics;
 * the process-random seeding is a quality/security property, not an ABI one. */
static int cm_umir_c_render_random_state_new(CmStrBuf *output,
    const CmHirContext *hir, const CmTyckSet *tyck,
    const CmHirItem *function, const CmInternedString *name)
{
    const CmHirItem *parent;
    CmTyId self;
    const CmTy *self_ty;
    const CmHirItem *self_item;
    const CmInternedString *self_name;
    if (function == NULL || function->kind != CM_HIR_ITEM_FUNCTION
        || name == NULL || name->len != 3u
        || memcmp(name->bytes, "new", 3u) != 0
        || function->data.function_item.signature.parameter_count != 0u
        || cm_hir_def_id_is_none(function->parent_definition)) return 0;
    parent = cm_umir_c_item_of(hir, function->parent_definition);
    if (parent == NULL || parent->kind != CM_HIR_ITEM_IMPL) return 0;
    self = cm_ty_from_hir((CmTyArena *)&tyck->arena, hir,
        parent->data.impl_item.self_type);
    self_ty = cm_ty_get((CmTyArena *)&tyck->arena,
        cm_ty_resolve((CmTyArena *)&tyck->arena, self));
    self_item = self_ty == NULL || self_ty->kind != CM_TY_ADT ? NULL
        : cm_umir_c_item_of(hir, self_ty->def);
    self_name = self_item == NULL ? NULL
        : cm_interner_get(&hir->strings, self_item->name);
    if (self_name == NULL || self_name->len != 11u
        || memcmp(self_name->bytes, "RandomState", 11u) != 0) return 0;
    cm_str_buf_append(output, "(void) { long long *b = (long long *)"
        "calloc(2, 8); return (long long)(intptr_t)b; }");
    return 1;
}

/* hashbrown's public HashMap::reserve is out-of-line in dependency metadata,
 * so its body is unavailable at the call site.  Install a generously sized
 * first table.  HashMap buckets are `(K, V)`, hence two ABI slots each. */
static int cm_umir_c_render_hashmap_reserve(CmStrBuf *output,
    const CmHirContext *hir, const CmTyckSet *tyck,
    const CmHirItem *function, const CmInternedString *name)
{
    const CmHirItem *parent;
    CmTyId self;
    const CmTy *self_ty;
    const CmHirItem *self_item;
    const CmInternedString *self_name;
    if (function == NULL || function->kind != CM_HIR_ITEM_FUNCTION
        || name == NULL || name->len != 7u
        || memcmp(name->bytes, "reserve", 7u) != 0
        || function->data.function_item.signature.parameter_count == 0u
        || function->data.function_item.signature.parameter_count > 2u
        || cm_hir_def_id_is_none(function->parent_definition)) return 0;
    parent = cm_umir_c_item_of(hir, function->parent_definition);
    if (parent == NULL || parent->kind != CM_HIR_ITEM_IMPL) return 0;
    self = cm_ty_from_hir((CmTyArena *)&tyck->arena, hir,
        parent->data.impl_item.self_type);
    self_ty = cm_ty_get((CmTyArena *)&tyck->arena,
        cm_ty_resolve((CmTyArena *)&tyck->arena, self));
    self_item = self_ty == NULL || self_ty->kind != CM_TY_ADT ? NULL
        : cm_umir_c_item_of(hir, self_ty->def);
    self_name = self_item == NULL ? NULL
        : cm_interner_get(&hir->strings, self_item->name);
    if (self_name == NULL || self_name->len != 7u
        || memcmp(self_name->bytes, "HashMap", 7u) != 0) return 0;
    cm_str_buf_append(output, "(long long p, long long additional) { "
        "long long *map = (long long *)(intptr_t)*(long long *)(intptr_t)p; "
        "long long *inner = (long long *)(intptr_t)map[1]; unsigned long "
        "buckets = 16384, bytes; unsigned char *host, *data, *ctrl; "
        "while (buckets < (unsigned long)additional * 2) buckets *= 2; "
        "if (inner[2] != 0) return 0; bytes = buckets * 16 + buckets + 8; "
        "host = (unsigned char *)calloc(1, bytes + 8); data = host + 8; "
        "ctrl = data + buckets * 16; memset(ctrl, 255, buckets + 8); "
        "inner[0] = (long long)(buckets - 1); inner[1] = (long long)"
        "(intptr_t)ctrl; inner[2] = (long long)(buckets * 7 / 8); "
        "inner[3] = 0; return 0; }");
    return 1;
}

/* hashbrown's SSE2 empty table uses a function-local aligned const.  Its
 * initializer is absent from u-MIR, but consumers require a struct block
 * whose sole represented field points at sixteen EMPTY (0xff) tag bytes. */
static int cm_umir_c_render_aligned_empty_tags(CmStrBuf *output,
    const CmHirItem *item, const CmInternedString *name)
{
    if (item == NULL || item->kind != CM_HIR_ITEM_CONST || name == NULL
        || name->len != 12u
        || memcmp(name->bytes, "ALIGNED_TAGS", 12u) != 0) return 0;
    cm_str_buf_append(output, "(void) { static long long tags[2] = "
        "{-1LL, -1LL}; static long long b[1]; b[0] = (long long)"
        "(intptr_t)tags; return (long long)(intptr_t)b; }");
    return 1;
}

/* `RawTableInner::new` takes a reference to hashbrown's function-local
 * `ALIGNED_TAGS` const.  Dependency u-MIR exposes the const getter, but loses
 * the promotion on `&ALIGNED_TAGS.tags` and otherwise returns a pointer to a
 * dead frame slot.  Materialize the same empty-table value with static tag
 * storage.  The six-slot allocation matches the conservative ADT layout used
 * by ordinary aggregate rendering; its represented fields occupy slots 0..3. */
static int cm_umir_c_render_raw_table_inner_new(CmStrBuf *output,
    const CmHirContext *hir, const CmTyckSet *tyck,
    const CmHirItem *item, const CmUMirInstance *instance)
{
    const CmInternedString *name;
    const CmHirItem *parent;
    const CmHirItem *self_item;
    const CmInternedString *self_name;
    CmTyId self;
    const CmTy *self_ty;
    if (item == NULL || item->kind != CM_HIR_ITEM_FUNCTION
        || item->data.function_item.signature.parameter_count != 0u
        || cm_hir_def_id_is_none(item->parent_definition)) return 0;
    name = cm_interner_get(&hir->strings, item->name);
    if (name == NULL || name->len != 3u
        || memcmp(name->bytes, "new", 3u) != 0) return 0;
    parent = cm_umir_c_item_of(hir, item->parent_definition);
    if (parent == NULL || parent->kind != CM_HIR_ITEM_IMPL) return 0;
    self = cm_ty_from_hir((CmTyArena *)&tyck->arena, hir,
        parent->data.impl_item.self_type);
    self_ty = cm_ty_get((CmTyArena *)&tyck->arena,
        cm_ty_resolve((CmTyArena *)&tyck->arena, self));
    self_item = self_ty == NULL || self_ty->kind != CM_TY_ADT ? NULL
        : cm_umir_c_item_of(hir, self_ty->def);
    self_name = self_item == NULL ? NULL
        : cm_interner_get(&hir->strings, self_item->name);
    if (self_name == NULL || self_name->len != 13u
        || memcmp(self_name->bytes, "RawTableInner", 13u) != 0) return 0;
    cm_str_buf_append(output, "long long ");
    cm_umir_c_render_symbol(output, item->definition);
    if (instance != NULL && (instance->count != 0u
            || instance->self_type != CM_TY_NONE)) {
        cm_str_buf_append(output, "_i");
        cm_umir_c_render_number(output, instance->index);
    }
    cm_str_buf_append(output, "(void) { static long long tags[2] = "
        "{-1LL, -1LL}; long long *b = (long long *)calloc(6, 8); "
        "b[1] = (long long)(intptr_t)tags; return (long long)(intptr_t)b; "
        "} /* shim: promoted RawTableInner::new */\n");
    return 1;
}

/* alloc's specialization fallback can expose
 * `SpecToString<T = str>::spec_to_string` even when the generated nested-ref
 * impl is the applicable leaf.  Formatting through the generic Display body
 * adds another erased-reference layer to its writer.  For the concrete str
 * instance, perform the leaf implementation directly: copy the fat string
 * into the slot representation of an owned String/Vec<u8>. */
static int cm_umir_c_render_str_spec_to_string(CmStrBuf *output,
    const CmHirContext *hir, const CmTyckSet *tyck,
    const CmHirItem *item, const CmUMirInstance *instance)
{
    const CmInternedString *name;
    const CmTy *type;
    if (item == NULL || item->kind != CM_HIR_ITEM_FUNCTION
        || instance == NULL || instance->count != 1u) return 0;
    name = cm_interner_get(&hir->strings, item->name);
    if (name == NULL || name->len != 14u
        || memcmp(name->bytes, "spec_to_string", 14u) != 0) return 0;
    type = cm_ty_get((CmTyArena *)&tyck->arena,
        cm_ty_resolve((CmTyArena *)&tyck->arena, instance->types[0]));
    if (type == NULL || type->kind != CM_TY_STR) return 0;
    cm_str_buf_append(output, "long long ");
    cm_umir_c_render_symbol(output, item->definition);
    cm_str_buf_append(output, "_i");
    cm_umir_c_render_number(output, instance->index);
    cm_str_buf_append(output,
        "(long long p) { long long *pair = (long long *)(intptr_t)"
        "*(long long *)(intptr_t)p; unsigned long n = (unsigned long)pair[1]; "
        "unsigned char *host = (unsigned char *)malloc(8 + (n ? n : 1)); "
        "unsigned char *data = host + 8; long long *raw = (long long *)"
        "calloc(5, 8); "
        "long long *vec = (long long *)calloc(4, 8); ((long long *)host)[0] "
        "= (long long)n; if (n != 0) memmove(data, (void *)(intptr_t)"
        "pair[0], n); raw[0] = (long long)(intptr_t)data; raw[1] = "
        "(long long)n; vec[0] = "
        "(long long)(intptr_t)raw; vec[1] = (long long)n; return "
        "(long long)(intptr_t)vec; } /* shim: str spec_to_string */\n");
    return 1;
}

/* rustc_parse_format's Parser::format is a small state machine, but its
 * deeply nested Option/tuple patterns can remain outside the currently
 * admitted u-MIR frontier.  Keep the override specific to Parser and return
 * the ordinary slot ABI for FormatSpec.  input_vec contains pointers to
 * (Range<usize>, byte position, char) blocks; the parser cursor is slot 3.
 * This covers the structural format grammar used before the final type word
 * while leaving `}` unconsumed, exactly as Parser::next expects. */
static int cm_umir_c_render_parse_format(CmStrBuf *output,
    const CmHirContext *hir, const CmTyckSet *tyck,
    const CmHirItem *item, const CmUMirInstance *instance)
{
    const CmInternedString *name;
    const CmHirItem *parent;
    const CmHirItem *self_item;
    const CmInternedString *self_name;
    const CmHirType *self_ty;
    if (item == NULL || item->kind != CM_HIR_ITEM_FUNCTION
        || item->data.function_item.signature.parameter_count != 1u
        || cm_hir_def_id_is_none(item->parent_definition)) return 0;
    name = cm_interner_get(&hir->strings, item->name);
    if (name == NULL || name->len != 6u
        || memcmp(name->bytes, "format", 6u) != 0) return 0;
    parent = cm_umir_c_item_of(hir, item->parent_definition);
    if (parent == NULL || parent->kind != CM_HIR_ITEM_IMPL) return 0;
    (void)tyck;
    self_ty = cm_hir_get_type(hir, parent->data.impl_item.self_type);
    self_item = self_ty == NULL || self_ty->kind != CM_HIR_TYPE_ADT_KIND
        ? NULL : cm_umir_c_item_of(hir,
            self_ty->data.named_type.definition);
    self_name = self_item == NULL ? NULL
        : cm_interner_get(&hir->strings, self_item->name);
    if (self_name == NULL || self_name->len != 6u
        || memcmp(self_name->bytes, "Parser", 6u) != 0) return 0;

    cm_str_buf_append(output, "long long ");
    cm_umir_c_render_symbol(output, item->definition);
    if (instance != NULL && (instance->count != 0u
            || instance->self_type != CM_TY_NONE)) {
        cm_str_buf_append(output, "_i");
        cm_umir_c_render_number(output, instance->index);
    }
    cm_str_buf_append(output,
        "(long long p) { long long *parser = (long long *)(intptr_t)"
        "*(long long *)(intptr_t)p; long long *vec = (long long *)"
        "(intptr_t)parser[2]; long long *raw = (long long *)(intptr_t)"
        "vec[0]; long long *data = (long long *)(intptr_t)raw[0]; "
        "unsigned long n = (unsigned long)vec[1], i = (unsigned long)"
        "parser[3], value = 0; long long ch = 0, next = 0; "
        "long long *spec = (long long *)calloc(13, 8); "
        "long long *fill = (long long *)calloc(3, 8); "
        "long long *fill_span = (long long *)calloc(3, 8); "
        "long long *align = (long long *)calloc(2, 8); "
        "long long *sign = (long long *)calloc(3, 8); "
        "long long *debug_hex = (long long *)calloc(3, 8); "
        "long long *precision = (long long *)calloc(4, 8); "
        "long long *precision_span = (long long *)calloc(3, 8); "
        "long long *width = (long long *)calloc(4, 8); "
        "long long *width_span = (long long *)calloc(3, 8); "
        "long long *ty_span = (long long *)calloc(3, 8); "
        "long long *empty = (long long *)calloc(3, 8); "
        "align[0] = 3; precision[0] = 4; width[0] = 4; "
        "empty[0] = (long long)(intptr_t)&empty[1]; "
        "spec[0] = (long long)(intptr_t)fill; spec[1] = (long long)"
        "(intptr_t)fill_span; spec[2] = (long long)(intptr_t)align; "
        "spec[3] = (long long)(intptr_t)sign; spec[6] = (long long)"
        "(intptr_t)debug_hex; spec[7] = (long long)(intptr_t)precision; "
        "spec[8] = (long long)(intptr_t)precision_span; spec[9] = "
        "(long long)(intptr_t)width; spec[10] = (long long)(intptr_t)"
        "width_span; spec[11] = (long long)(intptr_t)&empty[0]; "
        "spec[12] = (long long)(intptr_t)ty_span; "
        "if (i >= n || ((long long *)(intptr_t)data[i])[2] != 58) "
        "return (long long)(intptr_t)spec; ++i; "
        "if (i < n) ch = ((long long *)(intptr_t)data[i])[2]; "
        "if (i + 1 < n) next = ((long long *)(intptr_t)data[i + 1])[2]; "
        "if (next == 60 || next == 62 || next == 94) { fill[0] = 1; "
        "fill[1] = ch; ++i; ch = next; } "
        "if (ch == 60 || ch == 62 || ch == 94) { align[0] = ch == 60 "
        "? 0 : ch == 62 ? 1 : 2; ++i; } "
        "if (i < n) ch = ((long long *)(intptr_t)data[i])[2]; "
        "if (ch == 43 || ch == 45) { long long *which = (long long *)"
        "calloc(2, 8); which[0] = ch == 43 ? 0 : 1; sign[0] = 1; "
        "sign[1] = (long long)(intptr_t)which; ++i; } "
        "if (i < n && ((long long *)(intptr_t)data[i])[2] == 35) { "
        "spec[4] = 1; ++i; } "
        "if (i < n && ((long long *)(intptr_t)data[i])[2] == 48) { "
        "spec[5] = 1; ++i; } "
        "while (i < n) { ch = ((long long *)(intptr_t)data[i])[2]; "
        "if (ch < 48 || ch > 57) break; value = value * 10 + "
        "(unsigned long)(ch - 48); ++i; } "
        "if (value != 0) { width[0] = 0; width[1] = (long long)"
        "(uint16_t)value; } "
        "if (i < n && ((long long *)(intptr_t)data[i])[2] == 46) { "
        "++i; value = 0; if (i < n && ((long long *)(intptr_t)data[i])"
        "[2] == 42) { precision[0] = 3; precision[1] = parser[5]++; "
        "++i; } else { while (i < n) { ch = ((long long *)(intptr_t)"
        "data[i])[2]; if (ch < 48 || ch > 57) break; value = value * "
        "10 + (unsigned long)(ch - 48); ++i; } precision[0] = 0; "
        "precision[1] = (long long)(uint16_t)value; } } "
        "if (i < n) { ch = ((long long *)(intptr_t)data[i])[2]; "
        "if (ch == 120 || ch == 88) { long long *which = (long long *)"
        "calloc(2, 8); which[0] = ch == 120 ? 0 : 1; ++i; "
        "if (i < n && ((long long *)(intptr_t)data[i])[2] == 63) { "
        "debug_hex[0] = 1; debug_hex[1] = (long long)(intptr_t)which; "
        "++i; } } else if (ch == 63) ++i; else while (i < n) { ch = "
        "((long long *)(intptr_t)data[i])[2]; if (!((ch >= 65 && ch <= "
        "90) || (ch >= 97 && ch <= 122) || (ch >= 48 && ch <= 57) || "
        "ch == 95)) break; ++i; } } parser[3] = (long long)i; return "
        "(long long)(intptr_t)spec; } /* shim: Parser::format */\n");
    return 1;
}

/* `String::as_bytes` and `String::as_str` cross String's transparent Vec
 * wrapper into an unsized slice.  The String block stores Vec's raw block and
 * String's live length, while the raw block stores the data pointer and
 * capacity.  Materialize a fat descriptor from data plus live length; using
 * either existing block directly would pair the data with capacity or leave
 * an extra reference layer. */
static int cm_umir_c_render_string_slice(CmStrBuf *output,
    const CmHirContext *hir, const CmTyckSet *tyck,
    const CmHirItem *item, const CmUMirInstance *instance)
{
    const CmInternedString *name;
    const CmHirItem *parent;
    const CmHirItem *self_item;
    const CmInternedString *self_name;
    CmTyId self;
    const CmTy *self_ty;
    if (item == NULL || item->kind != CM_HIR_ITEM_FUNCTION
        || cm_hir_def_id_is_none(item->parent_definition)) return 0;
    name = cm_interner_get(&hir->strings, item->name);
    if (name == NULL
        || !((name->len == 8u
                && memcmp(name->bytes, "as_bytes", 8u) == 0)
            || (name->len == 6u
                && memcmp(name->bytes, "as_str", 6u) == 0))) return 0;
    parent = cm_umir_c_item_of(hir, item->parent_definition);
    if (parent == NULL || parent->kind != CM_HIR_ITEM_IMPL) return 0;
    self = cm_ty_from_hir((CmTyArena *)&tyck->arena, hir,
        parent->data.impl_item.self_type);
    self_ty = cm_ty_get((CmTyArena *)&tyck->arena,
        cm_ty_resolve((CmTyArena *)&tyck->arena, self));
    self_item = self_ty == NULL || self_ty->kind != CM_TY_ADT ? NULL
        : cm_umir_c_item_of(hir, self_ty->def);
    self_name = self_item == NULL ? NULL
        : cm_interner_get(&hir->strings, self_item->name);
    if (self_name == NULL || self_name->len != 6u
        || memcmp(self_name->bytes, "String", 6u) != 0) return 0;
    cm_str_buf_append(output, "long long ");
    cm_umir_c_render_symbol(output, item->definition);
    if (instance != NULL && (instance->count != 0u
            || instance->self_type != CM_TY_NONE)) {
        cm_str_buf_append(output, "_i");
        cm_umir_c_render_number(output, instance->index);
    }
    cm_str_buf_append(output, "(long long p) { long long *s = "
        "(long long *)(intptr_t)*(long long *)(intptr_t)p; long long *raw = "
        "(long long *)(intptr_t)s[0]; long long *b = (long long *)"
        "malloc(24); b[1] = raw[0]; b[2] = s[1]; b[0] = "
        "(long long)(intptr_t)&b[1]; return (long long)(intptr_t)&b[0]; } "
        "/* shim: String unsized-slice descriptor */\n");
    return 1;
}

/* `escape::backslash<const N>` is reached from static associated constructors
 * whose concrete `EscapeIterInner<N, _>` destination is absent in partial
 * dependency u-MIR.  All valid instantiations need only the first two cells;
 * use the largest reachable buffer (char escaping's N=10) when N remains
 * unknown.  This is also safe for ascii's N=4 consumer, whose live range is
 * still exactly 0..2. */
static int cm_umir_c_render_unknown_escape_backslash(CmStrBuf *output,
    const CmHirContext *hir, const CmTyckSet *tyck,
    const CmHirItem *item, const CmUMirInstance *instance)
{
    const CmInternedString *name;
    const CmHirItem *parent;
    const CmTy *argument;
    if (item == NULL || item->kind != CM_HIR_ITEM_FUNCTION
        || instance == NULL || instance->count != 1u
        || item->generic_parameter_count != 1u
        || item->data.function_item.signature.parameter_count != 1u)
        return 0;
    name = cm_interner_get(&hir->strings, item->name);
    if (name == NULL || name->len != 9u
        || memcmp(name->bytes, "backslash", 9u) != 0) return 0;
    parent = cm_hir_def_id_is_none(item->parent_definition) ? NULL
        : cm_umir_c_item_of(hir, item->parent_definition);
    if (parent != NULL && (parent->kind == CM_HIR_ITEM_IMPL
            || parent->kind == CM_HIR_ITEM_TRAIT)) return 0;
    argument = cm_ty_get((CmTyArena *)&tyck->arena,
        cm_ty_resolve((CmTyArena *)&tyck->arena, instance->types[0]));
    if (argument == NULL || (argument->kind != CM_TY_CONST_PARAM
            && argument->kind != CM_TY_CONST_UNKNOWN)) return 0;
    cm_str_buf_append(output, "long long ");
    cm_umir_c_render_symbol(output, item->definition);
    cm_str_buf_append(output, "_i");
    cm_umir_c_render_number(output, instance->index);
    cm_str_buf_append(output,
        "(long long a) { unsigned long i; long long *nullv = (long long *)"
        "calloc(2, 8), *slash = (long long *)calloc(2, 8), *range = "
        "(long long *)calloc(4, 8), *tuple = (long long *)calloc(4, 8); "
        "long long *array = (long long *)((char *)malloc(8 + 11 * 8) + 8); "
        "array[-1] = 10; for (i = 0; i < 10; ++i) array[i] = "
        "(long long)(intptr_t)nullv; slash[0] = 92; array[0] = "
        "(long long)(intptr_t)slash; array[1] = a; range[0] = 0; "
        "range[1] = 2; tuple[0] = (long long)(intptr_t)array; tuple[1] = "
        "(long long)(intptr_t)range; return (long long)(intptr_t)tuple; "
        "} /* shim: unresolved escape::backslash<N> */\n");
    return 1;
}

/* The source spelling of `_mm_set1_epi8` calls a 16-argument SIMD
 * constructor, wider than one u-MIR call operand record.  The slot ABI uses
 * the packed low eight lanes consistently (and reports Group::WIDTH as 8). */
static int cm_umir_c_render_packed_simd_override(CmStrBuf *output,
    const CmHirContext *hir, const CmHirItem *item,
    const CmUMirInstance *instance)
{
    const CmInternedString *name = item == NULL ? NULL
        : cm_interner_get(&hir->strings, item->name);
    if (item == NULL || item->kind != CM_HIR_ITEM_FUNCTION || name == NULL
        || name->len != 13u
        || memcmp(name->bytes, "_mm_set1_epi8", 13u) != 0) return 0;
    cm_str_buf_append(output, "long long ");
    cm_umir_c_render_symbol(output, item->definition);
    if (instance != NULL && (instance->count != 0u
            || instance->self_type != CM_TY_NONE)) {
        cm_str_buf_append(output, "_i");
        cm_umir_c_render_number(output, instance->index);
    }
    cm_str_buf_append(output, "(long long a) { unsigned long long x = "
        "(unsigned char)a; x |= x << 8; x |= x << 16; x |= x << 32; "
        "return (long long)x; } /* shim: packed _mm_set1_epi8 */\n");
    return 1;
}

/* Body-less intrinsics with a lenient C rendering: the frame is uniform
 * `long long` slots (aggregates travel as slot pointers), so bit-casts and
 * hints are identity and the divergent ones abort.  NULL when unknown. */
static const char *cm_umir_c_intrinsic_shim(const CmInternedString *name)
{
    static const struct { const char *name; const char *body; } table[] = {
        { "assert_inhabited", "() { return 0; }" },
        { "assert_zero_valid", "() { return 0; }" },
        { "assert_mem_uninitialized_valid", "() { return 0; }" },
        { "ub_checks", "() { return 0; }" },
        { "cold_path", "() { return 0; }" },
        { "likely", "(long long a) { return a; }" },
        { "unlikely", "(long long a) { return a; }" },
        { "black_box", "(long long a) { return a; }" },
        { "assume", "(long long a) { (void)a; return 0; }" },
        { "forget", "(long long a) { (void)a; return 0; }" },
        /* A declaration-only Clone fallback copies the value held in the
         * receiver slot.  Scalars copy directly; aggregate values are boxed
         * slot pointers, so this is the representation's shallow clone. */
        { "clone", "(long long p) { return p ? *(long long *)(intptr_t)p"
            " : 0; }" },
        /* `#[lang = "drop_in_place"]` has an intentionally recursive Rust
         * body: rustc replaces it with compiler drop glue.  Our DROP rvalue
         * already invokes the concrete Drop impl and walks struct fields. */
        { "drop_in_place", "(long long p) { (void)p; return 0; }" },
        { "caller_location", "(void) { return 0; }" },
        { "abort", "(void) { abort(); return 0; }" },
        { "unreachable", "(void) { abort(); return 0; }" },
        /* No unwinding: the try fn runs to completion and the catch fn
         * is never invoked (`std::panicking::try` then reads `data.r`). */
        { "catch_unwind", "(long long f, long long d, long long c) { "
            "(void)c; ((long long (*)(long long))(intptr_t)f)(d); "
            "return 0; }" },
    };
    size_t scan;
    if (name == NULL) return NULL;
    for (scan = 0u; scan < sizeof table / sizeof table[0]; ++scan)
        if (strlen(table[scan].name) == name->len
            && memcmp(table[scan].name, name->bytes, name->len) == 0)
            return table[scan].body;
    return NULL;
}

static const CmHirItem *cm_umir_c_item_of(const CmHirContext *hir,
    CmHirDefId def)
{
    const CmHirDefinition *record = cm_hir_lookup_definition(hir, def);
    if (record == NULL || record->kind != CM_HIR_DEFINITION_ITEM)
        return NULL;
    return cm_hir_get_item(hir, record->entity.item_id);
}

/* Body id owned by `def`, or 0. */
static const CmUMirBody *cm_umir_c_umir_body(const CmUMirSet *umir,
    const CmHirContext *hir, CmHirDefId def, CmUExprId closure_expr)
{
    size_t index;
    for (index = 0u; index < umir->bodies.len; ++index) {
        const CmUMirBody *body = (const CmUMirBody *)cm_vec_at_const(
            &umir->bodies, index);
        const CmHirBody *hir_body;
        if (body == NULL || !body->complete
            || body->closure_expr != closure_expr) continue;
        hir_body = cm_hir_get_body(hir, body->source);
        if (hir_body != NULL
            && cm_hir_def_id_equal(hir_body->origin.definition, def))
            return body;
    }
    return NULL;
}

/* Find or add the instance (def, types[count]); returns its index. */
static long cm_umir_c_instance(CmUMirProgram *program, CmHirDefId def,
    CmUExprId closure_expr, const CmTyId *types, uint32_t count,
    CmTyId self_type)
{
    size_t index;
    CmUMirInstance instance;
    const CmHirItem *item = cm_umir_c_item_of(program->hir, def);
    CmHirGenericParamId parameters[32];
    uint32_t parameter_count;
    uint32_t arg;
    if (item == NULL) return -1;
    parameter_count = cm_umir_c_collect_parameters(program->hir, item,
        parameters, 32u);
    if (count > parameter_count) count = parameter_count;
    for (index = 0u; index < program->instances.len; ++index) {
        const CmUMirInstance *have = (const CmUMirInstance *)
            cm_vec_at_const(&program->instances, index);
        int same = have != NULL && cm_hir_def_id_equal(have->definition,
            def) && have->closure_expr == closure_expr
            && have->count == count
            && cm_ty_resolve((CmTyArena *)&program->tyck->arena,
                have->self_type)
                == cm_ty_resolve((CmTyArena *)&program->tyck->arena,
                    self_type);
        for (arg = 0u; same && arg < count; ++arg)
            if (cm_ty_resolve((CmTyArena *)&program->tyck->arena,
                    have->types[arg])
                != cm_ty_resolve((CmTyArena *)&program->tyck->arena,
                    types[arg])) same = 0;
        if (same) return (long)index;
    }
    memset(&instance, 0, sizeof(instance));
    instance.definition = def;
    instance.closure_expr = closure_expr;
    instance.self_type = self_type;
    {
        const CmUMirBody *found = cm_umir_c_umir_body(program->umir,
            program->hir, def, closure_expr);
        instance.body = found == NULL ? 0u : found->source;
    }
    instance.count = count;
    instance.types = (CmTyId *)cm_alloc_zeroed(count == 0u ? 1u : count,
        sizeof(CmTyId));
    instance.parameters = (CmHirGenericParamId *)cm_alloc_zeroed(
        count == 0u ? 1u : count, sizeof(CmHirGenericParamId));
    if (getenv("CMRUSTC_UMIR_DEBUG") != NULL) {
        /* `UMIR instance <name> #<n> [args]` — a bare parameter argument
         * marks an instance created without a binding for it. */
        const CmInternedString *name = cm_interner_get(&program->hir->strings,
            item->name);
        CmStrBuf text;
        int bare = 0;
        cm_str_buf_init(&text);
        for (arg = 0u; arg < count; ++arg) {
            const CmTy *at = cm_ty_get((CmTyArena *)&program->tyck->arena,
                cm_ty_resolve((CmTyArena *)&program->tyck->arena,
                    types[arg]));
            if (arg != 0u) cm_str_buf_append(&text, ", ");
            cm_ty_print((CmTyArena *)&program->tyck->arena, program->hir,
                types[arg], &text);
            if (at != NULL && at->kind == CM_TY_PARAM) bare = 1;
        }
        fprintf(stderr, "UMIR instance %.*s #%lu%s [%.*s]\n",
            name == NULL ? 1 : (int)name->len,
            name == NULL ? "?" : (const char *)name->bytes,
            (unsigned long)program->instances.len,
            bare ? " BARE" : "", (int)text.len, text.data);
        cm_str_buf_destroy(&text);
    }
    for (arg = 0u; arg < count; ++arg) {
        instance.types[arg] = types[arg];
        instance.parameters[arg] = parameters[arg];
    }
    instance.index = (unsigned long)program->instances.len;
    (void)cm_vec_push(&program->instances, &instance);
    return (long)(program->instances.len - 1u);
}

/* Structural type equality (ids are not hash-consed across sources). */
static int cm_umir_c_ty_equal(const CmTyckSet *tyck, CmTyId left,
    CmTyId right, unsigned int depth)
{
    CmTyArena *arena = (CmTyArena *)&tyck->arena;
    const CmTy *a = cm_ty_get(arena, cm_ty_resolve(arena, left));
    const CmTy *b = cm_ty_get(arena, cm_ty_resolve(arena, right));
    uint32_t index;
    if (a == b) return 1;
    if (a == NULL || b == NULL || depth > 16u) return 0;
    if (a->kind != b->kind || a->count != b->count) return 0;
    switch (a->kind) {
    case CM_TY_INT:
    case CM_TY_FLOAT:
    case CM_TY_REF:
    case CM_TY_PTR:
    case CM_TY_PARAM:
        if (a->a != b->a) return 0;
        break;
    case CM_TY_ADT:
    case CM_TY_FN_DEF:
    case CM_TY_SELF:
    case CM_TY_FOREIGN:
    case CM_TY_DYN:
        if (!cm_hir_def_id_equal(a->def, b->def)) return 0;
        break;
    case CM_TY_CONST:
        if (a->lo != b->lo || a->hi != b->hi) return 0;
        break;
    default:
        break;
    }
    for (index = 0u; index < a->count; ++index)
        if (!cm_umir_c_ty_equal(tyck, a->children[index],
                b->children[index], depth + 1u)) return 0;
    return 1;
}

/* Structural match of an impl self type `pattern` against `actual`,
 * binding the impl's generic parameters (`impl<W> Tr for &mut W` against
 * `&mut Acc` binds W := Acc) so the resolved method instance carries them. */
static int cm_umir_c_ty_match(const CmTyckSet *tyck, CmTyId pattern,
    CmTyId actual, CmHirGenericParamId *params, CmTyId *binds,
    uint32_t *count, uint32_t capacity, unsigned int depth)
{
    CmTyArena *arena = (CmTyArena *)&tyck->arena;
    const CmTy *a = cm_ty_get(arena, cm_ty_resolve(arena, pattern));
    const CmTy *b = cm_ty_get(arena, cm_ty_resolve(arena, actual));
    uint32_t index;
    if (a == NULL || b == NULL || depth > 16u) return 0;
    if (a->kind == CM_TY_PARAM || a->kind == CM_TY_CONST_PARAM) {
        /* Type and const generic parameters bind alike (`[T; N]`). */
        for (index = 0u; index < *count; ++index)
            if (params[index] == (CmHirGenericParamId)a->a)
                return cm_umir_c_ty_equal(tyck, binds[index], actual,
                    depth);
        if (*count >= capacity) return 0;
        params[*count] = (CmHirGenericParamId)a->a;
        binds[*count] = actual;
        *count += 1u;
        return 1;
    }
    if (a == b) return 1;
    if (a->kind != b->kind || a->count != b->count) return 0;
    switch (a->kind) {
    case CM_TY_INT:
    case CM_TY_FLOAT:
    case CM_TY_REF:
    case CM_TY_PTR:
        if (a->a != b->a) return 0;
        break;
    case CM_TY_ADT:
    case CM_TY_FN_DEF:
    case CM_TY_SELF:
    case CM_TY_FOREIGN:
    case CM_TY_DYN:
        if (!cm_hir_def_id_equal(a->def, b->def)) return 0;
        break;
    case CM_TY_CONST:
        if (a->lo != b->lo || a->hi != b->hi) return 0;
        break;
    default:
        break;
    }
    for (index = 0u; index < a->count; ++index)
        if (!cm_umir_c_ty_match(tyck, a->children[index],
                b->children[index], params, binds, count, capacity,
                depth + 1u)) return 0;
    return 1;
}

/* Select `<Self as Trait>::Assoc` from the matching impl and instantiate
 * its target with the generic bindings recovered from Self.  Typeck keeps
 * projections while a surrounding generic call is open; codegen sees the
 * concrete monomorphized Self and can finish that selection here. */
static CmTyId cm_umir_c_normalize_projection(const CmHirContext *hir,
    const CmTyckSet *tyck, CmTyId type)
{
    CmTyArena *arena = (CmTyArena *)&tyck->arena;
    unsigned int depth;
    for (depth = 0u; depth < 8u; ++depth) {
        const CmTy *projection = type == CM_TY_NONE ? NULL
            : cm_ty_get(arena, cm_ty_resolve(arena, type));
        CmTyId selected = CM_TY_NONE;
        int blanket_pass;
        size_t impl_index;
        if (projection == NULL || projection->kind != CM_TY_PROJECTION
            || projection->count == 0u) break;
        /* A concrete structural impl wins over a bare-parameter blanket
         * impl.  Without predicate solving both appear to match (for
         * example `IntoIterator for &Vec<T>` and `IntoIterator for I`),
         * but only the former supplies the collection's real IntoIter. */
        for (blanket_pass = 0; blanket_pass < 2
                && selected == CM_TY_NONE; ++blanket_pass) {
        for (impl_index = 0u; impl_index < hir->items.len; ++impl_index) {
            const CmHirItem *impl_item = (const CmHirItem *)cm_vec_at_const(
                &hir->items, impl_index);
            CmHirGenericParamId parameters[32];
            CmTyId arguments[32];
            uint32_t argument_count = 0u;
            CmTyId impl_self;
            size_t member_index;
            if (impl_item == NULL || impl_item->kind != CM_HIR_ITEM_IMPL
                || !impl_item->data.impl_item.has_trait
                || impl_item->data.impl_item.polarity
                    != CM_HIR_IMPL_POSITIVE
                || !cm_hir_def_id_equal(
                    impl_item->data.impl_item.trait_type.definition,
                    projection->def)) continue;
            impl_self = cm_ty_from_hir(arena, hir,
                impl_item->data.impl_item.self_type);
            {
                const CmTy *pattern = cm_ty_get(arena,
                    cm_ty_resolve(arena, impl_self));
                int is_blanket = pattern != NULL
                    && (pattern->kind == CM_TY_PARAM
                        || pattern->kind == CM_TY_SELF);
                if (is_blanket != (blanket_pass == 1)) continue;
            }
            if (!cm_umir_c_ty_match(tyck, impl_self,
                    projection->children[0], parameters, arguments,
                    &argument_count, 32u, 0u)) continue;
            for (member_index = 0u; member_index < hir->items.len;
                    ++member_index) {
                const CmHirItem *member = (const CmHirItem *)cm_vec_at_const(
                    &hir->items, member_index);
                CmTySubst subst;
                CmTyId target;
                if (member == NULL || member->kind != CM_HIR_ITEM_TYPE_ALIAS
                    || !cm_hir_def_id_equal(member->parent_definition,
                        impl_item->definition)
                    || !cm_hir_def_id_equal(
                        member->data.type_alias_item.trait_item_definition,
                        projection->def2)
                    || member->data.type_alias_item.target
                        == CM_HIR_TYPE_NONE) continue;
                target = cm_ty_from_hir(arena, hir,
                    member->data.type_alias_item.target);
                subst.parameters = parameters;
                subst.types = arguments;
                subst.count = argument_count;
                subst.self_type = projection->children[0];
                target = cm_ty_subst(arena, target, &subst);
                if (selected != CM_TY_NONE
                    && !cm_umir_c_ty_equal(tyck, selected, target, 0u))
                    return type;
                selected = target;
            }
        }
        }
        if (selected == CM_TY_NONE || selected == type) break;
        type = selected;
    }
    return type;
}

/* Resolve a trait-method declaration to the impl method for `self`. */
static const CmUMirBody *cm_umir_c_active_body;
/* Concrete first trait argument carried by the active method FN_DEF.  Impl
 * selection uses it to distinguish equal-Self impls such as the integer
 * `TryFrom<Source> for Target` matrix. */
static CmTyId cm_umir_c_expected_trait_arg = CM_TY_NONE;
static CmTyId cm_umir_c_local_type(const CmUMirBody *body, CmUMirLocalId local);

/* Whether `method`'s parameter types (with impl generics bound so far)
 * accept the call's operand types: distinguishes `impl SliceIndex<[T]>`
 * from `impl SliceIndex<str>` for the same Self. */
static int cm_umir_c_method_accepts(const CmHirContext *hir,
    const CmTyckSet *tyck, const CmHirItem *method,
    const CmUMirStatement *statement, uint32_t first_arg,
    CmHirGenericParamId *bound_params, CmTyId *bound_types, uint32_t *bound)
{
    const CmHirFunctionSignature *sig = &method->data.function_item.signature;
    uint32_t operands;
    uint32_t skip;
    uint32_t param;
    if (statement == NULL || cm_umir_c_active_body == NULL
        || statement->operand_overflow != 0u
        || statement->operand_count < first_arg) return 1;
    operands = statement->operand_count - first_arg;
    skip = sig->parameter_count == operands ? 0u
        : sig->parameter_count + 1u == operands ? 1u : 0xFFFFu;
    if (skip == 0xFFFFu) return 1;
    for (param = 0u; param < sig->parameter_count; ++param) {
        CmTyId pattern = cm_ty_from_hir((CmTyArena *)&tyck->arena, hir,
            sig->parameters[param].type);
        /* The operand's type through the active instance: a bare `T'`
         * in the caller's body is concrete here, and must bind the impl's
         * own `T` (the offset shim scales by the element it binds). */
        CmTyId actual = cm_umir_c_subst(cm_umir_c_local_type(
            cm_umir_c_active_body,
            statement->operands[first_arg + skip + param]));
        const CmTy *pt;
        const CmTy *at;
        if (pattern == CM_TY_NONE || actual == CM_TY_NONE) continue;
        pt = cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena, pattern));
        at = cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena, actual));
        /* Only shape-level disagreement rejects (a `Self`, projection,
         * or infer on either side is accepted). */
        if (pt == NULL || at == NULL || pt->kind == CM_TY_SELF
            || pt->kind == CM_TY_PROJECTION || at->kind == CM_TY_INFER
            || at->kind == CM_TY_PARAM || at->kind == CM_TY_PROJECTION)
            continue;
        if (!cm_umir_c_ty_match(tyck, pattern, actual, bound_params,
                bound_types, bound, 32u, 0u)) {
            /* Reference/pointer flavor differences are tolerated: peel
             * both sides (independently — an autoref'd receiver carries
             * one fewer layer than its `&mut Self` parameter, `&[T]`
             * coerces to a `*const [T]` parameter) and match the
             * pointees so the impl's own parameters still bind. */
            CmTyId pp_id = pattern;
            CmTyId ap_id = actual;
            const CmTy *pp = pt;
            const CmTy *ap = at;
            uint32_t pattern_layers = 0u;
            uint32_t actual_layers = 0u;
            int is_receiver = param == 0u
                && sig->receiver != CM_HIR_RECEIVER_NONE;
            while (pp != NULL
                && (pp->kind == CM_TY_REF || pp->kind == CM_TY_PTR)) {
                pp_id = pp->children[0];
                pp = cm_ty_get((CmTyArena *)&tyck->arena,
                    cm_ty_resolve((CmTyArena *)&tyck->arena, pp_id));
                ++pattern_layers;
            }
            while (ap != NULL
                && (ap->kind == CM_TY_REF || ap->kind == CM_TY_PTR)) {
                ap_id = ap->children[0];
                ap = cm_ty_get((CmTyArena *)&tyck->arena,
                    cm_ty_resolve((CmTyArena *)&tyck->arena, ap_id));
                ++actual_layers;
            }
            {
                uint32_t peeled_bound = *bound;
                int peeled_matches = pp != NULL && ap != NULL
                    && cm_umir_c_ty_match(tyck, pp_id, ap_id, bound_params,
                        bound_types, &peeled_bound, 32u, 0u);
            /* Only a receiver is auto-referenced: an ordinary argument
             * never gains a layer, so a `&mut T` parameter does not accept
             * a bare `Inner<T>` (that impl is a sibling, not this one —
             * `Unique<T>: From<&mut T>` calls `Self::from(NonNull<T>)`).
             * Operator lowering is the exception: it passes a bare `P` for
             * source-level `&P`; accept that only when the peeled nominal
             * pointee is exactly the operand type. */
            if (!is_receiver && pattern_layers > actual_layers
                && ap != NULL && ap->kind != CM_TY_PARAM
                && ap->kind != CM_TY_INFER && ap->kind != CM_TY_PROJECTION
                && ap->kind != CM_TY_SELF && !peeled_matches) {
                if (getenv("CMRUSTC_UMIR_DEBUG") != NULL)
                    fprintf(stderr, "UMIR reject param=%u layers %u>%u\n",
                        (unsigned)param, (unsigned)pattern_layers,
                        (unsigned)actual_layers);
                return 0;
            }
            if (peeled_matches) *bound = peeled_bound;
            }
            if (pp != NULL && ap != NULL && pp->kind != ap->kind
                && pp->kind != CM_TY_PARAM && pp->kind != CM_TY_SELF
                && pp->kind != CM_TY_PROJECTION && ap->kind != CM_TY_PARAM
                && ap->kind != CM_TY_INFER) {
                if (getenv("CMRUSTC_UMIR_DEBUG") != NULL) {
                    CmStrBuf text;
                    cm_str_buf_init(&text);
                    cm_ty_print((CmTyArena *)&tyck->arena, hir, pattern,
                        &text);
                    cm_str_buf_append(&text, " vs ");
                    cm_ty_print((CmTyArena *)&tyck->arena, hir, actual,
                        &text);
                    fprintf(stderr, "UMIR reject param=%u %.*s\n",
                        (unsigned)param, (int)text.len, text.data);
                    cm_str_buf_destroy(&text);
                }
                return 0;
            }
        }
    }
    return 1;
}

/* `declaration`'s method inside `impl`, if its parameters accept the
 * call; binds the impl's generics positionally into out_types. */
static CmHirDefId cm_umir_c_impl_method(const CmHirContext *hir,
    const CmTyckSet *tyck, const CmHirItem *impl,
    const CmHirItem *declaration, const CmUMirStatement *statement,
    uint32_t first_arg, CmHirGenericParamId *bound_params,
    CmTyId *bound_types, uint32_t *bound, CmTyId *out_types,
    uint32_t *out_count)
{
    size_t child;
    for (child = 0u; child < hir->items.len; ++child) {
        const CmHirItem *method = (const CmHirItem *)cm_vec_at_const(
            &hir->items, child);
        if (method == NULL || method->kind != declaration->kind
            || (method->kind != CM_HIR_ITEM_FUNCTION
                && method->kind != CM_HIR_ITEM_CONST)
            || !cm_hir_def_id_equal(method->parent_definition,
                impl->definition)
            || method->name != declaration->name) continue;
        if (method->kind == CM_HIR_ITEM_FUNCTION
            && !cm_umir_c_method_accepts(hir, tyck, method, statement,
                first_arg, bound_params, bound_types, bound))
            return cm_hir_def_id_none();
        if (out_types != NULL && out_count != NULL) {
            /* Positional over the impl's parameters; an unbound one keeps
             * itself (identity substitution). */
            uint32_t param;
            uint32_t limit = impl->generic_parameter_count > 32u
                ? 32u : impl->generic_parameter_count;
            for (param = 0u; param < limit; ++param) {
                CmHirGenericParamId id = impl->generic_parameter_start + param;
                uint32_t scan;
                out_types[param] = cm_ty_param((CmTyArena *)&tyck->arena, id);
                for (scan = 0u; scan < *bound; ++scan)
                    if (bound_params[scan] == id)
                        out_types[param] = bound_types[scan];
            }
            *out_count = limit;
        }
        return method->definition;
    }
    return cm_hir_def_id_none();
}

static CmUMirProgram *cm_umir_c_active_program;
/* The crate compiled last: its `fn main` is the program's entry. */
static CmHirCrateId cm_umir_c_root_crate;
/* The program's `fn main` (none when absent) and, when std provides
 * it, the `#[lang = "start"]` instance for `T = ()` that wraps it. */
static int cm_umir_c_have_root_main;
static CmHirDefId cm_umir_c_root_main_def;
static long cm_umir_c_lang_start_instance = -1;
/* Set when the lang_start instance rendered as a stub: the entry then
 * calls `main` directly rather than a runtime that returns at once. */
static int cm_umir_c_lang_start_stubbed;

/* A parameterless, parentless `fn main` of the root crate. */
static int cm_umir_c_is_root_main(const CmHirContext *hir,
    const CmHirItem *owner, CmHirDefId def)
{
    const CmInternedString *name = owner == NULL ? NULL
        : cm_interner_get(&hir->strings, owner->name);
    return name != NULL && name->len == 4u
        && memcmp(name->bytes, "main", 4u) == 0
        && owner->kind == CM_HIR_ITEM_FUNCTION
        && cm_hir_def_id_is_none(owner->parent_definition)
        && owner->data.function_item.signature.parameter_count == 0u
        && def.crate_id == cm_umir_c_root_crate;
}

/* The program instance of closure type `ct` (its body instanced on the
 * enclosing scope: children[0] = Self or a bare SELF, then the generic
 * arguments), or -1 without a program. */
static long cm_umir_c_closure_instance_of(const CmHirContext *hir,
    const CmTyckSet *tyck, const CmTy *ct)
{
    const CmHirBody *closure_body;
    CmTyId closure_self = CM_TY_NONE;
    const CmTyId *closure_args = NULL;
    uint32_t closure_arg_count = 0u;
    if (cm_umir_c_active_program == NULL || ct == NULL
        || ct->kind != CM_TY_CLOSURE) return -1;
    closure_body = cm_hir_get_body(hir, (CmHirBodyId)ct->a);
    if (closure_body == NULL) return -1;
    if (ct->count != 0u) {
        const CmTy *self_child = cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena, ct->children[0]));
        if (self_child != NULL && self_child->kind != CM_TY_SELF)
            closure_self = ct->children[0];
        closure_args = ct->children + 1;
        closure_arg_count = ct->count - 1u;
    }
    return cm_umir_c_instance(cm_umir_c_active_program,
        closure_body->origin.definition, (CmUExprId)ct->b, closure_args,
        closure_arg_count, closure_self);
}

/* `call` / `call_mut` / `call_once`: the Fn-family method a closure's
 * vtable entry or a `dyn Fn*` call dispatches to. */
static int cm_umir_c_is_fn_call_method(const CmHirContext *hir,
    const CmHirItem *method)
{
    const CmInternedString *name = method == NULL ? NULL
        : cm_interner_get(&hir->strings, method->name);
    if (name == NULL) return 0;
    return (name->len == 4u && memcmp(name->bytes, "call", 4u) == 0)
        || (name->len == 8u && memcmp(name->bytes, "call_mut", 8u) == 0)
        || (name->len == 9u && memcmp(name->bytes, "call_once", 9u) == 0);
}

#define CM_UMIR_C_MAX_TRAIT_CLOSURE 64u
static size_t cm_umir_c_trait_closure(const CmHirContext *hir,
    CmHirDefId trait_def, CmHirDefId *out, size_t count);

/* The Fn-family method declared by trait `principal` (or a supertrait),
 * for dispatching a call on `dyn principal`; none when it has none. */
static CmHirDefId cm_umir_c_dyn_call_method(const CmHirContext *hir,
    CmHirDefId principal)
{
    CmHirDefId closure[CM_UMIR_C_MAX_TRAIT_CLOSURE];
    size_t closure_count = cm_umir_c_trait_closure(hir, principal, closure,
        0u);
    size_t closure_index;
    for (closure_index = 0u; closure_index < closure_count;
            ++closure_index) {
        size_t scan;
        for (scan = 0u; scan < hir->items.len; ++scan) {
            const CmHirItem *method = (const CmHirItem *)cm_vec_at_const(
                &hir->items, scan);
            if (method == NULL || method->kind != CM_HIR_ITEM_FUNCTION
                || !cm_hir_def_id_equal(method->parent_definition,
                    closure[closure_index])) continue;
            if (cm_umir_c_is_fn_call_method(hir, method))
                return method->definition;
        }
    }
    return cm_hir_def_id_none();
}

static CmHirDefId cm_umir_c_resolve_impl_method(const CmHirContext *hir,
    const CmTyckSet *tyck, const CmHirItem *declaration, CmTyId self,
    CmTyId *out_types, uint32_t *out_count,
    const CmUMirStatement *statement, uint32_t first_arg)
{
    const CmHirItem *trait_item;
    size_t index;
    int blanket_pass;
    CmTyId self_resolved = cm_ty_resolve((CmTyArena *)&tyck->arena, self);
    CmHirDefId bodiless = cm_hir_def_id_none();
    CmTyId bodiless_types[32];
    uint32_t bodiless_count = 0u;
    if (out_count != NULL) *out_count = 0u;
    if (declaration == NULL
        || cm_hir_def_id_is_none(declaration->parent_definition))
        return cm_hir_def_id_none();
    trait_item = cm_umir_c_item_of(hir, declaration->parent_definition);
    if (trait_item == NULL || trait_item->kind != CM_HIR_ITEM_TRAIT)
        return cm_hir_def_id_none();
    /* Specific impls before blanket ones (self a bare parameter, bounds
     * unchecked here): `impl<F: FnPtr> Debug for F` precedes `impl Debug
     * for bool` in item order and would otherwise capture `bool`. */
    for (blanket_pass = 0; blanket_pass < 2; ++blanket_pass)
    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *impl = (const CmHirItem *)cm_vec_at_const(
            &hir->items, index);
        CmTyId impl_self;
        CmTyId matched_self = self_resolved;
        int matched;
        if (impl == NULL || impl->kind != CM_HIR_ITEM_IMPL
            || !cm_hir_def_id_equal(
                impl->data.impl_item.trait_type.definition,
                trait_item->definition)) continue;
        CmHirGenericParamId bound_params[32];
        CmTyId bound_types[32];
        uint32_t bound = 0u;
        impl_self = cm_ty_from_hir((CmTyArena *)&tyck->arena, hir,
            impl->data.impl_item.self_type);
        {
            const CmTy *pattern = cm_ty_get((CmTyArena *)&tyck->arena,
                cm_ty_resolve((CmTyArena *)&tyck->arena, impl_self));
            int is_blanket = pattern != NULL
                && (pattern->kind == CM_TY_PARAM
                    || pattern->kind == CM_TY_SELF);
            if (is_blanket != (blanket_pass == 1)) continue;
        }
        matched = cm_umir_c_ty_match(tyck, impl_self, matched_self,
            bound_params, bound_types, &bound, 32u, 0u);
        if (!matched && blanket_pass == 0) {
            const CmTy *receiver = cm_ty_get((CmTyArena *)&tyck->arena,
                cm_ty_resolve((CmTyArena *)&tyck->arena, self_resolved));
            if (receiver != NULL && receiver->kind == CM_TY_REF
                && receiver->a != 0u) {
                CmTyId pointee = receiver->children[0];
                matched_self = cm_ty_ref((CmTyArena *)&tyck->arena,
                    pointee, 0);
                bound = 0u;
                matched = cm_umir_c_ty_match(tyck, impl_self,
                    matched_self, bound_params, bound_types, &bound, 32u,
                    0u);
            }
        }
        if (!matched) {
            if (getenv("CMRUSTC_UMIR_DEBUG") != NULL) {
                const CmTy *a = cm_ty_get((CmTyArena *)&tyck->arena,
                    cm_ty_resolve((CmTyArena *)&tyck->arena, impl_self));
                const CmTy *b = cm_ty_get((CmTyArena *)&tyck->arena,
                    self_resolved);
                if (a != NULL && b != NULL && a->kind == b->kind
                    && (a->kind != CM_TY_ADT
                        || cm_hir_def_id_equal(a->def, b->def))) {
                    CmStrBuf text;
                    cm_str_buf_init(&text);
                    cm_ty_print((CmTyArena *)&tyck->arena, hir, impl_self,
                        &text);
                    cm_str_buf_append(&text, " vs ");
                    cm_ty_print((CmTyArena *)&tyck->arena, hir,
                        self_resolved, &text);
                    fprintf(stderr, "UMIR impl-miss %.*s (children %u vs "
                        "%u)\n", (int)text.len, text.data,
                        (unsigned)a->count, (unsigned)b->count);
                    cm_str_buf_destroy(&text);
                }
            }
            continue;
        }
        if (cm_umir_c_expected_trait_arg != CM_TY_NONE
            && impl->data.impl_item.trait_type.argument_count != 0u
            && impl->data.impl_item.trait_type.arguments != NULL
            && impl->data.impl_item.trait_type.arguments[0].kind
                == CM_HIR_GENERIC_ARG_TYPE) {
            CmTySubst subst;
            CmTyId impl_arg = cm_ty_from_hir((CmTyArena *)&tyck->arena, hir,
                impl->data.impl_item.trait_type.arguments[0].data.type);
            subst.parameters = bound_params;
            subst.types = bound_types;
            subst.count = bound;
            subst.self_type = self_resolved;
            impl_arg = cm_ty_subst((CmTyArena *)&tyck->arena, impl_arg,
                &subst);
            if (!cm_umir_c_ty_equal(tyck, impl_arg,
                    cm_umir_c_expected_trait_arg, 0u))
                continue;
        }
        {
            CmHirDefId found = cm_umir_c_impl_method(hir, tyck, impl,
                declaration, statement, first_arg, bound_params,
                bound_types, &bound, out_types, out_count);
            if (!cm_hir_def_id_is_none(found)) {
                /* Bounds are not modelled: `impl<T: Clone> ConvertVec for
                 * T` and `impl<T: Copy> ConvertVec for T` both accept
                 * `u8`.  An accepting method without a u-MIR body (a
                 * partial default) yields to a later one that has a
                 * body; it stays the fallback. */
                const CmUMirProgram *program = cm_umir_c_active_program;
                if (program == NULL || program->umir == NULL
                    || cm_umir_c_umir_body(program->umir, hir, found,
                        CM_U_EXPR_NONE) != NULL)
                    return found;
                if (cm_hir_def_id_is_none(bodiless)) {
                    bodiless = found;
                    if (out_types != NULL && out_count != NULL) {
                        memcpy(bodiless_types, out_types,
                            *out_count * sizeof(CmTyId));
                        bodiless_count = *out_count;
                    }
                }
            }
        }
    }
    if (!cm_hir_def_id_is_none(bodiless)) {
        if (out_types != NULL && out_count != NULL) {
            memcpy(out_types, bodiless_types,
                bodiless_count * sizeof(CmTyId));
            *out_count = bodiless_count;
        }
        return bodiless;
    }
    {
        /* Integer-width leniency: an index inferred as the default `i32`
         * still reaches the `usize` impl (`SliceIndex<[T]> for usize`). */
        const CmTy *self_ty = cm_ty_get((CmTyArena *)&tyck->arena,
            self_resolved);
        int exact_impl = 0;
        /* An impl for the exact integer type (even one inheriting the
         * method as a default) must not be shadowed by another width. */
        for (index = 0u; self_ty != NULL && self_ty->kind == CM_TY_INT
                && index < hir->items.len; ++index) {
            const CmHirItem *impl = (const CmHirItem *)cm_vec_at_const(
                &hir->items, index);
            if (impl == NULL || impl->kind != CM_HIR_ITEM_IMPL
                || !cm_hir_def_id_equal(
                    impl->data.impl_item.trait_type.definition,
                    trait_item->definition)) continue;
            if (cm_umir_c_ty_equal(tyck, cm_ty_from_hir(
                    (CmTyArena *)&tyck->arena, hir,
                    impl->data.impl_item.self_type), self_resolved, 0u)) {
                exact_impl = 1;
                break;
            }
        }
        if (self_ty != NULL && self_ty->kind == CM_TY_INT && !exact_impl) {
            for (index = 0u; index < hir->items.len; ++index) {
                const CmHirItem *impl = (const CmHirItem *)cm_vec_at_const(
                    &hir->items, index);
                const CmTy *impl_ty;
                if (impl == NULL || impl->kind != CM_HIR_ITEM_IMPL
                    || !cm_hir_def_id_equal(
                        impl->data.impl_item.trait_type.definition,
                        trait_item->definition)) continue;
                impl_ty = cm_ty_get((CmTyArena *)&tyck->arena,
                    cm_ty_resolve((CmTyArena *)&tyck->arena,
                        cm_ty_from_hir((CmTyArena *)&tyck->arena, hir,
                            impl->data.impl_item.self_type)));
                if (impl_ty == NULL || impl_ty->kind != CM_TY_INT) continue;
                {
                    /* The same parameter filter as the exact scan: the
                     * `usize` impls of `SliceIndex<ByteStr>` and
                     * `SliceIndex<[T]>` differ only in their `slice`. */
                    CmHirGenericParamId fb_params[32];
                    CmTyId fb_types[32];
                    uint32_t fb_bound = 0u;
                    CmHirDefId found = cm_umir_c_impl_method(hir, tyck,
                        impl, declaration, statement, first_arg, fb_params,
                        fb_types, &fb_bound, out_types, out_count);
                    if (!cm_hir_def_id_is_none(found)) return found;
                }
            }
        }
    }
    return cm_hir_def_id_none();
}

static CmUMirProgram *cm_umir_c_active_program = NULL;
static const CmUMirInstance *cm_umir_c_active_instance = NULL;

/* Find or add the vtable (trait, concrete type); returns its index. */
static long cm_umir_c_vtable(CmUMirProgram *program, CmHirDefId trait_def,
    CmTyId type)
{
    size_t index;
    CmUMirVtable entry;
    for (index = 0u; index < program->vtables.len; ++index) {
        const CmUMirVtable *have = (const CmUMirVtable *)cm_vec_at_const(
            &program->vtables, index);
        if (have != NULL && cm_hir_def_id_equal(have->trait_def, trait_def)
            && cm_umir_c_ty_equal(program->tyck, have->type, type, 0u))
            return (long)index;
    }
    entry.trait_def = trait_def;
    entry.type = type;
    if (cm_vec_push(&program->vtables, &entry) == NULL) return -1;
    return (long)(program->vtables.len - 1u);
}

/* A `dyn P` vtable lays out every trait in P's supertrait closure —
 * supertraits first, in declaration order, then P — so `dyn FnMut(A) ->
 * R` dispatches `FnOnce::call_once` and `dyn Sub<Out = T>` a `Base`
 * method through one table.  Returns the number of traits written. */
static size_t cm_umir_c_trait_closure(const CmHirContext *hir,
    CmHirDefId trait_def, CmHirDefId *out, size_t count)
{
    const CmHirItem *item;
    size_t index;
    uint32_t super_index;

    for (index = 0u; index < count; ++index)
        if (cm_hir_def_id_equal(out[index], trait_def)) return count;
    if (count >= CM_UMIR_C_MAX_TRAIT_CLOSURE) return count;
    item = cm_umir_c_item_of(hir, trait_def);
    if (item != NULL && item->kind == CM_HIR_ITEM_TRAIT) {
        for (super_index = 0u;
             super_index < item->data.trait_item.supertrait_count;
             ++super_index) {
            count = cm_umir_c_trait_closure(hir,
                item->data.trait_item.supertraits[super_index].trait_type
                    .definition, out, count);
        }
    }
    for (index = 0u; index < count; ++index)
        if (cm_hir_def_id_equal(out[index], trait_def)) return count;
    if (count < CM_UMIR_C_MAX_TRAIT_CLOSURE) out[count++] = trait_def;
    return count;
}

/* Position of a trait method in `dyn principal`'s vtable. */
static long cm_umir_c_trait_method_index(const CmHirContext *hir,
    CmHirDefId principal, CmHirDefId method)
{
    const CmHirItem *item = cm_umir_c_item_of(hir, method);
    CmHirDefId closure[CM_UMIR_C_MAX_TRAIT_CLOSURE];
    size_t closure_count;
    size_t trait_index;
    size_t index;
    long slot = 0;
    if (item == NULL || cm_hir_def_id_is_none(item->parent_definition))
        return -1;
    {
        /* A dyn type reached through a closure capture may carry a
         * non-trait def: lay the table out from the method's own trait. */
        const CmHirItem *principal_item = cm_hir_def_id_is_none(principal)
            ? NULL : cm_umir_c_item_of(hir, principal);
        if (principal_item == NULL || principal_item->kind != CM_HIR_ITEM_TRAIT)
            principal = item->parent_definition;
    }
    closure_count = cm_umir_c_trait_closure(hir, principal, closure, 0u);
    for (trait_index = 0u; trait_index < closure_count; ++trait_index) {
        for (index = 0u; index < hir->items.len; ++index) {
            const CmHirItem *cand = (const CmHirItem *)cm_vec_at_const(
                &hir->items, index);
            if (cand == NULL || cand->kind != CM_HIR_ITEM_FUNCTION
                || !cm_hir_def_id_equal(cand->parent_definition,
                    closure[trait_index])) continue;
            if (cm_hir_def_id_equal(cand->definition, method)) return slot;
            slot += 1;
        }
    }
    /* The receiver's recorded principal does not reach the method's
     * trait (an upcast or a differently recorded dyn): the table was
     * built for the method's own trait — index it there. */
    if (!cm_hir_def_id_equal(principal, item->parent_definition)) {
        if (getenv("CMRUSTC_UMIR_DEBUG") != NULL) {
            const CmHirItem *pi = cm_umir_c_item_of(hir, principal);
            const CmHirItem *ti = cm_umir_c_item_of(hir,
                item->parent_definition);
            const CmInternedString *pn = pi == NULL ? NULL
                : cm_interner_get(&hir->strings, pi->name);
            const CmInternedString *tn = ti == NULL ? NULL
                : cm_interner_get(&hir->strings, ti->name);
            fprintf(stderr, "UMIR dyn-slot fallback principal=%.*s "
                "method-trait=%.*s\n", pn == NULL ? 1 : (int)pn->len,
                pn == NULL ? "?" : (const char *)pn->bytes,
                tn == NULL ? 1 : (int)tn->len,
                tn == NULL ? "?" : (const char *)tn->bytes);
        }
        return cm_umir_c_trait_method_index(hir, item->parent_definition,
            method);
    }
    return -1;
}

static CmTyId cm_umir_c_subst(CmTyId type)
{
    CmTySubst subst;
    if (cm_umir_c_active_program == NULL
        || cm_umir_c_active_instance == NULL
        || (cm_umir_c_active_instance->count == 0u
            && cm_umir_c_active_instance->self_type == CM_TY_NONE)
        || type == CM_TY_NONE) return type;
    subst.parameters = cm_umir_c_active_instance->parameters;
    subst.types = cm_umir_c_active_instance->types;
    subst.count = cm_umir_c_active_instance->count;
    subst.self_type = cm_umir_c_active_instance->self_type;
    return cm_ty_subst((CmTyArena *)&cm_umir_c_active_program->tyck->arena,
        type, &subst);
}

/* Render a value-path naming a const generic from the active monomorphized
 * instance (`self.len() == N`).  Type substitution already carries const
 * arguments as CM_TY_CONST; unlike type parameters there is no runtime value
 * or item body to call. */
static int cm_umir_c_render_const_parameter(CmStrBuf *output,
    const CmTyckSet *tyck, const CmUExpr *expr)
{
    uint32_t index;
    if (expr == NULL || expr->kind != CM_U_EXPR_PATH
        || expr->data.path.resolution.kind
            != CM_U_RESOLVED_GENERIC_PARAM
        || cm_umir_c_active_instance == NULL
        || cm_umir_c_active_instance->parameters == NULL
        || cm_umir_c_active_instance->types == NULL) return 0;
    for (index = 0u; index < cm_umir_c_active_instance->count; ++index) {
        const CmTy *argument;
        if (cm_umir_c_active_instance->parameters[index]
                != expr->data.path.resolution.generic_parameter)
            continue;
        argument = cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena,
                cm_umir_c_active_instance->types[index]));
        if (argument == NULL || argument->kind != CM_TY_CONST
            || argument->hi != 0u) return 0;
        cm_umir_c_render_number(output, (unsigned long)argument->lo);
        return 1;
    }
    return 0;
}

/* 1 + the autoderef step tyck found a METHOD_CALL's method at (0 when
 * unknown).  Lowering shapes the receiver operand from it: at step 0 the
 * operand is an autoref whose local keeps the referent's type (so it
 * reads as Self already); at a later step it is the reference reached
 * there, i.e. `&Self`. */
static unsigned int cm_umir_c_receiver_steps(const CmTyckSet *tyck,
    const CmUMirStatement *statement)
{
    const CmTyckBody *tb = statement == NULL || cm_umir_c_active_body == NULL
        ? NULL : cm_tyck_get(tyck, cm_umir_c_active_body->source);
    if (tb == NULL || tb->receiver_steps == NULL
        || statement->expr == CM_U_EXPR_NONE) return 0u;
    return tb->receiver_steps[statement->expr];
}

/* Symbol for a callee reached with FN_DEF type `callee_type`: registers
 * the instance when the program is collecting. */
static void cm_umir_c_render_callee_symbol(CmStrBuf *output,
    const CmHirContext *hir, const CmTyckSet *tyck, CmHirDefId def,
    CmTyId callee_type, CmTyId receiver_type,
    const CmUMirStatement *statement, uint32_t first_arg)
{
    CmUMirProgram *program = cm_umir_c_active_program;
    const CmTyckBody *active_tb = statement == NULL
            || cm_umir_c_active_body == NULL ? NULL
        : cm_tyck_get(tyck, cm_umir_c_active_body->source);
    const CmHirItem *item = cm_umir_c_item_of(hir, def);
    const CmHirItem *parent = item == NULL
            || cm_hir_def_id_is_none(item->parent_definition) ? NULL
        : cm_umir_c_item_of(hir, item->parent_definition);
    const CmTy *fn_ty;
    CmTyId args[32];
    uint32_t count = 0u;
    uint32_t index;
    long instance = -1;
    CmTyId bound_self = CM_TY_NONE;
    if (program != NULL && item != NULL
        && !cm_hir_def_id_is_none(item->parent_definition)) {
        if (parent != NULL && parent->kind == CM_HIR_ITEM_IMPL
            && !cm_hir_def_id_is_none(
                parent->data.impl_item.trait_type.definition)
            && receiver_type != CM_TY_NONE) {
            /* tyck may record an impl's method for a receiver typed by a
             * parameter (`self.start.clone()` with `T: Step`, bound to
             * `Clone` through a supertrait, landed on `[T; N]`'s impl).
             * If the impl's Self does not fit the concrete receiver,
             * re-resolve through the trait's declaration. */
            CmTyId self = cm_umir_c_subst(receiver_type);
            const CmTy *self_ty = cm_ty_get((CmTyArena *)&tyck->arena,
                cm_ty_resolve((CmTyArena *)&tyck->arena, self));
            CmTyId impl_self = cm_ty_from_hir((CmTyArena *)&tyck->arena,
                hir, parent->data.impl_item.self_type);
            CmHirGenericParamId probe_params[32];
            CmTyId probe_types[32];
            uint32_t probe_bound = 0u;
            int fits;
            const CmHirItem *impl_trait = cm_umir_c_item_of(hir,
                parent->data.impl_item.trait_type.definition);
            const CmInternedString *impl_trait_name = impl_trait == NULL
                ? NULL : cm_interner_get(&hir->strings, impl_trait->name);
            const CmHirItem *receiver_item = self_ty == NULL
                    || self_ty->kind != CM_TY_ADT ? NULL
                : cm_umir_c_item_of(hir, self_ty->def);
            const CmInternedString *receiver_name = receiver_item == NULL
                ? NULL : cm_interner_get(&hir->strings, receiver_item->name);
            int validate_arguments = impl_trait_name != NULL
                && impl_trait_name->len == 9u
                && memcmp(impl_trait_name->bytes, "PartialEq", 9u) == 0
                && receiver_name != NULL && receiver_name->len == 6u
                && memcmp(receiver_name->bytes, "String", 6u) == 0;
            CmHirReceiverKind callee_receiver = item->kind
                    == CM_HIR_ITEM_FUNCTION
                ? item->data.function_item.signature.receiver
                : CM_HIR_RECEIVER_NONE;
            unsigned int steps = cm_umir_c_receiver_steps(tyck, statement);
            /* A `&self` / `&mut self` callee reached at a later autoderef
             * step got the reference itself as operand, i.e. `&Self`:
             * Self is one layer down, before a bare `T` pattern gets the
             * chance to swallow the reference.  (At step 0 the autoref
             * local already carries Self; unknown keeps the lenient
             * peel-while-mismatched walk below.) */
            if ((callee_receiver == CM_HIR_RECEIVER_REF_SHARED
                    || callee_receiver == CM_HIR_RECEIVER_REF_MUTABLE)
                && steps >= 2u
                && self_ty != NULL && (self_ty->kind == CM_TY_REF
                    || self_ty->kind == CM_TY_PTR)) {
                self = self_ty->children[0];
                self_ty = cm_ty_get((CmTyArena *)&tyck->arena,
                    cm_ty_resolve((CmTyArena *)&tyck->arena, self));
            }
            while (self_ty != NULL && (self_ty->kind == CM_TY_REF
                    || self_ty->kind == CM_TY_PTR)
                && !cm_umir_c_ty_match(tyck, impl_self, self, probe_params,
                    probe_types, &probe_bound, 32u, 0u)) {
                probe_bound = 0u;
                self = self_ty->children[0];
                self_ty = cm_ty_get((CmTyArena *)&tyck->arena,
                    cm_ty_resolve((CmTyArena *)&tyck->arena, self));
            }
            probe_bound = 0u;
            if (getenv("CMRUSTC_UMIR_DEBUG") != NULL) {
                CmStrBuf text;
                cm_str_buf_init(&text);
                cm_ty_print((CmTyArena *)&tyck->arena, hir, impl_self, &text);
                cm_str_buf_append(&text, " vs ");
                cm_ty_print((CmTyArena *)&tyck->arena, hir, self, &text);
                fprintf(stderr, "UMIR impl-callee %.*s %.*s steps=%u expr=%lu\n",
                    (int)cm_interner_get(&hir->strings, item->name)->len,
                    (const char *)cm_interner_get(&hir->strings,
                        item->name)->bytes, (int)text.len, text.data,
                    steps, statement == NULL ? 0ul
                        : (unsigned long)statement->expr);
                cm_str_buf_destroy(&text);
            }
            fits = self_ty == NULL || self_ty->kind == CM_TY_PARAM
                || self_ty->kind == CM_TY_INFER
                || self_ty->kind == CM_TY_PROJECTION
                || self_ty->kind == CM_TY_SELF
                || cm_umir_c_ty_match(tyck, impl_self, self, probe_params,
                    probe_types, &probe_bound, 32u, 0u);
            if (fits && validate_arguments
                && item->kind == CM_HIR_ITEM_FUNCTION
                && !cm_umir_c_method_accepts(hir, tyck, item, statement,
                    first_arg, probe_params, probe_types, &probe_bound))
                fits = 0;
            if (!fits) {
                const CmHirItem *trait_item = cm_umir_c_item_of(hir,
                    parent->data.impl_item.trait_type.definition);
                size_t scan;
                for (scan = 0u; trait_item != NULL
                        && scan < hir->items.len; ++scan) {
                    const CmHirItem *decl = (const CmHirItem *)
                        cm_vec_at_const(&hir->items, scan);
                    if (decl != NULL && decl->kind == CM_HIR_ITEM_FUNCTION
                        && cm_hir_def_id_equal(decl->parent_definition,
                            trait_item->definition)
                        && decl->name == item->name) {
                        if (getenv("CMRUSTC_UMIR_DEBUG") != NULL)
                            fprintf(stderr, "UMIR impl-reroute %.*s\n",
                                (int)cm_interner_get(&hir->strings,
                                    item->name)->len,
                                (const char *)cm_interner_get(&hir->strings,
                                    item->name)->bytes);
                        item = decl;
                        def = decl->definition;
                        parent = trait_item;
                        callee_type = CM_TY_NONE;
                        break;
                    }
                }
            }
        }
        if (parent != NULL && parent->kind == CM_HIR_ITEM_TRAIT) {
            /* Declaration: resolve against the substituted receiver (a
             * qualified `<T as Tr>::f(..)` carries Self in the FN_DEF's
             * first slot); a trait default method stays itself with Self
             * bound. */
            CmTyId self = cm_umir_c_subst(receiver_type);
            CmTyId receiver_self;
            CmHirDefId resolved;
            int exact_self = cm_umir_c_exact_self;
            cm_umir_c_expected_trait_arg = CM_TY_NONE;
            if (self == CM_TY_NONE && callee_type != CM_TY_NONE) {
                const CmTy *ct = cm_ty_get((CmTyArena *)&tyck->arena,
                    cm_ty_resolve((CmTyArena *)&tyck->arena,
                        cm_umir_c_subst(callee_type)));
                if (ct != NULL && ct->kind == CM_TY_FN_DEF && ct->count != 0u) {
                    /* A path's Self (`T::method` with `T = &mut Buffer`)
                     * is exact, like a vtable's concrete type. */
                    self = cm_umir_c_subst(ct->children[0]);
                    exact_self = 1;
                    if (ct->count > 1u) {
                        const CmInternedString *trait_name =
                            cm_interner_get(&hir->strings, parent->name);
                        CmTyId candidate = cm_umir_c_subst(ct->children[1]);
                        const CmTy *self_ty = cm_ty_get(
                            (CmTyArena *)&tyck->arena,
                            cm_ty_resolve((CmTyArena *)&tyck->arena, self));
                        const CmTy *arg_ty = cm_ty_get(
                            (CmTyArena *)&tyck->arena,
                            cm_ty_resolve((CmTyArena *)&tyck->arena,
                                candidate));
                        if (trait_name != NULL && trait_name->len == 7u
                            && memcmp(trait_name->bytes, "TryFrom", 7u) == 0
                            && self_ty != NULL && arg_ty != NULL
                            && self_ty->kind == CM_TY_INT
                            && self_ty->a == CM_HIR_INT_U64
                            && arg_ty->kind == CM_TY_INT
                            && arg_ty->a == CM_HIR_INT_I32)
                            cm_umir_c_expected_trait_arg = candidate;
                    }
                }
            }
            self = cm_umir_c_normalize_projection(hir, tyck, self);
            receiver_self = self;
            CmTyId bound_types[32];
            uint32_t bound_count = 0u;
            CmHirReceiverKind receiver =
                item->data.function_item.signature.receiver;
            resolved = cm_hir_def_id_none();
            if (exact_self) {
                /* A vtable's concrete type is exact: `&mut Buffer: Write`
                 * is the forwarding `impl Write for &mut W`, not
                 * `Buffer`'s own impl (that one expects `&mut Buffer`,
                 * not `&mut &mut Buffer`, as its data pointer). */
                resolved = cm_umir_c_resolve_impl_method(hir, tyck, item,
                    self, bound_types, &bound_count, statement, first_arg);
            }
            if (cm_hir_def_id_is_none(resolved)) {
                /* `&self` / `&mut self` receivers: the impl's self type
                 * is the referent, one layer down; a by-value `self`
                 * keeps the receiver type (impls for `&mut W` exist). */
                const CmTy *self_ty = cm_ty_get((CmTyArena *)&tyck->arena,
                    cm_ty_resolve((CmTyArena *)&tyck->arena, self));
                if (self_ty != NULL && (self_ty->kind == CM_TY_REF
                        || self_ty->kind == CM_TY_PTR)
                    && (receiver == CM_HIR_RECEIVER_REF_SHARED
                        || receiver == CM_HIR_RECEIVER_REF_MUTABLE)
                    /* not at autoderef step 0: that operand is an autoref
                     * local already typed as Self */
                    && cm_umir_c_receiver_steps(tyck, statement) != 1u)
                    self = self_ty->children[0];
                resolved = cm_umir_c_resolve_impl_method(hir, tyck, item,
                    self, bound_types, &bound_count, statement, first_arg);
            }
            if (cm_hir_def_id_is_none(resolved)) {
                const CmTy *projection = cm_ty_get(
                    (CmTyArena *)&tyck->arena,
                    cm_ty_resolve((CmTyArena *)&tyck->arena, self));
                const CmHirItem *projection_trait = projection == NULL
                        || projection->kind != CM_TY_PROJECTION ? NULL
                    : cm_umir_c_item_of(hir, projection->def);
                const CmHirItem *projection_assoc = projection == NULL
                        || projection->kind != CM_TY_PROJECTION ? NULL
                    : cm_umir_c_item_of(hir, projection->def2);
                const CmInternedString *trait_name = projection_trait == NULL
                    ? NULL : cm_interner_get(&hir->strings,
                        projection_trait->name);
                const CmInternedString *assoc_name = projection_assoc == NULL
                    ? NULL : cm_interner_get(&hir->strings,
                        projection_assoc->name);
                if (projection != NULL && projection->count != 0u
                    && trait_name != NULL && trait_name->len == 12u
                    && memcmp(trait_name->bytes, "IntoIterator", 12u) == 0
                    && assoc_name != NULL && assoc_name->len == 8u
                    && memcmp(assoc_name->bytes, "IntoIter", 8u) == 0) {
                    /* The blanket `IntoIterator for I where I: Iterator`
                     * has `IntoIter = I`.  Resolve against that Self only
                     * when it really supplies this method; collection types
                     * with a distinct IntoIter keep their projection. */
                    CmTyId identity = projection->children[0];
                    CmHirDefId identity_method =
                        cm_umir_c_resolve_impl_method(hir, tyck, item,
                            identity, bound_types, &bound_count, statement,
                            first_arg);
                    if (!cm_hir_def_id_is_none(identity_method)) {
                        self = identity;
                        resolved = identity_method;
                    }
                }
            }
            {
                /* Lenient fallback: peel every reference layer. */
                const CmTy *self_ty = cm_ty_get((CmTyArena *)&tyck->arena,
                    cm_ty_resolve((CmTyArena *)&tyck->arena, self));
                while (cm_hir_def_id_is_none(resolved) && self_ty != NULL
                    && (self_ty->kind == CM_TY_REF
                        || self_ty->kind == CM_TY_PTR)) {
                    self = self_ty->children[0];
                    self_ty = cm_ty_get((CmTyArena *)&tyck->arena,
                        cm_ty_resolve((CmTyArena *)&tyck->arena, self));
                    resolved = cm_umir_c_resolve_impl_method(hir, tyck,
                        item, self, bound_types, &bound_count, statement,
                        first_arg);
                }
            }
            if (getenv("CMRUSTC_UMIR_DEBUG") != NULL) {
                CmStrBuf text;
                cm_str_buf_init(&text);
                cm_ty_print((CmTyArena *)&tyck->arena, hir, receiver_type,
                    &text);
                cm_str_buf_append(&text, " => ");
                cm_ty_print((CmTyArena *)&tyck->arena, hir, self, &text);
                {
                    const CmInternedString *tn = cm_interner_get(
                        &hir->strings, parent->name);
                    const CmInternedString *mn = cm_interner_get(
                        &hir->strings, item->name);
                    size_t impls = 0u;
                    size_t scan;
                    for (scan = 0u; scan < hir->items.len; ++scan) {
                        const CmHirItem *impl = (const CmHirItem *)
                            cm_vec_at_const(&hir->items, scan);
                        if (impl != NULL && impl->kind == CM_HIR_ITEM_IMPL
                            && cm_hir_def_id_equal(
                                impl->data.impl_item.trait_type.definition,
                                parent->definition)) impls += 1u;
                    }
                    fprintf(stderr, "UMIR decl-call %.*s::%.*s def=%u:%u "
                        "impls=%lu receiver %.*s resolved=%d\n",
                        tn == NULL ? 1 : (int)tn->len,
                        tn == NULL ? "?" : (const char *)tn->bytes,
                        mn == NULL ? 1 : (int)mn->len,
                        mn == NULL ? "?" : (const char *)mn->bytes,
                        (unsigned)def.crate_id, (unsigned)def.index,
                        (unsigned long)impls, (int)text.len, text.data,
                        !cm_hir_def_id_is_none(resolved));
                }
                cm_str_buf_destroy(&text);
            }
            if (!cm_hir_def_id_is_none(resolved)) {
                const CmTy *resolved_fn = callee_type == CM_TY_NONE ? NULL
                    : cm_ty_get((CmTyArena *)&tyck->arena,
                        cm_ty_resolve((CmTyArena *)&tyck->arena,
                            cm_umir_c_subst(callee_type)));
                def = resolved;
                item = cm_umir_c_item_of(hir, def);
                for (index = 0u; index < bound_count && count < 32u; ++index)
                    args[count++] = bound_types[index];
                /* Matching an impl's Self only binds parameters that occur
                 * in Self.  Trait arguments can carry additional impl
                 * parameters (`SpecFromIterNested<T, I> for Vec<T>`): merge
                 * their concrete FN_DEF slots into still-bare impl args
                 * before discarding the declaration's callee type. */
                if (resolved_fn != NULL && resolved_fn->kind == CM_TY_FN_DEF
                    && resolved_fn->count > 1u) {
                    uint32_t merge;
                    for (merge = 0u; merge < count
                            && merge + 1u < resolved_fn->count; ++merge) {
                        const CmTy *have = cm_ty_get(
                            (CmTyArena *)&tyck->arena,
                            cm_ty_resolve((CmTyArena *)&tyck->arena,
                                args[merge]));
                        CmTyId candidate = cm_umir_c_subst(
                            resolved_fn->children[merge + 1u]);
                        const CmTy *given = cm_ty_get(
                            (CmTyArena *)&tyck->arena,
                            cm_ty_resolve((CmTyArena *)&tyck->arena,
                                candidate));
                        if (have != NULL && (have->kind == CM_TY_PARAM
                                || have->kind == CM_TY_INFER
                                || have->kind == CM_TY_PROJECTION
                                || have->kind == CM_TY_CONST_PARAM
                                || have->kind == CM_TY_CONST_UNKNOWN)
                            && given != NULL
                            && given->kind != CM_TY_PARAM
                            && given->kind != CM_TY_INFER
                            && given->kind != CM_TY_CONST_PARAM
                            && given->kind != CM_TY_CONST_UNKNOWN)
                            args[merge] = candidate;
                    }
                }
                callee_type = CM_TY_NONE;
            } else {
                /* Resolution may peel references while probing candidate
                 * impls.  Keep the actual receiver as declaration Self;
                 * the common borrowed-reference peel below handles `&T`,
                 * while preserving a raw `*mut T` as a legitimate Self. */
                bound_self = receiver_self;
            }
            cm_umir_c_expected_trait_arg = CM_TY_NONE;
        } else if (parent != NULL && parent->kind == CM_HIR_ITEM_IMPL
            && parent->generic_parameter_count != 0u) {
            /* Impl method reached directly with no type arguments on the
             * callee: bind the impl's generics by matching its Self
             * against the receiver (`impl<W> Tr for &mut W`). */
            CmTyArena *arena = (CmTyArena *)&tyck->arena;
            const CmTy *have = callee_type == CM_TY_NONE ? NULL
                : cm_ty_get(arena, cm_ty_resolve(arena,
                    cm_umir_c_subst(callee_type)));
            CmTyId impl_actual = receiver_type;
            int has_written_self = active_tb != NULL
                && active_tb->path_self_types != NULL
                && statement != NULL && statement->expr != CM_U_EXPR_NONE
                && active_tb->path_self_types[statement->expr] != CM_TY_NONE;
            if (has_written_self)
                impl_actual = active_tb->path_self_types[statement->expr];
            if ((have == NULL || have->kind != CM_TY_FN_DEF
                    || have->count == 0u || has_written_self)
                && impl_actual != CM_TY_NONE) {
                CmTyId self = cm_umir_c_subst(impl_actual);
                CmTyId impl_self = cm_ty_from_hir(arena, hir,
                    parent->data.impl_item.self_type);
                CmHirGenericParamId bound_params[32];
                CmTyId bound_types[32];
                uint32_t bound = 0u;
                const CmTy *self_ty = cm_ty_get(arena,
                    cm_ty_resolve(arena, self));
                CmHirReceiverKind direct_receiver = item->kind
                        == CM_HIR_ITEM_FUNCTION
                    ? item->data.function_item.signature.receiver
                    : CM_HIR_RECEIVER_NONE;
                int matched;
                /* A `&self` callee reached at a later autoderef step has
                 * `&Self` as operand: Self is one layer down (a bare `T`
                 * pattern would otherwise bind the reference itself). */
                if ((direct_receiver == CM_HIR_RECEIVER_REF_SHARED
                        || direct_receiver == CM_HIR_RECEIVER_REF_MUTABLE)
                    && cm_umir_c_receiver_steps(tyck, statement) >= 2u
                    && self_ty != NULL && (self_ty->kind == CM_TY_REF
                        || self_ty->kind == CM_TY_PTR)) {
                    self = self_ty->children[0];
                    self_ty = cm_ty_get(arena, cm_ty_resolve(arena, self));
                }
                matched = cm_umir_c_ty_match(tyck, impl_self, self,
                    bound_params, bound_types, &bound, 32u, 0u);
                while (!matched && self_ty != NULL
                    && (self_ty->kind == CM_TY_REF
                        || self_ty->kind == CM_TY_PTR)) {
                    self = self_ty->children[0];
                    self_ty = cm_ty_get(arena, cm_ty_resolve(arena, self));
                    bound = 0u;
                    matched = cm_umir_c_ty_match(tyck, impl_self, self,
                        bound_params, bound_types, &bound, 32u, 0u);
                }
                if (matched) {
                    uint32_t param;
                    uint32_t limit = parent->generic_parameter_count > 32u
                        ? 32u : parent->generic_parameter_count;
                    for (param = 0u; param < limit && count < 32u; ++param) {
                        CmHirGenericParamId id =
                            parent->generic_parameter_start + param;
                        uint32_t scan;
                        args[count] = cm_ty_param(arena, id);
                        for (scan = 0u; scan < bound; ++scan)
                            if (bound_params[scan] == id)
                                args[count] = bound_types[scan];
                        count += 1u;
                    }
                    callee_type = CM_TY_NONE;
                }
            }
        }
    }
    if (getenv("CMRUSTC_UMIR_DEBUG") != NULL && item != NULL)
        fprintf(stderr, "UMIR pre-fn %.*s count=%u callee_type=%d active_body=%d\n",
            (int)cm_interner_get(&hir->strings, item->name)->len,
            (const char *)cm_interner_get(&hir->strings, item->name)->bytes,
            count, callee_type != CM_TY_NONE, cm_umir_c_active_body != NULL);
    fn_ty = callee_type == CM_TY_NONE ? NULL
        : cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena,
                cm_umir_c_subst(callee_type)));
    if (fn_ty != NULL && fn_ty->kind == CM_TY_FN_DEF) {
        /* A trait method's FN_DEF carries Self in slot 0, which is not a
         * generic argument. */
        const CmHirItem *fn_item = cm_umir_c_item_of(hir, fn_ty->def);
        const CmHirItem *fn_parent = fn_item == NULL
                || cm_hir_def_id_is_none(fn_item->parent_definition) ? NULL
            : cm_umir_c_item_of(hir, fn_item->parent_definition);
        uint32_t first = fn_parent != NULL
            && fn_parent->kind == CM_HIR_ITEM_TRAIT ? 1u : 0u;
        for (index = first; index < fn_ty->count && count < 32u; ++index)
            args[count++] = cm_umir_c_subst(fn_ty->children[index]);
    }
    {
        /* Receiver-derived Self: strip the borrowed-reference layers a
         * method receiver carries.  A raw pointer is itself a legitimate
         * Self type (`Write::write` for `*mut T`) and must be preserved. */
        const CmTy *self_ty = bound_self == CM_TY_NONE
                || cm_umir_c_receiver_steps(tyck, statement) == 1u ? NULL
            : cm_ty_get((CmTyArena *)&tyck->arena,
                cm_ty_resolve((CmTyArena *)&tyck->arena, bound_self));
        while (self_ty != NULL && self_ty->kind == CM_TY_REF) {
            bound_self = self_ty->children[0];
            self_ty = cm_ty_get((CmTyArena *)&tyck->arena,
                cm_ty_resolve((CmTyArena *)&tyck->arena, bound_self));
        }
    }
    if (item != NULL && item->kind == CM_HIR_ITEM_FUNCTION
        && statement != NULL && cm_umir_c_active_body != NULL
        && statement->operand_overflow == 0u) {
        /* Generic parameters still unbound after the callee type and impl
         * matching (`[T]::get_unchecked<I>`: I from the argument) bind by
         * matching the signature's parameter and return types against
         * the call's operand and destination types. */
        CmHirGenericParamId parameters[32];
        uint32_t parameter_count = cm_umir_c_collect_parameters(hir, item,
            parameters, 32u);
        uint32_t unknown = 0u;
        {
            /* Arguments the callee type left unresolved (an inferred
             * const length, a leftover variable) count as unbound. */
            uint32_t check;
            for (check = 0u; check < count; ++check) {
                const CmTy *have = args[check] == CM_TY_NONE ? NULL
                    : cm_ty_get((CmTyArena *)&tyck->arena,
                        cm_ty_resolve((CmTyArena *)&tyck->arena,
                            args[check]));
                if (have == NULL || have->kind == CM_TY_CONST_UNKNOWN
                    || have->kind == CM_TY_INFER
                    || have->kind == CM_TY_CONST_PARAM
                    || have->kind == CM_TY_PARAM) {
                    args[check] = CM_TY_NONE;
                    unknown += 1u;
                }
            }
        }
        if (count < parameter_count || unknown != 0u) {
            const CmHirFunctionSignature *sig =
                &item->data.function_item.signature;
            CmTyArena *arena = (CmTyArena *)&tyck->arena;
            CmHirGenericParamId bound_params[32];
            CmTyId bound_types[32];
            uint32_t bound = 0u;
            uint32_t operands = statement->operand_count - first_arg;
            uint32_t skip = sig->parameter_count == operands ? 0u
                : sig->parameter_count + 1u == operands ? 1u : 0xFFFFu;
            uint32_t param;
            if (skip != 0xFFFFu)
                for (param = 0u; param < sig->parameter_count; ++param) {
                    CmTyId pattern = cm_ty_from_hir(arena, hir,
                        sig->parameters[param].type);
                    CmTyId actual = cm_umir_c_local_type(cm_umir_c_active_body,
                        statement->operands[first_arg + skip + param]);
                    const CmTy *pattern_ty = pattern == CM_TY_NONE ? NULL
                        : cm_ty_get(arena, cm_ty_resolve(arena, pattern));
                    if (pattern_ty != NULL && pattern_ty->kind == CM_TY_SELF
                        && parent != NULL
                        && parent->kind == CM_HIR_ITEM_IMPL)
                        pattern = cm_ty_from_hir(arena, hir,
                            parent->data.impl_item.self_type);
                    if (pattern == CM_TY_NONE || actual == CM_TY_NONE)
                        continue;
                    if (param == 0u && skip == 0u
                        && (sig->receiver == CM_HIR_RECEIVER_REF_SHARED
                            || sig->receiver == CM_HIR_RECEIVER_REF_MUTABLE)
                        && cm_umir_c_receiver_steps(tyck, statement) >= 2u) {
                        /* The receiver parameter's written type is Self
                         * (its kind carries the `&`); an operand reached
                         * at a later autoderef step is `&Self`. */
                        const CmTy *at = cm_ty_get(arena,
                            cm_ty_resolve(arena, actual));
                        if (at != NULL && (at->kind == CM_TY_REF
                                || at->kind == CM_TY_PTR))
                            actual = at->children[0];
                    }
                    (void)cm_umir_c_ty_match(tyck, pattern, actual,
                        bound_params, bound_types, &bound, 32u, 0u);
                }
            if (getenv("CMRUSTC_UMIR_DEBUG") != NULL) {
                CmStrBuf text;
                cm_str_buf_init(&text);
                if (sig->parameter_count != 0u && skip != 0xFFFFu) {
                    cm_ty_print(arena, hir, cm_ty_from_hir(arena, hir,
                        sig->parameters[0].type), &text);
                    cm_str_buf_append(&text, " vs ");
                    cm_ty_print(arena, hir, cm_umir_c_local_type(
                        cm_umir_c_active_body,
                        statement->operands[first_arg + skip]), &text);
                }
                fprintf(stderr, "UMIR bind %.*s sigparams=%u operands=%u"
                    " skip=%u bound=%u count=%u params=%u recv=%d %.*s\n",
                    (int)cm_interner_get(&hir->strings, item->name)->len,
                    (const char *)cm_interner_get(&hir->strings,
                        item->name)->bytes, sig->parameter_count, operands,
                    skip, bound, count, parameter_count, (int)sig->receiver,
                    (int)text.len, text.data);
                cm_str_buf_destroy(&text);
            }
            {
                CmTyId destination_type = statement->type;
                if (destination_type == CM_TY_NONE
                    && cm_umir_c_active_body != NULL)
                    destination_type = cm_umir_c_local_type(
                        cm_umir_c_active_body, statement->destination);
            if (destination_type != CM_TY_NONE) {
                CmTyId pattern = cm_ty_from_hir(arena, hir, sig->return_type);
                const CmTy *pattern_ty = pattern == CM_TY_NONE ? NULL
                    : cm_ty_get(arena, cm_ty_resolve(arena, pattern));
                /* Static associated constructors commonly return `Self` and
                 * have no receiver from which to infer their impl generics
                 * (`EscapeIterInner<N, E>::backslash`).  Match the impl's
                 * written Self type against the concrete destination. */
                if (pattern_ty != NULL && pattern_ty->kind == CM_TY_SELF
                    && parent != NULL && parent->kind == CM_HIR_ITEM_IMPL)
                    pattern = cm_ty_from_hir(arena, hir,
                        parent->data.impl_item.self_type);
                if (pattern != CM_TY_NONE)
                    (void)cm_umir_c_ty_match(tyck, pattern,
                        cm_umir_c_subst(destination_type), bound_params,
                        bound_types, &bound, 32u, 0u);
            }
            }
            {
                for (param = 0u; param < parameter_count && param < 32u;
                        ++param) {
                    uint32_t scan;
                    if (param < count && args[param] != CM_TY_NONE) continue;
                    args[param] = cm_ty_param(arena, parameters[param]);
                    for (scan = 0u; scan < bound; ++scan)
                        if (bound_params[scan] == parameters[param])
                            args[param] = bound_types[scan];
                }
                if (bound != 0u || unknown != 0u)
                    count = parameter_count > 32u ? 32u : parameter_count;
            }
        }
    }
    if (getenv("CMRUSTC_UMIR_DEBUG") != NULL && item != NULL) {
        CmStrBuf text;
        uint32_t show;
        cm_str_buf_init(&text);
        for (show = 0u; show < count; ++show) {
            if (show != 0u) cm_str_buf_append(&text, ", ");
            cm_ty_print((CmTyArena *)&tyck->arena, hir, args[show], &text);
        }
        fprintf(stderr, "UMIR register %.*s count=%u [%.*s] bound_self=%d\n",
            (int)cm_interner_get(&hir->strings, item->name)->len,
            (const char *)cm_interner_get(&hir->strings, item->name)->bytes,
            count, (int)text.len, text.data, bound_self != CM_TY_NONE);
        cm_str_buf_destroy(&text);
    }
    if (program != NULL)
        instance = cm_umir_c_instance(program, def, CM_U_EXPR_NONE, args,
            count, bound_self);
    cm_umir_c_render_symbol(output, def);
    if (instance >= 0) {
        const CmUMirInstance *have = (const CmUMirInstance *)
            cm_vec_at_const(&program->instances, (size_t)instance);
        if (have != NULL && (have->count != 0u
                || have->self_type != CM_TY_NONE)) {
            cm_str_buf_append(output, "_i");
            cm_umir_c_render_number(output, have->index);
        }
    }
}

/* `((T *)(intptr_t)PAIR[0])[i]` — element `i` of the slice/str behind
 * `base` (a reference to a `[data, len]` pair); scalar elements are
 * addressed at their own width, others as slots. */
static void cm_umir_c_render_slice_element(CmStrBuf *output,
    const CmTyckSet *tyck, const CmUMirBody *body, CmUMirLocalId base,
    CmUMirLocalId index)
{
    CmTyId base_type = cm_umir_c_local_type(body, base);
    CmTyId pointee = cm_umir_c_peel(tyck, base_type);
    const CmTy *pt = cm_ty_get((CmTyArena *)&tyck->arena,
        cm_ty_resolve((CmTyArena *)&tyck->arena, pointee));
    const char *scalar = pt != NULL && pt->kind == CM_TY_STR ? "uint8_t"
        : pt != NULL && pt->count != 0u
            ? cm_umir_c_scalar_type(tyck, pt->children[0]) : NULL;
    cm_str_buf_append(output, "((");
    cm_str_buf_append(output, scalar == NULL ? "long long" : scalar);
    cm_str_buf_append(output, " *)(intptr_t)");
    cm_umir_c_render_base(output, base, cm_umir_c_ref_depth(tyck, base_type));
    cm_str_buf_append(output, "[0])[");
    cm_umir_c_render_local(output, index);
    cm_str_buf_push(output, ']');
}

/* `"..."` -> `_agg<d>[1] = "...", _agg<d>[2] = len, _agg<d>[0] = &_agg<d>[1]`
 * and the destination is `&_agg<d>[0]`: a `&str` reference to a pair.
 * Escapes shared with C pass through; `\u{..}` and raw strings are
 * re-encoded.  Returns 0 for spellings this renderer cannot carry. */
static int cm_umir_c_render_string_literal(CmStrBuf *output,
    const CmUBodySet *ubodies, const CmUExpr *expr, CmUMirLocalId dest)
{
    const CmInternedString *text = cm_interner_get(&ubodies->strings,
        expr->data.literal.text);
    const unsigned char *bytes;
    size_t len;
    size_t start;
    size_t end;
    size_t index;
    unsigned long count = 0ul;
    int raw = 0;
    unsigned int hashes = 0u;
    int byte_string = expr->data.literal.kind == CM_U_LITERAL_BYTE_STRING;
    if (text == NULL) return 0;
    bytes = (const unsigned char *)text->bytes;
    len = text->len;
    start = 0u;
    if (start < len && bytes[start] == 'b') { start += 1u; byte_string = 1; }
    if (start < len && bytes[start] == 'r') { raw = 1; start += 1u; }
    while (start < len && bytes[start] == '#') { hashes += 1u; start += 1u; }
    if (start >= len || bytes[start] != '"') return 0;
    start += 1u;
    if (len < start + 1u + hashes) return 0;
    end = len - 1u - hashes;
    if (end < start || bytes[end] != '"') return 0;
    /* Byte length and C-compatibility scan. */
    cm_str_buf_append(output, "0; ");
    if (byte_string) {
        /* memmove the bytes behind the header. */
        cm_str_buf_append(output, "memmove((char *)&_agg");
        cm_umir_c_render_number(output, (unsigned long)dest);
        cm_str_buf_append(output, "[2], \"");
    } else {
        cm_str_buf_append(output, "_agg");
        cm_umir_c_render_number(output, (unsigned long)dest);
        cm_str_buf_append(output, "[1] = (long long)(intptr_t)\"");
    }
    for (index = start; index < end; ++index) {
        unsigned char c = bytes[index];
        if (!raw && c == '\\' && index + 1u < end) {
            unsigned char n = bytes[index + 1u];
            if (n == 'u') {
                /* \u{XXXX}: decode to UTF-8 octal escapes. */
                unsigned long cp = 0ul;
                size_t scan = index + 2u;
                unsigned char buf[4];
                unsigned int nbytes;
                unsigned int b;
                if (scan >= end || bytes[scan] != '{') return 0;
                scan += 1u;
                while (scan < end && bytes[scan] != '}') {
                    unsigned char h = bytes[scan];
                    if (h == '_') { scan += 1u; continue; }
                    cp = cp * 16ul + (unsigned long)(h >= 'a' ? h - 'a' + 10
                        : h >= 'A' ? h - 'A' + 10 : h - '0');
                    scan += 1u;
                }
                if (cp < 0x80ul) { buf[0] = (unsigned char)cp; nbytes = 1u; }
                else if (cp < 0x800ul) {
                    buf[0] = (unsigned char)(0xC0u | (cp >> 6));
                    buf[1] = (unsigned char)(0x80u | (cp & 0x3Fu));
                    nbytes = 2u;
                } else if (cp < 0x10000ul) {
                    buf[0] = (unsigned char)(0xE0u | (cp >> 12));
                    buf[1] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
                    buf[2] = (unsigned char)(0x80u | (cp & 0x3Fu));
                    nbytes = 3u;
                } else {
                    buf[0] = (unsigned char)(0xF0u | (cp >> 18));
                    buf[1] = (unsigned char)(0x80u | ((cp >> 12) & 0x3Fu));
                    buf[2] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
                    buf[3] = (unsigned char)(0x80u | (cp & 0x3Fu));
                    nbytes = 4u;
                }
                for (b = 0u; b < nbytes; ++b) {
                    char oct[5];
                    oct[0] = '\\';
                    oct[1] = (char)('0' + (buf[b] >> 6));
                    oct[2] = (char)('0' + ((buf[b] >> 3) & 7u));
                    oct[3] = (char)('0' + (buf[b] & 7u));
                    oct[4] = 0;
                    cm_str_buf_append(output, oct);
                }
                count += nbytes;
                index = scan;
                continue;
            }
            if (n == '\n') {
                /* Line continuation: skip the newline and leading blanks. */
                index += 1u;
                while (index + 1u < end && (bytes[index + 1u] == ' '
                        || bytes[index + 1u] == '\t'
                        || bytes[index + 1u] == '\n'
                        || bytes[index + 1u] == '\r')) index += 1u;
                continue;
            }
            if (n == 'x') {
                if (index + 3u >= end + 0u && index + 3u > end) return 0;
                cm_str_buf_push(output, '\\');
                cm_str_buf_push(output, 'x');
                cm_str_buf_push(output, (char)bytes[index + 2u]);
                cm_str_buf_push(output, (char)bytes[index + 3u]);
                /* Terminate the hex escape so a following hex digit does
                 * not extend it in C. */
                cm_str_buf_append(output, "\" \"");
                count += 1ul;
                index += 3u;
                continue;
            }
            /* \n \t \r \0 \\ \" \' are C escapes with the same value. */
            cm_str_buf_push(output, '\\');
            cm_str_buf_push(output, (char)n);
            count += 1ul;
            index += 1u;
            continue;
        }
        if (c == '"' || c == '\\') { cm_str_buf_push(output, '\\');
            cm_str_buf_push(output, (char)c); }
        else if (c == '\n') cm_str_buf_append(output, "\\n");
        else if (c == '\r') cm_str_buf_append(output, "\\r");
        else if (c == '?') cm_str_buf_append(output, "\\?");
        else if (c < 0x20u || c >= 0x7Fu) {
            char oct[5];
            oct[0] = '\\';
            oct[1] = (char)('0' + (c >> 6));
            oct[2] = (char)('0' + ((c >> 3) & 7u));
            oct[3] = (char)('0' + (c & 7u));
            oct[4] = 0;
            cm_str_buf_append(output, oct);
        } else cm_str_buf_push(output, (char)c);
        count += 1ul;
    }
    if (byte_string) {
        /* `&[u8; N]`: slot 1 = header (N), bytes at slot 2..; the array
         * block is &slot[2] and slot 0 references it. */
        cm_str_buf_append(output, "\", ");
        cm_umir_c_render_number(output, count);
        cm_str_buf_append(output, "); _agg");
        cm_umir_c_render_number(output, (unsigned long)dest);
        cm_str_buf_append(output, "[1] = ");
        cm_umir_c_render_number(output, count);
        cm_str_buf_append(output, "; _agg");
        cm_umir_c_render_number(output, (unsigned long)dest);
        cm_str_buf_append(output, "[0] = (long long)(intptr_t)&_agg");
        cm_umir_c_render_number(output, (unsigned long)dest);
        cm_str_buf_append(output, "[2]; ");
        cm_umir_c_render_local(output, dest);
        cm_str_buf_append(output, " = (long long)(intptr_t)&_agg");
        cm_umir_c_render_number(output, (unsigned long)dest);
        cm_str_buf_append(output, "[0]");
        return 1;
    }
    cm_str_buf_append(output, "\"; _agg");
    cm_umir_c_render_number(output, (unsigned long)dest);
    cm_str_buf_append(output, "[2] = ");
    cm_umir_c_render_number(output, count);
    cm_str_buf_append(output, "; _agg");
    cm_umir_c_render_number(output, (unsigned long)dest);
    cm_str_buf_append(output, "[0] = (long long)(intptr_t)&_agg");
    cm_umir_c_render_number(output, (unsigned long)dest);
    cm_str_buf_append(output, "[1]; ");
    cm_umir_c_render_local(output, dest);
    cm_str_buf_append(output, " = (long long)(intptr_t)&_agg");
    cm_umir_c_render_number(output, (unsigned long)dest);
    cm_str_buf_append(output, "[0]");
    return 1;
}

/* `a == b` / `a + b` on non-scalar operands: call the operator method
 * tyck resolved (`PartialEq::eq(&a, &b)`, `Add::add(a, b)`); `&self`
 * operators take both operands by reference.  Returns 0 to fall back to
 * the C operator (scalars, pointers to scalars, unresolved). */
static int cm_umir_c_render_operator_call(CmStrBuf *output,
    const CmHirContext *hir, const CmTyckSet *tyck, const CmUMirBody *body,
    const CmUMirStatement *statement)
{
    const CmTyckBody *tb = cm_tyck_get(tyck, body->source);
    CmHirDefId def = tb == NULL || tb->method_targets == NULL
        ? cm_hir_def_id_none() : tb->method_targets[statement->expr];
    const CmHirItem *item = cm_hir_def_id_is_none(def) ? NULL
        : cm_umir_c_item_of(hir, def);
    CmTyId left = cm_umir_c_local_type(body, statement->operands[0]);
    const CmTy *lt = left == CM_TY_NONE ? NULL
        : cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena, left));
    CmStrBuf symbol;
    int by_reference;
    uint32_t arg;
    if (item == NULL || item->kind != CM_HIR_ITEM_FUNCTION || lt == NULL)
        return 0;
    if (lt->kind == CM_TY_INT || lt->kind == CM_TY_BOOL
        || lt->kind == CM_TY_CHAR || lt->kind == CM_TY_FLOAT
        || lt->kind == CM_TY_PTR || lt->kind == CM_TY_FN_PTR
        || lt->kind == CM_TY_FN_DEF || lt->kind == CM_TY_PARAM)
        return 0;
    if (lt->kind == CM_TY_REF) {
        const CmTy *pt = cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena, lt->children[0]));
        if (pt == NULL || pt->kind == CM_TY_INT || pt->kind == CM_TY_BOOL
            || pt->kind == CM_TY_CHAR || pt->kind == CM_TY_FLOAT
            || pt->kind == CM_TY_PTR || pt->kind == CM_TY_PARAM
            || pt->kind == CM_TY_STR || pt->kind == CM_TY_SLICE)
            return 0;
    }
    by_reference = item->data.function_item.signature.receiver
            == CM_HIR_RECEIVER_REF_SHARED
        || item->data.function_item.signature.receiver
            == CM_HIR_RECEIVER_REF_MUTABLE;
    cm_str_buf_init(&symbol);
    cm_umir_c_render_callee_symbol(&symbol, hir, tyck, def, CM_TY_NONE,
        left, statement, 0u);
    cm_str_buf_append(output, "0; { long long ");
    cm_str_buf_append_n(output, symbol.data, symbol.len);
    cm_str_buf_append(output, "(); ");
    cm_umir_c_render_local(output, statement->destination);
    cm_str_buf_append(output, " = ");
    cm_str_buf_append_n(output, symbol.data, symbol.len);
    cm_str_buf_push(output, '(');
    for (arg = 0u; arg < 2u; ++arg) {
        if (arg != 0u) cm_str_buf_append(output, ", ");
        if (by_reference) cm_str_buf_append(output, "(long long)(intptr_t)&");
        cm_umir_c_render_local(output, statement->operands[arg]);
    }
    cm_str_buf_append(output, "); }");
    cm_str_buf_destroy(&symbol);
    return 1;
}

static void cm_umir_c_render_str_slot(CmStrBuf *output,
    CmUMirLocalId local, unsigned int slot)
{
    cm_str_buf_append(output, "((long long *)(intptr_t)");
    cm_umir_c_render_local(output, local);
    cm_str_buf_append(output, ")[");
    cm_umir_c_render_number(output, slot);
    cm_str_buf_push(output, ']');
}

/* `&str` is a fat reference represented by a descriptor whose slots 1 and
 * 2 are the byte pointer and length.  Scalar C equality would compare the
 * descriptor addresses, so equal text materialized by two expressions
 * would compare unequal.  Keep this primitive case local instead of
 * retaining the generic `PartialEq<&B> for &A` forwarding graph. */
static int cm_umir_c_render_str_comparison(CmStrBuf *output,
    const CmTyckSet *tyck, const CmUMirBody *body,
    const CmUMirStatement *statement, CmUBinaryOp op)
{
    CmTyId left = cm_umir_c_local_type(body, statement->operands[0]);
    CmTyId right = cm_umir_c_local_type(body, statement->operands[1]);
    const CmTy *lt = left == CM_TY_NONE ? NULL
        : cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena, left));
    const CmTy *rt = right == CM_TY_NONE ? NULL
        : cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena, right));
    const CmTy *lp;
    const CmTy *rp;
    if ((op != CM_U_BINARY_EQ && op != CM_U_BINARY_NE)
        || lt == NULL || lt->kind != CM_TY_REF
        || rt == NULL || rt->kind != CM_TY_REF)
        return 0;
    lp = cm_ty_get((CmTyArena *)&tyck->arena,
        cm_ty_resolve((CmTyArena *)&tyck->arena, lt->children[0]));
    rp = cm_ty_get((CmTyArena *)&tyck->arena,
        cm_ty_resolve((CmTyArena *)&tyck->arena, rt->children[0]));
    if (lp == NULL || lp->kind != CM_TY_STR
        || rp == NULL || rp->kind != CM_TY_STR)
        return 0;
    cm_str_buf_append(output, "(long long)(");
    if (op == CM_U_BINARY_NE) cm_str_buf_push(output, '!');
    cm_str_buf_push(output, '(');
    cm_umir_c_render_str_slot(output, statement->operands[0], 2u);
    cm_str_buf_append(output, " == ");
    cm_umir_c_render_str_slot(output, statement->operands[1], 2u);
    cm_str_buf_append(output, " && memcmp((const void *)(intptr_t)");
    cm_umir_c_render_str_slot(output, statement->operands[0], 1u);
    cm_str_buf_append(output, ", (const void *)(intptr_t)");
    cm_umir_c_render_str_slot(output, statement->operands[1], 1u);
    cm_str_buf_append(output, ", (unsigned long)");
    cm_umir_c_render_str_slot(output, statement->operands[0], 2u);
    cm_str_buf_append(output, ") == 0))");
    return 1;
}

/* A fn item used as a value: the address of its instance symbol (a
 * trait method path takes Self from the FN_DEF's first argument). */
static void cm_umir_c_render_fn_value(CmStrBuf *output,
    const CmHirContext *hir, const CmTyckSet *tyck, const CmUMirBody *body,
    const CmUMirStatement *statement, CmHirDefId def)
{
    const CmTyckBody *tb = cm_tyck_get(tyck, body->source);
    const CmHirItem *item = cm_umir_c_item_of(hir, def);
    const CmHirItem *parent = item == NULL
            || cm_hir_def_id_is_none(item->parent_definition) ? NULL
        : cm_umir_c_item_of(hir, item->parent_definition);
    CmTyId fn_type = tb == NULL || tb->expr_types == NULL ? CM_TY_NONE
        : tb->expr_types[statement->expr];
    const CmTy *ft = fn_type == CM_TY_NONE ? NULL
        : cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena,
                cm_umir_c_subst(fn_type)));
    int trait_method = parent != NULL && parent->kind == CM_HIR_ITEM_TRAIT;
    CmTyId receiver = trait_method && ft != NULL && ft->kind == CM_TY_FN_DEF
        && ft->count != 0u ? cm_umir_c_subst(ft->children[0]) : CM_TY_NONE;
    CmStrBuf symbol;
    if (getenv("CMRUSTC_UMIR_DEBUG") != NULL && trait_method) {
        CmStrBuf text;
        uint32_t index;
        cm_str_buf_init(&text);
        cm_ty_print((CmTyArena *)&tyck->arena, hir, fn_type, &text);
        cm_str_buf_append(&text, " => ");
        cm_ty_print((CmTyArena *)&tyck->arena, hir, receiver, &text);
        fprintf(stderr, "UMIR fn-value %.*s active=%s count=%u [",
            (int)text.len, text.data,
            cm_umir_c_active_instance == NULL ? "none" : "yes",
            cm_umir_c_active_instance == NULL ? 0u
                : cm_umir_c_active_instance->count);
        for (index = 0u; cm_umir_c_active_instance != NULL
                && index < cm_umir_c_active_instance->count; ++index) {
            CmStrBuf one;
            cm_str_buf_init(&one);
            cm_ty_print((CmTyArena *)&tyck->arena, hir,
                cm_umir_c_active_instance->types[index], &one);
            fprintf(stderr, " p%u=%.*s",
                (unsigned)cm_umir_c_active_instance->parameters[index],
                (int)one.len, one.data);
            cm_str_buf_destroy(&one);
        }
        fprintf(stderr, " ]\n");
        cm_str_buf_destroy(&text);
    }
    cm_str_buf_init(&symbol);
    /* A path names Self exactly: `<&str as Display>::fmt` is the
     * forwarding `impl Display for &T`, not `str`'s own impl. */
    cm_umir_c_exact_self = trait_method;
    cm_umir_c_render_callee_symbol(&symbol, hir, tyck, def,
        fn_type, receiver, NULL, 0u);
    cm_umir_c_exact_self = 0;
    cm_str_buf_append(output, "0; { long long ");
    cm_str_buf_append_n(output, symbol.data, symbol.len);
    cm_str_buf_append(output, "(); ");
    cm_umir_c_render_local(output, statement->destination);
    cm_str_buf_append(output, " = (long long)(intptr_t)&");
    cm_str_buf_append_n(output, symbol.data, symbol.len);
    cm_str_buf_append(output, "; }");
    cm_str_buf_destroy(&symbol);
}

/* `Iterator::next`: the fn `next` declared by the trait named
 * `Iterator` (core's, or a no_core program's own). */
static CmHirDefId cm_umir_c_iterator_next(const CmHirContext *hir)
{
    size_t index;
    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *item = (const CmHirItem *)cm_vec_at_const(
            &hir->items, index);
        const CmInternedString *name;
        const CmHirItem *parent;
        if (item == NULL || item->kind != CM_HIR_ITEM_FUNCTION
            || cm_hir_def_id_is_none(item->parent_definition)) continue;
        name = cm_interner_get(&hir->strings, item->name);
        if (name == NULL || name->len != 4u
            || memcmp(name->bytes, "next", 4u) != 0) continue;
        parent = cm_umir_c_item_of(hir, item->parent_definition);
        if (parent == NULL || parent->kind != CM_HIR_ITEM_TRAIT) continue;
        name = cm_interner_get(&hir->strings, parent->name);
        if (name != NULL && name->len == 8u
            && memcmp(name->bytes, "Iterator", 8u) == 0)
            return item->definition;
    }
    return cm_hir_def_id_none();
}

/* `IntoIterator::into_iter`: the trait declaration used to turn the
 * source of a `for` expression into the stateful iterator stored by MIR. */
static CmHirDefId cm_umir_c_into_iterator(const CmHirContext *hir)
{
    size_t index;
    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *item = (const CmHirItem *)cm_vec_at_const(
            &hir->items, index);
        const CmInternedString *name;
        const CmHirItem *parent;
        if (item == NULL || item->kind != CM_HIR_ITEM_FUNCTION
            || cm_hir_def_id_is_none(item->parent_definition)) continue;
        name = cm_interner_get(&hir->strings, item->name);
        if (name == NULL || name->len != 9u
            || memcmp(name->bytes, "into_iter", 9u) != 0) continue;
        parent = cm_umir_c_item_of(hir, item->parent_definition);
        if (parent == NULL || parent->kind != CM_HIR_ITEM_TRAIT) continue;
        name = cm_interner_get(&hir->strings, parent->name);
        if (name != NULL && name->len == 12u
            && memcmp(name->bytes, "IntoIterator", 12u) == 0)
            return item->definition;
    }
    return cm_hir_def_id_none();
}

/* `Deref::deref` (`DerefMut::deref_mut` when `mutable`): the lang trait's
 * declaration, resolved per receiver type at the call. */
static CmHirDefId cm_umir_c_deref_fn(const CmHirContext *hir, int mutable)
{
    const char *trait_name = mutable ? "DerefMut" : "Deref";
    const char *fn_name = mutable ? "deref_mut" : "deref";
    size_t trait_len = strlen(trait_name);
    size_t fn_len = strlen(fn_name);
    size_t index;
    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *item = (const CmHirItem *)cm_vec_at_const(
            &hir->items, index);
        const CmInternedString *name;
        const CmHirItem *parent;
        if (item == NULL || item->kind != CM_HIR_ITEM_FUNCTION
            || cm_hir_def_id_is_none(item->parent_definition)) continue;
        name = cm_interner_get(&hir->strings, item->name);
        if (name == NULL || name->len != fn_len
            || memcmp(name->bytes, fn_name, fn_len) != 0) continue;
        parent = cm_umir_c_item_of(hir, item->parent_definition);
        if (parent == NULL || parent->kind != CM_HIR_ITEM_TRAIT) continue;
        name = cm_interner_get(&hir->strings, parent->name);
        if (name != NULL && name->len == trait_len
            && memcmp(name->bytes, trait_name, trait_len) == 0)
            return item->definition;
    }
    return cm_hir_def_id_none();
}

/* `Drop::drop`'s declaration, by name (like `cm_umir_c_deref_fn`). */
static CmHirDefId cm_umir_c_drop_fn(const CmHirContext *hir)
{
    size_t index;
    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *item = (const CmHirItem *)cm_vec_at_const(
            &hir->items, index);
        const CmInternedString *name;
        const CmHirItem *parent;
        if (item == NULL || item->kind != CM_HIR_ITEM_FUNCTION
            || cm_hir_def_id_is_none(item->parent_definition)) continue;
        name = cm_interner_get(&hir->strings, item->name);
        if (name == NULL || name->len != 4u
            || memcmp(name->bytes, "drop", 4u) != 0) continue;
        parent = cm_umir_c_item_of(hir, item->parent_definition);
        if (parent == NULL || parent->kind != CM_HIR_ITEM_TRAIT) continue;
        name = cm_interner_get(&hir->strings, parent->name);
        if (name != NULL && name->len == 4u
            && memcmp(name->bytes, "Drop", 4u) == 0)
            return item->definition;
    }
    return cm_hir_def_id_none();
}

/* The `impl Drop for T` method for `type`, or none. */
static CmHirDefId cm_umir_c_drop_impl(const CmHirContext *hir,
    const CmTyckSet *tyck, CmHirDefId drop_decl, CmTyId type)
{
    const CmHirItem *decl = cm_hir_def_id_is_none(drop_decl) ? NULL
        : cm_umir_c_item_of(hir, drop_decl);
    if (decl == NULL) return cm_hir_def_id_none();
    return cm_umir_c_resolve_impl_method(hir, tyck, decl, type, NULL, NULL,
        NULL, 0u);
}

static int cm_umir_c_is_core_refmut(const CmHirContext *hir,
    const CmHirItem *item)
{
    const CmHirModule *module;
    const CmHirCrate *crate;
    const CmInternedString *item_name;
    const CmInternedString *module_name;
    const CmInternedString *crate_name;
    if (item == NULL || item->kind != CM_HIR_ITEM_STRUCT) return 0;
    item_name = cm_interner_get(&hir->strings, item->name);
    module = cm_hir_get_module(hir, item->owner_module);
    module_name = module == NULL ? NULL
        : cm_interner_get(&hir->strings, module->name);
    crate = module == NULL ? NULL : cm_hir_get_crate(hir, module->crate_id);
    crate_name = crate == NULL ? NULL
        : cm_interner_get(&hir->strings, crate->name);
    return item_name != NULL && item_name->len == 6u
        && memcmp(item_name->bytes, "RefMut", 6u) == 0
        && module_name != NULL && module_name->len == 4u
        && memcmp(module_name->bytes, "cell", 4u) == 0
        && crate_name != NULL && crate_name->len == 4u
        && memcmp(crate_name->bytes, "core", 4u) == 0;
}

/* Drop glue reaches: a `Drop` impl on the type, or one inside a struct's
 * fields (a RefMut's BorrowRefMut).  Enums and pointers are not walked. */
static int cm_umir_c_type_needs_drop(const CmHirContext *hir,
    const CmTyckSet *tyck, CmHirDefId drop_decl, CmTyId type,
    unsigned int depth)
{
    const CmTy *ty = type == CM_TY_NONE ? NULL
        : cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena, type));
    const CmHirDefinition *record;
    const CmHirItem *item;
    uint32_t index;
    if (ty == NULL || ty->kind != CM_TY_ADT || depth > 6u) return 0;
    if (!cm_hir_def_id_is_none(cm_umir_c_drop_impl(hir, tyck, drop_decl,
            type))) return 1;
    record = cm_hir_lookup_definition(hir, ty->def);
    if (record == NULL || record->kind != CM_HIR_DEFINITION_ITEM) return 0;
    item = cm_hir_get_item(hir, record->entity.item_id);
    if (item == NULL || item->kind != CM_HIR_ITEM_STRUCT) return 0;
    for (index = 0u; index < item->data.aggregate_item.field_count; ++index) {
        CmTyId field_type = cm_umir_c_transparent_inner(hir, tyck, type,
            (long)index);
        if (cm_umir_c_type_needs_drop(hir, tyck, drop_decl, field_type,
                depth + 1u)) return 1;
        item = cm_hir_get_item(hir, record->entity.item_id);
    }
    return 0;
}

/* Emit drop glue for the value whose slot address is `address` (a C
 * expression yielding `long long`): the type's own `Drop::drop`, then
 * each struct field's glue (field i is the block's slot i, a transparent
 * wrapper's field the value itself). */
static void cm_umir_c_render_drop(CmStrBuf *output, const CmHirContext *hir,
    const CmTyckSet *tyck, CmHirDefId drop_decl, CmTyId type,
    const char *address, size_t address_len, unsigned int depth)
{
    const CmTy *ty = type == CM_TY_NONE ? NULL
        : cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena, type));
    const CmHirDefinition *record;
    const CmHirItem *item;
    uint32_t index;
    long representative;
    if (ty == NULL || ty->kind != CM_TY_ADT || depth > 6u) return;
    if (!cm_hir_def_id_is_none(cm_umir_c_drop_impl(hir, tyck, drop_decl,
            type))) {
        CmStrBuf symbol;
        cm_str_buf_init(&symbol);
        cm_umir_c_render_callee_symbol(&symbol, hir, tyck, drop_decl,
            CM_TY_NONE, type, NULL, 0u);
        cm_str_buf_append(output, "{ long long ");
        cm_str_buf_append_n(output, symbol.data, symbol.len);
        cm_str_buf_append(output, "(); ");
        cm_str_buf_append_n(output, symbol.data, symbol.len);
        cm_str_buf_push(output, '(');
        cm_str_buf_append_n(output, address, address_len);
        cm_str_buf_append(output, "); } ");
        cm_str_buf_destroy(&symbol);
    }
    ty = cm_ty_get((CmTyArena *)&tyck->arena,
        cm_ty_resolve((CmTyArena *)&tyck->arena, type));
    record = cm_hir_lookup_definition(hir, ty->def);
    if (record == NULL || record->kind != CM_HIR_DEFINITION_ITEM) return;
    item = cm_hir_get_item(hir, record->entity.item_id);
    if (item == NULL || item->kind != CM_HIR_ITEM_STRUCT) return;
    representative = cm_umir_c_transparent_field(hir, tyck, type);
    for (index = 0u; index < item->data.aggregate_item.field_count; ++index) {
        CmTyId field_type = cm_umir_c_transparent_inner(hir, tyck, type,
            (long)index);
        CmStrBuf field_address;
        if (!cm_umir_c_type_needs_drop(hir, tyck, drop_decl, field_type,
                depth + 1u)) {
            item = cm_hir_get_item(hir, record->entity.item_id);
            continue;
        }
        cm_str_buf_init(&field_address);
        if (representative >= 0) {
            if ((long)index == representative)
                cm_str_buf_append_n(&field_address, address, address_len);
        } else {
            cm_str_buf_append(&field_address,
                "(long long)(intptr_t)&((long long *)(intptr_t)"
                "*(long long *)(intptr_t)");
            cm_str_buf_append_n(&field_address, address, address_len);
            cm_str_buf_append(&field_address, ")[");
            cm_umir_c_render_number(&field_address, (unsigned long)index);
            cm_str_buf_push(&field_address, ']');
        }
        if (field_address.len != 0u)
            cm_umir_c_render_drop(output, hir, tyck, drop_decl, field_type,
                field_address.data, field_address.len, depth + 1u);
        cm_str_buf_destroy(&field_address);
        item = cm_hir_get_item(hir, record->entity.item_id);
    }
}

/* A borrow rooted at a const path is a promoted value even when it occurs
 * inside an ordinary function (`fn f() -> &'static T { &LOCAL_CONST.x }`). */
static int cm_umir_c_ref_is_const_promotion(const CmHirContext *hir,
    const CmUBody *ub, const CmUMirStatement *statement)
{
    const CmUExpr *expr;
    CmUExprId place;
    unsigned int depth;
    if (hir == NULL || ub == NULL || statement == NULL
        || statement->kind != CM_UMIR_RVALUE_REF
        || statement->expr == CM_U_EXPR_NONE) return 0;
    expr = cm_ubody_get_expr(ub, statement->expr);
    if (expr == NULL || expr->kind != CM_U_EXPR_REF) return 0;
    place = expr->data.ref.operand;
    for (depth = 0u; depth < 8u && place != CM_U_EXPR_NONE; ++depth) {
        const CmUExpr *part = cm_ubody_get_expr(ub, place);
        if (part == NULL) return 0;
        if (part->kind == CM_U_EXPR_FIELD) {
            place = part->data.field.base;
            continue;
        }
        if (part->kind == CM_U_EXPR_TUPLE_FIELD) {
            place = part->data.tuple_field.base;
            continue;
        }
        if (part->kind == CM_U_EXPR_INDEX) {
            place = part->data.index.base;
            continue;
        }
        if (part->kind == CM_U_EXPR_PATH
            && part->data.path.resolution.kind
                == CM_U_RESOLVED_DEFINITION) {
            const CmHirDefinition *record = cm_hir_lookup_definition(hir,
                part->data.path.resolution.definition);
            const CmHirItem *item = record == NULL
                    || record->kind != CM_HIR_DEFINITION_ITEM ? NULL
                : cm_hir_get_item(hir, record->entity.item_id);
            return item != NULL && item->kind == CM_HIR_ITEM_CONST;
        }
        return 0;
    }
    return 0;
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
    /* Definition: one frame array of uniform slots; parameters arrive
     * as p<i>.  Closure bodies take the enclosing frame as `env`. */
    cm_umir_c_active_body = body;
    cm_umir_c_active_ub = ub;
    cm_umir_c_hir = hir;
    cm_str_buf_append(output, "long long ");
    if (body->closure_expr != CM_U_EXPR_NONE) {
        const CmUExpr *closure = cm_ubody_get_expr(ub, body->closure_expr);
        uint32_t closure_params = closure == NULL ? 0u
            : closure->data.closure.parameter_count;
        cm_umir_c_render_closure_symbol(output, body->source,
            body->closure_expr, cm_umir_c_active_instance == NULL ? -1
                : (long)cm_umir_c_active_instance->index);
        cm_str_buf_append(output, "(long long *env");
        for (param = 0u; param < closure_params; ++param) {
            cm_str_buf_append(output, ", long long p");
            cm_umir_c_render_number(output, (unsigned long)param);
        }
        cm_str_buf_append(output, ")\n{\n    long long _l[");
        cm_umir_c_render_number(output, (unsigned long)
            (body->locals.len - body->env_count + 1u));
        cm_str_buf_append(output, "] = {0};\n");
        for (param = 0u; param < closure_params; ++param) {
            const CmUPat *pat = closure == NULL ? NULL
                : cm_ubody_get_pat(ub,
                    closure->data.closure.parameters[param].pattern);
            CmUMirLocalId receiver = (CmUMirLocalId)0u;
            if (pat != NULL && pat->kind == CM_U_PAT_BINDING
                && pat->data.binding.local != CM_U_LOCAL_NONE)
                receiver = (CmUMirLocalId)(1u + pat->data.binding.local);
            else if (param < body->closure_param_count)
                receiver = body->closure_param_locals[param];
            if (receiver == (CmUMirLocalId)0u) continue;
            cm_str_buf_append(output, "    ");
            cm_umir_c_render_local(output, receiver);
            cm_str_buf_append(output, " = p");
            cm_umir_c_render_number(output, (unsigned long)param);
            cm_str_buf_append(output, ";\n");
        }
    } else {
        int is_static = owner != NULL && owner->kind == CM_HIR_ITEM_STATIC;
        if (is_static) {
            /* A `static` is one place: its initializer runs once and the
             * value (an aggregate's block pointer, or the scalar) is
             * cached, so every use shares it — `static COUNTER:
             * AtomicU32` keeps its count.  The body itself renders as
             * `<symbol>_init`; this wrapper fronts it. */
            /* (the `long long ` before this branch prefixes the
             * prototype) */
            cm_umir_c_render_symbol(output, def);
            cm_str_buf_append(output, "_init(void);\nlong long ");
            cm_umir_c_render_symbol(output, def);
            cm_str_buf_append(output, "_addr(void);\nlong long ");
            cm_umir_c_render_symbol(output, def);
            cm_str_buf_append(output, "(void) { return *(long long *)"
                "(intptr_t)");
            cm_umir_c_render_symbol(output, def);
            cm_str_buf_append(output, "_addr(); }\nlong long ");
            cm_umir_c_render_symbol(output, def);
            cm_str_buf_append(output, "_addr(void) { static long long"
                " cm_slot; static int cm_ready; if (!cm_ready) {"
                " cm_ready = 1; cm_slot = ");
            cm_umir_c_render_symbol(output, def);
            cm_str_buf_append(output, "_init(); } return (long long)"
                "(intptr_t)&cm_slot; }\nlong long ");
        }
        cm_umir_c_render_symbol(output, def);
        if (is_static) cm_str_buf_append(output, "_init");
        if (cm_umir_c_active_instance != NULL
            && (cm_umir_c_active_instance->count != 0u
                || cm_umir_c_active_instance->self_type != CM_TY_NONE)) {
            cm_str_buf_append(output, "_i");
            cm_umir_c_render_number(output,
                cm_umir_c_active_instance->index);
        }
        if (owner != NULL) {
            const CmInternedString *owner_name = cm_interner_get(
                &hir->strings, owner->name);
            if (owner_name != NULL) {
                cm_str_buf_append(output, " /* ");
                cm_str_buf_append_n(output,
                    (const char *)owner_name->bytes, owner_name->len);
                cm_str_buf_append(output, " */");
            }
        }
        cm_str_buf_push(output, '(');
        if (ub->parameter_count == 0u) cm_str_buf_append(output, "void");
        for (param = 0u; param < ub->parameter_count; ++param) {
            if (param != 0u) cm_str_buf_append(output, ", ");
            cm_str_buf_append(output, "long long p");
            cm_umir_c_render_number(output, (unsigned long)param);
        }
        {
            /* A frame captured by a closure may outlive the call (returned
             * or stored iterators): such frames live on the heap. */
            int captured = 0;
            size_t scan_block;
            for (scan_block = 0u; scan_block < body->blocks.len && !captured;
                    ++scan_block) {
                const CmUMirBlock *scan = (const CmUMirBlock *)
                    cm_vec_at_const(&body->blocks, scan_block);
                size_t scan_statement;
                if (scan == NULL) continue;
                for (scan_statement = 0u;
                        scan_statement < scan->statements.len;
                        ++scan_statement) {
                    const CmUMirStatement *st = (const CmUMirStatement *)
                        cm_vec_at_const(&scan->statements, scan_statement);
                    if (st != NULL && st->kind == CM_UMIR_RVALUE_CLOSURE) {
                        captured = 1;
                        break;
                    }
                }
            }
            if (captured) {
                cm_str_buf_append(output,
                    ")\n{\n    long long *_l = (long long *)calloc(");
                cm_umir_c_render_number(output, (unsigned long)
                    (body->locals.len + 1u));
                cm_str_buf_append(output, ", 8);\n");
            } else {
                cm_str_buf_append(output, ")\n{\n    long long _l[");
                cm_umir_c_render_number(output, (unsigned long)
                    (body->locals.len + 1u));
                cm_str_buf_append(output, "] = {0};\n");
            }
        }
    }
    if (getenv("CMRUSTC_UMIR_TRACE") != NULL
        && body->closure_expr == CM_U_EXPR_NONE) {
        /* Runtime trace: symbol and parameters on entry. */
        const CmInternedString *owner_name = owner == NULL ? NULL
            : cm_interner_get(&hir->strings, owner->name);
        cm_str_buf_append(output, "    fprintf(stderr, \"enter ");
        cm_umir_c_render_symbol(output, def);
        if (owner_name != NULL) {
            cm_str_buf_push(output, ' ');
            cm_str_buf_append_n(output, (const char *)owner_name->bytes,
                owner_name->len);
        }
        for (param = 0u; param < ub->parameter_count; ++param)
            cm_str_buf_append(output, " %lld");
        cm_str_buf_append(output, "\\n\"");
        for (param = 0u; param < ub->parameter_count; ++param) {
            cm_str_buf_append(output, ", p");
            cm_umir_c_render_number(output, (unsigned long)param);
        }
        cm_str_buf_append(output, ");\n");
    }
    (void)local_index;
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
            if (statement == NULL) continue;
            if (statement->kind == CM_UMIR_RVALUE_REF
                && ((owner != NULL && owner->kind == CM_HIR_ITEM_CONST)
                    || cm_umir_c_ref_is_const_promotion(hir, ub,
                        statement))) {
                /* A reference produced by a const initializer is promoted:
                 * its referent slot must outlive this initializer call. */
                cm_str_buf_append(output, "    static long long _agg");
                cm_umir_c_render_number(output,
                    (unsigned long)statement->destination);
                cm_str_buf_append(output, "[1];\n");
                continue;
            } else if (statement->kind == CM_UMIR_RVALUE_LITERAL) {
                /* A string literal: slot 0 holds the pair address so the
                 * `&str` local is a reference like any other. */
                const CmUExpr *lit = cm_ubody_get_expr(ub, statement->expr);
                if (lit == NULL || lit->kind != CM_U_EXPR_LITERAL
                    || (lit->data.literal.kind != CM_U_LITERAL_STRING
                        && lit->data.literal.kind != CM_U_LITERAL_BYTE_STRING))
                    continue;
                if (lit->data.literal.kind == CM_U_LITERAL_BYTE_STRING) {
                    /* `b"..."` is a `&[u8; N]`: slot 0 references the
                     * block, slot 1 is the block's length header, the
                     * bytes follow (the spelling bounds the length). */
                    const CmInternedString *text = cm_interner_get(
                        &ubodies->strings, lit->data.literal.text);
                    slots = 2ul + ((text == NULL ? 0ul : text->len) + 7ul) / 8ul
                        + 1ul;
                } else
                    slots = 3ul;
                /* Literal data is static; so is its pair, which callers
                 * keep after this frame returns (`-> &'static str`). */
                cm_str_buf_append(output, "    static");
            } else if (statement->kind == CM_UMIR_RVALUE_UNSIZE
                    || statement->kind == CM_UMIR_RVALUE_SUBSLICE) {
                slots = 3ul;
            } else if (statement->kind != CM_UMIR_RVALUE_AGGREGATE
                    && statement->kind != CM_UMIR_RVALUE_VARIANT) {
                continue;
            } else
                slots = (unsigned long)statement->operand_count
                    + (unsigned long)statement->operand_overflow
                    + (statement->kind == CM_UMIR_RVALUE_VARIANT ? 1u : 0u);
            if (statement->kind != CM_UMIR_RVALUE_LITERAL) {
                /* Aggregates outlive their constructing frame (returned
                 * by value, stored through references): heap blocks,
                 * allocated at the construction statement. */
                cm_str_buf_append(output, "    long long *_agg");
                cm_umir_c_render_number(output,
                    (unsigned long)statement->destination);
                cm_str_buf_append(output, ";\n");
                continue;
            }
            cm_str_buf_append(output, " long long _agg");
            cm_umir_c_render_number(output,
                (unsigned long)statement->destination);
            cm_str_buf_append(output, "[");
            cm_umir_c_render_number(output, slots == 0u ? 1u : slots);
            cm_str_buf_append(output, "];\n");
        }
    }
    /* Prologue: bind parameter patterns to their local slots. */
    for (param = 0u; body->closure_expr == CM_U_EXPR_NONE
            && param < ub->parameter_count; ++param) {
        const CmUPat *pat = cm_ubody_get_pat(ub, ub->parameters[param]);
        CmUMirLocalId receiver = (CmUMirLocalId)0u;
        if (pat != NULL && pat->kind == CM_U_PAT_BINDING
            && pat->data.binding.local != CM_U_LOCAL_NONE)
            receiver = (CmUMirLocalId)(1u + pat->data.binding.local);
        else if (param < body->closure_param_count)
            /* A destructuring parameter's receiver local. */
            receiver = body->closure_param_locals[param];
        if (receiver == (CmUMirLocalId)0u) continue;
        cm_str_buf_append(output, "    ");
        cm_umir_c_render_local(output, receiver);
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
                        || expr->data.literal.kind == CM_U_LITERAL_BOOL
                        || expr->data.literal.kind == CM_U_LITERAL_CHAR
                        || expr->data.literal.kind == CM_U_LITERAL_BYTE))
                    cm_umir_c_render_number(output, (unsigned long)
                        expr->data.literal.value_low);
                else if (expr != NULL && expr->kind == CM_U_EXPR_LITERAL
                    && (expr->data.literal.kind == CM_U_LITERAL_STRING
                        || expr->data.literal.kind == CM_U_LITERAL_BYTE_STRING)
                    && cm_umir_c_render_string_literal(output, ubodies,
                        expr, statement->destination)) {
                    /* rendered as a [data, len] pair (or a byte block) */
                } else if (expr == NULL || expr->kind != CM_U_EXPR_LITERAL) {
                    /* Synthetic constant (pattern-test results). */
                    cm_umir_c_render_number(output,
                        (unsigned long)statement->immediate);
                } else {
                    cm_str_buf_append(output, "0 /* literal */");
                    complete = 0;
                }
                break;
            case CM_UMIR_RVALUE_LOCAL:
                if (statement->operand_count == 1u)
                    cm_umir_c_render_local(output,
                        statement->operands[0]);
                else {
                    /* An item used as a value: a const/static evaluates
                     * its initializer body (tyck recorded the item), a
                     * unit struct is an empty block; fn items stay
                     * symbolic (callee operands never read them). */
                    if (cm_umir_c_render_const_parameter(output, tyck,
                            expr))
                        break;
                    const CmTyckBody *ptb = cm_tyck_get(tyck, body->source);
                    CmHirDefId value_def = ptb == NULL
                            || ptb->method_targets == NULL
                        ? cm_hir_def_id_none()
                        : ptb->method_targets[statement->expr];
                    const CmHirItem *value_item = cm_hir_def_id_is_none(
                        value_def) ? NULL : cm_umir_c_item_of(hir, value_def);
                    if (value_item != NULL
                        && value_item->kind == CM_HIR_ITEM_FUNCTION) {
                        cm_umir_c_render_fn_value(output, hir, tyck, body,
                            statement, value_def);
                        break;
                    }
                    if (value_item != NULL
                        && (value_item->kind == CM_HIR_ITEM_CONST
                            || value_item->kind == CM_HIR_ITEM_STATIC)) {
                        CmStrBuf symbol;
                        /* A trait's associated const resolves through
                         * the path's Self (`Self::BASE` in an instance,
                         * `T::BASE`, `<Ty>::BASE`) to the impl's item. */
                        CmTyId const_self = CM_TY_NONE;
                        if (expr != NULL && expr->kind == CM_U_EXPR_PATH) {
                            const CmUResolution *pr =
                                &expr->data.path.resolution;
                            if (pr->kind == CM_U_RESOLVED_SELF_TYPE)
                                const_self = cm_umir_c_subst(cm_ty_with_def(
                                    (CmTyArena *)&tyck->arena, CM_TY_SELF,
                                    value_item->parent_definition, NULL, 0u));
                            else if (pr->kind == CM_U_RESOLVED_GENERIC_PARAM)
                                const_self = cm_umir_c_subst(cm_ty_param(
                                    (CmTyArena *)&tyck->arena,
                                    pr->generic_parameter));
                            else if (pr->kind == CM_U_RESOLVED_TYPE_ASSOC)
                                const_self = cm_umir_c_subst(cm_ty_with_def(
                                    (CmTyArena *)&tyck->arena, CM_TY_ADT,
                                    pr->definition, NULL, 0u));
                        } else if (expr != NULL
                            && expr->kind == CM_U_EXPR_QUALIFIED_PATH
                            && ptb->path_self_types != NULL
                            && ptb->path_self_types[statement->expr]
                                != CM_TY_NONE) {
                            /* `<T>::C` / `<T as Tr>::C`: tyck recorded the
                             * written Self (`<usize>::MAX` in `isize::MAX`). */
                            const_self = cm_umir_c_subst(
                                ptb->path_self_types[statement->expr]);
                        }
                        cm_str_buf_init(&symbol);
                        cm_umir_c_render_callee_symbol(&symbol, hir, tyck,
                            value_def, CM_TY_NONE, const_self, NULL, 0u);
                        cm_str_buf_append(output, "0; { long long ");
                        cm_str_buf_append_n(output, symbol.data, symbol.len);
                        cm_str_buf_append(output, "(); ");
                        cm_umir_c_render_local(output,
                            statement->destination);
                        cm_str_buf_append(output, " = ");
                        cm_str_buf_append_n(output, symbol.data, symbol.len);
                        cm_str_buf_append(output, "(); }");
                        cm_str_buf_destroy(&symbol);
                        break;
                    }
                    if (value_item == NULL && expr != NULL
                        && expr->kind == CM_U_EXPR_PATH
                        && expr->data.path.resolution.kind
                            == CM_U_RESOLVED_DEFINITION) {
                        const CmHirItem *path_item = cm_umir_c_item_of(hir,
                            expr->data.path.resolution.definition);
                        if (path_item != NULL
                            && path_item->kind == CM_HIR_ITEM_STRUCT) {
                            cm_str_buf_append(output,
                                "(long long)(intptr_t)malloc(8)");
                            break;
                        }
                        if (path_item != NULL
                            && path_item->kind == CM_HIR_ITEM_FUNCTION) {
                            cm_umir_c_render_fn_value(output, hir, tyck,
                                body, statement,
                                expr->data.path.resolution.definition);
                            break;
                        }
                    }
                    {
                        /* Any other item path tyck typed as a fn item
                         * (`Display::fmt`, `<T as Tr>::f`): a fn value. */
                        const CmTyckBody *ftb = cm_tyck_get(tyck,
                            body->source);
                        const CmTy *fty = ftb == NULL || ftb->expr_types == NULL
                            ? NULL
                            : cm_ty_get((CmTyArena *)&tyck->arena,
                                cm_ty_resolve((CmTyArena *)&tyck->arena,
                                    cm_umir_c_subst(
                                        ftb->expr_types[statement->expr])));
                        if (fty != NULL && fty->kind == CM_TY_FN_DEF
                            && cm_umir_c_item_of(hir, fty->def) != NULL) {
                            cm_umir_c_render_fn_value(output, hir, tyck, body,
                                statement, fty->def);
                            break;
                        }
                    }
                    cm_str_buf_append(output, "0 /* item path */");
                    complete = 0;
                }
                break;
            case CM_UMIR_RVALUE_BINARY:
                if (statement->operand_count == 2u && expr != NULL
                    && expr->kind == CM_U_EXPR_BINARY
                    && cm_umir_c_render_str_comparison(output, tyck, body,
                        statement, expr->data.binary.op)) {
                    /* Fat string references compare their byte contents. */
                } else if (statement->operand_count == 2u && expr != NULL
                    && expr->kind == CM_U_EXPR_BINARY
                    && cm_umir_c_render_operator_call(output, hir, tyck,
                        body, statement)) {
                    /* Non-scalar operands: the operator trait method. */
                } else if (statement->operand_count == 2u && expr != NULL
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
                    if (expr->data.unary.op == CM_U_UNARY_DEREF) {
                        /* Load through the reference at the pointee's
                         * scalar width (packed arrays store elements at
                         * their own width). */
                        const char *scalar = cm_umir_c_scalar_type(tyck,
                            cm_umir_c_subst(statement->type));
                        if (scalar != NULL) {
                            cm_str_buf_append(output, "(long long)*(");
                            cm_str_buf_append(output, scalar);
                            cm_str_buf_append(output, " *)(intptr_t)");
                        } else {
                            cm_str_buf_append(output,
                                "*(long long *)(intptr_t)");
                        }
                        cm_umir_c_render_local(output,
                            statement->operands[0]);
                    } else {
                        cm_str_buf_append(output, "(long long)(");
                        if (expr->data.unary.op == CM_U_UNARY_NEG)
                            cm_str_buf_push(output, '-');
                        else
                            cm_str_buf_push(output,
                                strcmp(operand_abi, "uint8_t") == 0
                                    ? '!' : '~');
                        cm_str_buf_push(output, '(');
                        cm_str_buf_append(output, operand_abi);
                        cm_str_buf_push(output, ')');
                        cm_umir_c_render_local(output,
                            statement->operands[0]);
                        cm_str_buf_push(output, ')');
                    }
                } else {
                    cm_str_buf_append(output, "0 /* unary */");
                    complete = 0;
                }
                break;
            case CM_UMIR_RVALUE_AGGREGATE: {
                uint32_t field;
                uint32_t slot_count = statement->operand_count;
                int mapped = statement->operand_overflow == 0u;
                CmTyId agg_type = cm_umir_c_subst(statement->type);
                const CmTy *at = agg_type == CM_TY_NONE ? NULL
                    : cm_ty_get((CmTyArena *)&tyck->arena,
                        cm_ty_resolve((CmTyArena *)&tyck->arena, agg_type));
                if (statement->operand_overflow == 0u
                    && cm_umir_c_transparent_field(hir, tyck, agg_type) >= 0) {
                    /* The representative field's operand is the value; a
                     * literal that only names zero-sized fields is 0. */
                    long representative = cm_umir_c_transparent_field(hir,
                        tyck, agg_type);
                    uint32_t member;
                    int rendered = 0;
                    for (member = 0u; member < statement->operand_count; ++member) {
                        long slot = (long)member;
                        if (expr != NULL && expr->kind == CM_U_EXPR_STRUCT
                            && member < expr->data.struct_expr.field_count)
                            slot = cm_umir_c_field_index(hir, tyck, ubodies,
                                statement->type,
                                expr->data.struct_expr.fields[member].name);
                        if (slot == representative) {
                            cm_umir_c_render_local(output,
                                statement->operands[member]);
                            rendered = 1;
                            break;
                        }
                    }
                    if (!rendered) cm_str_buf_append(output, "0");
                    break;
                }
                if (at != NULL && at->kind == CM_TY_ARRAY) {
                    /* Arrays pack scalar elements at their width so slices
                     * made from them index consistently; other elements
                     * are slots.  `[v; N]` fills N copies. */
                    const char *elem = cm_umir_c_scalar_type(tyck,
                        at->children[0]);
                    unsigned long esize = elem == NULL ? 8ul
                        : cm_umir_c_scalar_size(tyck, at->children[0]);
                    int repeat = expr != NULL
                        && expr->kind == CM_U_EXPR_ARRAY_REPEAT;
                    unsigned long n = repeat
                        ? cm_umir_c_array_len(tyck, agg_type)
                        : (unsigned long)slot_count
                            + (unsigned long)statement->operand_overflow;
                    /* Repeat count: the type's constant, else the length
                     * operand at run time. */
                    CmStrBuf count_text;
                    cm_str_buf_init(&count_text);
                    if (repeat && n == 0ul && statement->operand_count == 2u)
                        cm_umir_c_render_local(&count_text,
                            statement->operands[1]);
                    else
                        cm_umir_c_render_number(&count_text, n);
                    /* An array block carries its element count in a
                     * hidden header slot (`block[-1]`) so an unsize to a
                     * slice knows the length even when the type's length
                     * is a const the typechecker could not evaluate. */
                    cm_str_buf_append(output, "0; _agg");
                    cm_umir_c_render_number(output,
                        (unsigned long)statement->destination);
                    cm_str_buf_append(output,
                        " = (long long *)((char *)malloc(8 + (");
                    cm_str_buf_append_n(output, count_text.data,
                        count_text.len);
                    cm_str_buf_append(output, " + 1) * ");
                    cm_umir_c_render_number(output, esize);
                    cm_str_buf_append(output, ") + 8); ((long long *)_agg");
                    cm_umir_c_render_number(output,
                        (unsigned long)statement->destination);
                    cm_str_buf_append(output, ")[-1] = ");
                    cm_str_buf_append_n(output, count_text.data,
                        count_text.len);
                    cm_str_buf_append(output, "; ");
                    if (repeat && statement->operand_count >= 1u) {
                        cm_str_buf_append(output,
                            "{ unsigned long _k; for (_k = 0; _k < ");
                        cm_str_buf_append_n(output, count_text.data,
                            count_text.len);
                        cm_str_buf_append(output, "; ++_k) ((");
                        cm_str_buf_append(output,
                            elem == NULL ? "long long" : elem);
                        cm_str_buf_append(output, " *)_agg");
                        cm_umir_c_render_number(output,
                            (unsigned long)statement->destination);
                        cm_str_buf_append(output, ")[_k] = (");
                        cm_str_buf_append(output,
                            elem == NULL ? "long long" : elem);
                        cm_str_buf_push(output, ')');
                        cm_umir_c_render_local(output,
                            statement->operands[0]);
                        cm_str_buf_append(output, "; } ");
                    } else if (mapped) {
                        for (field = 0u; field < slot_count; ++field) {
                            cm_str_buf_append(output, "((");
                            cm_str_buf_append(output,
                                elem == NULL ? "long long" : elem);
                            cm_str_buf_append(output, " *)_agg");
                            cm_umir_c_render_number(output,
                                (unsigned long)statement->destination);
                            cm_str_buf_append(output, ")[");
                            cm_umir_c_render_number(output,
                                (unsigned long)field);
                            cm_str_buf_append(output, "] = (");
                            cm_str_buf_append(output,
                                elem == NULL ? "long long" : elem);
                            cm_str_buf_push(output, ')');
                            cm_umir_c_render_local(output,
                                statement->operands[field]);
                            cm_str_buf_append(output, "; ");
                        }
                    } else if (expr != NULL && expr->kind == CM_U_EXPR_ARRAY
                        && expr->data.list.element_count == n) {
                        uint32_t literal_index;
                        int literals = 1;
                        for (literal_index = 0u;
                                literal_index < expr->data.list.element_count;
                                ++literal_index) {
                            const CmUExpr *element = cm_ubody_get_expr(ub,
                                expr->data.list.elements[literal_index]);
                            if (element == NULL
                                || element->kind != CM_U_EXPR_LITERAL
                                || (element->data.literal.kind
                                        != CM_U_LITERAL_INTEGER
                                    && element->data.literal.kind
                                        != CM_U_LITERAL_BOOL
                                    && element->data.literal.kind
                                        != CM_U_LITERAL_CHAR
                                    && element->data.literal.kind
                                        != CM_U_LITERAL_BYTE)) {
                                literals = 0;
                                break;
                            }
                        }
                        for (literal_index = 0u; literals
                                && literal_index
                                    < expr->data.list.element_count;
                                ++literal_index) {
                            const CmUExpr *element = cm_ubody_get_expr(ub,
                                expr->data.list.elements[literal_index]);
                            cm_str_buf_append(output, "((");
                            cm_str_buf_append(output,
                                elem == NULL ? "long long" : elem);
                            cm_str_buf_append(output, " *)_agg");
                            cm_umir_c_render_number(output,
                                (unsigned long)statement->destination);
                            cm_str_buf_append(output, ")[");
                            cm_umir_c_render_number(output,
                                (unsigned long)literal_index);
                            cm_str_buf_append(output, "] = (");
                            cm_str_buf_append(output,
                                elem == NULL ? "long long" : elem);
                            cm_str_buf_push(output, ')');
                            cm_umir_c_render_number(output, (unsigned long)
                                element->data.literal.value_low);
                            cm_str_buf_append(output, "; ");
                        }
                        if (!literals) {
                            cm_str_buf_append(output, "/* array */");
                            complete = 0;
                        }
                    } else {
                        cm_str_buf_append(output, "/* array */");
                        complete = 0;
                    }
                    cm_umir_c_render_local(output, statement->destination);
                    cm_str_buf_append(output, " = (long long)(intptr_t)_agg");
                    cm_umir_c_render_number(output,
                        (unsigned long)statement->destination);
                    cm_str_buf_destroy(&count_text);
                    break;
                }
                /* Zeroed: fields a literal omits (RangeInclusive's
                 * `exhausted`) read 0. */
                cm_str_buf_append(output, "0; _agg");
                cm_umir_c_render_number(output,
                    (unsigned long)statement->destination);
                cm_str_buf_append(output, " = (long long *)calloc(");
                cm_umir_c_render_number(output, (unsigned long)
                    (slot_count + statement->operand_overflow + 2u));
                cm_str_buf_append(output, ", 8); ");
                for (field = 0u; field < slot_count && mapped; ++field) {
                    long slot = (long)field;
                    if (expr != NULL && expr->kind == CM_U_EXPR_STRUCT
                        && field < expr->data.struct_expr.field_count) {
                        slot = cm_umir_c_field_index(hir, tyck, ubodies,
                            statement->type,
                            expr->data.struct_expr.fields[field].name);
                        if (slot < 0) { mapped = 0; break; }
                        if (getenv("CMRUSTC_UMIR_DEBUG") != NULL) {
                            uint32_t earlier;
                            for (earlier = 0u; earlier < field; ++earlier)
                                if (cm_umir_c_field_index(hir, tyck, ubodies,
                                        statement->type,
                                        expr->data.struct_expr.fields[earlier]
                                            .name) == slot) {
                                    const CmInternedString *fname =
                                        cm_interner_get(&ubodies->strings,
                                            expr->data.struct_expr
                                                .fields[field].name);
                                    CmStrBuf text;
                                    cm_str_buf_init(&text);
                                    cm_ty_print((CmTyArena *)&tyck->arena,
                                        hir, statement->type, &text);
                                    fprintf(stderr, "UMIR dup-field %.*s "
                                        "slot=%ld in %.*s\n",
                                        fname == NULL ? 1 : (int)fname->len,
                                        fname == NULL ? "?"
                                            : (const char *)fname->bytes,
                                        slot, (int)text.len, text.data);
                                    cm_str_buf_destroy(&text);
                                }
                        }
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
                cm_str_buf_append(output, " = (long long *)calloc(");
                cm_umir_c_render_number(output, (unsigned long)
                    (statement->operand_count + statement->operand_overflow
                        + 2u));
                cm_str_buf_append(output, ", 8); _agg");
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
            case CM_UMIR_RVALUE_SLOT: {
                CmTyId base_type = cm_umir_c_local_type(body,
                    statement->operands[0]);
                unsigned int depth = cm_umir_c_ref_depth(tyck, base_type);
                {
                    long representative = cm_umir_c_transparent_field(hir,
                        tyck, cm_umir_c_peel(tyck, base_type));
                    if (representative >= 0) {
                        if ((unsigned long)representative
                                == (unsigned long)statement->immediate)
                            cm_umir_c_render_loaded(output,
                                statement->operands[0], depth);
                        else
                            cm_str_buf_append(output, "0");
                        break;
                    }
                }
                cm_umir_c_render_base(output, statement->operands[0], depth);
                cm_str_buf_push(output, '[');
                cm_umir_c_render_number(output,
                    (unsigned long)statement->immediate);
                cm_str_buf_push(output, ']');
                break;
            }
            case CM_UMIR_RVALUE_REF_SLOT: {
                /* The payload slot's address (a by-reference binding
                 * behind a reference scrutinee). */
                CmTyId base_type = cm_umir_c_local_type(body,
                    statement->operands[0]);
                unsigned int depth = cm_umir_c_ref_depth(tyck, base_type);
                long representative = cm_umir_c_transparent_field(hir,
                    tyck, cm_umir_c_peel(tyck, base_type));
                if (representative >= 0) {
                    /* A transparent wrapper is its field: the reference
                     * itself is the field's address. */
                    if (depth >= 1u) {
                        cm_umir_c_render_loaded(output,
                            statement->operands[0], depth - 1u);
                    } else {
                        cm_str_buf_append(output, "(long long)(intptr_t)&");
                        cm_umir_c_render_local(output,
                            statement->operands[0]);
                    }
                    break;
                }
                cm_str_buf_append(output, "(long long)(intptr_t)&");
                cm_umir_c_render_base(output, statement->operands[0], depth);
                cm_str_buf_push(output, '[');
                cm_umir_c_render_number(output,
                    (unsigned long)statement->immediate);
                cm_str_buf_push(output, ']');
                break;
            }
            case CM_UMIR_RVALUE_REF:
                /* A reference is the address of the referent's slot. */
                if ((owner != NULL && owner->kind == CM_HIR_ITEM_CONST)
                    || cm_umir_c_ref_is_const_promotion(hir, ub,
                        statement)) {
                    cm_str_buf_append(output, "0; _agg");
                    cm_umir_c_render_number(output,
                        (unsigned long)statement->destination);
                    cm_str_buf_append(output, "[0] = ");
                    cm_umir_c_render_local(output, statement->operands[0]);
                    cm_str_buf_append(output, "; ");
                    cm_umir_c_render_local(output, statement->destination);
                    cm_str_buf_append(output, " = (long long)(intptr_t)&_agg");
                    cm_umir_c_render_number(output,
                        (unsigned long)statement->destination);
                    cm_str_buf_append(output, "[0]");
                } else {
                    cm_str_buf_append(output, "(long long)(intptr_t)&");
                    cm_umir_c_render_local(output, statement->operands[0]);
                }
                break;
            case CM_UMIR_RVALUE_REBORROW: {
                /* References normally address a value slot.  A raw pointer
                 * to an enum instead addresses its tag/payload block
                 * directly, so give `&*raw` a durable slot containing that
                 * block pointer.  Other reborrows retain their established
                 * identity representation. */
                CmTyId source = cm_umir_c_local_type(body,
                    statement->operands[0]);
                const CmTy *source_ty = source == CM_TY_NONE ? NULL
                    : cm_ty_get((CmTyArena *)&tyck->arena,
                        cm_ty_resolve((CmTyArena *)&tyck->arena, source));
                int source_is_raw = source_ty != NULL
                    && source_ty->kind == CM_TY_PTR;
                CmTyId target = cm_umir_c_subst(statement->type);
                const CmTy *target_ty = target == CM_TY_NONE ? NULL
                    : cm_ty_get((CmTyArena *)&tyck->arena,
                        cm_ty_resolve((CmTyArena *)&tyck->arena, target));
                CmTyId pointee = target_ty != NULL
                        && (target_ty->kind == CM_TY_REF
                            || target_ty->kind == CM_TY_PTR)
                        && target_ty->count != 0u
                    ? cm_umir_c_representation(hir, tyck,
                        target_ty->children[0]) : CM_TY_NONE;
                const CmTy *pointee_ty = pointee == CM_TY_NONE ? NULL
                    : cm_ty_get((CmTyArena *)&tyck->arena,
                        cm_ty_resolve((CmTyArena *)&tyck->arena, pointee));
                int direct_ref_cast = 0;
                if (expr != NULL && expr->kind == CM_U_EXPR_REF
                    && cm_umir_c_active_ub != NULL) {
                    const CmUExpr *deref = cm_ubody_get_expr(
                        cm_umir_c_active_ub, expr->data.ref.operand);
                    const CmUExpr *raw = deref != NULL
                            && deref->kind == CM_U_EXPR_UNARY
                        ? cm_ubody_get_expr(cm_umir_c_active_ub,
                            deref->data.unary.operand) : NULL;
                    const CmUExpr *value = raw != NULL
                            && raw->kind == CM_U_EXPR_CAST
                        ? cm_ubody_get_expr(cm_umir_c_active_ub,
                            raw->data.cast.value) : NULL;
                    direct_ref_cast = value != NULL
                        && value->kind == CM_U_EXPR_REF;
                }
                int enum_raw = 0;
                if (pointee_ty != NULL && pointee_ty->kind == CM_TY_ADT) {
                    const CmHirDefinition *record = cm_hir_lookup_definition(
                        hir, pointee_ty->def);
                    const CmHirItem *item = record == NULL
                            || record->kind != CM_HIR_DEFINITION_ITEM ? NULL
                        : cm_hir_get_item(hir, record->entity.item_id);
                    enum_raw = item != NULL && item->kind == CM_HIR_ITEM_ENUM
                        && pointee_ty->def.crate_id == cm_umir_c_root_crate;
                }
                int aggregate_raw = source_is_raw && !direct_ref_cast
                    && (enum_raw || (pointee_ty != NULL
                        && (pointee_ty->kind == CM_TY_TUPLE
                            || pointee_ty->kind == CM_TY_ARRAY)));
                if (aggregate_raw) {
                    cm_str_buf_append(output,
                        "0; { long long *_rr = (long long *)malloc(8); "
                        "*_rr = ");
                    cm_umir_c_render_local(output, statement->operands[0]);
                    cm_str_buf_append(output, "; ");
                    cm_umir_c_render_local(output, statement->destination);
                    cm_str_buf_append(output,
                        " = (long long)(intptr_t)_rr; }");
                } else if (!source_is_raw
                    && cm_umir_c_ref_depth(tyck, source)
                        > cm_umir_c_ref_depth(tyck, target)) {
                    /* `&**rr`: peel the reference layers consumed by the
                     * explicit dereferences.  Identity is correct for
                     * `&*p`, but `rr: &&str` must load once to produce the
                     * inner `&str` fat-pointer slot. */
                    cm_umir_c_render_loaded(output, statement->operands[0],
                        cm_umir_c_ref_depth(tyck, source)
                            - cm_umir_c_ref_depth(tyck, target));
                } else
                    cm_umir_c_render_local(output, statement->operands[0]);
                break;
            }
            case CM_UMIR_RVALUE_CAST: {
                CmTyId from = cm_umir_c_local_type(body,
                    statement->operands[0]);
                CmTyId to = cm_umir_c_subst(statement->type);
                /* Only a direct `*const str` / `&[T]` is a fat pointer:
                 * `*const &str as *const ()` (Argument::new's
                 * `NonNull::from_ref(x).cast()`) is thin on both sides. */
                if (cm_umir_c_is_fat(hir, tyck,
                        cm_umir_c_peel(tyck, from))
                    && cm_umir_c_ref_depth(tyck, from) == 1u
                    && !cm_umir_c_is_fat(hir, tyck,
                        cm_umir_c_peel(tyck, to))) {
                    /* `s as *const str as *const u8`: the data pointer. */
                    cm_umir_c_render_base(output, statement->operands[0],
                        cm_umir_c_ref_depth(tyck, from));
                    cm_str_buf_append(output, "[0]");
                    break;
                }
                {
                    /* `&arr as *const [T; N] as *const T`: the element
                     * pointer is the array block the reference points at. */
                    const CmTy *fp = cm_ty_get((CmTyArena *)&tyck->arena,
                        cm_ty_resolve((CmTyArena *)&tyck->arena,
                            cm_umir_c_peel(tyck, from)));
                    const CmTy *tp = cm_ty_get((CmTyArena *)&tyck->arena,
                        cm_ty_resolve((CmTyArena *)&tyck->arena,
                            cm_umir_c_peel(tyck, to)));
                    if (fp != NULL && fp->kind == CM_TY_ARRAY
                        && cm_umir_c_ref_depth(tyck, from) == 1u
                        && cm_umir_c_ref_depth(tyck, to) != 0u
                        && (tp == NULL || tp->kind != CM_TY_ARRAY)) {
                        cm_umir_c_render_loaded(output, statement->operands[0],
                            cm_umir_c_ref_depth(tyck, from));
                        break;
                    }
                }
                if (cm_umir_c_render_enum_cast(output, hir, tyck, from, to,
                        statement->operands[0])) break;
                cm_str_buf_append(output, "(long long)(");
                cm_str_buf_append(output, cm_umir_c_abi_type(&tyck->arena,
                    to));
                cm_str_buf_push(output, ')');
                cm_umir_c_render_local(output, statement->operands[0]);
                break;
            }
            case CM_UMIR_RVALUE_STORE_FIELD: {
                long slot = -1;
                CmTyId carrier = CM_TY_NONE;
                unsigned int user_deref_depth = 0u;
                if (expr != NULL && (expr->kind == CM_U_EXPR_TUPLE_FIELD
                        || expr->kind == CM_U_EXPR_FIELD))
                    carrier = cm_umir_c_field_carrier(hir, tyck, ubodies,
                        cm_umir_c_local_type(body, statement->operands[0]),
                        expr, &slot, &user_deref_depth);
                if (slot >= 0) {
                    CmTyId base_type = cm_umir_c_local_type(body,
                        statement->operands[0]);
                    unsigned int depth = cm_umir_c_ref_depth(tyck, base_type)
                        + user_deref_depth;
                    cm_str_buf_append(output, "0; ");
                    {
                        long representative = cm_umir_c_transparent_field(hir,
                            tyck, carrier);
                        if (representative >= 0) {
                            /* The field is the value: assign the referent
                             * (or the local itself); a zero-sized field
                             * store is a no-op. */
                            if (representative != slot) break;
                            cm_umir_c_render_loaded(output,
                                statement->operands[0], depth);
                            cm_str_buf_append(output, " = ");
                            cm_umir_c_render_local(output,
                                statement->operands[1]);
                            break;
                        }
                    }
                    cm_umir_c_render_base(output, statement->operands[0],
                        depth);
                    cm_str_buf_push(output, '[');
                    cm_umir_c_render_number(output, (unsigned long)slot);
                    cm_str_buf_append(output, "] = ");
                    cm_umir_c_render_local(output, statement->operands[1]);
                } else {
                    cm_str_buf_append(output, "0 /* store field */");
                    complete = 0;
                }
                break;
            }
            case CM_UMIR_RVALUE_STORE_INDEX: {
                CmTyId base_type = cm_umir_c_local_type(body,
                    statement->operands[0]);
                CmTyId pointee = cm_umir_c_peel(tyck, base_type);
                const char *elem = cm_umir_c_array_elem_scalar(tyck, pointee);
                cm_str_buf_append(output, "0; ");
                if (cm_umir_c_is_fat(hir, tyck, pointee)) {
                    cm_umir_c_render_slice_element(output, tyck, body,
                        statement->operands[0], statement->operands[1]);
                    cm_str_buf_append(output, " = ");
                } else if (elem != NULL) {
                    cm_str_buf_append(output, "((");
                    cm_str_buf_append(output, elem);
                    cm_str_buf_append(output, " *)(intptr_t)");
                    cm_umir_c_render_base(output, statement->operands[0],
                        cm_umir_c_ref_depth(tyck, base_type));
                    cm_str_buf_append(output, ")[");
                    cm_umir_c_render_local(output, statement->operands[1]);
                    cm_str_buf_append(output, "] = (");
                    cm_str_buf_append(output, elem);
                    cm_str_buf_push(output, ')');
                } else {
                    cm_umir_c_render_base(output, statement->operands[0],
                        cm_umir_c_ref_depth(tyck, base_type));
                    cm_str_buf_push(output, '[');
                    cm_umir_c_render_local(output, statement->operands[1]);
                    cm_str_buf_append(output, "] = ");
                }
                cm_umir_c_render_local(output, statement->operands[2]);
                break;
            }
            case CM_UMIR_RVALUE_LOAD: {
                /* Scalars load at their own width so byte pointers into
                 * string data never over-read; everything else is a slot. */
                const char *scalar = cm_umir_c_scalar_type(tyck,
                    cm_umir_c_subst(statement->type));
                if (getenv("CMRUSTC_UMIR_DEBUG") != NULL) {
                    CmStrBuf text;
                    cm_str_buf_init(&text);
                    cm_ty_print((CmTyArena *)&tyck->arena, hir,
                        cm_umir_c_subst(statement->type), &text);
                    fprintf(stderr, "UMIR load-type %.*s scalar=%s\n",
                        (int)text.len, text.data,
                        scalar == NULL ? "-" : scalar);
                    cm_str_buf_destroy(&text);
                }
                if (scalar != NULL) {
                    cm_str_buf_append(output, "(long long)*(");
                    cm_str_buf_append(output, scalar);
                    cm_str_buf_append(output, " *)(intptr_t)");
                } else
                    cm_str_buf_append(output, "*(long long *)(intptr_t)");
                cm_umir_c_render_local(output, statement->operands[0]);
                break;
            }
            case CM_UMIR_RVALUE_STORE_DEREF: {
                const char *scalar = statement->operand_count < 2u ? NULL
                    : cm_umir_c_scalar_type(tyck, cm_umir_c_local_type(body,
                        statement->operands[1]));
                if (scalar != NULL) {
                    cm_str_buf_append(output, "0; *(");
                    cm_str_buf_append(output, scalar);
                    cm_str_buf_append(output, " *)(intptr_t)");
                    cm_umir_c_render_local(output, statement->operands[0]);
                    cm_str_buf_append(output, " = (");
                    cm_str_buf_append(output, scalar);
                    cm_str_buf_push(output, ')');
                } else {
                    cm_str_buf_append(output, "0; *(long long *)(intptr_t)");
                    cm_umir_c_render_local(output, statement->operands[0]);
                    cm_str_buf_append(output, " = ");
                }
                cm_umir_c_render_local(output, statement->operands[1]);
                break;
            }
            case CM_UMIR_RVALUE_FIELD:
            case CM_UMIR_RVALUE_REF_FIELD: {
                /* REF_FIELD: the field slot's address instead of its
                 * value (a transparent wrapper's field is the value: its
                 * address is the base's). */
                long slot = -1;
                CmTyId carrier = CM_TY_NONE;
                unsigned int user_deref_depth = 0u;
                int want_address = statement->kind == CM_UMIR_RVALUE_REF_FIELD;
                if (statement->operand_count == 1u && expr != NULL
                    && (expr->kind == CM_U_EXPR_TUPLE_FIELD
                        || expr->kind == CM_U_EXPR_FIELD))
                    carrier = cm_umir_c_field_carrier(hir, tyck, ubodies,
                        cm_umir_c_local_type(body, statement->operands[0]),
                        expr, &slot, &user_deref_depth);
                if (slot >= 0) {
                    CmTyId base_type = cm_umir_c_local_type(body,
                        statement->operands[0]);
                    unsigned int depth = cm_umir_c_ref_depth(tyck, base_type)
                        + user_deref_depth;
                    {
                        long representative = cm_umir_c_transparent_field(hir,
                            tyck, carrier);
                        if (representative >= 0) {
                            /* A zero-sized field reads 0; its address
                             * is the base's (`ptr::read(&b.1)` on a
                             * Box's Global allocator dereferences it). */
                            if (representative == slot) {
                                if (want_address)
                                    cm_str_buf_append(output,
                                        "(long long)(intptr_t)&");
                                cm_umir_c_render_loaded(output,
                                    statement->operands[0], depth);
                            } else if (want_address) {
                                cm_str_buf_append(output,
                                    "(long long)(intptr_t)&");
                                cm_umir_c_render_loaded(output,
                                    statement->operands[0], depth);
                            } else {
                                cm_str_buf_append(output, "0");
                            }
                            break;
                        }
                    }
                    if (want_address)
                        cm_str_buf_append(output, "(long long)(intptr_t)&");
                    cm_umir_c_render_base(output, statement->operands[0],
                        depth);
                    cm_str_buf_push(output, '[');
                    cm_umir_c_render_number(output, (unsigned long)slot);
                    cm_str_buf_push(output, ']');
                } else {
                    cm_str_buf_append(output, "0 /* field */");
                    complete = 0;
                }
                break;
            }
            case CM_UMIR_RVALUE_INDEX:
            case CM_UMIR_RVALUE_REF_INDEX: {
                /* REF_INDEX: the element's address instead of its value.
                 * An immediate k > 0 indexes from the end (`len - k`, a
                 * slice pattern's `[.., last]`). */
                CmTyId base_type = cm_umir_c_local_type(body,
                    statement->operands[0]);
                CmTyId pointee = cm_umir_c_peel(tyck, base_type);
                const char *elem = cm_umir_c_array_elem_scalar(tyck, pointee);
                const char *prefix = statement->kind
                    == CM_UMIR_RVALUE_REF_INDEX ? "(long long)(intptr_t)&"
                    : "(long long)";
                int fat = cm_umir_c_is_fat(hir, tyck, pointee);
                CmStrBuf index;
                cm_str_buf_init(&index);
                if (statement->immediate != 0u) {
                    cm_str_buf_append(&index, "(");
                    if (fat) {
                        cm_umir_c_render_base(&index, statement->operands[0],
                            cm_umir_c_ref_depth(tyck, base_type));
                        cm_str_buf_append(&index, "[1]");
                    } else
                        cm_umir_c_render_number(&index,
                            cm_umir_c_array_len(tyck, pointee));
                    cm_str_buf_append(&index, " - ");
                    cm_umir_c_render_number(&index,
                        (unsigned long)statement->immediate);
                    cm_str_buf_append(&index, ")");
                } else
                    cm_umir_c_render_local(&index, statement->operands[1]);
                if (fat) {
                    const CmTy *pt = cm_ty_get((CmTyArena *)&tyck->arena,
                        cm_ty_resolve((CmTyArena *)&tyck->arena, pointee));
                    const char *scalar = pt != NULL && pt->kind == CM_TY_STR
                        ? "uint8_t" : pt != NULL && pt->count != 0u
                            ? cm_umir_c_scalar_type(tyck, pt->children[0])
                            : NULL;
                    cm_str_buf_append(output, prefix);
                    cm_str_buf_append(output, "((");
                    cm_str_buf_append(output, scalar == NULL ? "long long"
                        : scalar);
                    cm_str_buf_append(output, " *)(intptr_t)");
                    cm_umir_c_render_base(output, statement->operands[0],
                        cm_umir_c_ref_depth(tyck, base_type));
                    cm_str_buf_append(output, "[0])[");
                    cm_str_buf_append_n(output, index.data, index.len);
                    cm_str_buf_push(output, ']');
                    cm_str_buf_destroy(&index);
                    break;
                }
                if (elem != NULL) {
                    cm_str_buf_append(output, prefix);
                    cm_str_buf_append(output, "((");
                    cm_str_buf_append(output, elem);
                    cm_str_buf_append(output, " *)(intptr_t)");
                    cm_umir_c_render_base(output, statement->operands[0],
                        cm_umir_c_ref_depth(tyck, base_type));
                    cm_str_buf_append(output, ")[");
                    cm_str_buf_append_n(output, index.data, index.len);
                    cm_str_buf_push(output, ']');
                    cm_str_buf_destroy(&index);
                    break;
                }
                if (statement->kind == CM_UMIR_RVALUE_REF_INDEX)
                    cm_str_buf_append(output, "(long long)(intptr_t)&");
                cm_umir_c_render_base(output, statement->operands[0],
                    cm_umir_c_ref_depth(tyck, base_type));
                cm_str_buf_push(output, '[');
                cm_str_buf_append_n(output, index.data, index.len);
                cm_str_buf_push(output, ']');
                cm_str_buf_destroy(&index);
                break;
            }
            case CM_UMIR_RVALUE_CONST_PATTERN: {
                /* The constant a path pattern names: its initializer. */
                const CmUBody *pat_body = cm_ubody_get(ubodies, body->source);
                const CmUPat *pat = statement->pattern == CM_U_PAT_NONE
                    || pat_body == NULL ? NULL
                    : cm_ubody_get_pat(pat_body, statement->pattern);
                const CmHirItem *const_item = pat == NULL
                        || pat->kind != CM_U_PAT_PATH ? NULL
                    : cm_umir_c_item_of(hir,
                        pat->data.path.resolution.definition);
                if (const_item != NULL && (const_item->kind
                        == CM_HIR_ITEM_CONST
                        || const_item->kind == CM_HIR_ITEM_STATIC)) {
                    CmStrBuf symbol;
                    cm_str_buf_init(&symbol);
                    cm_umir_c_render_callee_symbol(&symbol, hir, tyck,
                        const_item->definition, CM_TY_NONE, CM_TY_NONE,
                        NULL, 0u);
                    cm_str_buf_append(output, "0; { long long ");
                    cm_str_buf_append_n(output, symbol.data, symbol.len);
                    cm_str_buf_append(output, "(); ");
                    cm_umir_c_render_local(output, statement->destination);
                    cm_str_buf_append(output, " = ");
                    cm_str_buf_append_n(output, symbol.data, symbol.len);
                    cm_str_buf_append(output, "(); }");
                    cm_str_buf_destroy(&symbol);
                } else {
                    cm_str_buf_append(output, "0 /* const pattern */");
                    complete = 0;
                }
                break;
            }
            case CM_UMIR_RVALUE_RANGE_TEST:
                /* lo <= v <= hi on the slot value (signed, 32-bit bounds). */
                cm_str_buf_append(output, "(long long)((long long)");
                cm_umir_c_render_local(output, statement->operands[0]);
                cm_str_buf_append(output, " >= (long long)");
                cm_umir_c_render_local(output, statement->operands[1]);
                cm_str_buf_append(output, " && (long long)");
                cm_umir_c_render_local(output, statement->operands[0]);
                cm_str_buf_append(output, " <= (long long)");
                cm_umir_c_render_local(output, statement->operands[2]);
                cm_str_buf_push(output, ')');
                break;
            case CM_UMIR_RVALUE_CLOSURE:
                /* The closure value is its environment: this frame. */
                cm_str_buf_append(output, "(long long)(intptr_t)");
                cm_str_buf_append(output, body->closure_expr
                    == CM_U_EXPR_NONE ? "_l" : "env");
                break;
            case CM_UMIR_RVALUE_CALL: {
                /* Callee is the first operand's defining PATH. */
                CmHirDefId callee_def = cm_hir_def_id_none();
                {
                    /* A closure-typed callee calls the closure body with
                     * its environment. */
                    const CmTyckBody *ctb0 = cm_tyck_get(tyck, body->source);
                    CmTyId callee_type0 = ctb0 == NULL
                            || ctb0->expr_types == NULL || expr == NULL
                            || expr->kind != CM_U_EXPR_CALL ? CM_TY_NONE
                        : cm_umir_c_subst(ctb0->expr_types[
                            expr->data.call.callee]);
                    const CmTy *callee_ty0 = callee_type0 == CM_TY_NONE
                        ? NULL : cm_ty_get((CmTyArena *)&tyck->arena,
                            cm_ty_resolve((CmTyArena *)&tyck->arena,
                                callee_type0));
                    unsigned int closure_ref_depth = 0u;
                    /* Fn-family forwarding impls make `&mut F` callable.
                     * Once F is a concrete closure, dispatch its body and
                     * load through the forwarding references to recover the
                     * closure environment stored in the referent slot. */
                    while (callee_ty0 != NULL
                        && (callee_ty0->kind == CM_TY_REF
                            || callee_ty0->kind == CM_TY_PTR)) {
                        callee_type0 = callee_ty0->children[0];
                        callee_ty0 = cm_ty_get((CmTyArena *)&tyck->arena,
                            cm_ty_resolve((CmTyArena *)&tyck->arena,
                                callee_type0));
                        closure_ref_depth += 1u;
                    }
                    {
                        /* `f(x)` with `f: &mut dyn FnMut(A) -> R`: the
                         * arguments travel as one tuple block through the
                         * pair's vtable at the principal's `call*` slot. */
                        CmTyId callee_local_type = statement->operand_count
                                == 0u ? CM_TY_NONE
                            : cm_umir_c_local_type(body,
                                statement->operands[0]);
                        const CmTy *dyn_ty = callee_local_type == CM_TY_NONE
                            ? NULL : cm_ty_get((CmTyArena *)&tyck->arena,
                                cm_ty_resolve((CmTyArena *)&tyck->arena,
                                    cm_umir_c_peel(tyck, callee_local_type)));
                        if (dyn_ty != NULL && dyn_ty->kind == CM_TY_DYN
                            && statement->operand_overflow == 0u) {
                            CmHirDefId method = cm_umir_c_dyn_call_method(
                                hir, dyn_ty->def);
                            long slot = cm_hir_def_id_is_none(method) ? -1
                                : cm_umir_c_trait_method_index(hir,
                                    dyn_ty->def, method);
                            unsigned int depth = cm_umir_c_ref_depth(tyck,
                                callee_local_type);
                            uint32_t arg;
                            if (slot >= 0) {
                                cm_str_buf_append(output,
                                    "0; { long long (**_vt)() = "
                                    "(long long (**)())(intptr_t)");
                                cm_umir_c_render_base(output,
                                    statement->operands[0], depth);
                                cm_str_buf_append(output,
                                    "[1]; long long *_targs = (long long *)"
                                    "calloc(");
                                cm_umir_c_render_number(output,
                                    (unsigned long)(statement->operand_count
                                        > 1u ? statement->operand_count - 1u
                                        : 1u));
                                cm_str_buf_append(output, ", 8); ");
                                for (arg = 1u; arg < statement->operand_count;
                                        ++arg) {
                                    cm_str_buf_append(output, "_targs[");
                                    cm_umir_c_render_number(output,
                                        (unsigned long)(arg - 1u));
                                    cm_str_buf_append(output, "] = ");
                                    cm_umir_c_render_local(output,
                                        statement->operands[arg]);
                                    cm_str_buf_append(output, "; ");
                                }
                                cm_umir_c_render_local(output,
                                    statement->destination);
                                cm_str_buf_append(output, " = _vt[");
                                cm_umir_c_render_number(output,
                                    (unsigned long)slot);
                                cm_str_buf_append(output, "](");
                                cm_umir_c_render_base(output,
                                    statement->operands[0], depth);
                                cm_str_buf_append(output,
                                    "[0], (long long)(intptr_t)_targs); }");
                                break;
                            }
                        }
                    }
                    if (callee_ty0 != NULL
                        && callee_ty0->kind == CM_TY_CLOSURE
                        && statement->operand_count != 0u
                        && statement->operand_overflow == 0u) {
                        uint32_t arg;
                        long closure_instance = -1;
                        if (cm_umir_c_active_program != NULL) {
                            /* The closure body is instanced on its
                             * enclosing scope (children[0] = Self or a
                             * bare SELF, then the generic arguments). */
                            const CmHirBody *closure_body =
                                cm_hir_get_body(hir,
                                    (CmHirBodyId)callee_ty0->a);
                            CmTyId closure_self = CM_TY_NONE;
                            const CmTyId *closure_args = NULL;
                            uint32_t closure_arg_count = 0u;
                            if (callee_ty0->count != 0u) {
                                const CmTy *self_child = cm_ty_get(
                                    (CmTyArena *)&tyck->arena,
                                    cm_ty_resolve((CmTyArena *)&tyck->arena,
                                        callee_ty0->children[0]));
                                if (self_child != NULL
                                    && self_child->kind != CM_TY_SELF)
                                    closure_self = callee_ty0->children[0];
                                closure_args = callee_ty0->children + 1;
                                closure_arg_count = callee_ty0->count - 1u;
                            }
                            if (closure_body != NULL)
                                closure_instance = cm_umir_c_instance(
                                    cm_umir_c_active_program,
                                    closure_body->origin.definition,
                                    (CmUExprId)callee_ty0->b, closure_args,
                                    closure_arg_count, closure_self);
                        }
                        cm_str_buf_append(output, "0; { long long ");
                        cm_umir_c_render_closure_symbol(output,
                            (CmHirBodyId)callee_ty0->a,
                            (CmUExprId)callee_ty0->b, closure_instance);
                        cm_str_buf_append(output, "(); ");
                        cm_umir_c_render_local(output,
                            statement->destination);
                        cm_str_buf_append(output, " = ");
                        cm_umir_c_render_closure_symbol(output,
                            (CmHirBodyId)callee_ty0->a,
                            (CmUExprId)callee_ty0->b, closure_instance);
                        cm_str_buf_append(output,
                            "((long long *)(intptr_t)");
                        cm_umir_c_render_loaded(output,
                            statement->operands[0], closure_ref_depth);
                        for (arg = 1u; arg < statement->operand_count;
                                ++arg) {
                            cm_str_buf_append(output, ", ");
                            cm_umir_c_render_local(output,
                                statement->operands[arg]);
                        }
                        cm_str_buf_append(output, "); }");
                        break;
                    }
                }
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
                if (cm_hir_def_id_is_none(callee_def)
                    && statement->operand_count != 0u
                    && statement->operand_overflow == 0u) {
                    /* A fn-pointer value (`(self.formatter)(ptr, f)`):
                     * call through it. */
                    const CmTy *pt = cm_ty_get((CmTyArena *)&tyck->arena,
                        cm_ty_resolve((CmTyArena *)&tyck->arena,
                            cm_umir_c_local_type(body,
                                statement->operands[0])));
                    if (pt != NULL && pt->kind == CM_TY_FN_DEF) {
                        /* A fn item held in a local (`f` with `F: FnOnce`
                         * instantiated by `ToOwned::to_owned` in core's
                         * `Option::map_or_else`): a direct call to that
                         * item's instance; the value itself carries no
                         * data. */
                        if (cm_umir_c_render_call(output, hir, tyck,
                                statement, pt->def, 1u,
                                cm_umir_c_local_type(body,
                                    statement->operands[0]), CM_TY_NONE))
                            break;
                    }
                    if (pt != NULL && pt->kind == CM_TY_FN_PTR) {
                        uint32_t arg;
                        cm_str_buf_append(output,
                            "0; { long long (*_fp)() = (long long (*)())"
                            "(intptr_t)");
                        cm_umir_c_render_local(output, statement->operands[0]);
                        cm_str_buf_append(output, "; ");
                        cm_umir_c_render_local(output, statement->destination);
                        cm_str_buf_append(output, " = _fp(");
                        for (arg = 1u; arg < statement->operand_count; ++arg) {
                            if (arg != 1u) cm_str_buf_append(output, ", ");
                            cm_umir_c_render_local(output,
                                statement->operands[arg]);
                        }
                        cm_str_buf_append(output, "); }");
                        break;
                    }
                }
                if (!cm_umir_c_render_call(output, hir, tyck, statement,
                        callee_def, 1u,
                        expr != NULL && expr->kind == CM_U_EXPR_CALL
                            && tb != NULL && tb->expr_types != NULL
                            ? tb->expr_types[expr->data.call.callee]
                            : CM_TY_NONE,
                        CM_TY_NONE)) {
                    cm_str_buf_append(output, "0 /* call */");
                    complete = 0;
                    if (getenv("CMRUSTC_UMIR_DEBUG") != NULL) {
                        CmStrBuf text;
                        cm_str_buf_init(&text);
                        if (tb != NULL && tb->expr_types != NULL
                            && expr != NULL && expr->kind == CM_U_EXPR_CALL)
                            cm_ty_print((CmTyArena *)&tyck->arena, hir,
                                tb->expr_types[expr->data.call.callee],
                                &text);
                        {
                            const CmInternedString *on = owner == NULL
                                ? NULL : cm_interner_get(&hir->strings,
                                    owner->name);
                            fprintf(stderr, "UMIR call-miss in=%.*s ",
                                on == NULL ? 1 : (int)on->len,
                                on == NULL ? "?" : (const char *)on->bytes);
                        }
                        fprintf(stderr, "UMIR call-miss callee-kind=%d "
                            "res=%d def=%u:%u type=%.*s\n",
                            callee == NULL ? -1 : (int)callee->kind,
                            callee == NULL
                                || callee->kind != CM_U_EXPR_PATH ? -1
                                : (int)callee->data.path.resolution.kind,
                            (unsigned)callee_def.crate_id,
                            (unsigned)callee_def.index, (int)text.len,
                            text.data);
                        cm_str_buf_destroy(&text);
                    }
                }
                break;
            }
            case CM_UMIR_RVALUE_TRY_UNWRAP:
                /* The `Some`/`Ok` payload (slot 1) of a matched value. */
                cm_str_buf_append(output, "((long long *)(intptr_t)");
                cm_umir_c_render_local(output, statement->operands[0]);
                cm_str_buf_append(output, ")[1]");
                break;
            case CM_UMIR_RVALUE_INTO_ITER: {
                CmHirDefId into_iter = cm_umir_c_into_iterator(hir);
                CmTyId source_type = cm_umir_c_local_type(body,
                    statement->operands[0]);
                if (cm_hir_def_id_is_none(into_iter)
                    || source_type == CM_TY_NONE
                    || statement->operand_count != 1u) {
                    cm_str_buf_append(output, "0 /* into-iter */");
                    complete = 0;
                    break;
                }
                {
                    CmStrBuf symbol;
                    cm_str_buf_init(&symbol);
                    cm_umir_c_render_callee_symbol(&symbol, hir, tyck,
                        into_iter, CM_TY_NONE, source_type, statement, 0u);
                    cm_str_buf_append(output, "0; { long long ");
                    cm_str_buf_append_n(output, symbol.data, symbol.len);
                    cm_str_buf_append(output, "(); ");
                    cm_umir_c_render_local(output, statement->destination);
                    cm_str_buf_append(output, " = ");
                    cm_str_buf_append_n(output, symbol.data, symbol.len);
                    cm_str_buf_push(output, '(');
                    cm_umir_c_render_local(output, statement->operands[0]);
                    cm_str_buf_append(output, "); }");
                    cm_str_buf_destroy(&symbol);
                }
                break;
            }
            case CM_UMIR_RVALUE_ITER_NEXT: {
                /* `Iterator::next(&mut iterable)`: resolve the impl for
                 * the iterable's type; the argument is the slot address. */
                CmHirDefId next_def = cm_umir_c_iterator_next(hir);
                CmTyId iter_type = cm_umir_c_local_type(body,
                    statement->operands[0]);
                if (cm_hir_def_id_is_none(next_def) || iter_type == CM_TY_NONE
                    || statement->operand_count != 1u) {
                    cm_str_buf_append(output, "0 /* iter-next */");
                    complete = 0;
                    break;
                }
                {
                    CmStrBuf symbol;
                    cm_str_buf_init(&symbol);
                    cm_umir_c_render_callee_symbol(&symbol, hir, tyck,
                        next_def, CM_TY_NONE, iter_type, NULL, 0u);
                    cm_str_buf_append(output, "0; { long long ");
                    cm_str_buf_append_n(output, symbol.data, symbol.len);
                    cm_str_buf_append(output, "(); ");
                    cm_umir_c_render_local(output, statement->destination);
                    cm_str_buf_append(output, " = ");
                    cm_str_buf_append_n(output, symbol.data, symbol.len);
                    cm_str_buf_append(output, "((long long)(intptr_t)&");
                    cm_umir_c_render_local(output, statement->operands[0]);
                    cm_str_buf_append(output, "); }");
                    cm_str_buf_destroy(&symbol);
                }
                break;
            }
            case CM_UMIR_RVALUE_STATIC_ADDR: {
                /* The static's storage slot (its wrapper's cache). */
                const CmUExpr *path_expr = statement->expr == CM_U_EXPR_NONE
                    ? NULL : cm_ubody_get_expr(ub, statement->expr);
                if (path_expr == NULL || path_expr->kind != CM_U_EXPR_PATH
                    || path_expr->data.path.resolution.kind
                        != CM_U_RESOLVED_DEFINITION) {
                    cm_str_buf_append(output, "0 /* static-addr */");
                    complete = 0;
                    break;
                }
                /* `&VAL` alone must still emit the static (thread_local!'s
                 * `|_| { static VAL: T = __INIT; &VAL }`). */
                if (cm_umir_c_active_program != NULL)
                    (void)cm_umir_c_instance(cm_umir_c_active_program,
                        path_expr->data.path.resolution.definition,
                        CM_U_EXPR_NONE, NULL, 0u, CM_TY_NONE);
                cm_str_buf_append(output, "0; { long long ");
                cm_umir_c_render_symbol(output,
                    path_expr->data.path.resolution.definition);
                cm_str_buf_append(output, "_addr(); ");
                cm_umir_c_render_local(output, statement->destination);
                cm_str_buf_append(output, " = ");
                cm_umir_c_render_symbol(output,
                    path_expr->data.path.resolution.definition);
                cm_str_buf_append(output, "_addr(); }");
                break;
            }
            case CM_UMIR_RVALUE_DROP:
            case CM_UMIR_RVALUE_SCOPE_DROP: {
                /* The temporary's drop glue; `&mut self` is its slot. */
                CmHirDefId drop_decl = cm_umir_c_drop_fn(hir);
                CmTyId dropped_type = cm_umir_c_subst(statement->type);
                int scope_drop = statement->kind
                    == CM_UMIR_RVALUE_SCOPE_DROP;
                int allowed = 1;
                CmStrBuf address;
                if (scope_drop) {
                    /* General local drops need move-path flags.  Until those
                     * land, run an outer type's own RAII destructor, plus
                     * RefMut's field-owned borrow guard, and exclude Vec,
                     * whose value is routinely moved through return/aggregate
                     * temporaries in the lenient u-MIR. */
                    CmHirDefId own = cm_umir_c_drop_impl(hir, tyck,
                        drop_decl, dropped_type);
                    const CmTy *dt = dropped_type == CM_TY_NONE ? NULL
                        : cm_ty_get((CmTyArena *)&tyck->arena,
                            cm_ty_resolve((CmTyArena *)&tyck->arena,
                                dropped_type));
                    const CmHirItem *type_item = dt == NULL
                            || dt->kind != CM_TY_ADT ? NULL
                        : cm_umir_c_item_of(hir, dt->def);
                    const CmInternedString *type_name = type_item == NULL
                        ? NULL : cm_interner_get(&hir->strings,
                            type_item->name);
                    allowed = (!cm_hir_def_id_is_none(own)
                            || cm_umir_c_is_core_refmut(hir, type_item))
                        && !(type_name != NULL && type_name->len == 3u
                            && memcmp(type_name->bytes, "Vec", 3u) == 0);
                }
                cm_str_buf_init(&address);
                cm_str_buf_append(&address, "(long long)(intptr_t)&");
                cm_umir_c_render_local(&address, statement->operands[0]);
                cm_str_buf_append(output, "0; ");
                if (allowed && !cm_hir_def_id_is_none(drop_decl)
                    && cm_umir_c_type_needs_drop(hir, tyck, drop_decl,
                        dropped_type, 0u))
                    cm_umir_c_render_drop(output, hir, tyck, drop_decl,
                        dropped_type, address.data, address.len, 0u);
                cm_str_buf_destroy(&address);
                break;
            }
            case CM_UMIR_RVALUE_SUBSLICE: {
                /* `text[offset..]`: a fresh [data, len] pair over the
                 * base's elements (scalar elements at their own width,
                 * others as slots), like an unsize but offset. */
                CmTyId base_type = cm_umir_c_local_type(body,
                    statement->operands[0]);
                CmTyId pointee = cm_umir_c_peel(tyck, base_type);
                const CmTy *pt = cm_ty_get((CmTyArena *)&tyck->arena,
                    cm_ty_resolve((CmTyArena *)&tyck->arena, pointee));
                unsigned int depth = cm_umir_c_ref_depth(tyck, base_type);
                int fat = cm_umir_c_is_fat(hir, tyck, pointee);
                const char *scalar = pt != NULL && pt->kind == CM_TY_STR
                    ? "uint8_t" : pt != NULL && pt->count != 0u
                        && (pt->kind == CM_TY_SLICE || pt->kind == CM_TY_ARRAY)
                    ? cm_umir_c_scalar_type(tyck, pt->children[0]) : NULL;
                unsigned long form = (unsigned long)statement->immediate;
                CmStrBuf start;
                CmStrBuf end;
                if (!fat && (pt == NULL || pt->kind != CM_TY_ARRAY)) {
                    cm_str_buf_append(output, "0 /* subslice */");
                    complete = 0;
                    break;
                }
                cm_str_buf_init(&start);
                cm_str_buf_init(&end);
                /* start / end by range form. */
                if (form == 1ul || form == 3ul || form == 5ul) {
                    if (form == 1ul)
                        cm_umir_c_render_local(&start, statement->operands[1]);
                    else {
                        cm_str_buf_append(&start, "((long long *)(intptr_t)");
                        cm_umir_c_render_local(&start, statement->operands[1]);
                        cm_str_buf_append(&start, ")[0]");
                    }
                } else
                    cm_str_buf_append(&start, "0");
                if (form == 1ul || form == 4ul) {
                    if (fat) {
                        cm_umir_c_render_base(&end, statement->operands[0],
                            depth);
                        cm_str_buf_append(&end, "[1]");
                    } else
                        cm_umir_c_render_number(&end,
                            cm_umir_c_array_len(tyck, pointee));
                } else if (form == 2ul || form == 6ul) {
                    cm_umir_c_render_local(&end, statement->operands[1]);
                    if (form == 6ul) cm_str_buf_append(&end, " + 1");
                } else {
                    cm_str_buf_append(&end, "((long long *)(intptr_t)");
                    cm_umir_c_render_local(&end, statement->operands[1]);
                    cm_str_buf_append(&end, ")[1]");
                    if (form == 5ul) cm_str_buf_append(&end, " + 1");
                }
                cm_str_buf_append(output, "0; _agg");
                cm_umir_c_render_number(output,
                    (unsigned long)statement->destination);
                cm_str_buf_append(output, " = (long long *)malloc(24); _agg");
                cm_umir_c_render_number(output,
                    (unsigned long)statement->destination);
                cm_str_buf_append(output, "[1] = (long long)(intptr_t)((");
                cm_str_buf_append(output, scalar == NULL ? "long long"
                    : scalar);
                cm_str_buf_append(output, " *)(intptr_t)");
                if (fat) {
                    cm_umir_c_render_base(output, statement->operands[0],
                        depth);
                    cm_str_buf_append(output, "[0]");
                } else {
                    cm_umir_c_render_base(output, statement->operands[0],
                        depth);
                }
                cm_str_buf_append(output, " + ");
                cm_str_buf_append_n(output, start.data, start.len);
                cm_str_buf_append(output, "); _agg");
                cm_umir_c_render_number(output,
                    (unsigned long)statement->destination);
                cm_str_buf_append(output, "[2] = (");
                cm_str_buf_append_n(output, end.data, end.len);
                cm_str_buf_append(output, ") - (");
                cm_str_buf_append_n(output, start.data, start.len);
                cm_str_buf_append(output, "); _agg");
                cm_umir_c_render_number(output,
                    (unsigned long)statement->destination);
                cm_str_buf_append(output, "[0] = (long long)(intptr_t)&_agg");
                cm_umir_c_render_number(output,
                    (unsigned long)statement->destination);
                cm_str_buf_append(output, "[1]; ");
                cm_umir_c_render_local(output, statement->destination);
                cm_str_buf_append(output, " = (long long)(intptr_t)&_agg");
                cm_umir_c_render_number(output,
                    (unsigned long)statement->destination);
                cm_str_buf_append(output, "[0]");
                cm_str_buf_destroy(&start);
                cm_str_buf_destroy(&end);
                break;
            }
            case CM_UMIR_RVALUE_SLICE_LEN: {
                /* Length of a slice/str (the pair's second slot, through
                 * the reference layers) or of an array (its type). */
                CmTyId base_type = cm_umir_c_local_type(body,
                    statement->operands[0]);
                CmTyId pointee = cm_umir_c_peel(tyck, base_type);
                const CmTy *pt = cm_ty_get((CmTyArena *)&tyck->arena,
                    cm_ty_resolve((CmTyArena *)&tyck->arena, pointee));
                if (cm_umir_c_is_fat(hir, tyck, pointee)) {
                    cm_umir_c_render_base(output, statement->operands[0],
                        cm_umir_c_ref_depth(tyck, base_type));
                    cm_str_buf_append(output, "[1]");
                } else if (pt != NULL && pt->kind == CM_TY_ARRAY) {
                    cm_umir_c_render_number(output,
                        cm_umir_c_array_len(tyck, pointee));
                } else {
                    cm_str_buf_append(output, "0 /* slice-len */");
                    complete = 0;
                }
                break;
            }
            case CM_UMIR_RVALUE_DEREF_CALL: {
                /* `Deref::deref(&receiver)`: the operand is already the
                 * reference; the impl is the pointee's. */
                CmHirDefId deref_def = cm_umir_c_deref_fn(hir,
                    statement->immediate != 0u);
                CmTyId ref_type = statement->operand_count == 1u
                    ? cm_umir_c_local_type(body, statement->operands[0])
                    : CM_TY_NONE;
                const CmTy *ref_ty = ref_type == CM_TY_NONE ? NULL
                    : cm_ty_get((CmTyArena *)&tyck->arena,
                        cm_ty_resolve((CmTyArena *)&tyck->arena,
                            cm_umir_c_subst(ref_type)));
                if (cm_hir_def_id_is_none(deref_def) || ref_ty == NULL) {
                    cm_str_buf_append(output, "0 /* deref-call */");
                    complete = 0;
                    break;
                }
                {
                    /* An address-of local keeps the referent's type
                     * (lowering's convention); a real reference peels. */
                    CmTyId self_type = ref_ty->kind == CM_TY_REF
                            || ref_ty->kind == CM_TY_PTR
                        ? ref_ty->children[0] : cm_umir_c_subst(ref_type);
                    CmStrBuf symbol;
                    cm_str_buf_init(&symbol);
                    cm_umir_c_render_callee_symbol(&symbol, hir, tyck,
                        deref_def, CM_TY_NONE, self_type, NULL, 0u);
                    cm_str_buf_append(output, "0; { long long ");
                    cm_str_buf_append_n(output, symbol.data, symbol.len);
                    cm_str_buf_append(output, "(); ");
                    cm_umir_c_render_local(output, statement->destination);
                    cm_str_buf_append(output, " = ");
                    cm_str_buf_append_n(output, symbol.data, symbol.len);
                    cm_str_buf_append(output, "(");
                    cm_umir_c_render_local(output, statement->operands[0]);
                    cm_str_buf_append(output, "); }");
                    cm_str_buf_destroy(&symbol);
                }
                break;
            }
            case CM_UMIR_RVALUE_UNSIZE: {
                /* `&T -> &dyn Trait`: slot 1 the reference, slot 2 the
                 * vtable; the destination references the pair. */
                /* A pointer-shaped ADT unsizes as its representation:
                 * `Box<[T; N]>` -> `Box<[T]>` is `*mut [T; N]` -> `*mut [T]`. */
                CmTyId target = cm_umir_c_representation(hir, tyck,
                    cm_umir_c_subst(statement->type));
                /* Both representations before any CmTy pointer is held:
                 * they can create types and move the arena. */
                CmTyId source = statement->operand_count == 0u ? CM_TY_NONE
                    : cm_umir_c_representation(hir, tyck,
                        cm_umir_c_local_type(body, statement->operands[0]));
                const CmTy *dt = cm_ty_get((CmTyArena *)&tyck->arena,
                    cm_ty_resolve((CmTyArena *)&tyck->arena,
                        cm_umir_c_peel(tyck, target)));
                const CmTy *st = source == CM_TY_NONE ? NULL
                    : cm_ty_get((CmTyArena *)&tyck->arena,
                        cm_ty_resolve((CmTyArena *)&tyck->arena, source));
                CmTyId concrete = st != NULL && (st->kind == CM_TY_REF
                    || st->kind == CM_TY_PTR) ? st->children[0] : source;
                long vt = dt == NULL || dt->kind != CM_TY_DYN
                        || cm_umir_c_active_program == NULL
                        || concrete == CM_TY_NONE ? -1
                    : cm_umir_c_vtable(cm_umir_c_active_program, dt->def,
                        concrete);
                if (dt != NULL && dt->kind == CM_TY_SLICE) {
                    /* `&[T; N] -> &[T]`: the pair is [element block, N];
                     * the block is what the array reference points at. */
                    cm_str_buf_append(output, "0; _agg");
                    cm_umir_c_render_number(output,
                        (unsigned long)statement->destination);
                    cm_str_buf_append(output,
                        " = (long long *)malloc(24); _agg");
                    cm_umir_c_render_number(output,
                        (unsigned long)statement->destination);
                    cm_str_buf_append(output, "[1] = *(long long *)(intptr_t)");
                    cm_umir_c_render_local(output, statement->operands[0]);
                    cm_str_buf_append(output, "; _agg");
                    cm_umir_c_render_number(output,
                        (unsigned long)statement->destination);
                    cm_str_buf_append(output, "[2] = ");
                    if (cm_umir_c_array_len(tyck, concrete) != 0ul)
                        cm_umir_c_render_number(output,
                            cm_umir_c_array_len(tyck, concrete));
                    else {
                        /* Length unknown to the type: the block header. */
                        cm_str_buf_append(output, "((long long *)(intptr_t)_agg");
                        cm_umir_c_render_number(output,
                            (unsigned long)statement->destination);
                        cm_str_buf_append(output, "[1])[-1]");
                    }
                    cm_str_buf_append(output, "; _agg");
                    cm_umir_c_render_number(output,
                        (unsigned long)statement->destination);
                    cm_str_buf_append(output,
                        "[0] = (long long)(intptr_t)&_agg");
                    cm_umir_c_render_number(output,
                        (unsigned long)statement->destination);
                    cm_str_buf_append(output, "[1]; ");
                    cm_umir_c_render_local(output, statement->destination);
                    cm_str_buf_append(output, " = (long long)(intptr_t)&_agg");
                    cm_umir_c_render_number(output,
                        (unsigned long)statement->destination);
                    cm_str_buf_append(output, "[0]");
                    break;
                }
                if (dt != NULL && dt->kind == CM_TY_FN_PTR && st != NULL
                    && st->kind == CM_TY_CLOSURE) {
                    /* A capture-free closure as a fn pointer: its thunk's
                     * address (the closure body is instanced as a call
                     * through the value would instance it). */
                    long closure_instance = -1;
                    const CmHirBody *closure_body = cm_hir_get_body(hir,
                        (CmHirBodyId)st->a);
                    if (cm_umir_c_active_program != NULL
                        && closure_body != NULL) {
                        CmTyId closure_self = CM_TY_NONE;
                        const CmTyId *closure_args = NULL;
                        uint32_t closure_arg_count = 0u;
                        if (st->count != 0u) {
                            const CmTy *self_child = cm_ty_get(
                                (CmTyArena *)&tyck->arena,
                                cm_ty_resolve((CmTyArena *)&tyck->arena,
                                    st->children[0]));
                            if (self_child != NULL
                                && self_child->kind != CM_TY_SELF)
                                closure_self = st->children[0];
                            closure_args = st->children + 1;
                            closure_arg_count = st->count - 1u;
                        }
                        closure_instance = cm_umir_c_instance(
                            cm_umir_c_active_program,
                            closure_body->origin.definition,
                            (CmUExprId)st->b, closure_args,
                            closure_arg_count, closure_self);
                    }
                    cm_str_buf_append(output, "0; { long long ");
                    cm_umir_c_render_closure_symbol(output,
                        (CmHirBodyId)st->a, (CmUExprId)st->b,
                        closure_instance);
                    cm_str_buf_append(output, "_fp(); ");
                    cm_umir_c_render_local(output, statement->destination);
                    cm_str_buf_append(output,
                        " = (long long)(intptr_t)&");
                    cm_umir_c_render_closure_symbol(output,
                        (CmHirBodyId)st->a, (CmUExprId)st->b,
                        closure_instance);
                    cm_str_buf_append(output, "_fp; }");
                    break;
                }
                if (vt < 0) {
                    if (getenv("CMRUSTC_UMIR_DEBUG") != NULL) {
                        CmStrBuf text;
                        cm_str_buf_init(&text);
                        cm_ty_print((CmTyArena *)&tyck->arena, hir,
                            cm_umir_c_subst(statement->type), &text);
                        cm_str_buf_append(&text, " rep ");
                        cm_ty_print((CmTyArena *)&tyck->arena, hir, target,
                            &text);
                        cm_str_buf_append(&text, " from ");
                        if (source != CM_TY_NONE)
                            cm_ty_print((CmTyArena *)&tyck->arena, hir,
                                source, &text);
                        fprintf(stderr, "UMIR unsize-miss %.*s dt=%d\n",
                            (int)text.len, text.data,
                            dt == NULL ? -1 : (int)dt->kind);
                        cm_str_buf_destroy(&text);
                    }
                    cm_umir_c_render_local(output, statement->operands[0]);
                    cm_str_buf_append(output, " /* unsize */");
                    if (cm_umir_c_active_program != NULL) complete = 0;
                    break;
                }
                cm_str_buf_append(output, "0; _agg");
                cm_umir_c_render_number(output,
                    (unsigned long)statement->destination);
                cm_str_buf_append(output, " = (long long *)malloc(24); _agg");
                cm_umir_c_render_number(output,
                    (unsigned long)statement->destination);
                cm_str_buf_append(output, "[1] = ");
                cm_umir_c_render_local(output, statement->operands[0]);
                cm_str_buf_append(output, "; _agg");
                cm_umir_c_render_number(output,
                    (unsigned long)statement->destination);
                cm_str_buf_append(output, "[2] = (long long)(intptr_t)cm_vt_");
                cm_umir_c_render_number(output, (unsigned long)vt);
                cm_str_buf_append(output, "; _agg");
                cm_umir_c_render_number(output,
                    (unsigned long)statement->destination);
                cm_str_buf_append(output, "[0] = (long long)(intptr_t)&_agg");
                cm_umir_c_render_number(output,
                    (unsigned long)statement->destination);
                cm_str_buf_append(output, "[1]; ");
                cm_umir_c_render_local(output, statement->destination);
                cm_str_buf_append(output, " = (long long)(intptr_t)&_agg");
                cm_umir_c_render_number(output,
                    (unsigned long)statement->destination);
                cm_str_buf_append(output, "[0]");
                break;
            }
            case CM_UMIR_RVALUE_METHOD_CALL: {
                const CmTyckBody *mtb = cm_tyck_get(tyck, body->source);
                CmHirDefId method_def = mtb == NULL
                        || mtb->method_targets == NULL
                    ? cm_hir_def_id_none()
                    : mtb->method_targets[statement->expr];
                if (statement->operand_count != 0u
                    && statement->operand_overflow == 0u
                    && !cm_hir_def_id_is_none(method_def)) {
                    /* Trait object receiver: index the vtable behind the
                     * pair and pass the data reference as `self`. */
                    CmTyId rtype = cm_umir_c_local_type(body,
                        statement->operands[0]);
                    const CmTy *rt = cm_ty_get((CmTyArena *)&tyck->arena,
                        cm_ty_resolve((CmTyArena *)&tyck->arena,
                            cm_umir_c_peel(tyck, rtype)));
                    long slot = rt != NULL && rt->kind == CM_TY_DYN
                        ? cm_umir_c_trait_method_index(hir, rt->def,
                            method_def) : -1;
                    if (rt != NULL && rt->kind == CM_TY_DYN && slot < 0
                        && getenv("CMRUSTC_UMIR_DEBUG") != NULL)
                        fprintf(stderr, "UMIR dyn-slot miss def=%u:%u "
                            "method=%u:%u\n", (unsigned)rt->def.crate_id,
                            (unsigned)rt->def.index,
                            (unsigned)method_def.crate_id,
                            (unsigned)method_def.index);
                    if (slot >= 0) {
                        uint32_t arg;
                        cm_str_buf_append(output,
                            "0; { long long (**_vt)() = (long long (**)())"
                            "(intptr_t)");
                        cm_umir_c_render_base(output, statement->operands[0],
                            cm_umir_c_ref_depth(tyck, rtype));
                        cm_str_buf_append(output, "[1]; ");
                        cm_umir_c_render_local(output,
                            statement->destination);
                        cm_str_buf_append(output, " = _vt[");
                        cm_umir_c_render_number(output, (unsigned long)slot);
                        cm_str_buf_append(output, "](");
                        cm_umir_c_render_base(output, statement->operands[0],
                            cm_umir_c_ref_depth(tyck, rtype));
                        cm_str_buf_append(output, "[0]");
                        for (arg = 1u; arg < statement->operand_count;
                                ++arg) {
                            cm_str_buf_append(output, ", ");
                            cm_umir_c_render_local(output,
                                statement->operands[arg]);
                        }
                        cm_str_buf_append(output, "); }");
                        break;
                    }
                }
                if (!cm_umir_c_render_call(output, hir, tyck, statement,
                        method_def, 0u, CM_TY_NONE,
                        statement->operand_count != 0u
                            ? cm_umir_c_local_type(body,
                                statement->operands[0])
                            : CM_TY_NONE)) {
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
            cm_str_buf_append(output, "return ");
            cm_umir_c_render_local(output, body->closure_expr
                == CM_U_EXPR_NONE ? 0u : (CmUMirLocalId)body->env_count);
            cm_str_buf_append(output, ";\n");
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
            /* A scalar scrutinee (literal patterns) switches on its value;
             * an aggregate switches on the block's discriminant slot. */
            int scalar = cm_umir_c_active_body != NULL
                && cm_umir_c_scalar_type(tyck, cm_umir_c_subst(
                    cm_umir_c_local_type(cm_umir_c_active_body,
                        block->condition))) != NULL;
            if (scalar) {
                cm_str_buf_append(output, "switch ((long long)");
                cm_umir_c_render_local(output, block->condition);
                cm_str_buf_append(output, ") {");
            } else {
                /* A `&Enum` scrutinee (`match self` in a `&self` method,
                 * default binding modes): the discriminant sits behind
                 * the reference, as the payload slots already assume. */
                int behind_ref = 0;
                if (cm_umir_c_active_body != NULL) {
                    CmTyArena *arena = (CmTyArena *)&tyck->arena;
                    CmTyId ct = cm_umir_c_subst(cm_umir_c_local_type(
                        cm_umir_c_active_body, block->condition));
                    const CmTy *t = ct == CM_TY_NONE ? NULL
                        : cm_ty_get(arena, cm_ty_resolve(arena, ct));
                    if (t != NULL && (t->kind == CM_TY_REF
                            || t->kind == CM_TY_PTR) && t->count != 0u) {
                        const CmTy *pointee = cm_ty_get(arena,
                            cm_ty_resolve(arena, t->children[0]));
                        behind_ref = pointee != NULL
                            && pointee->kind == CM_TY_ADT;
                    }
                }
                cm_str_buf_append(output, behind_ref
                    ? "switch ((int)((long long *)(intptr_t)"
                        "*(long long *)(intptr_t)"
                    : "switch ((int)((long long *)(intptr_t)");
                cm_umir_c_render_local(output, block->condition);
                cm_str_buf_append(output, ")[0]) {");
            }
            for (arm = 0u; arm < block->arm_count; ++arm) {
                uint32_t earlier;
                int duplicate = 0;
                if (block->arm_discriminants[arm] == CM_UMIR_ARM_DEFAULT) {
                    if (fallback == block->goto_target)
                        fallback = block->arm_targets[arm];
                    continue;
                }
                /* First entry per discriminant wins; later entries are
                 * reached by guard fall-through. */
                for (earlier = 0u; earlier < arm; ++earlier)
                    if (block->arm_discriminants[earlier]
                            == block->arm_discriminants[arm]) duplicate = 1;
                if (duplicate) continue;
                cm_str_buf_append(output, " case ");
                if (block->arm_discriminants[arm] < 0) {
                    cm_str_buf_push(output, '-');
                    cm_umir_c_render_number(output,
                        (unsigned long)-block->arm_discriminants[arm]);
                } else {
                    cm_umir_c_render_number(output,
                        (unsigned long)block->arm_discriminants[arm]);
                }
                cm_str_buf_append(output, "LL: goto _b");
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
            cm_str_buf_append(output, "return ");
            cm_umir_c_render_local(output, body->closure_expr
                == CM_U_EXPR_NONE ? 0u : (CmUMirLocalId)body->env_count);
            cm_str_buf_append(output, ";\n");
            break;
        }
    }
    cm_str_buf_append(output, "}\n");
    if (body->closure_expr != CM_U_EXPR_NONE) {
        /* The fn-pointer form of a (capture-free) closure: a thunk with
         * the closure's parameters that supplies a scratch frame as the
         * environment (thread_local!'s `LocalKey::new(|init| ..)`). */
        const CmUExpr *closure = cm_ubody_get_expr(ub, body->closure_expr);
        uint32_t closure_params = closure == NULL ? 0u
            : closure->data.closure.parameter_count;
        cm_str_buf_append(output, "long long ");
        cm_umir_c_render_closure_symbol(output, body->source,
            body->closure_expr, cm_umir_c_active_instance == NULL ? -1
                : (long)cm_umir_c_active_instance->index);
        cm_str_buf_append(output, "_fp(");
        if (closure_params == 0u) cm_str_buf_append(output, "void");
        for (param = 0u; param < closure_params; ++param) {
            if (param != 0u) cm_str_buf_append(output, ", ");
            cm_str_buf_append(output, "long long p");
            cm_umir_c_render_number(output, (unsigned long)param);
        }
        cm_str_buf_append(output, ")\n{\n    long long *env = (long long *)"
            "calloc(");
        cm_umir_c_render_number(output, (unsigned long)(body->env_count + 1u));
        cm_str_buf_append(output, ", 8);\n    return ");
        cm_umir_c_render_closure_symbol(output, body->source,
            body->closure_expr, cm_umir_c_active_instance == NULL ? -1
                : (long)cm_umir_c_active_instance->index);
        cm_str_buf_append(output, "(env");
        for (param = 0u; param < closure_params; ++param) {
            cm_str_buf_append(output, ", p");
            cm_umir_c_render_number(output, (unsigned long)param);
        }
        cm_str_buf_append(output, ");\n}\n");
        /* The vtable form (`&mut closure` as `&mut dyn FnMut(A) -> R`):
         * `self` is the object's data pointer -- the address of the slot
         * holding the closure value, its environment -- and the
         * arguments arrive as one tuple block. */
        cm_str_buf_append(output, "long long ");
        cm_umir_c_render_closure_symbol(output, body->source,
            body->closure_expr, cm_umir_c_active_instance == NULL ? -1
                : (long)cm_umir_c_active_instance->index);
        cm_str_buf_append(output, "_vt(long long self_ref, long long args)"
            "\n{\n    long long *env = (long long *)(intptr_t)"
            "*(long long *)(intptr_t)self_ref;\n    (void)args;\n"
            "    return ");
        cm_umir_c_render_closure_symbol(output, body->source,
            body->closure_expr, cm_umir_c_active_instance == NULL ? -1
                : (long)cm_umir_c_active_instance->index);
        cm_str_buf_append(output, "(env");
        for (param = 0u; param < closure_params; ++param) {
            cm_str_buf_append(output, ", ((long long *)(intptr_t)args)[");
            cm_umir_c_render_number(output, (unsigned long)param);
            cm_str_buf_push(output, ']');
        }
        cm_str_buf_append(output, ");\n}\n");
    }
    cm_umir_c_active_body = NULL;
    /* `#[no_mangle]` exports: an ABI-typed wrapper with the item name. */
    if (body->closure_expr == CM_U_EXPR_NONE && owner != NULL
        && owner->kind == CM_HIR_ITEM_FUNCTION
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

/* CMRUSTC_UMIR_FORCE_STUB="module::fn,module::fn": functions the host
 * replaces with a stub even though they lowered (the lenient runtime:
 * std's `stack_overflow::init` needs C-layout `sigaction` / `stack_t`
 * structs the slot representation cannot yet pass to libc). */
static int cm_umir_c_forced_stub(const CmHirContext *hir, CmHirDefId def)
{
    const char *list = getenv("CMRUSTC_UMIR_FORCE_STUB");
    const CmHirItem *item;
    const CmHirModule *module;
    const CmInternedString *name;
    const CmInternedString *mname;
    if (list == NULL || *list == 0) return 0;
    item = cm_umir_c_item_of(hir, def);
    if (item == NULL) return 0;
    name = cm_interner_get(&hir->strings, item->name);
    module = cm_hir_get_module(hir, item->owner_module);
    mname = module == NULL ? NULL
        : cm_interner_get(&hir->strings, module->name);
    if (name == NULL || mname == NULL) return 0;
    (void)mname;
    while (*list != 0) {
        /* `a::b::fn`: the fn name, then each module segment against the
         * owner module and its parents (`stack_overflow::imp::init`). */
        const char *entry = list;
        const char *end;
        const char *cursor;
        const CmHirModule *walk = module;
        int matched = 1;
        while (*list != 0 && *list != ',') list += 1;
        end = list;
        if (*list == ',') list += 1;
        cursor = end;
        while (cursor > entry && !(cursor[-1] == ':' && cursor - 2 >= entry
                && cursor[-2] == ':')) cursor -= 1;
        if (cursor == entry) continue; /* no module segment */
        if ((size_t)(end - cursor) != name->len
            || memcmp(cursor, name->bytes, name->len) != 0) continue;
        end = cursor - 2;
        while (matched && end > entry) {
            const char *seg_start = end;
            const CmInternedString *wname;
            while (seg_start > entry && !(seg_start[-1] == ':'
                    && seg_start - 2 >= entry && seg_start[-2] == ':'))
                seg_start -= 1;
            wname = walk == NULL ? NULL
                : cm_interner_get(&hir->strings, walk->name);
            if (wname == NULL || (size_t)(end - seg_start) != wname->len
                || memcmp(seg_start, wname->bytes, wname->len) != 0) {
                matched = 0;
                break;
            }
            walk = cm_hir_get_module(hir, walk->parent);
            end = seg_start == entry ? entry : seg_start - 2;
        }
        if (matched) return 1;
    }
    return 0;
}

/* Host symbols the prelude prototypes itself (`void abort(void)`,
 * `void *malloc(unsigned long)`, ...): a wrapper re-declaring them as
 * `long long NAME()` would conflict, so those forward with the C types.
 * Returns 1 when `host` was one of them. */
static int cm_umir_c_render_prelude_foreign(CmStrBuf *output,
    const char *host, size_t host_len, uint32_t params)
{
    if (host_len == 5u && memcmp(host, "abort", 5u) == 0) {
        cm_str_buf_append(output, ") { abort(); return 0; } /* foreign */\n");
        return 1;
    }
    if (host_len == 6u && memcmp(host, "malloc", 6u) == 0 && params == 1u) {
        cm_str_buf_append(output, ") { return (long long)(intptr_t)"
            "malloc((unsigned long)p0); } /* foreign */\n");
        return 1;
    }
    if (host_len == 6u && memcmp(host, "calloc", 6u) == 0 && params == 2u) {
        cm_str_buf_append(output, ") { return (long long)(intptr_t)"
            "calloc((unsigned long)p0, (unsigned long)p1); } /* foreign */\n");
        return 1;
    }
    if (host_len == 7u && memcmp(host, "memmove", 7u) == 0 && params == 3u) {
        cm_str_buf_append(output, ") { return (long long)(intptr_t)"
            "memmove((void *)(intptr_t)p0, (const void *)(intptr_t)p1, "
            "(unsigned long)p2); } /* foreign */\n");
        return 1;
    }
    if (host_len == 6u && memcmp(host, "memset", 6u) == 0 && params == 3u) {
        cm_str_buf_append(output, ") { return (long long)(intptr_t)"
            "memset((void *)(intptr_t)p0, (int)p1, (unsigned long)p2); }"
            " /* foreign */\n");
        return 1;
    }
    return 0;
}

/* Arrays in the slot runtime keep their length in the word immediately
 * before the data pointer.  Give allocations made through Rust's global
 * allocator the same one-word prefix, and translate back to the host base
 * for realloc/dealloc.  This also makes a boxed array handed to Vec safe to
 * free after its data pointer has crossed the Rust allocation APIs. */
static int cm_umir_c_render_rust_allocator(CmStrBuf *output,
    const char *host, size_t host_len, uint32_t params)
{
    if (host_len == 12u && memcmp(host, "__rust_alloc", 12u) == 0
        && params == 2u) {
        cm_str_buf_append(output, ") { long long ");
        cm_str_buf_append_n(output, host, host_len);
        cm_str_buf_append(output, "(); long long b = (long long)");
        cm_str_buf_append_n(output, host, host_len);
        cm_str_buf_append(output, "(p0 + 8, p1); if (!b) return 0; "
            "*(long long *)(intptr_t)b = 0; return b + 8; }"
            " /* rust allocator */\n");
        return 1;
    }
    if (host_len == 19u && memcmp(host, "__rust_alloc_zeroed", 19u) == 0
        && params == 2u) {
        cm_str_buf_append(output, ") { long long ");
        cm_str_buf_append_n(output, host, host_len);
        cm_str_buf_append(output, "(); long long b = (long long)");
        cm_str_buf_append_n(output, host, host_len);
        cm_str_buf_append(output, "(p0 + 8, p1); return b ? b + 8 : 0; }"
            " /* rust allocator */\n");
        return 1;
    }
    if (host_len == 14u && memcmp(host, "__rust_realloc", 14u) == 0
        && params == 4u) {
        cm_str_buf_append(output, ") { long long ");
        cm_str_buf_append_n(output, host, host_len);
        cm_str_buf_append(output, "(); long long b = (long long)");
        cm_str_buf_append_n(output, host, host_len);
        cm_str_buf_append(output, "(p0 ? p0 - 8 : 0, p1 + 8, p2, p3 + 8); "
            "return b ? b + 8 : 0; } /* rust allocator */\n");
        return 1;
    }
    if (host_len == 14u && memcmp(host, "__rust_dealloc", 14u) == 0
        && params == 3u) {
        cm_str_buf_append(output, ") { void ");
        cm_str_buf_append_n(output, host, host_len);
        cm_str_buf_append(output, "(); if (p0) ");
        cm_str_buf_append_n(output, host, host_len);
        cm_str_buf_append(output, "(p0 - 8, p1 + 8, p2); return 0; }"
            " /* rust allocator */\n");
        return 1;
    }
    return 0;
}

/* `Iterator::find` is a default trait method.  Trait-owned bodies are still
 * lowered only for the deliberately closed scalar subset, so the generic
 * implementation is otherwise a declaration-only zero stub.  Recreate its
 * small semantic loop at the instance boundary: dispatch `next` through the
 * concrete Self, and invoke the monomorphized predicate. */
static int cm_umir_c_render_iterator_find(CmStrBuf *output,
    const CmHirContext *hir, const CmTyckSet *tyck,
    const CmHirItem *method, const CmUMirInstance *instance)
{
    const CmHirItem *trait_item;
    const CmInternedString *trait_name;
    const CmInternedString *method_name;
    CmHirDefId next = cm_hir_def_id_none();
    CmTyId predicate_type;
    CmTyId predicate_inner;
    const CmTy *predicate;
    const CmHirItem *predicate_item;
    const CmInternedString *predicate_name;
    CmStrBuf next_symbol;
    CmStrBuf predicate_symbol;
    size_t scan;
    long closure_instance = -1;
    int nonempty_predicate = 0;

    if (method == NULL || method->kind != CM_HIR_ITEM_FUNCTION
        || instance == NULL || instance->self_type == CM_TY_NONE
        || instance->count == 0u) return 0;
    trait_item = cm_umir_c_item_of(hir, method->parent_definition);
    trait_name = trait_item == NULL ? NULL
        : cm_interner_get(&hir->strings, trait_item->name);
    method_name = cm_interner_get(&hir->strings, method->name);
    if (trait_item == NULL || trait_item->kind != CM_HIR_ITEM_TRAIT
        || trait_name == NULL || trait_name->len != 8u
        || memcmp(trait_name->bytes, "Iterator", 8u) != 0
        || method_name == NULL || method_name->len != 4u
        || memcmp(method_name->bytes, "find", 4u) != 0) return 0;

    for (scan = 0u; scan < hir->items.len; ++scan) {
        const CmHirItem *candidate = (const CmHirItem *)cm_vec_at_const(
            &hir->items, scan);
        const CmInternedString *candidate_name;
        if (candidate == NULL || candidate->kind != CM_HIR_ITEM_FUNCTION
            || !cm_hir_def_id_equal(candidate->parent_definition,
                trait_item->definition)) continue;
        candidate_name = cm_interner_get(&hir->strings, candidate->name);
        if (candidate_name != NULL && candidate_name->len == 4u
            && memcmp(candidate_name->bytes, "next", 4u) == 0) {
            next = candidate->definition;
            break;
        }
    }
    if (cm_hir_def_id_is_none(next)) return 0;

    predicate_type = instance->types[instance->count - 1u];
    predicate_inner = cm_umir_c_peel(tyck, predicate_type);
    predicate = predicate_inner == CM_TY_NONE ? NULL
        : cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena, predicate_inner));
    predicate_item = predicate == NULL || predicate->kind != CM_TY_ADT
        ? NULL : cm_umir_c_item_of(hir, predicate->def);
    predicate_name = predicate_item == NULL ? NULL
        : cm_interner_get(&hir->strings, predicate_item->name);
    nonempty_predicate = predicate_name != NULL
        && ((predicate_name->len == 10u
                && memcmp(predicate_name->bytes, "IsNotEmpty", 10u) == 0)
            || (predicate_name->len == 15u
                && memcmp(predicate_name->bytes, "BytesIsNotEmpty", 15u)
                    == 0));
    if (predicate == NULL || (predicate->kind != CM_TY_CLOSURE
            && !nonempty_predicate)) return 0;

    cm_str_buf_init(&next_symbol);
    cm_umir_c_render_callee_symbol(&next_symbol, hir, tyck, next,
        CM_TY_NONE, instance->self_type, NULL, 0u);
    cm_str_buf_init(&predicate_symbol);
    if (predicate->kind == CM_TY_CLOSURE) {
        closure_instance = cm_umir_c_closure_instance_of(hir, tyck,
            predicate);
        cm_umir_c_render_closure_symbol(&predicate_symbol,
            (CmHirBodyId)predicate->a, (CmUExprId)predicate->b,
            closure_instance);
    }

    cm_str_buf_append(output, "(long long self, long long pred) { long long " );
    cm_str_buf_append_n(output, next_symbol.data, next_symbol.len);
    cm_str_buf_append(output, "(); for (;;) { long long o = ");
    cm_str_buf_append_n(output, next_symbol.data, next_symbol.len);
    cm_str_buf_append(output, "(self); long long item; if (!o || "
        "((long long *)(intptr_t)o)[0] == 0) return o; item = "
        "((long long *)(intptr_t)o)[1]; if (");
    if (nonempty_predicate) {
        /* `item` is `&str`/`&[u8]`: a slot pointing at its [data,len] pair. */
        cm_str_buf_append(output, "item && *(long long *)(intptr_t)item && "
            "((long long *)(intptr_t)*(long long *)(intptr_t)item)[1] != 0");
    } else {
        cm_str_buf_append(output, "({ long long ");
        cm_str_buf_append_n(output, predicate_symbol.data,
            predicate_symbol.len);
        cm_str_buf_append(output, "(); ");
        cm_str_buf_append_n(output, predicate_symbol.data,
            predicate_symbol.len);
        cm_str_buf_append(output,
            "((long long *)(intptr_t)pred, (long long)(intptr_t)&item); })");
    }
    cm_str_buf_append(output, ") return o; } }");
    cm_str_buf_destroy(&predicate_symbol);
    cm_str_buf_destroy(&next_symbol);
    return 1;
}

/* `Iterator::all` is another declaration-only default when core is loaded
 * through the lenient dependency path.  Its semantics are a short-circuiting
 * loop over `next`; support both an ordinary function item/pointer and the
 * closure representation used by generated iterator adapters. */
static int cm_umir_c_render_iterator_all(CmStrBuf *output,
    const CmHirContext *hir, const CmTyckSet *tyck,
    const CmHirItem *method, const CmUMirInstance *instance)
{
    const CmHirItem *trait_item;
    const CmInternedString *trait_name;
    const CmInternedString *method_name;
    CmHirDefId next = cm_hir_def_id_none();
    CmTyId predicate_type;
    const CmTy *predicate;
    CmStrBuf next_symbol;
    CmStrBuf predicate_symbol;
    size_t scan;
    long closure_instance = -1;

    if (method == NULL || method->kind != CM_HIR_ITEM_FUNCTION
        || instance == NULL || instance->self_type == CM_TY_NONE
        || instance->count == 0u) return 0;
    trait_item = cm_umir_c_item_of(hir, method->parent_definition);
    trait_name = trait_item == NULL ? NULL
        : cm_interner_get(&hir->strings, trait_item->name);
    method_name = cm_interner_get(&hir->strings, method->name);
    if (trait_item == NULL || trait_item->kind != CM_HIR_ITEM_TRAIT
        || trait_name == NULL || trait_name->len != 8u
        || memcmp(trait_name->bytes, "Iterator", 8u) != 0
        || method_name == NULL || method_name->len != 3u
        || memcmp(method_name->bytes, "all", 3u) != 0) return 0;

    for (scan = 0u; scan < hir->items.len; ++scan) {
        const CmHirItem *candidate = (const CmHirItem *)cm_vec_at_const(
            &hir->items, scan);
        const CmInternedString *candidate_name;
        if (candidate == NULL || candidate->kind != CM_HIR_ITEM_FUNCTION
            || !cm_hir_def_id_equal(candidate->parent_definition,
                trait_item->definition)) continue;
        candidate_name = cm_interner_get(&hir->strings, candidate->name);
        if (candidate_name != NULL && candidate_name->len == 4u
            && memcmp(candidate_name->bytes, "next", 4u) == 0) {
            next = candidate->definition;
            break;
        }
    }
    if (cm_hir_def_id_is_none(next)) return 0;

    predicate_type = cm_ty_resolve((CmTyArena *)&tyck->arena,
        instance->types[instance->count - 1u]);
    predicate = cm_ty_get((CmTyArena *)&tyck->arena, predicate_type);
    if (predicate == NULL || (predicate->kind != CM_TY_FN_DEF
            && predicate->kind != CM_TY_FN_PTR
            && predicate->kind != CM_TY_CLOSURE)) return 0;

    cm_str_buf_init(&next_symbol);
    cm_umir_c_render_callee_symbol(&next_symbol, hir, tyck, next,
        CM_TY_NONE, instance->self_type, NULL, 0u);
    cm_str_buf_init(&predicate_symbol);
    if (predicate->kind == CM_TY_CLOSURE) {
        closure_instance = cm_umir_c_closure_instance_of(hir, tyck,
            predicate);
        cm_umir_c_render_closure_symbol(&predicate_symbol,
            (CmHirBodyId)predicate->a, (CmUExprId)predicate->b,
            closure_instance);
    }

    cm_str_buf_append(output, "(long long self, long long pred) { long long ");
    cm_str_buf_append_n(output, next_symbol.data, next_symbol.len);
    cm_str_buf_append(output, "(); for (;;) { long long o = ");
    cm_str_buf_append_n(output, next_symbol.data, next_symbol.len);
    cm_str_buf_append(output, "(self); long long item; if (!o || "
        "((long long *)(intptr_t)o)[0] == 0) return 1; item = "
        "((long long *)(intptr_t)o)[1]; if (!");
    if (predicate->kind == CM_TY_CLOSURE) {
        cm_str_buf_append(output, "({ long long ");
        cm_str_buf_append_n(output, predicate_symbol.data,
            predicate_symbol.len);
        cm_str_buf_append(output, "(); ");
        cm_str_buf_append_n(output, predicate_symbol.data,
            predicate_symbol.len);
        cm_str_buf_append(output,
            "((long long *)(intptr_t)pred, item); })");
    } else {
        cm_str_buf_append(output,
            "((long long (*)())(intptr_t)pred)(item)");
    }
    cm_str_buf_append(output, ") return 0; } }");
    cm_str_buf_destroy(&predicate_symbol);
    cm_str_buf_destroy(&next_symbol);
    return 1;
}

/* Searcher::{next_match,next_reject} and their reverse counterparts are
 * declaration-only default methods when core arrives through metadata.  Each
 * is a small filter over the concrete searcher's required next/next_back. */
static int cm_umir_c_render_searcher_step_filter(CmStrBuf *output,
    const CmHirContext *hir, const CmTyckSet *tyck,
    const CmHirItem *method, const CmUMirInstance *instance)
{
    const CmHirItem *trait_item;
    const CmInternedString *trait_name;
    const CmInternedString *method_name;
    const char *step_name = NULL;
    size_t step_name_len = 0u;
    long wanted = -1;
    CmHirDefId step = cm_hir_def_id_none();
    CmStrBuf step_symbol;
    size_t scan;

    if (method == NULL || method->kind != CM_HIR_ITEM_FUNCTION
        || instance == NULL || instance->self_type == CM_TY_NONE) return 0;
    trait_item = cm_umir_c_item_of(hir, method->parent_definition);
    trait_name = trait_item == NULL ? NULL
        : cm_interner_get(&hir->strings, trait_item->name);
    method_name = cm_interner_get(&hir->strings, method->name);
    if (trait_item == NULL || trait_item->kind != CM_HIR_ITEM_TRAIT
        || trait_name == NULL || method_name == NULL) return 0;

    if (trait_name->len == 8u
        && memcmp(trait_name->bytes, "Searcher", 8u) == 0) {
        step_name = "next";
        step_name_len = 4u;
        if (method_name->len == 10u
            && memcmp(method_name->bytes, "next_match", 10u) == 0)
            wanted = 0;
        else if (method_name->len == 11u
            && memcmp(method_name->bytes, "next_reject", 11u) == 0)
            wanted = 1;
    } else if (trait_name->len == 15u
        && memcmp(trait_name->bytes, "ReverseSearcher", 15u) == 0) {
        step_name = "next_back";
        step_name_len = 9u;
        if (method_name->len == 15u
            && memcmp(method_name->bytes, "next_match_back", 15u) == 0)
            wanted = 0;
        else if (method_name->len == 16u
            && memcmp(method_name->bytes, "next_reject_back", 16u) == 0)
            wanted = 1;
    }
    if (wanted < 0 || step_name == NULL) return 0;

    for (scan = 0u; scan < hir->items.len; ++scan) {
        const CmHirItem *candidate = (const CmHirItem *)cm_vec_at_const(
            &hir->items, scan);
        const CmInternedString *candidate_name;
        if (candidate == NULL || candidate->kind != CM_HIR_ITEM_FUNCTION
            || !cm_hir_def_id_equal(candidate->parent_definition,
                trait_item->definition)) continue;
        candidate_name = cm_interner_get(&hir->strings, candidate->name);
        if (candidate_name != NULL && candidate_name->len == step_name_len
            && memcmp(candidate_name->bytes, step_name,
                step_name_len) == 0) {
            step = candidate->definition;
            break;
        }
    }
    if (cm_hir_def_id_is_none(step)) return 0;

    cm_str_buf_init(&step_symbol);
    cm_umir_c_render_callee_symbol(&step_symbol, hir, tyck, step,
        CM_TY_NONE, instance->self_type, NULL, 0u);
    cm_str_buf_append(output, "(long long self) { long long ");
    cm_str_buf_append_n(output, step_symbol.data, step_symbol.len);
    cm_str_buf_append(output, "(); for (;;) { long long s = ");
    cm_str_buf_append_n(output, step_symbol.data, step_symbol.len);
    cm_str_buf_append(output, "(self); long long tag; long long *o; "
        "if (!s) return 0; tag = ((long long *)(intptr_t)s)[0]; if (tag == ");
    cm_umir_c_render_number(output, (unsigned long)wanted);
    cm_str_buf_append(output, ") { long long *pair = (long long *)"
        "calloc(2, 8); pair[0] = ((long long *)(intptr_t)s)[1]; "
        "pair[1] = ((long long *)(intptr_t)s)[2]; o = (long long *)"
        "calloc(2, 8); o[0] = 1; o[1] = (long long)(intptr_t)pair; "
        "return (long long)(intptr_t)o; } if (tag == 2) { o = "
        "(long long *)calloc(2, 8); return (long long)(intptr_t)o; } } }");
    cm_str_buf_destroy(&step_symbol);
    return 1;
}

size_t cm_umir_c_render_program(CmStrBuf *output, const CmHirContext *hir,
    const CmUMirSet *umir, const CmUBodySet *ubodies,
    const CmTyckSet *tyck)
{
    CmUMirProgram program;
    size_t index;
    size_t rendered = 0u;
    size_t stubs = 0u;
    size_t shims = 0u;
    size_t vtables_done = 0u;
    CmStrBuf bodies;
    CmStrBuf vtables;
    CmStrBuf *final_output = output;
    memset(&program, 0, sizeof(program));
    cm_vec_init(&program.instances, sizeof(CmUMirInstance));
    cm_vec_init(&program.vtables, sizeof(CmUMirVtable));
    cm_umir_c_hir = hir;
    program.hir = hir;
    program.umir = umir;
    program.ubodies = ubodies;
    program.tyck = tyck;
    if (getenv("CMRUSTC_UMIR_TRACE") != NULL)
        cm_str_buf_append(output, "#include <stdio.h>\n");
    /* Slots are `long long` and are read back through narrower and
     * pointer types: the unit relies on `-fno-strict-aliasing` (the
     * pragma covers gcc; drivers pass the flag for every host cc). */
    cm_str_buf_append(output, "#pragma GCC optimize(\"no-strict-aliasing\")\n"
        "#include <stdint.h>\nvoid abort(void);\n"
        "void *malloc(unsigned long);\n"
        "void *calloc(unsigned long, unsigned long);\n"
        "void *memmove(void *, const void *, unsigned long);\n"
        "void *memset(void *, int, unsigned long);\n"
        "int memcmp(const void *, const void *, unsigned long);\n"
        "static int cm_umir_simd_compare;\n");
    /* Roots: `#[no_mangle]` exports, and the root crate's `fn main`
     * (the crate compiled last has the highest crate id); everything
     * else is reached through call instances, so a core-linked program
     * emits just what the exports need. */
    cm_umir_c_root_crate = 0u;
    for (index = 0u; index < umir->bodies.len; ++index) {
        const CmUMirBody *body = (const CmUMirBody *)cm_vec_at_const(
            &umir->bodies, index);
        const CmHirBody *hir_body = body == NULL ? NULL
            : cm_hir_get_body(hir, body->source);
        if (hir_body != NULL
            && hir_body->origin.definition.crate_id > cm_umir_c_root_crate)
            cm_umir_c_root_crate = hir_body->origin.definition.crate_id;
    }
    for (index = 0u; index < umir->bodies.len; ++index) {
        const CmUMirBody *body = (const CmUMirBody *)cm_vec_at_const(
            &umir->bodies, index);
        const CmHirBody *hir_body;
        const CmHirItem *owner;
        CmHirGenericParamId parameters[32];
        if (body == NULL || !body->complete
            || body->closure_expr != CM_U_EXPR_NONE) continue;
        hir_body = cm_hir_get_body(hir, body->source);
        if (hir_body == NULL) continue;
        owner = cm_umir_c_item_of(hir, hir_body->origin.definition);
        if (getenv("CMRUSTC_UMIR_DEBUG") != NULL && owner != NULL) {
            const CmInternedString *dbg_name = cm_interner_get(&hir->strings,
                owner->name);
            if (dbg_name != NULL && dbg_name->len == 4u
                && memcmp(dbg_name->bytes, "main", 4u) == 0)
                fprintf(stderr, "UMIR root-main candidate crate=%u root=%u "
                    "kind=%d params=%u parent=%d complete=%d\n",
                    (unsigned)hir_body->origin.definition.crate_id,
                    (unsigned)cm_umir_c_root_crate, (int)owner->kind,
                    owner->kind == CM_HIR_ITEM_FUNCTION
                        ? (unsigned)owner->data.function_item.signature
                            .parameter_count : 99u,
                    !cm_hir_def_id_is_none(owner->parent_definition),
                    body->complete);
        }
        if (owner == NULL || owner->kind != CM_HIR_ITEM_FUNCTION
            || (!cm_umir_c_item_has_attribute(hir, owner, "no_mangle")
                && !cm_umir_c_is_root_main(hir, owner,
                    hir_body->origin.definition)))
            continue;
        if (cm_umir_c_collect_parameters(hir, owner, parameters, 32u)
                != 0u) continue;
        (void)cm_umir_c_instance(&program, hir_body->origin.definition,
            CM_U_EXPR_NONE, NULL, 0u, CM_TY_NONE);
        if (cm_umir_c_is_root_main(hir, owner, hir_body->origin.definition)) {
            cm_umir_c_have_root_main = 1;
            cm_umir_c_root_main_def = hir_body->origin.definition;
        }
    }
    cm_umir_c_lang_start_instance = -1;
    cm_umir_c_lang_start_stubbed = 0;
    if (cm_umir_c_have_root_main) {
        /* std's `#[lang = "start"] fn lang_start<T: Termination>(main:
         * fn() -> T, argc, argv, sigpipe) -> isize` wraps `main` with the
         * runtime's setup (args, panics, exit code); instanced for `()`. */
        size_t scan;
        for (scan = 0u; scan < hir->items.len; ++scan) {
            const CmHirItem *item = (const CmHirItem *)cm_vec_at_const(
                &hir->items, scan);
            CmTyId unit_type;
            if (item == NULL || item->kind != CM_HIR_ITEM_FUNCTION
                || !cm_umir_c_item_has_attribute(hir, item, "lang = \"start\""))
                continue;
            unit_type = tyck->arena.unit;
            cm_umir_c_lang_start_instance = cm_umir_c_instance(&program,
                item->definition, CM_U_EXPR_NONE, &unit_type, 1u,
                CM_TY_NONE);
            break;
        }
    }
    cm_umir_c_active_program = &program;
    cm_str_buf_init(&bodies);
    cm_str_buf_init(&vtables);
    output = &bodies;
    /* Bodies and vtables each register more instances (and vtables), so
     * alternate until both are exhausted. */
    for (;;) {
    for (index = 0u; index < program.instances.len; ++index) {
        CmUMirInstance *instance = (CmUMirInstance *)cm_vec_at(
            &program.instances, index);
        const CmUMirBody *body;
        const CmUBody *ub;
        const CmHirItem *render_item;
        int builtin_drop_in_place;
        if (instance == NULL || instance->rendered) continue;
        instance->rendered = 1;
        body = cm_umir_c_umir_body(umir, hir, instance->definition,
            instance->closure_expr);
        ub = body == NULL ? NULL : cm_ubody_get(ubodies, body->source);
        render_item = cm_umir_c_item_of(hir, instance->definition);
        if (cm_umir_c_render_unknown_escape_backslash(output, hir, tyck,
                render_item, instance)) {
            shims += 1u;
            continue;
        }
        if (cm_umir_c_render_string_slice(output, hir, tyck, render_item,
                instance)) {
            shims += 1u;
            continue;
        }
        if (cm_umir_c_render_str_spec_to_string(output, hir, tyck,
                render_item, instance)) {
            shims += 1u;
            continue;
        }
        if (cm_umir_c_render_parse_format(output, hir, tyck, render_item,
                instance)) {
            shims += 1u;
            continue;
        }
        if (cm_umir_c_render_raw_table_inner_new(output, hir, tyck,
                render_item, instance)) {
            shims += 1u;
            continue;
        }
        if (cm_umir_c_render_packed_simd_override(output, hir, render_item,
                instance)) {
            shims += 1u;
            continue;
        }
        builtin_drop_in_place = render_item != NULL
            && render_item->kind == CM_HIR_ITEM_FUNCTION
            && cm_umir_c_item_has_attribute(hir, render_item,
                "lang = \"drop_in_place\"");
        if (body == NULL || ub == NULL
            || cm_umir_c_forced_stub(hir, instance->definition)
            || builtin_drop_in_place) {
            /* No complete u-MIR body (partial typeck, declaration without
             * a default, unsupported construct): emit a stub so the unit
             * links; the census counts these as the remaining frontier. */
            cm_str_buf_append(output, "long long ");
            cm_umir_c_render_symbol(output, instance->definition);
            if (instance->count != 0u
                || instance->self_type != CM_TY_NONE) {
                cm_str_buf_append(output, "_i");
                cm_umir_c_render_number(output, instance->index);
            }
            {
                /* Name and reason, so the census names the frontier. */
                const CmHirItem *stub_item = render_item;
                const CmInternedString *stub_name = stub_item == NULL
                    ? NULL : cm_interner_get(&hir->strings, stub_item->name);
                const char *reason = builtin_drop_in_place
                    ? "compiler drop glue"
                    : cm_umir_c_forced_stub(hir, instance->definition)
                        ? "forced (CMRUSTC_UMIR_FORCE_STUB)"
                        : "no u-MIR body";
                size_t scan;
                for (scan = 0u; scan < umir->bodies.len; ++scan) {
                    const CmUMirBody *candidate = (const CmUMirBody *)
                        cm_vec_at_const(&umir->bodies, scan);
                    const CmHirBody *candidate_hir;
                    if (candidate == NULL) continue;
                    candidate_hir = cm_hir_get_body(hir, candidate->source);
                    if (candidate_hir != NULL && cm_hir_def_id_equal(
                            candidate_hir->origin.definition,
                            instance->definition)
                        && candidate->closure_expr
                            == instance->closure_expr) {
                        reason = candidate->complete ? "no u-MIR body"
                            : "u-MIR blocked";
                        break;
                    }
                }
                if (stub_item != NULL && stub_item->kind
                        == CM_HIR_ITEM_STATIC
                    && stub_item->data.value_item.body == 0u
                    && stub_item->data.value_item.is_foreign) {
                    /* A foreign static is the host's symbol: `_addr` is
                     * its address, the getter reads the first word. */
                    const char *host;
                    size_t host_len;
                    (void)cm_umir_c_item_link_name(hir, stub_item, &host,
                        &host_len);
                    cm_str_buf_append(output, "_addr(void) { extern char ");
                    cm_str_buf_append_n(output, host, host_len);
                    cm_str_buf_append(output, "[]; return (long long)"
                        "(intptr_t)");
                    cm_str_buf_append_n(output, host, host_len);
                    cm_str_buf_append(output, "; } /* foreign static */\n"
                        "long long ");
                    cm_umir_c_render_symbol(output, instance->definition);
                    cm_str_buf_append(output, "(void) { return *(long long *)"
                        "(intptr_t)");
                    cm_umir_c_render_symbol(output, instance->definition);
                    cm_str_buf_append(output, "_addr(); }\n");
                    shims += 1u;
                    continue;
                }
                if (stub_item != NULL && stub_item->kind
                        == CM_HIR_ITEM_FUNCTION
                    && stub_item->data.function_item.body == 0u
                    && stub_item->data.function_item.is_foreign) {
                    /* An extern-block declaration forwards to the host's
                     * symbol of that name (`__rust_alloc`, `host_add`):
                     * `(p0..) { long long NAME(); return NAME(p0..); }`. */
                    uint32_t params = stub_item->data.function_item
                        .signature.parameter_count;
                    uint32_t fp;
                    const char *host;
                    size_t host_len;
                    int llvm_intrinsic = 0;
                    size_t scan_host;
                    (void)cm_umir_c_item_link_name(hir, stub_item, &host,
                        &host_len);
                    for (scan_host = 0u; scan_host < host_len; ++scan_host)
                        if (host[scan_host] == '.') llvm_intrinsic = 1;
                    cm_str_buf_push(output, '(');
                    for (fp = 0u; fp < params; ++fp) {
                        if (fp != 0u) cm_str_buf_append(output, ", ");
                        cm_str_buf_append(output, "long long p");
                        cm_umir_c_render_number(output, (unsigned long)fp);
                    }
                    if (params == 0u) cm_str_buf_append(output, "void");
                    if (llvm_intrinsic) {
                        /* `#[link_name = "llvm.x86.sse2.pause"]` (core_arch's
                         * `_mm_pause`): an LLVM intrinsic has no C symbol;
                         * a hint intrinsic is a no-op. */
                        cm_str_buf_append(output,
                            ") { return 0; } /* llvm intrinsic */\n");
                        shims += 1u;
                        continue;
                    }
                    if (cm_umir_c_render_rust_allocator(output, host,
                            host_len, params)) {
                        shims += 1u;
                        continue;
                    }
                    if (cm_umir_c_render_prelude_foreign(output, host,
                            host_len, params)) {
                        shims += 1u;
                        continue;
                    }
                    {
                        const CmHirType *ret = cm_hir_get_type(hir, stub_item
                            ->data.function_item.signature.return_type);
                        int never = ret != NULL
                            && ret->kind == CM_HIR_TYPE_NEVER_KIND;
                        /* `fn exit(code) -> !` binds a `void` host symbol:
                         * its value cannot be cast, so the wrapper calls it
                         * and then traps. */
                        cm_str_buf_append(output, never ? ") { void "
                            : ") { long long ");
                        cm_str_buf_append_n(output, host, host_len);
                        cm_str_buf_append(output, never ? "(); "
                            : "(); return (long long)");
                        cm_str_buf_append_n(output, host, host_len);
                        cm_str_buf_push(output, '(');
                        for (fp = 0u; fp < params; ++fp) {
                            if (fp != 0u) cm_str_buf_append(output, ", ");
                            cm_str_buf_push(output, 'p');
                            cm_umir_c_render_number(output, (unsigned long)fp);
                        }
                        cm_str_buf_append(output, never
                            ? "); abort(); return 0; } /* foreign never */\n"
                            : "); } /* foreign */\n");
                    }
                    shims += 1u;
                    continue;
                }
                if (stub_item != NULL && stub_item->kind
                        == CM_HIR_ITEM_FUNCTION
                    && cm_umir_c_render_hashmap_reserve(output, hir, tyck,
                        stub_item, stub_name)) {
                    cm_str_buf_append(output,
                        " /* shim: unavailable HashMap::reserve */\n");
                    shims += 1u;
                    continue;
                }
                if (stub_item != NULL && stub_item->kind
                        == CM_HIR_ITEM_FUNCTION
                    && cm_umir_c_render_random_state_new(output, hir, tyck,
                        stub_item, stub_name)) {
                    cm_str_buf_append(output,
                        " /* shim: unavailable RandomState::new */\n");
                    shims += 1u;
                    continue;
                }
                if (cm_umir_c_render_aligned_empty_tags(output, stub_item,
                        stub_name)) {
                    cm_str_buf_append(output,
                        " /* shim: unavailable aligned empty tags */\n");
                    shims += 1u;
                    continue;
                }
                if (stub_item != NULL && stub_item->kind
                        == CM_HIR_ITEM_FUNCTION
                    && (stub_item->data.function_item.body == 0u
                        || builtin_drop_in_place)) {
                    const char *shim = cm_umir_c_intrinsic_shim(stub_name);
                    reason = "declaration without body";
                    if (shim == NULL && cm_umir_c_render_typed_shim(output,
                            hir, tyck, stub_name, instance))
                        shim = "";
                    if (shim != NULL) {
                        cm_str_buf_append(output, shim);
                        cm_str_buf_append(output, " /* shim: ");
                        cm_str_buf_append_n(output,
                            (const char *)stub_name->bytes, stub_name->len);
                        cm_str_buf_append(output, " */\n");
                        shims += 1u;
                        continue;
                    }
                }
                if (stub_item != NULL && stub_item->kind
                        == CM_HIR_ITEM_FUNCTION) {
                    CmUMirInstance active_copy = *instance;
                    cm_umir_c_active_instance = &active_copy;
                    if (cm_umir_c_render_iterator_find(output, hir, tyck,
                            stub_item, &active_copy)
                        || cm_umir_c_render_iterator_all(output, hir, tyck,
                            stub_item, &active_copy)
                        || cm_umir_c_render_searcher_step_filter(output, hir,
                            tyck, stub_item, &active_copy)) {
                        cm_umir_c_active_instance = NULL;
                        cm_str_buf_append(output,
                            " /* shim: trait default */\n");
                        shims += 1u;
                        continue;
                    }
                    cm_umir_c_active_instance = NULL;
                }
                if (stub_item != NULL && stub_item->kind == CM_HIR_ITEM_CONST
                    && stub_name != NULL
                    && ((stub_name->len == 8u
                            && memcmp(stub_name->bytes, "MERGE_BY", 8u) == 0)
                        || (stub_name->len == 9u
                            && memcmp(stub_name->bytes, "EXPAND_BY", 9u)
                                == 0))) {
                    /* InPlaceIterable's associated constants are optional
                     * optimization hints.  A declaration-only instance means
                     * the concrete value was not materialized; `None` safely
                     * selects Vec's ordinary collection path.  It must still
                     * use the enum aggregate representation, not a null word. */
                    cm_str_buf_append(output, "(void) { long long *b = "
                        "(long long *)calloc(2, 8); return (long long)"
                        "(intptr_t)b; } /* shim: unavailable in-place hint */\n");
                    shims += 1u;
                    continue;
                }
                {
                    /* A `-> !` stub (core's do_panic) must not return
                     * into its caller as if the panic had not happened. */
                    const CmHirType *ret = stub_item != NULL
                            && stub_item->kind == CM_HIR_ITEM_FUNCTION
                        ? cm_hir_get_type(hir, stub_item->data.function_item
                            .signature.return_type) : NULL;
                    if (ret != NULL && ret->kind == CM_HIR_TYPE_NEVER_KIND)
                        cm_str_buf_append(output, "() { abort(); return 0;"
                            " /* stub (never): ");
                    else
                        cm_str_buf_append(output, "() { return 0; /* stub: ");
                }
                cm_str_buf_append(output, reason);
                cm_str_buf_append(output, ": ");
                if (stub_name != NULL)
                    cm_str_buf_append_n(output,
                        (const char *)stub_name->bytes, stub_name->len);
                cm_str_buf_append(output, " */ }\n");
                if (cm_umir_c_lang_start_instance >= 0
                    && (long)index == cm_umir_c_lang_start_instance)
                    cm_umir_c_lang_start_stubbed = 1;
                if (getenv("CMRUSTC_UMIR_DEBUG") != NULL)
                    fprintf(stderr, "UMIR stub %.*s reason=%s def=%u:%u\n",
                        stub_name == NULL ? 1 : (int)stub_name->len,
                        stub_name == NULL ? "?"
                            : (const char *)stub_name->bytes, reason,
                        (unsigned)instance->definition.crate_id,
                        (unsigned)instance->definition.index);
            }
            stubs += 1u;
            continue;
        }
        {
            /* Render from a copy: registering instances while the body
             * renders reallocates the vec (the copy's types/parameters
             * arrays are separately owned and stay valid). */
            CmUMirInstance active_copy = *instance;
            cm_umir_c_active_instance = &active_copy;
            (void)cm_umir_c_render_body(output, hir, body, ubodies, ub, tyck);
            cm_umir_c_active_instance = NULL;
        }
        /* Rendering may have appended instances; the vec may have moved,
         * so re-fetch by index on the next iteration. */
        rendered += 1u;
    }
    if (vtables_done == program.vtables.len) break;
    while (vtables_done < program.vtables.len) {
        /* `long long (*cm_vt_<n>[])() = { impl methods... }` in the
         * trait's declaration order; each entry registers its instance. */
        const CmUMirVtable *vt = (const CmUMirVtable *)cm_vec_at_const(
            &program.vtables, vtables_done);
        CmStrBuf entries;
        CmStrBuf protos;
        size_t scan;
        unsigned long count = 0ul;
        CmHirDefId closure[CM_UMIR_C_MAX_TRAIT_CLOSURE];
        size_t closure_count = vt == NULL ? 0u
            : cm_umir_c_trait_closure(hir, vt->trait_def, closure, 0u);
        size_t closure_index;
        cm_str_buf_init(&entries);
        cm_str_buf_init(&protos);
        for (closure_index = 0u; closure_index < closure_count;
             ++closure_index)
        for (scan = 0u; vt != NULL && scan < hir->items.len; ++scan) {
            const CmHirItem *method = (const CmHirItem *)cm_vec_at_const(
                &hir->items, scan);
            CmStrBuf symbol;
            if (method == NULL || method->kind != CM_HIR_ITEM_FUNCTION
                || !cm_hir_def_id_equal(method->parent_definition,
                    closure[closure_index])) continue;
            cm_str_buf_init(&symbol);
            {
                /* A closure's Fn-family entries are its `_vt` thunk. */
                const CmTy *concrete = cm_ty_get((CmTyArena *)&tyck->arena,
                    cm_ty_resolve((CmTyArena *)&tyck->arena,
                        cm_umir_c_subst(vt->type)));
                if (concrete != NULL && concrete->kind == CM_TY_CLOSURE
                    && cm_umir_c_is_fn_call_method(hir, method)) {
                    long closure_instance = cm_umir_c_closure_instance_of(
                        hir, tyck, concrete);
                    cm_umir_c_render_closure_symbol(&symbol,
                        (CmHirBodyId)concrete->a, (CmUExprId)concrete->b,
                        closure_instance);
                    cm_str_buf_append(&symbol, "_vt");
                    vt = (const CmUMirVtable *)cm_vec_at_const(
                        &program.vtables, vtables_done);
                } else {
                    cm_umir_c_exact_self = 1;
                    cm_umir_c_render_callee_symbol(&symbol, hir, tyck,
                        method->definition, CM_TY_NONE, vt->type, NULL, 0u);
                    cm_umir_c_exact_self = 0;
                }
            }
            cm_str_buf_append(&protos, "long long ");
            cm_str_buf_append_n(&protos, symbol.data, symbol.len);
            cm_str_buf_append(&protos, "();\n");
            if (count != 0ul) cm_str_buf_append(&entries, ", ");
            cm_str_buf_append(&entries, "(long long (*)())");
            cm_str_buf_append_n(&entries, symbol.data, symbol.len);
            cm_str_buf_destroy(&symbol);
            count += 1ul;
            /* The vtable vec may have moved: re-fetch. */
            vt = (const CmUMirVtable *)cm_vec_at_const(&program.vtables,
                vtables_done);
        }
        cm_str_buf_append_n(&vtables, protos.data, protos.len);
        cm_str_buf_append(&vtables, "long long (*cm_vt_");
        cm_umir_c_render_number(&vtables, (unsigned long)vtables_done);
        cm_str_buf_append(&vtables, "[])() = { ");
        if (count == 0ul) cm_str_buf_append(&vtables, "0");
        else cm_str_buf_append_n(&vtables, entries.data, entries.len);
        cm_str_buf_append(&vtables, " };\n");
        cm_str_buf_destroy(&entries);
        cm_str_buf_destroy(&protos);
        vtables_done += 1u;
    }
    }
    output = final_output;
    for (index = 0u; index < program.vtables.len; ++index) {
        cm_str_buf_append(output, "extern long long (*cm_vt_");
        cm_umir_c_render_number(output, (unsigned long)index);
        cm_str_buf_append(output, "[])();\n");
    }
    cm_str_buf_append_n(output, bodies.data, bodies.len);
    cm_str_buf_append_n(output, vtables.data, vtables.len);
    cm_str_buf_destroy(&bodies);
    cm_str_buf_destroy(&vtables);
    if (cm_umir_c_have_root_main) {
        /* The C entry: std's lang_start around `main` when the program
         * has std, else `main` itself. */
        if (cm_umir_c_lang_start_instance >= 0
            && !cm_umir_c_lang_start_stubbed) {
            const CmUMirInstance *start = (const CmUMirInstance *)
                cm_vec_at_const(&program.instances,
                    (size_t)cm_umir_c_lang_start_instance);
            cm_str_buf_append(output, "int main(int argc, char **argv)\n{\n"
                "    long long ");
            cm_umir_c_render_symbol(output, start->definition);
            cm_str_buf_append(output, "_i");
            cm_umir_c_render_number(output, (unsigned long)start->index);
            cm_str_buf_append(output, "();\n    long long ");
            cm_umir_c_render_symbol(output, cm_umir_c_root_main_def);
            cm_str_buf_append(output, "();\n    return (int)");
            cm_umir_c_render_symbol(output, start->definition);
            cm_str_buf_append(output, "_i");
            cm_umir_c_render_number(output, (unsigned long)start->index);
            cm_str_buf_append(output, "((long long)(intptr_t)&");
            cm_umir_c_render_symbol(output, cm_umir_c_root_main_def);
            cm_str_buf_append(output, ", (long long)argc, "
                "(long long)(intptr_t)argv, 0);\n}\n");
        } else {
            cm_str_buf_append(output, "int main(void)\n{\n    long long ");
            cm_umir_c_render_symbol(output, cm_umir_c_root_main_def);
            cm_str_buf_append(output, "();\n    ");
            cm_umir_c_render_symbol(output, cm_umir_c_root_main_def);
            cm_str_buf_append(output, "();\n    return 0;\n}\n");
        }
        cm_umir_c_have_root_main = 0;
    }
    cm_umir_c_active_program = NULL;
    cm_str_buf_append(output, "/* cm_umir instances=");
    cm_umir_c_render_number(output, (unsigned long)program.instances.len);
    cm_str_buf_append(output, " rendered=");
    cm_umir_c_render_number(output, (unsigned long)rendered);
    cm_str_buf_append(output, " shims=");
    cm_umir_c_render_number(output, (unsigned long)shims);
    cm_str_buf_append(output, " stubs=");
    cm_umir_c_render_number(output, (unsigned long)stubs);
    cm_str_buf_append(output, " vtables=");
    cm_umir_c_render_number(output, (unsigned long)program.vtables.len);
    cm_str_buf_append(output, " */\n");
    cm_vec_destroy(&program.vtables);
    for (index = 0u; index < program.instances.len; ++index) {
        CmUMirInstance *instance = (CmUMirInstance *)cm_vec_at(
            &program.instances, index);
        if (instance != NULL) {
            cm_free(instance->types);
            cm_free(instance->parameters);
        }
    }
    cm_vec_destroy(&program.instances);
    return rendered;
}
