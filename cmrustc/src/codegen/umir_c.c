#include "cm/codegen/umir_c.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "cm/hir/model.h"
#include "cm/alloc.h"

static CmTyId cm_umir_c_subst(CmTyId type);
static void cm_umir_c_render_callee_symbol(CmStrBuf *output,
    const CmHirContext *hir, const CmTyckSet *tyck, CmHirDefId def,
    CmTyId callee_type, CmTyId receiver_type);

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
    CmHirBodyId body, CmUExprId expr)
{
    cm_str_buf_append(output, "cm_u_closure_");
    cm_umir_c_render_number(output, (unsigned long)body);
    cm_str_buf_push(output, '_');
    cm_umir_c_render_number(output, (unsigned long)expr);
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
        receiver_type);
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

typedef struct CmUMirProgram {
    CmVec instances;        /* CmUMirInstance */
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

/* Body-less intrinsics with a lenient C rendering: the frame is uniform
 * `long long` slots (aggregates travel as slot pointers), so bit-casts and
 * hints are identity and the divergent ones abort.  NULL when unknown. */
static const char *cm_umir_c_intrinsic_shim(const CmInternedString *name)
{
    static const struct { const char *name; const char *body; } table[] = {
        { "transmute", "(long long a) { return a; }" },
        { "transmute_unchecked", "(long long a) { return a; }" },
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
    if (a->kind == CM_TY_PARAM) {
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
static CmHirDefId cm_umir_c_resolve_impl_method(const CmHirContext *hir,
    const CmTyckSet *tyck, const CmHirItem *declaration, CmTyId self,
    CmTyId *out_types, uint32_t *out_count)
{
    const CmHirItem *trait_item;
    size_t index;
    CmTyId self_resolved = cm_ty_resolve((CmTyArena *)&tyck->arena, self);
    if (out_count != NULL) *out_count = 0u;
    if (declaration == NULL
        || cm_hir_def_id_is_none(declaration->parent_definition))
        return cm_hir_def_id_none();
    trait_item = cm_umir_c_item_of(hir, declaration->parent_definition);
    if (trait_item == NULL || trait_item->kind != CM_HIR_ITEM_TRAIT)
        return cm_hir_def_id_none();
    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *impl = (const CmHirItem *)cm_vec_at_const(
            &hir->items, index);
        CmTyId impl_self;
        size_t child;
        if (impl == NULL || impl->kind != CM_HIR_ITEM_IMPL
            || !cm_hir_def_id_equal(
                impl->data.impl_item.trait_type.definition,
                trait_item->definition)) continue;
        CmHirGenericParamId bound_params[32];
        CmTyId bound_types[32];
        uint32_t bound = 0u;
        impl_self = cm_ty_from_hir((CmTyArena *)&tyck->arena, hir,
            impl->data.impl_item.self_type);
        if (!cm_umir_c_ty_match(tyck, impl_self, self_resolved,
                bound_params, bound_types, &bound, 32u, 0u))
            continue;
        for (child = 0u; child < hir->items.len; ++child) {
            const CmHirItem *method = (const CmHirItem *)cm_vec_at_const(
                &hir->items, child);
            if (method != NULL && method->kind == CM_HIR_ITEM_FUNCTION
                && cm_hir_def_id_equal(method->parent_definition,
                    impl->definition)
                && method->name == declaration->name) {
                if (out_types != NULL && out_count != NULL) {
                    /* Positional over the impl's parameters; an unbound
                     * one keeps itself (identity substitution). */
                    uint32_t param;
                    uint32_t limit = impl->generic_parameter_count > 32u
                        ? 32u : impl->generic_parameter_count;
                    for (param = 0u; param < limit; ++param) {
                        CmHirGenericParamId id =
                            impl->generic_parameter_start + param;
                        uint32_t scan;
                        out_types[param] = cm_ty_param(
                            (CmTyArena *)&tyck->arena, id);
                        for (scan = 0u; scan < bound; ++scan)
                            if (bound_params[scan] == id)
                                out_types[param] = bound_types[scan];
                    }
                    *out_count = limit;
                }
                return method->definition;
            }
        }
    }
    return cm_hir_def_id_none();
}

static CmUMirProgram *cm_umir_c_active_program = NULL;
static const CmUMirInstance *cm_umir_c_active_instance = NULL;

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
    CmTyId callee_type, CmTyId receiver_type)
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
        if (parent != NULL && parent->kind == CM_HIR_ITEM_TRAIT) {
            /* Declaration: resolve against the substituted receiver; a
             * trait default method stays itself with Self bound. */
            CmTyId self = cm_umir_c_subst(receiver_type);
            CmHirDefId resolved;
            CmTyId bound_types[32];
            uint32_t bound_count = 0u;
            CmHirReceiverKind receiver =
                item->data.function_item.signature.receiver;
            {
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
            }
            resolved = cm_umir_c_resolve_impl_method(hir, tyck, item,
                self, bound_types, &bound_count);
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
                        item, self, bound_types, &bound_count);
                }
            }
            if (getenv("CMRUSTC_UMIR_DEBUG") != NULL) {
                CmStrBuf text;
                cm_str_buf_init(&text);
                cm_ty_print((CmTyArena *)&tyck->arena, hir, receiver_type,
                    &text);
                cm_str_buf_append(&text, " => ");
                cm_ty_print((CmTyArena *)&tyck->arena, hir, self, &text);
                fprintf(stderr, "UMIR decl-call def=%u:%u receiver %.*s"
                    " resolved=%d\n", (unsigned)def.crate_id,
                    (unsigned)def.index, (int)text.len, text.data,
                    !cm_hir_def_id_is_none(resolved));
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
    if (fn_ty != NULL && fn_ty->kind == CM_TY_FN_DEF)
        for (index = 0u; index < fn_ty->count && count < 32u; ++index)
            args[count++] = cm_umir_c_subst(fn_ty->children[index]);
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
    cm_str_buf_append(output, "long long ");
    if (body->closure_expr != CM_U_EXPR_NONE) {
        const CmUExpr *closure = cm_ubody_get_expr(ub, body->closure_expr);
        uint32_t closure_params = closure == NULL ? 0u
            : closure->data.closure.parameter_count;
        cm_umir_c_render_closure_symbol(output, body->source,
            body->closure_expr);
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
            if (pat == NULL || pat->kind != CM_U_PAT_BINDING
                || pat->data.binding.local == CM_U_LOCAL_NONE) continue;
            cm_str_buf_append(output, "    ");
            cm_umir_c_render_local(output,
                (CmUMirLocalId)(1u + pat->data.binding.local));
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
        cm_str_buf_push(output, '(');
        if (ub->parameter_count == 0u) cm_str_buf_append(output, "void");
        for (param = 0u; param < ub->parameter_count; ++param) {
            if (param != 0u) cm_str_buf_append(output, ", ");
            cm_str_buf_append(output, "long long p");
            cm_umir_c_render_number(output, (unsigned long)param);
        }
        cm_str_buf_append(output, ")\n{\n    long long _l[");
        cm_umir_c_render_number(output, (unsigned long)
            (body->locals.len + 1u));
        cm_str_buf_append(output, "] = {0};\n");
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
                    if (expr->data.unary.op == CM_U_UNARY_DEREF) {
                        /* Load through the reference. */
                        cm_str_buf_append(output,
                            "*(long long *)(intptr_t)");
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
                cm_umir_c_render_base(output, statement->operands[0],
                    cm_umir_c_ref_depth(tyck, cm_umir_c_local_type(body,
                        statement->operands[0])));
                cm_str_buf_push(output, '[');
                cm_umir_c_render_number(output,
                    (unsigned long)statement->immediate);
                cm_str_buf_push(output, ']');
                break;
            case CM_UMIR_RVALUE_REF:
                /* A reference is the address of the referent's slot. */
                cm_str_buf_append(output, "(long long)(intptr_t)&");
                cm_umir_c_render_local(output, statement->operands[0]);
                break;
            case CM_UMIR_RVALUE_CAST:
                cm_str_buf_append(output, "(long long)(");
                cm_str_buf_append(output, cm_umir_c_abi_type(&tyck->arena,
                    statement->type));
                cm_str_buf_push(output, ')');
                cm_umir_c_render_local(output, statement->operands[0]);
                break;
            case CM_UMIR_RVALUE_STORE_FIELD: {
                long slot = -1;
                if (expr != NULL && expr->kind == CM_U_EXPR_TUPLE_FIELD)
                    slot = (long)expr->data.tuple_field.index;
                else if (expr != NULL && expr->kind == CM_U_EXPR_FIELD)
                    slot = cm_umir_c_field_index(hir, tyck, ubodies,
                        cm_umir_c_local_type(body, statement->operands[0]),
                        expr->data.field.name);
                if (slot >= 0) {
                    cm_str_buf_append(output, "0; ");
                    cm_umir_c_render_base(output, statement->operands[0],
                        cm_umir_c_ref_depth(tyck, cm_umir_c_local_type(
                            body, statement->operands[0])));
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
            case CM_UMIR_RVALUE_STORE_INDEX:
                cm_str_buf_append(output, "0; ");
                cm_umir_c_render_base(output, statement->operands[0],
                    cm_umir_c_ref_depth(tyck, cm_umir_c_local_type(body,
                        statement->operands[0])));
                cm_str_buf_push(output, '[');
                cm_umir_c_render_local(output, statement->operands[1]);
                cm_str_buf_append(output, "] = ");
                cm_umir_c_render_local(output, statement->operands[2]);
                break;
            case CM_UMIR_RVALUE_LOAD:
                cm_str_buf_append(output, "*(long long *)(intptr_t)");
                cm_umir_c_render_local(output, statement->operands[0]);
                break;
            case CM_UMIR_RVALUE_STORE_DEREF:
                cm_str_buf_append(output, "0; *(long long *)(intptr_t)");
                cm_umir_c_render_local(output, statement->operands[0]);
                cm_str_buf_append(output, " = ");
                cm_umir_c_render_local(output, statement->operands[1]);
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
                    cm_umir_c_render_base(output, statement->operands[0],
                        cm_umir_c_ref_depth(tyck, cm_umir_c_local_type(
                            body, statement->operands[0])));
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
                cm_umir_c_render_base(output, statement->operands[0],
                    cm_umir_c_ref_depth(tyck, cm_umir_c_local_type(body,
                        statement->operands[0])));
                cm_str_buf_push(output, '[');
                cm_umir_c_render_local(output, statement->operands[1]);
                cm_str_buf_push(output, ']');
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
                        if (cm_umir_c_active_program != NULL) {
                            const CmHirBody *closure_body =
                                cm_hir_get_body(hir,
                                    (CmHirBodyId)callee_ty0->a);
                            if (closure_body != NULL)
                                (void)cm_umir_c_instance(
                                    cm_umir_c_active_program,
                                    closure_body->origin.definition,
                                    (CmUExprId)callee_ty0->b, NULL, 0u,
                                    CM_TY_NONE);
                        }
                        cm_str_buf_append(output, "0; { long long ");
                        cm_umir_c_render_closure_symbol(output,
                            (CmHirBodyId)callee_ty0->a,
                            (CmUExprId)callee_ty0->b);
                        cm_str_buf_append(output, "(); ");
                        cm_umir_c_render_local(output,
                            statement->destination);
                        cm_str_buf_append(output, " = ");
                        cm_umir_c_render_closure_symbol(output,
                            (CmHirBodyId)callee_ty0->a,
                            (CmUExprId)callee_ty0->b);
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
                if (!cm_umir_c_render_call(output, hir, tyck, statement,
                        callee_def, 1u,
                        expr != NULL && expr->kind == CM_U_EXPR_CALL
                            && tb != NULL && tb->expr_types != NULL
                            ? tb->expr_types[expr->data.call.callee]
                            : CM_TY_NONE,
                        CM_TY_NONE)) {
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
            cm_str_buf_append(output, "switch ((int)((long long *)(intptr_t)");
            cm_umir_c_render_local(output, block->condition);
            cm_str_buf_append(output, ")[0]) {");
            for (arm = 0u; arm < block->arm_count; ++arm) {
                uint32_t earlier;
                int duplicate = 0;
                if (block->arm_discriminants[arm] < 0) {
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
    memset(&program, 0, sizeof(program));
    cm_vec_init(&program.instances, sizeof(CmUMirInstance));
    program.hir = hir;
    program.umir = umir;
    program.ubodies = ubodies;
    program.tyck = tyck;
    cm_str_buf_append(output, "#include <stdint.h>\nvoid abort(void);\n");
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
                    && stub_item->data.function_item.body == 0u) {
                    const char *shim = cm_umir_c_intrinsic_shim(stub_name);
                    reason = "declaration without body";
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
        cm_umir_c_active_instance = instance;
        (void)cm_umir_c_render_body(output, hir, body, ubodies, ub, tyck);
        cm_umir_c_active_instance = NULL;
        /* Rendering may have appended instances; the vec may have moved,
         * so re-fetch by index on the next iteration. */
        rendered += 1u;
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
    cm_str_buf_append(output, " */\n");
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
