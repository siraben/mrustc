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
                    case CM_UMIR_RVALUE_REF_FIELD:
                    case CM_UMIR_RVALUE_INDEX:
                    case CM_UMIR_RVALUE_REF_INDEX:
                    case CM_UMIR_RVALUE_RANGE_TEST:
                    case CM_UMIR_RVALUE_AGGREGATE:
                        /* Renders through the layout engine's member
                         * names. */
                        break;
                    case CM_UMIR_RVALUE_TRY_UNWRAP:
                    case CM_UMIR_RVALUE_ITER_NEXT:
                    case CM_UMIR_RVALUE_VARIANT:
                    case CM_UMIR_RVALUE_SLOT:
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
static int cm_umir_c_render_call(CmStrBuf *output,
    const CmHirContext *hir, const CmTyckSet *tyck,
    const CmUMirStatement *statement, CmHirDefId def, uint32_t first_arg,
    CmTyId callee_type, CmTyId receiver_type)
{
    uint32_t index;
    CmStrBuf symbol;
    if (cm_hir_def_id_is_none(def) || statement->operand_overflow != 0u)
        return 0;
    cm_str_buf_init(&symbol);
    cm_umir_c_render_callee_symbol(&symbol, hir, tyck, def, callee_type,
        receiver_type, statement, first_arg);
    cm_str_buf_append(output, "0; { long long ");
    cm_str_buf_append_n(output, symbol.data, symbol.len);
    cm_str_buf_append(output, "(); ");
    cm_umir_c_render_local(output, statement->destination);
    cm_str_buf_append(output, " = ");
    cm_str_buf_append_n(output, symbol.data, symbol.len);
    cm_str_buf_destroy(&symbol);
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
        CmTyId field_type = cm_ty_from_hir((CmTyArena *)&tyck->arena, hir,
            item->data.aggregate_item.fields[index].type);
        if (cm_umir_c_is_zst(hir, tyck, field_type)) continue;
        if (representative >= 0) return -1;
        representative = (long)index;
    }
    return representative;
}


/* The representation type behind transparent wrappers. */
static CmTyId cm_umir_c_representation(const CmHirContext *hir,
    const CmTyckSet *tyck, CmTyId type)
{
    unsigned int guard = 0u;
    while (guard++ < 8u) {
        long field = cm_umir_c_transparent_field(hir, tyck, type);
        const CmTy *ty;
        const CmHirDefinition *record;
        const CmHirItem *item;
        if (field < 0) return type;
        ty = cm_ty_get((CmTyArena *)&tyck->arena,
            cm_ty_resolve((CmTyArena *)&tyck->arena, type));
        record = cm_hir_lookup_definition(hir, ty->def);
        item = cm_hir_get_item(hir, record->entity.item_id);
        {
            CmTySubst subst;
            CmHirGenericParamId params[32];
            uint32_t count = item->generic_parameter_count > 32u ? 32u
                : item->generic_parameter_count;
            uint32_t index;
            CmTyId raw = cm_ty_from_hir((CmTyArena *)&tyck->arena, hir,
                item->data.aggregate_item.fields[field].type);
            for (index = 0u; index < count; ++index)
                params[index] = item->generic_parameter_start + index;
            subst.parameters = params;
            subst.types = ty->children;
            subst.count = count < ty->count ? count : ty->count;
            subst.self_type = CM_TY_NONE;
            type = cm_ty_subst((CmTyArena *)&tyck->arena, raw, &subst);
        }
    }
    return type;
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

/* Unsized pointees travel as a `[data, len]` slot pair. */
static int cm_umir_c_is_fat(const CmTyckSet *tyck, CmTyId pointee)
{
    const CmTy *ty = pointee == CM_TY_NONE ? NULL
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
    if (name == NULL) return 0;
#define CM_SHIM_IS(text) (name->len == sizeof(text) - 1u \
        && memcmp(name->bytes, text, name->len) == 0)
    if (CM_SHIM_IS("transmute") || CM_SHIM_IS("transmute_unchecked")) {
        /* Niche layouts: core transmutes an integer/pointer straight to
         * `Option<NonZero<T>>` / `Option<&T>` (0 = None) and back; our
         * Option is a block, so build or unwrap one.  Otherwise the bit
         * pattern is the value itself. */
        CmTyId to = instance->count >= 2u ? instance->types[1] : CM_TY_NONE;
        int from_option = cm_umir_c_is_option(hir, tyck, first);
        int to_option = cm_umir_c_is_option(hir, tyck, to);
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
    if (CM_SHIM_IS("ptr_metadata")) {
        /* `*const [T]` / `*const str`: the pair's length; thin: unit. */
        if (cm_umir_c_is_fat(tyck, first))
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

/* Body-less intrinsics with a lenient C rendering: the frame is uniform
 * `long long` slots (aggregates travel as slot pointers), so bit-casts and
 * hints are identity and the divergent ones abort.  NULL when unknown. */
static const char *cm_umir_c_intrinsic_shim(const CmInternedString *name)
{
    static const struct { const char *name; const char *body; } table[] = {
        { "const_eval_select", "() { return 0; }" },
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
        { "caller_location", "(void) { return 0; }" },
        { "abort", "(void) { abort(); return 0; }" },
        { "unreachable", "(void) { abort(); return 0; }" },
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

/* Resolve a trait-method declaration to the impl method for `self`. */
static const CmUMirBody *cm_umir_c_active_body;
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
            /* Only a receiver is auto-referenced: an ordinary argument
             * never gains a layer, so a `&mut T` parameter does not accept
             * a bare `Inner<T>` (that impl is a sibling, not this one —
             * `Unique<T>: From<&mut T>` calls `Self::from(NonNull<T>)`). */
            if (!is_receiver && pattern_layers > actual_layers
                && ap != NULL && ap->kind != CM_TY_PARAM
                && ap->kind != CM_TY_INFER && ap->kind != CM_TY_PROJECTION
                && ap->kind != CM_TY_SELF) {
                if (getenv("CMRUSTC_UMIR_DEBUG") != NULL)
                    fprintf(stderr, "UMIR reject param=%u layers %u>%u\n",
                        (unsigned)param, (unsigned)pattern_layers,
                        (unsigned)actual_layers);
                return 0;
            }
            if (pp != NULL && ap != NULL && pp->kind == ap->kind)
                (void)cm_umir_c_ty_match(tyck, pp_id, ap_id, bound_params,
                    bound_types, bound, 32u, 0u);
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

static CmHirDefId cm_umir_c_resolve_impl_method(const CmHirContext *hir,
    const CmTyckSet *tyck, const CmHirItem *declaration, CmTyId self,
    CmTyId *out_types, uint32_t *out_count,
    const CmUMirStatement *statement, uint32_t first_arg)
{
    const CmHirItem *trait_item;
    size_t index;
    int blanket_pass;
    CmTyId self_resolved = cm_ty_resolve((CmTyArena *)&tyck->arena, self);
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
        if (!cm_umir_c_ty_match(tyck, impl_self, self_resolved,
                bound_params, bound_types, &bound, 32u, 0u)) {
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
        {
            CmHirDefId found = cm_umir_c_impl_method(hir, tyck, impl,
                declaration, statement, first_arg, bound_params,
                bound_types, &bound, out_types, out_count);
            if (!cm_hir_def_id_is_none(found)) return found;
        }
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

/* Position of a trait method among the trait's fn declarations. */
static long cm_umir_c_trait_method_index(const CmHirContext *hir,
    CmHirDefId method)
{
    const CmHirItem *item = cm_umir_c_item_of(hir, method);
    size_t index;
    long slot = 0;
    if (item == NULL || cm_hir_def_id_is_none(item->parent_definition))
        return -1;
    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *cand = (const CmHirItem *)cm_vec_at_const(
            &hir->items, index);
        if (cand == NULL || cand->kind != CM_HIR_ITEM_FUNCTION
            || !cm_hir_def_id_equal(cand->parent_definition,
                item->parent_definition)) continue;
        if (cm_hir_def_id_equal(cand->definition, method)) return slot;
        slot += 1;
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

/* Symbol for a callee reached with FN_DEF type `callee_type`: registers
 * the instance when the program is collecting. */
static void cm_umir_c_render_callee_symbol(CmStrBuf *output,
    const CmHirContext *hir, const CmTyckSet *tyck, CmHirDefId def,
    CmTyId callee_type, CmTyId receiver_type,
    const CmUMirStatement *statement, uint32_t first_arg)
{
    CmUMirProgram *program = cm_umir_c_active_program;
    const CmHirItem *item = cm_umir_c_item_of(hir, def);
    const CmTy *fn_ty;
    CmTyId args[32];
    uint32_t count = 0u;
    uint32_t index;
    long instance = -1;
    CmTyId bound_self = CM_TY_NONE;
    if (program != NULL && item != NULL
        && !cm_hir_def_id_is_none(item->parent_definition)) {
        const CmHirItem *parent = cm_umir_c_item_of(hir,
            item->parent_definition);
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
                fprintf(stderr, "UMIR impl-callee %.*s %.*s\n",
                    (int)cm_interner_get(&hir->strings, item->name)->len,
                    (const char *)cm_interner_get(&hir->strings,
                        item->name)->bytes, (int)text.len, text.data);
                cm_str_buf_destroy(&text);
            }
            fits = self_ty == NULL || self_ty->kind == CM_TY_PARAM
                || self_ty->kind == CM_TY_INFER
                || self_ty->kind == CM_TY_PROJECTION
                || self_ty->kind == CM_TY_SELF
                || cm_umir_c_ty_match(tyck, impl_self, self, probe_params,
                    probe_types, &probe_bound, 32u, 0u);
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
            CmHirDefId resolved;
            int exact_self = cm_umir_c_exact_self;
            if (self == CM_TY_NONE && callee_type != CM_TY_NONE) {
                const CmTy *ct = cm_ty_get((CmTyArena *)&tyck->arena,
                    cm_ty_resolve((CmTyArena *)&tyck->arena,
                        cm_umir_c_subst(callee_type)));
                if (ct != NULL && ct->kind == CM_TY_FN_DEF && ct->count != 0u) {
                    /* A path's Self (`T::method` with `T = &mut Buffer`)
                     * is exact, like a vtable's concrete type. */
                    self = cm_umir_c_subst(ct->children[0]);
                    exact_self = 1;
                }
            }
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
                        || receiver == CM_HIR_RECEIVER_REF_MUTABLE))
                    self = self_ty->children[0];
                resolved = cm_umir_c_resolve_impl_method(hir, tyck, item,
                    self, bound_types, &bound_count, statement, first_arg);
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
                def = resolved;
                item = cm_umir_c_item_of(hir, def);
                callee_type = CM_TY_NONE;
                for (index = 0u; index < bound_count && count < 32u; ++index)
                    args[count++] = bound_types[index];
            } else {
                bound_self = self;
            }
        } else if (parent != NULL && parent->kind == CM_HIR_ITEM_IMPL
            && parent->generic_parameter_count != 0u) {
            /* Impl method reached directly with no type arguments on the
             * callee: bind the impl's generics by matching its Self
             * against the receiver (`impl<W> Tr for &mut W`). */
            CmTyArena *arena = (CmTyArena *)&tyck->arena;
            const CmTy *have = callee_type == CM_TY_NONE ? NULL
                : cm_ty_get(arena, cm_ty_resolve(arena,
                    cm_umir_c_subst(callee_type)));
            if ((have == NULL || have->kind != CM_TY_FN_DEF
                    || have->count == 0u) && receiver_type != CM_TY_NONE) {
                CmTyId self = cm_umir_c_subst(receiver_type);
                CmTyId impl_self = cm_ty_from_hir(arena, hir,
                    parent->data.impl_item.self_type);
                CmHirGenericParamId bound_params[32];
                CmTyId bound_types[32];
                uint32_t bound = 0u;
                const CmTy *self_ty = cm_ty_get(arena,
                    cm_ty_resolve(arena, self));
                int matched = cm_umir_c_ty_match(tyck, impl_self, self,
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
        /* Receiver-derived Self: strip the reference layers a method
         * receiver carries. */
        const CmTy *self_ty = bound_self == CM_TY_NONE ? NULL
            : cm_ty_get((CmTyArena *)&tyck->arena,
                cm_ty_resolve((CmTyArena *)&tyck->arena, bound_self));
        while (self_ty != NULL && (self_ty->kind == CM_TY_REF
                || self_ty->kind == CM_TY_PTR)) {
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
                    if (pattern == CM_TY_NONE || actual == CM_TY_NONE)
                        continue;
                    (void)cm_umir_c_ty_match(tyck, pattern, actual,
                        bound_params, bound_types, &bound, 32u, 0u);
                }
            if (statement->type != CM_TY_NONE) {
                CmTyId pattern = cm_ty_from_hir(arena, hir, sig->return_type);
                if (pattern != CM_TY_NONE)
                    (void)cm_umir_c_ty_match(tyck, pattern,
                        cm_umir_c_subst(statement->type), bound_params,
                        bound_types, &bound, 32u, 0u);
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
        trait_method ? CM_TY_NONE : fn_type, receiver, NULL, 0u);
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
        cm_umir_c_render_symbol(output, def);
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
            if (statement->kind == CM_UMIR_RVALUE_LITERAL) {
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
            } else if (statement->kind == CM_UMIR_RVALUE_UNSIZE) {
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
            case CM_UMIR_RVALUE_REF:
                /* A reference is the address of the referent's slot. */
                cm_str_buf_append(output, "(long long)(intptr_t)&");
                cm_umir_c_render_local(output, statement->operands[0]);
                break;
            case CM_UMIR_RVALUE_CAST: {
                CmTyId from = cm_umir_c_local_type(body,
                    statement->operands[0]);
                CmTyId to = cm_umir_c_subst(statement->type);
                /* Only a direct `*const str` / `&[T]` is a fat pointer:
                 * `*const &str as *const ()` (Argument::new's
                 * `NonNull::from_ref(x).cast()`) is thin on both sides. */
                if (cm_umir_c_is_fat(tyck, cm_umir_c_peel(tyck, from))
                    && cm_umir_c_ref_depth(tyck, from) == 1u
                    && !cm_umir_c_is_fat(tyck, cm_umir_c_peel(tyck, to))) {
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
                cm_str_buf_append(output, "(long long)(");
                cm_str_buf_append(output, cm_umir_c_abi_type(&tyck->arena,
                    to));
                cm_str_buf_push(output, ')');
                cm_umir_c_render_local(output, statement->operands[0]);
                break;
            }
            case CM_UMIR_RVALUE_STORE_FIELD: {
                long slot = -1;
                if (expr != NULL && expr->kind == CM_U_EXPR_TUPLE_FIELD)
                    slot = (long)expr->data.tuple_field.index;
                else if (expr != NULL && expr->kind == CM_U_EXPR_FIELD)
                    slot = cm_umir_c_field_index(hir, tyck, ubodies,
                        cm_umir_c_local_type(body, statement->operands[0]),
                        expr->data.field.name);
                if (slot >= 0) {
                    CmTyId base_type = cm_umir_c_local_type(body,
                        statement->operands[0]);
                    unsigned int depth = cm_umir_c_ref_depth(tyck, base_type);
                    cm_str_buf_append(output, "0; ");
                    {
                        long representative = cm_umir_c_transparent_field(hir,
                            tyck, cm_umir_c_peel(tyck, base_type));
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
                if (cm_umir_c_is_fat(tyck, pointee)) {
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
                int want_address = statement->kind == CM_UMIR_RVALUE_REF_FIELD;
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
                    CmTyId base_type = cm_umir_c_local_type(body,
                        statement->operands[0]);
                    unsigned int depth = cm_umir_c_ref_depth(tyck, base_type);
                    {
                        long representative = cm_umir_c_transparent_field(hir,
                            tyck, cm_umir_c_peel(tyck, base_type));
                        if (representative >= 0) {
                            /* A zero-sized field reads 0. */
                            if (representative == slot) {
                                if (want_address)
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
                /* REF_INDEX: the element's address instead of its value. */
                CmTyId base_type = cm_umir_c_local_type(body,
                    statement->operands[0]);
                const char *elem = cm_umir_c_array_elem_scalar(tyck,
                    cm_umir_c_peel(tyck, base_type));
                const char *prefix = statement->kind
                    == CM_UMIR_RVALUE_REF_INDEX ? "(long long)(intptr_t)&"
                    : "(long long)";
                if (cm_umir_c_is_fat(tyck, cm_umir_c_peel(tyck, base_type))) {
                    cm_str_buf_append(output, prefix);
                    cm_umir_c_render_slice_element(output, tyck, body,
                        statement->operands[0], statement->operands[1]);
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
                    cm_umir_c_render_local(output, statement->operands[1]);
                    cm_str_buf_push(output, ']');
                    break;
                }
                if (statement->kind == CM_UMIR_RVALUE_REF_INDEX)
                    cm_str_buf_append(output, "(long long)(intptr_t)&");
                cm_umir_c_render_base(output, statement->operands[0],
                    cm_umir_c_ref_depth(tyck, base_type));
                cm_str_buf_push(output, '[');
                cm_umir_c_render_local(output, statement->operands[1]);
                cm_str_buf_push(output, ']');
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
                    const CmTy *callee_ty0 = ctb0 == NULL
                            || ctb0->expr_types == NULL || expr == NULL
                            || expr->kind != CM_U_EXPR_CALL ? NULL
                        : cm_ty_get((CmTyArena *)&tyck->arena,
                            cm_ty_resolve((CmTyArena *)&tyck->arena,
                                cm_umir_c_subst(ctb0->expr_types[
                                    expr->data.call.callee])));
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
                        cm_umir_c_render_local(output,
                            statement->operands[0]);
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
            case CM_UMIR_RVALUE_UNSIZE: {
                /* `&T -> &dyn Trait`: slot 1 the reference, slot 2 the
                 * vtable; the destination references the pair. */
                CmTyId target = cm_umir_c_subst(statement->type);
                const CmTy *dt = cm_ty_get((CmTyArena *)&tyck->arena,
                    cm_ty_resolve((CmTyArena *)&tyck->arena,
                        cm_umir_c_peel(tyck, target)));
                CmTyId source = statement->operand_count == 0u ? CM_TY_NONE
                    : cm_umir_c_local_type(body, statement->operands[0]);
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
                if (vt < 0) {
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
                        ? cm_umir_c_trait_method_index(hir, method_def) : -1;
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
                cm_str_buf_append(output,
                    "switch ((int)((long long *)(intptr_t)");
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
    cm_str_buf_append(output, "#include <stdint.h>\nvoid abort(void);\n"
        "void *malloc(unsigned long);\n"
        "void *calloc(unsigned long, unsigned long);\n"
        "void *memmove(void *, const void *, unsigned long);\n"
        "void *memset(void *, int, unsigned long);\n");
    /* Roots: `#[no_mangle]` exports only; everything else is reached
     * through call instances, so a core-linked program emits just what
     * the exports need. */
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
        if (owner == NULL || owner->kind != CM_HIR_ITEM_FUNCTION
            || !cm_umir_c_item_has_attribute(hir, owner, "no_mangle"))
            continue;
        if (cm_umir_c_collect_parameters(hir, owner, parameters, 32u)
                != 0u) continue;
        (void)cm_umir_c_instance(&program, hir_body->origin.definition,
            CM_U_EXPR_NONE, NULL, 0u, CM_TY_NONE);
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
        if (instance == NULL || instance->rendered) continue;
        instance->rendered = 1;
        body = cm_umir_c_umir_body(umir, hir, instance->definition,
            instance->closure_expr);
        ub = body == NULL ? NULL : cm_ubody_get(ubodies, body->source);
        if (body == NULL || ub == NULL) {
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
                const CmHirItem *stub_item = cm_umir_c_item_of(hir,
                    instance->definition);
                const CmInternedString *stub_name = stub_item == NULL
                    ? NULL : cm_interner_get(&hir->strings, stub_item->name);
                const char *reason = "no u-MIR body";
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
                        == CM_HIR_ITEM_FUNCTION
                    && stub_item->data.function_item.body == 0u
                    && stub_item->data.function_item.is_foreign) {
                    /* An extern-block declaration forwards to the host's
                     * symbol of that name (`__rust_alloc`, `host_add`):
                     * `(p0..) { long long NAME(); return NAME(p0..); }`. */
                    uint32_t params = stub_item->data.function_item
                        .signature.parameter_count;
                    uint32_t fp;
                    cm_str_buf_push(output, '(');
                    for (fp = 0u; fp < params; ++fp) {
                        if (fp != 0u) cm_str_buf_append(output, ", ");
                        cm_str_buf_append(output, "long long p");
                        cm_umir_c_render_number(output, (unsigned long)fp);
                    }
                    if (params == 0u) cm_str_buf_append(output, "void");
                    cm_str_buf_append(output, ") { long long ");
                    cm_str_buf_append_n(output,
                        (const char *)stub_name->bytes, stub_name->len);
                    cm_str_buf_append(output, "(); return (long long)");
                    cm_str_buf_append_n(output,
                        (const char *)stub_name->bytes, stub_name->len);
                    cm_str_buf_push(output, '(');
                    for (fp = 0u; fp < params; ++fp) {
                        if (fp != 0u) cm_str_buf_append(output, ", ");
                        cm_str_buf_push(output, 'p');
                        cm_umir_c_render_number(output, (unsigned long)fp);
                    }
                    cm_str_buf_append(output, "); } /* foreign */\n");
                    shims += 1u;
                    continue;
                }
                if (stub_item != NULL && stub_item->kind
                        == CM_HIR_ITEM_FUNCTION
                    && stub_item->data.function_item.body == 0u) {
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
                cm_str_buf_append(output, "() { return 0; /* stub: ");
                cm_str_buf_append(output, reason);
                cm_str_buf_append(output, ": ");
                if (stub_name != NULL)
                    cm_str_buf_append_n(output,
                        (const char *)stub_name->bytes, stub_name->len);
                cm_str_buf_append(output, " */ }\n");
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
        cm_str_buf_init(&entries);
        cm_str_buf_init(&protos);
        for (scan = 0u; vt != NULL && scan < hir->items.len; ++scan) {
            const CmHirItem *method = (const CmHirItem *)cm_vec_at_const(
                &hir->items, scan);
            CmStrBuf symbol;
            if (method == NULL || method->kind != CM_HIR_ITEM_FUNCTION
                || !cm_hir_def_id_equal(method->parent_definition,
                    vt->trait_def)) continue;
            cm_str_buf_init(&symbol);
            cm_umir_c_exact_self = 1;
            cm_umir_c_render_callee_symbol(&symbol, hir, tyck,
                method->definition, CM_TY_NONE, vt->type, NULL, 0u);
            cm_umir_c_exact_self = 0;
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
