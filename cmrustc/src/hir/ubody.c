#include "cm/hir/ubody.h"
#include "cm/alloc.h"
#include "cm/arena.h"
#include <stdlib.h>
#include <string.h>

/*
 * Untyped body lowering (M9-02).  See ubody.h.
 *
 * The lowering is deliberately lenient: it never rejects a body for a
 * semantic reason.  Paths that name resolution cannot decide are retained
 * as segments; types are AST references; nested items are recorded for a
 * later item-lowering slice.  A body fails only when its AST is
 * structurally unusable (missing node, unexpanded macro).
 */

#define CM_U_SCOPE_LIMIT 4096u

typedef struct CmUScopeEntry {
    CmInternId name;
    uint32_t local;
} CmUScopeEntry;

/* Items declared inside bodies, visible throughout their block. */
typedef struct CmUNestedItem {
    CmInternId name;
    CmAstItemId item;
} CmUNestedItem;

#define CM_U_NESTED_LIMIT 512u

typedef struct CmUItemKey {
    CmSourceId source;
    uint32_t start;
    CmHirDefId definition;
} CmUItemKey;

typedef struct CmUSourceAst {
    CmSourceId source;
    const CmAst *ast;
    CmModuleId module;
} CmUSourceAst;

typedef struct CmULowerState {
    CmUBodySet *set;
    const CmHirContext *hir;
    const CmModuleGraph *graph;
    CmModuleGraphRevision revision;
    const CmImportResolver *imports;
    const CmHirModuleMap *modules;
    /* (source, item span start) -> DefId for every HIR item. */
    CmUItemKey *item_keys;
    size_t item_key_count;
    /* source -> AST for every graph module. */
    CmUSourceAst *source_asts;
    size_t source_ast_count;
    /* Per-body state. */
    CmUBody *body;
    const CmAst *ast;
    CmSourceId source;
    CmModuleId module;
    const CmHirItem *item;
    const CmHirItem *parent_item;
    CmUScopeEntry scope[CM_U_SCOPE_LIMIT];
    size_t scope_count;
    CmUNestedItem nested[CM_U_NESTED_LIMIT];
    size_t nested_count;
    const char *failure;
} CmULowerState;

/* ------------------------------------------------------------------ */
/* Set                                                                  */

void cm_ubody_set_init(CmUBodySet *set)
{
    memset(set, 0, sizeof(*set));
    cm_interner_init(&set->strings, 65536u);
    cm_vec_init(&set->bodies, sizeof(CmUBody));
    cm_arena_init(&set->storage, 1u << 20);
}

void cm_ubody_set_destroy(CmUBodySet *set)
{
    size_t index;
    for (index = 0u; index < set->bodies.len; ++index) {
        CmUBody *body = (CmUBody *)cm_vec_at(&set->bodies, index);
        cm_vec_destroy(&body->expressions);
        cm_vec_destroy(&body->patterns);
        cm_vec_destroy(&body->statements);
        cm_vec_destroy(&body->locals);
    }
    cm_vec_destroy(&set->bodies);
    cm_arena_destroy(&set->storage);
    cm_interner_destroy(&set->strings);
}

const CmUBody *cm_ubody_get(const CmUBodySet *set, CmHirBodyId body)
{
    if (body == CM_HIR_BODY_NONE || (size_t)body > set->bodies.len)
        return NULL;
    return (const CmUBody *)cm_vec_at_const(&set->bodies, (size_t)body - 1u);
}

const CmUExpr *cm_ubody_get_expr(const CmUBody *body, CmUExprId id)
{
    if (id == CM_U_EXPR_NONE || (size_t)id > body->expressions.len)
        return NULL;
    return (const CmUExpr *)cm_vec_at_const(&body->expressions,
        (size_t)id - 1u);
}

const CmUPat *cm_ubody_get_pat(const CmUBody *body, CmUPatId id)
{
    if (id == CM_U_PAT_NONE || (size_t)id > body->patterns.len) return NULL;
    return (const CmUPat *)cm_vec_at_const(&body->patterns, (size_t)id - 1u);
}

const CmUStmt *cm_ubody_get_stmt(const CmUBody *body, CmUStmtId id)
{
    if (id == 0u || (size_t)id > body->statements.len) return NULL;
    return (const CmUStmt *)cm_vec_at_const(&body->statements,
        (size_t)id - 1u);
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                        */

static void *cm_u_alloc(CmULowerState *state, size_t count, size_t size)
{
    if (count == 0u) return NULL;
    return cm_arena_alloc_zeroed(&state->set->storage, count, size, 8u);
}

static CmInternId cm_u_intern_ast(CmULowerState *state, CmInternId ast_id)
{
    const CmInternedString *string;
    if (ast_id == CM_INTERN_ID_NONE) return CM_INTERN_ID_NONE;
    string = cm_ast_get_string(state->ast, ast_id);
    if (string == NULL) return CM_INTERN_ID_NONE;
    return cm_interner_intern(&state->set->strings, string->bytes,
        string->len);
}

static CmInternId cm_u_intern_text(CmULowerState *state, const char *text)
{
    return cm_interner_intern_c_str(&state->set->strings, text);
}

static int cm_u_ast_string_is(const CmAst *ast, CmInternId id,
    const char *expected)
{
    const CmInternedString *string = cm_ast_get_string(ast, id);
    size_t length = strlen(expected);
    return string != NULL && string->len == length
        && memcmp(string->bytes, expected, length) == 0;
}

static CmSpan cm_u_span(const CmULowerState *state, CmAstSpan span)
{
    CmSpan result;
    result.source = state->source;
    result.start = span.start;
    result.end = span.end;
    return result;
}

static void cm_u_fail(CmULowerState *state, const char *message)
{
    if (state->failure == NULL) state->failure = message;
}

static CmUExprId cm_u_push_expr(CmULowerState *state, const CmUExpr *expr)
{
    cm_vec_push(&state->body->expressions, expr);
    return (CmUExprId)state->body->expressions.len;
}

static CmUPatId cm_u_push_pat(CmULowerState *state, const CmUPat *pat)
{
    cm_vec_push(&state->body->patterns, pat);
    return (CmUPatId)state->body->patterns.len;
}

static CmUStmtId cm_u_push_stmt(CmULowerState *state, const CmUStmt *stmt)
{
    cm_vec_push(&state->body->statements, stmt);
    return (CmUStmtId)state->body->statements.len;
}

static uint32_t cm_u_add_local(CmULowerState *state, CmInternId name,
    int is_mutable, int by_ref, CmSpan span)
{
    CmULocal local;
    uint32_t index;
    memset(&local, 0, sizeof(local));
    local.name = name;
    local.is_mutable = is_mutable;
    local.by_ref = by_ref;
    local.span = span;
    cm_vec_push(&state->body->locals, &local);
    index = (uint32_t)(state->body->locals.len - 1u);
    if (state->scope_count < CM_U_SCOPE_LIMIT) {
        state->scope[state->scope_count].name = name;
        state->scope[state->scope_count].local = index;
        state->scope_count += 1u;
    }
    return index;
}

static uint32_t cm_u_find_local(const CmULowerState *state, CmInternId name)
{
    size_t index = state->scope_count;
    while (index != 0u) {
        index -= 1u;
        if (state->scope[index].name == name) return state->scope[index].local;
    }
    return CM_U_LOCAL_NONE;
}

static CmUAstTypeRef cm_u_type_ref(const CmULowerState *state,
    CmAstTypeId type)
{
    CmUAstTypeRef ref;
    ref.source = type == CM_AST_TYPE_NONE ? 0u : state->source;
    ref.type = type;
    return ref;
}

static CmUAstPathRef cm_u_path_ref(const CmULowerState *state,
    CmAstPathId path)
{
    CmUAstPathRef ref;
    ref.source = path == CM_AST_PATH_NONE ? 0u : state->source;
    ref.path = path;
    return ref;
}

/* ------------------------------------------------------------------ */
/* Definition lookup                                                    */

static int cm_u_item_key_compare(const void *left, const void *right)
{
    const CmUItemKey *a = (const CmUItemKey *)left;
    const CmUItemKey *b = (const CmUItemKey *)right;
    if (a->source != b->source) return a->source < b->source ? -1 : 1;
    if (a->start != b->start) return a->start < b->start ? -1 : 1;
    return 0;
}

static void cm_u_build_item_keys(CmULowerState *state)
{
    size_t index;
    size_t count = 0u;
    state->item_keys = (CmUItemKey *)cm_alloc_zeroed(
        state->hir->items.len + 1u, sizeof(*state->item_keys));
    for (index = 0u; index < state->hir->items.len; ++index) {
        const CmHirItem *item = (const CmHirItem *)cm_vec_at_const(
            &state->hir->items, index);
        if (item == NULL || item->span.source == 0u) continue;
        state->item_keys[count].source = item->span.source;
        state->item_keys[count].start = item->span.start;
        state->item_keys[count].definition = item->definition;
        count += 1u;
    }
    state->item_key_count = count;
    qsort(state->item_keys, count, sizeof(*state->item_keys),
        cm_u_item_key_compare);
}

static int cm_u_find_item_definition(const CmULowerState *state,
    CmSourceId source, uint32_t start, CmHirDefId *out)
{
    size_t low = 0u;
    size_t high = state->item_key_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        const CmUItemKey *key = &state->item_keys[middle];
        if (key->source == source && key->start == start) {
            *out = key->definition;
            return 1;
        }
        if (key->source < source
            || (key->source == source && key->start < start))
            low = middle + 1u;
        else
            high = middle;
    }
    return 0;
}

static void cm_u_build_source_asts(CmULowerState *state)
{
    size_t count = cm_module_graph_module_count(state->graph);
    size_t index;
    size_t used = 0u;
    state->source_asts = (CmUSourceAst *)cm_alloc_zeroed(count + 1u,
        sizeof(*state->source_asts));
    for (index = 0u; index < count; ++index) {
        CmResolveModuleInfo info;
        const CmAst *ast = NULL;
        size_t seen;
        if (!cm_module_graph_get_module_at(state->graph, index, &info)
            || !cm_module_graph_borrow_ast(state->graph, info.id, &ast)
            || ast == NULL) continue;
        for (seen = 0u; seen < used; ++seen)
            if (state->source_asts[seen].source == info.source) break;
        if (seen != used) continue;
        state->source_asts[used].source = info.source;
        state->source_asts[used].ast = ast;
        state->source_asts[used].module = info.id;
        used += 1u;
    }
    state->source_ast_count = used;
}

static const CmAst *cm_u_ast_for_source(const CmULowerState *state,
    CmSourceId source)
{
    size_t index;
    for (index = 0u; index < state->source_ast_count; ++index)
        if (state->source_asts[index].source == source)
            return state->source_asts[index].ast;
    return NULL;
}

/* Map a resolver binding to a HIR definition. */
static int cm_u_binding_definition(const CmULowerState *state,
    const CmResolvedBinding *binding, CmUResolution *out)
{
    const CmAst *declaring;
    const CmAstItem *item;
    CmHirDefId definition;
    if (binding->variant.enumeration.item != CM_AST_ITEM_NONE) {
        CmHirDefId enum_definition;
        const CmHirDefinition *record;
        size_t index;
        declaring = cm_u_ast_for_source(state,
            binding->variant.enumeration.source);
        item = declaring == NULL ? NULL
            : cm_ast_get_item(declaring, binding->variant.enumeration.item);
        if (item == NULL || !cm_u_find_item_definition(state,
                binding->variant.enumeration.source, item->span.start,
                &enum_definition)) return 0;
        record = cm_hir_lookup_definition(state->hir, enum_definition);
        if (record == NULL || record->kind != CM_HIR_DEFINITION_ITEM) return 0;
        /* Variant definitions are separate records naming the enum item. */
        for (index = 0u; index < state->hir->definitions.len; ++index) {
            const CmHirDefinition *candidate = (const CmHirDefinition *)
                cm_vec_at_const(&state->hir->definitions, index);
            if (candidate != NULL
                && candidate->kind == CM_HIR_DEFINITION_ENUM_VARIANT
                && candidate->entity.enum_variant.enum_item_id
                    == record->entity.item_id
                && candidate->entity.enum_variant.variant_index
                    == binding->variant.index) {
                out->kind = CM_U_RESOLVED_VARIANT;
                out->definition = candidate->id;
                return 1;
            }
        }
        return 0;
    }
    if (binding->primitive_kind != CM_RESOLVE_PRIMITIVE_NONE) {
        out->kind = CM_U_RESOLVED_PRIMITIVE;
        out->primitive = (CmHirPrimitiveKind)binding->primitive_kind;
        return 1;
    }
    if (binding->declaration.item == CM_AST_ITEM_NONE) return 0;
    declaring = cm_u_ast_for_source(state, binding->declaration.source);
    item = declaring == NULL ? NULL
        : cm_ast_get_item(declaring, binding->declaration.item);
    if (item == NULL) return 0;
    if (!cm_u_find_item_definition(state, binding->declaration.source,
            item->span.start, &definition)) return 0;
    out->kind = CM_U_RESOLVED_DEFINITION;
    out->definition = definition;
    return 1;
}

static const char *const cm_u_primitive_names[] = {
    NULL, "bool", "char", "str", "i8", "i16", "i32", "i64", "i128", "isize",
    "u8", "u16", "u32", "u64", "u128", "usize", "f16", "f32", "f64", "f128"
};

static CmHirPrimitiveKind cm_u_primitive_by_name(const CmAst *ast,
    CmInternId name)
{
    size_t index;
    for (index = 1u; index < sizeof(cm_u_primitive_names)
            / sizeof(cm_u_primitive_names[0]); ++index)
        if (cm_u_ast_string_is(ast, name, cm_u_primitive_names[index]))
            return (CmHirPrimitiveKind)index;
    return CM_HIR_PRIMITIVE_NONE;
}

static CmHirGenericParamId cm_u_generic_by_name(const CmULowerState *state,
    const CmHirItem *item, CmInternId ast_name)
{
    const CmInternedString *name;
    uint32_t index;
    if (item == NULL || item->generic_parameter_start
            == CM_HIR_GENERIC_PARAM_NONE) return CM_HIR_GENERIC_PARAM_NONE;
    name = cm_ast_get_string(state->ast, ast_name);
    if (name == NULL) return CM_HIR_GENERIC_PARAM_NONE;
    for (index = 0u; index < item->generic_parameter_count; ++index) {
        CmHirGenericParamId id = item->generic_parameter_start + index;
        const CmHirGenericParam *parameter = cm_hir_get_generic_param(
            state->hir, id);
        const CmInternedString *hir_name;
        if (parameter == NULL) continue;
        hir_name = cm_interner_get(&state->hir->strings, parameter->name);
        if (hir_name != NULL && hir_name->len == name->len
            && memcmp(hir_name->bytes, name->bytes, name->len) == 0)
            return id;
    }
    return CM_HIR_GENERIC_PARAM_NONE;
}

/*
 * Resolve an AST path in value (or type) position.  Fills `out` and returns
 * 1 when at least a prefix resolved; otherwise marks UNRESOLVED and returns
 * 0.  `prefer_type` tries the type namespace first (struct literals and
 * patterns).
 */
static int cm_u_resolve_path(CmULowerState *state, const CmAstPath *path,
    int prefer_type, CmUResolution *out)
{
    CmResolvePathSegmentView views[64];
    uint32_t count = path == NULL ? 0u : path->segment_count;
    uint32_t index;
    CmResolvedBinding binding;
    memset(out, 0, sizeof(*out));
    out->kind = CM_U_RESOLVED_UNRESOLVED;
    out->rest_from = 0u;
    if (path == NULL || count == 0u || count > 64u || path->segments == NULL)
        return 0;
    for (index = 0u; index < count; ++index) {
        const CmInternedString *name = cm_ast_get_string(state->ast,
            path->segments[index].name);
        if (name == NULL) return 0;
        views[index].bytes = name->bytes;
        views[index].length = name->len;
    }
    /* Locals shadow everything for a single unqualified segment. */
    if (count == 1u && !path->absolute) {
        uint32_t local = cm_u_find_local(state,
            cm_u_intern_ast(state, path->segments[0].name));
        if (local != CM_U_LOCAL_NONE) {
            out->kind = CM_U_RESOLVED_LOCAL;
            out->local = local;
            out->rest_from = 1u;
            return 1;
        }
    }
    if (!path->absolute) {
        /* Items declared in an enclosing block. */
        CmInternId name = cm_u_intern_ast(state, path->segments[0].name);
        size_t nested = state->nested_count;
        while (nested != 0u) {
            nested -= 1u;
            if (state->nested[nested].name == name) {
                out->kind = CM_U_RESOLVED_NESTED_ITEM;
                out->nested_source = state->source;
                out->nested_item = state->nested[nested].item;
                out->rest_from = 1u;
                return 1;
            }
        }
    }
    if (!path->absolute) {
        /* `Self`, generic parameters, and primitives as prefixes. */
        CmInternId first = path->segments[0].name;
        CmHirGenericParamId generic;
        CmHirPrimitiveKind primitive;
        if (cm_u_ast_string_is(state->ast, first, "Self")) {
            out->kind = CM_U_RESOLVED_SELF_TYPE;
            out->rest_from = 1u;
            return 1;
        }
        generic = cm_u_generic_by_name(state, state->item, first);
        if (generic == CM_HIR_GENERIC_PARAM_NONE)
            generic = cm_u_generic_by_name(state, state->parent_item, first);
        if (generic != CM_HIR_GENERIC_PARAM_NONE) {
            out->kind = CM_U_RESOLVED_GENERIC_PARAM;
            out->generic_parameter = generic;
            out->rest_from = 1u;
            return 1;
        }
        primitive = cm_u_primitive_by_name(state->ast, first);
        if (primitive != CM_HIR_PRIMITIVE_NONE && count > 1u) {
            out->kind = CM_U_RESOLVED_PRIMITIVE;
            out->primitive = primitive;
            out->rest_from = 1u;
            return 1;
        }
    }
    {
        int first_type = prefer_type;
        int attempt;
        for (attempt = 0; attempt < 2; ++attempt) {
            CmResolveNamespace ns = (attempt == 0) == (first_type != 0)
                ? CM_RESOLVE_NAMESPACE_TYPE : CM_RESOLVE_NAMESPACE_VALUE;
            if (cm_import_resolve_path_checked(state->imports, state->graph,
                    state->revision, state->module, path->absolute, views,
                    count, ns, &binding) == CM_IMPORT_LOOKUP_OK
                && cm_u_binding_definition(state, &binding, out)) {
                out->rest_from = count;
                return 1;
            }
        }
    }
    /* The implicit prelude: `crate::prelude::v1::<path>` (core) or the
     * dependency's prelude for other crates (not modelled yet). */
    if (!path->absolute && count <= 61u) {
        CmResolvePathSegmentView prelude[64];
        int attempt;
        prelude[0].bytes = (const unsigned char *)"crate";
        prelude[0].length = 5u;
        prelude[1].bytes = (const unsigned char *)"prelude";
        prelude[1].length = 7u;
        prelude[2].bytes = (const unsigned char *)"v1";
        prelude[2].length = 2u;
        for (index = 0u; index < count; ++index) prelude[3u + index] = views[index];
        for (attempt = 0; attempt < 2; ++attempt) {
            CmResolveNamespace ns = (attempt == 0) == (prefer_type != 0)
                ? CM_RESOLVE_NAMESPACE_TYPE : CM_RESOLVE_NAMESPACE_VALUE;
            if (cm_import_resolve_path_checked(state->imports, state->graph,
                    state->revision, state->module, 0, prelude, count + 3u,
                    ns, &binding) == CM_IMPORT_LOOKUP_OK
                && cm_u_binding_definition(state, &binding, out)) {
                out->rest_from = count;
                return 1;
            }
        }
        /* Prelude type prefix + associated tail (`Option::Some`, `Some`). */
        for (index = count - 1u; index != 0u; --index) {
            CmUResolution prefix;
            memset(&prefix, 0, sizeof(prefix));
            if (cm_import_resolve_path_checked(state->imports, state->graph,
                    state->revision, state->module, 0, prelude, index + 3u,
                    CM_RESOLVE_NAMESPACE_TYPE, &binding)
                    == CM_IMPORT_LOOKUP_OK
                && cm_u_binding_definition(state, &binding, &prefix)) {
                out->kind = CM_U_RESOLVED_TYPE_ASSOC;
                out->definition = prefix.definition;
                out->rest_from = index;
                return 1;
            }
        }
    }
    /* Longest resolvable type prefix + associated tail. */
    for (index = count - 1u; index != 0u; --index) {
        CmUResolution prefix;
        memset(&prefix, 0, sizeof(prefix));
        if (cm_import_resolve_path_checked(state->imports, state->graph,
                state->revision, state->module, path->absolute, views, index,
                CM_RESOLVE_NAMESPACE_TYPE, &binding) == CM_IMPORT_LOOKUP_OK
            && cm_u_binding_definition(state, &binding, &prefix)) {
            if (prefix.kind == CM_U_RESOLVED_PRIMITIVE) {
                *out = prefix;
            } else {
                out->kind = CM_U_RESOLVED_TYPE_ASSOC;
                out->definition = prefix.definition;
            }
            out->rest_from = index;
            return 1;
        }
    }
    out->kind = CM_U_RESOLVED_UNRESOLVED;
    out->rest_from = 0u;
    return 0;
}

static CmInternId *cm_u_segments(CmULowerState *state, const CmAstPath *path,
    uint32_t *out_count)
{
    CmInternId *segments;
    uint32_t index;
    *out_count = 0u;
    if (path == NULL || path->segment_count == 0u) return NULL;
    segments = (CmInternId *)cm_u_alloc(state, path->segment_count,
        sizeof(*segments));
    for (index = 0u; index < path->segment_count; ++index)
        segments[index] = cm_u_intern_ast(state, path->segments[index].name);
    *out_count = path->segment_count;
    return segments;
}

/* ------------------------------------------------------------------ */
/* Literals                                                             */

static void cm_u_classify_literal(CmULowerState *state, CmInternId text_id,
    CmUExpr *node)
{
    const CmInternedString *string = cm_ast_get_string(state->ast, text_id);
    const char *text;
    size_t length;
    size_t index = 0u;
    node->data.literal.kind = CM_U_LITERAL_INTEGER;
    node->data.literal.text = cm_u_intern_ast(state, text_id);
    node->data.literal.suffix = CM_INTERN_ID_NONE;
    if (string == NULL || string->len == 0u) return;
    text = (const char *)string->bytes;
    length = string->len;
    if (length == 4u && memcmp(text, "true", 4u) == 0) {
        node->data.literal.kind = CM_U_LITERAL_BOOL;
        node->data.literal.value_low = 1u;
        return;
    }
    if (length == 5u && memcmp(text, "false", 5u) == 0) {
        node->data.literal.kind = CM_U_LITERAL_BOOL;
        return;
    }
    if (text[0] == '"' || (text[0] == 'r' && length > 1u
            && (text[1] == '"' || text[1] == '#'))) {
        node->data.literal.kind = CM_U_LITERAL_STRING;
        return;
    }
    if (text[0] == 'b' && length > 1u) {
        if (text[1] == '\'') {
            node->data.literal.kind = CM_U_LITERAL_BYTE;
            return;
        }
        if (text[1] == '"' || text[1] == 'r') {
            node->data.literal.kind = CM_U_LITERAL_BYTE_STRING;
            return;
        }
    }
    if (text[0] == 'c' && length > 1u && (text[1] == '"' || text[1] == 'r')) {
        node->data.literal.kind = CM_U_LITERAL_C_STRING;
        return;
    }
    if (text[0] == '\'') {
        node->data.literal.kind = CM_U_LITERAL_CHAR;
        return;
    }
    /* Numeric: integer unless it has a fractional/exponent part. */
    {
        unsigned int base = 10u;
        uint64_t low = 0u;
        uint64_t high = 0u;
        int is_float = 0;
        size_t suffix_start;
        if (length > 2u && text[0] == '0'
            && (text[1] == 'x' || text[1] == 'o' || text[1] == 'b')) {
            base = text[1] == 'x' ? 16u : text[1] == 'o' ? 8u : 2u;
            index = 2u;
        }
        for (; index < length; ++index) {
            char c = text[index];
            unsigned int digit;
            if (c == '_') continue;
            if (c >= '0' && c <= '9') digit = (unsigned int)(c - '0');
            else if (base == 16u && c >= 'a' && c <= 'f')
                digit = (unsigned int)(c - 'a') + 10u;
            else if (base == 16u && c >= 'A' && c <= 'F')
                digit = (unsigned int)(c - 'A') + 10u;
            else break;
            if (digit >= base) break;
            /* 128-bit multiply-add. */
            {
                uint64_t low_product_high;
                uint64_t new_low;
                uint64_t a = low & 0xFFFFFFFFu;
                uint64_t b = low >> 32;
                uint64_t a_mul = a * base;
                uint64_t b_mul = b * base + (a_mul >> 32);
                new_low = (b_mul << 32) | (a_mul & 0xFFFFFFFFu);
                low_product_high = b_mul >> 32;
                high = high * base + low_product_high;
                if (new_low > UINT64_MAX - digit) high += 1u;
                low = new_low + digit;
            }
        }
        suffix_start = index;
        if (base == 10u && index < length) {
            if (text[index] == '.'
                && !(index + 1u < length && text[index + 1u] == '.')) {
                /* `1.` or `1.5`; a method call after the literal was already
                 * separated by the parser. */
                is_float = 1;
            } else if (text[index] == 'e' || text[index] == 'E') {
                is_float = 1;
            }
        }
        if (is_float) {
            node->data.literal.kind = CM_U_LITERAL_FLOAT;
            while (index < length && (text[index] == '.'
                    || (text[index] >= '0' && text[index] <= '9')
                    || text[index] == '_' || text[index] == 'e'
                    || text[index] == 'E' || text[index] == '+'
                    || text[index] == '-')) index += 1u;
            suffix_start = index;
        } else {
            node->data.literal.value_low = low;
            node->data.literal.value_high = high;
        }
        if (suffix_start < length)
            node->data.literal.suffix = cm_interner_intern(
                &state->set->strings, text + suffix_start,
                length - suffix_start);
        if (!is_float && suffix_start < length
            && (text[suffix_start] == 'f'))
            node->data.literal.kind = CM_U_LITERAL_FLOAT;
    }
}

/* ------------------------------------------------------------------ */
/* Operators                                                            */

static int cm_u_binary_op(const CmAst *ast, CmInternId name, CmUBinaryOp *op,
    int *is_assign_op)
{
    static const struct { const char *text; CmUBinaryOp op; } table[] = {
        { "+", CM_U_BINARY_ADD }, { "-", CM_U_BINARY_SUB },
        { "*", CM_U_BINARY_MUL }, { "/", CM_U_BINARY_DIV },
        { "%", CM_U_BINARY_REM }, { "&&", CM_U_BINARY_AND },
        { "||", CM_U_BINARY_OR }, { "&", CM_U_BINARY_BIT_AND },
        { "|", CM_U_BINARY_BIT_OR }, { "^", CM_U_BINARY_BIT_XOR },
        { "<<", CM_U_BINARY_SHL }, { ">>", CM_U_BINARY_SHR },
        { "==", CM_U_BINARY_EQ }, { "!=", CM_U_BINARY_NE },
        { "<", CM_U_BINARY_LT }, { "<=", CM_U_BINARY_LE },
        { ">", CM_U_BINARY_GT }, { ">=", CM_U_BINARY_GE },
        { "+=", CM_U_BINARY_ADD }, { "-=", CM_U_BINARY_SUB },
        { "*=", CM_U_BINARY_MUL }, { "/=", CM_U_BINARY_DIV },
        { "%=", CM_U_BINARY_REM }, { "&=", CM_U_BINARY_BIT_AND },
        { "|=", CM_U_BINARY_BIT_OR }, { "^=", CM_U_BINARY_BIT_XOR },
        { "<<=", CM_U_BINARY_SHL }, { ">>=", CM_U_BINARY_SHR }
    };
    const CmInternedString *string = cm_ast_get_string(ast, name);
    size_t index;
    if (string == NULL) return 0;
    for (index = 0u; index < sizeof(table) / sizeof(table[0]); ++index) {
        size_t length = strlen(table[index].text);
        if (string->len == length
            && memcmp(string->bytes, table[index].text, length) == 0) {
            *op = table[index].op;
            *is_assign_op = length >= 2u
                && table[index].text[length - 1u] == '='
                && strcmp(table[index].text, "==") != 0
                && strcmp(table[index].text, "!=") != 0
                && strcmp(table[index].text, "<=") != 0
                && strcmp(table[index].text, ">=") != 0;
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Patterns                                                             */

static CmUPatId cm_u_lower_pat(CmULowerState *state, CmAstPatternId id);

static CmUPatId cm_u_lower_pat_list(CmULowerState *state,
    const CmAstPatternId *patterns, uint32_t count, CmUPatId *out_ids)
{
    uint32_t index;
    for (index = 0u; index < count; ++index)
        out_ids[index] = cm_u_lower_pat(state, patterns[index]);
    return 1u;
}

static CmUPatId cm_u_lower_pat(CmULowerState *state, CmAstPatternId id)
{
    const CmAstPattern *pat = cm_ast_get_pattern(state->ast, id);
    CmUPat node;
    if (pat == NULL) return CM_U_PAT_NONE;
    memset(&node, 0, sizeof(node));
    node.span = cm_u_span(state, pat->span);
    node.ast = id;
    switch (pat->kind) {
    case CM_AST_PATTERN_BINDING: {
        CmInternId name = cm_u_intern_ast(state, pat->data.binding.name);
        CmUPatId sub = CM_U_PAT_NONE;
        if (cm_u_ast_string_is(state->ast, pat->data.binding.name, "_")) {
            node.kind = CM_U_PAT_WILD;
            break;
        }
        /* An identifier naming a unit variant, unit struct, or const is a
         * path pattern, not a binding. */
        if (pat->data.binding.subpattern == CM_AST_PATTERN_NONE
            && !pat->data.binding.is_ref && !pat->data.binding.is_mutable) {
            CmResolvePathSegmentView view;
            const CmInternedString *string = cm_ast_get_string(state->ast,
                pat->data.binding.name);
            CmResolvedBinding binding;
            CmUResolution resolution;
            if (string != NULL) {
                view.bytes = string->bytes;
                view.length = string->len;
                memset(&resolution, 0, sizeof(resolution));
                if (cm_import_resolve_path_checked(state->imports,
                        state->graph, state->revision, state->module, 0,
                        &view, 1u, CM_RESOLVE_NAMESPACE_VALUE, &binding)
                        == CM_IMPORT_LOOKUP_OK
                    && (binding.item_kind == CM_AST_ITEM_CONST
                        || binding.item_kind == CM_AST_ITEM_STATIC
                        || binding.variant.enumeration.item
                            != CM_AST_ITEM_NONE
                        || binding.item_kind == CM_AST_ITEM_STRUCT)
                    && cm_u_binding_definition(state, &binding,
                        &resolution)) {
                    node.kind = CM_U_PAT_PATH;
                    node.data.path.resolution = resolution;
                    node.data.path.resolution.rest_from = 1u;
                    node.data.path.segments = (CmInternId *)cm_u_alloc(state,
                        1u, sizeof(CmInternId));
                    node.data.path.segments[0] = name;
                    node.data.path.segment_count = 1u;
                    break;
                }
            }
        }
        if (pat->data.binding.subpattern != CM_AST_PATTERN_NONE)
            sub = cm_u_lower_pat(state, pat->data.binding.subpattern);
        node.kind = CM_U_PAT_BINDING;
        node.data.binding.local = cm_u_add_local(state, name,
            pat->data.binding.is_mutable, pat->data.binding.is_ref,
            node.span);
        node.data.binding.subpattern = sub;
        node.data.binding.by_ref = pat->data.binding.is_ref;
        node.data.binding.is_mutable = pat->data.binding.is_mutable;
        break;
    }
    case CM_AST_PATTERN_LITERAL: {
        const CmInternedString *string = cm_ast_get_string(state->ast,
            pat->data.literal.text);
        node.kind = CM_U_PAT_LITERAL;
        node.data.literal.text = cm_u_intern_ast(state, pat->data.literal.text);
        node.data.literal.negated = string != NULL && string->len != 0u
            && string->bytes[0] == '-';
        break;
    }
    case CM_AST_PATTERN_PATH: {
        const CmAstPath *path = cm_ast_get_path(state->ast,
            pat->data.path.path);
        node.kind = CM_U_PAT_PATH;
        node.data.path.ast = cm_u_path_ref(state, pat->data.path.path);
        node.data.path.segments = cm_u_segments(state, path,
            &node.data.path.segment_count);
        if (!cm_u_resolve_path(state, path, 0, &node.data.path.resolution)) {
            if (path != NULL && path->segment_count == 1u && !path->absolute) {
                /* An unresolvable bare identifier is a fresh binding. */
                CmInternId name = cm_u_intern_ast(state,
                    path->segments[0].name);
                memset(&node.data, 0, sizeof(node.data));
                node.kind = CM_U_PAT_BINDING;
                node.data.binding.local = cm_u_add_local(state, name, 0, 0,
                    node.span);
                break;
            }
            state->body->unresolved_paths += 1u;
        }
        break;
    }
    case CM_AST_PATTERN_REFERENCE:
        node.kind = CM_U_PAT_REF;
        node.data.ref.is_mutable = pat->data.reference.is_mutable;
        node.data.ref.pattern = cm_u_lower_pat(state,
            pat->data.reference.pattern);
        break;
    case CM_AST_PATTERN_TUPLE:
    case CM_AST_PATTERN_SLICE:
    case CM_AST_PATTERN_OR: {
        uint32_t count = pat->data.list.pattern_count;
        CmUPatId *ids = (CmUPatId *)cm_u_alloc(state, count, sizeof(*ids));
        node.kind = pat->kind == CM_AST_PATTERN_TUPLE ? CM_U_PAT_TUPLE
            : pat->kind == CM_AST_PATTERN_SLICE ? CM_U_PAT_SLICE
            : CM_U_PAT_OR;
        if (pat->kind == CM_AST_PATTERN_OR) {
            /* Alternatives bind the same names: lower the first, then
             * reuse its locals for the others by name. */
            size_t saved = state->scope_count;
            uint32_t alternative;
            for (alternative = 0u; alternative < count; ++alternative) {
                if (alternative != 0u) state->scope_count = saved;
                ids[alternative] = cm_u_lower_pat(state,
                    pat->data.list.patterns[alternative]);
            }
        } else {
            cm_u_lower_pat_list(state, pat->data.list.patterns, count, ids);
        }
        node.data.list.patterns = ids;
        node.data.list.pattern_count = count;
        node.data.list.has_rest = pat->data.list.has_rest;
        node.data.list.rest_index = pat->data.list.rest_index;
        break;
    }
    case CM_AST_PATTERN_STRUCT: {
        const CmAstPath *path = cm_ast_get_path(state->ast,
            pat->data.struct_pattern.path);
        uint32_t count = pat->data.struct_pattern.field_count;
        uint32_t index;
        node.kind = pat->data.struct_pattern.is_tuple
            ? CM_U_PAT_TUPLE_STRUCT : CM_U_PAT_STRUCT;
        node.data.struct_pat.ast = cm_u_path_ref(state,
            pat->data.struct_pattern.path);
        if (!cm_u_resolve_path(state, path, 1,
                &node.data.struct_pat.resolution))
            state->body->unresolved_paths += 1u;
        node.data.struct_pat.has_rest = pat->data.struct_pattern.has_rest;
        if (pat->data.struct_pattern.is_tuple) {
            CmUPatId *ids = (CmUPatId *)cm_u_alloc(state, count,
                sizeof(*ids));
            for (index = 0u; index < count; ++index)
                ids[index] = cm_u_lower_pat(state,
                    pat->data.struct_pattern.fields[index].pattern);
            node.data.struct_pat.patterns = ids;
            node.data.struct_pat.pattern_count = count;
        } else {
            CmUPatField *fields = (CmUPatField *)cm_u_alloc(state, count,
                sizeof(*fields));
            for (index = 0u; index < count; ++index) {
                const CmAstPatternField *field =
                    &pat->data.struct_pattern.fields[index];
                fields[index].name = cm_u_intern_ast(state, field->name);
                if (field->pattern != CM_AST_PATTERN_NONE) {
                    fields[index].pattern = cm_u_lower_pat(state,
                        field->pattern);
                } else {
                    /* `Struct { x }` shorthand binds `x`. */
                    CmUPat binding;
                    memset(&binding, 0, sizeof(binding));
                    binding.kind = CM_U_PAT_BINDING;
                    binding.span = node.span;
                    binding.data.binding.local = cm_u_add_local(state,
                        fields[index].name, 0, 0, node.span);
                    fields[index].pattern = cm_u_push_pat(state, &binding);
                }
            }
            node.data.struct_pat.fields = fields;
            node.data.struct_pat.field_count = count;
        }
        break;
    }
    case CM_AST_PATTERN_RANGE:
        node.kind = CM_U_PAT_RANGE;
        node.data.range.start = cm_u_lower_pat(state, pat->data.range.start);
        node.data.range.end = cm_u_lower_pat(state, pat->data.range.end);
        node.data.range.is_inclusive = pat->data.range.is_inclusive;
        break;
    default:
        node.kind = CM_U_PAT_WILD;
        break;
    }
    return cm_u_push_pat(state, &node);
}

/* ------------------------------------------------------------------ */
/* Expressions                                                          */

static CmUExprId cm_u_lower_expr(CmULowerState *state, CmAstExprId id);

static CmUExprId *cm_u_lower_expr_list(CmULowerState *state,
    const CmAstExprId *ids, uint32_t count)
{
    CmUExprId *result = (CmUExprId *)cm_u_alloc(state, count,
        sizeof(*result));
    uint32_t index;
    for (index = 0u; index < count; ++index)
        result[index] = cm_u_lower_expr(state, ids[index]);
    return result;
}

static CmUStmtId cm_u_lower_stmt(CmULowerState *state, CmAstStmtId id)
{
    const CmAstStmt *stmt = cm_ast_get_stmt(state->ast, id);
    CmUStmt node;
    if (stmt == NULL) return 0u;
    memset(&node, 0, sizeof(node));
    node.span = cm_u_span(state, stmt->span);
    switch (stmt->kind) {
    case CM_AST_STMT_LET:
        node.kind = CM_U_STMT_LET;
        /* Initializer and else block see the outer scope; the pattern binds
         * afterwards for the rest of the block. */
        node.data.let_stmt.initializer = cm_u_lower_expr(state,
            stmt->data.let_stmt.initializer);
        node.data.let_stmt.else_block = cm_u_lower_expr(state,
            stmt->data.let_stmt.else_block);
        node.data.let_stmt.type = cm_u_type_ref(state,
            stmt->data.let_stmt.type);
        node.data.let_stmt.pattern = cm_u_lower_pat(state,
            stmt->data.let_stmt.pattern);
        break;
    case CM_AST_STMT_EXPR:
        node.kind = CM_U_STMT_EXPR;
        node.data.expr_stmt.expression = cm_u_lower_expr(state,
            stmt->data.expr_stmt.expression);
        node.data.expr_stmt.has_semicolon = stmt->data.expr_stmt.has_semicolon;
        break;
    case CM_AST_STMT_ITEM:
        node.kind = CM_U_STMT_ITEM;
        node.data.item_stmt.source = state->source;
        node.data.item_stmt.item = stmt->data.item_stmt.item;
        state->body->nested_items += 1u;
        break;
    }
    return cm_u_push_stmt(state, &node);
}

static CmUExprId cm_u_lower_expr(CmULowerState *state, CmAstExprId id)
{
    const CmAstExpr *expr = cm_ast_get_expr(state->ast, id);
    CmUExpr node;
    uint32_t index;
    if (expr == NULL) return CM_U_EXPR_NONE;
    memset(&node, 0, sizeof(node));
    node.span = cm_u_span(state, expr->span);
    node.ast = id;
    switch (expr->kind) {
    case CM_AST_EXPR_LITERAL:
        node.kind = CM_U_EXPR_LITERAL;
        cm_u_classify_literal(state, expr->data.literal.text, &node);
        break;
    case CM_AST_EXPR_PATH: {
        const CmAstPath *path = cm_ast_get_path(state->ast,
            expr->data.path.path);
        node.kind = CM_U_EXPR_PATH;
        node.data.path.ast = cm_u_path_ref(state, expr->data.path.path);
        node.data.path.segments = cm_u_segments(state, path,
            &node.data.path.segment_count);
        if (!cm_u_resolve_path(state, path, 0, &node.data.path.resolution))
            state->body->unresolved_paths += 1u;
        break;
    }
    case CM_AST_EXPR_QUALIFIED_PATH:
        node.kind = CM_U_EXPR_QUALIFIED_PATH;
        node.data.qualified_path.self_type = cm_u_type_ref(state,
            expr->data.qualified_path.self_type);
        node.data.qualified_path.trait_path = cm_u_path_ref(state,
            expr->data.qualified_path.trait_path);
        node.data.qualified_path.associated_path = cm_u_path_ref(state,
            expr->data.qualified_path.associated_path);
        break;
    case CM_AST_EXPR_BLOCK:
    case CM_AST_EXPR_TRY_BLOCK: {
        size_t saved = state->scope_count;
        size_t saved_nested = state->nested_count;
        uint32_t count = expr->data.block.statement_count;
        CmUStmtId *ids = (CmUStmtId *)cm_u_alloc(state, count, sizeof(*ids));
        node.kind = CM_U_EXPR_BLOCK;
        /* Items are visible throughout the block, before their statement. */
        for (index = 0u; index < count; ++index) {
            const CmAstStmt *stmt = cm_ast_get_stmt(state->ast,
                expr->data.block.statements[index]);
            const CmAstItem *item = stmt == NULL
                    || stmt->kind != CM_AST_STMT_ITEM ? NULL
                : cm_ast_get_item(state->ast, stmt->data.item_stmt.item);
            if (item == NULL || item->name == CM_INTERN_ID_NONE
                || state->nested_count == CM_U_NESTED_LIMIT) continue;
            state->nested[state->nested_count].name = cm_u_intern_ast(state,
                item->name);
            state->nested[state->nested_count].item =
                stmt->data.item_stmt.item;
            state->nested_count += 1u;
        }
        for (index = 0u; index < count; ++index)
            ids[index] = cm_u_lower_stmt(state,
                expr->data.block.statements[index]);
        node.data.block.statements = ids;
        node.data.block.statement_count = count;
        node.data.block.tail = cm_u_lower_expr(state, expr->data.block.tail);
        node.data.block.is_unsafe = expr->data.block.is_unsafe;
        node.data.block.is_const = expr->data.block.is_const;
        state->scope_count = saved;
        state->nested_count = saved_nested;
        break;
    }
    case CM_AST_EXPR_CALL:
        node.kind = CM_U_EXPR_CALL;
        node.data.call.callee = cm_u_lower_expr(state, expr->data.call.callee);
        node.data.call.arguments = cm_u_lower_expr_list(state,
            expr->data.call.arguments, expr->data.call.argument_count);
        node.data.call.argument_count = expr->data.call.argument_count;
        break;
    case CM_AST_EXPR_METHOD_CALL:
        node.kind = CM_U_EXPR_METHOD_CALL;
        node.data.method_call.receiver = cm_u_lower_expr(state,
            expr->data.method_call.receiver);
        node.data.method_call.name = cm_u_intern_ast(state,
            expr->data.method_call.name);
        node.data.method_call.generic_arguments.source =
            expr->data.method_call.generic_argument_count != 0u
            ? state->source : 0u;
        node.data.method_call.generic_arguments.path = CM_AST_PATH_NONE;
        node.data.method_call.arguments = cm_u_lower_expr_list(state,
            expr->data.method_call.arguments,
            expr->data.method_call.argument_count);
        node.data.method_call.argument_count =
            expr->data.method_call.argument_count;
        break;
    case CM_AST_EXPR_FIELD:
        node.kind = CM_U_EXPR_FIELD;
        node.data.field.base = cm_u_lower_expr(state, expr->data.field.base);
        node.data.field.name = cm_u_intern_ast(state, expr->data.field.name);
        break;
    case CM_AST_EXPR_TUPLE_FIELD:
        node.kind = CM_U_EXPR_TUPLE_FIELD;
        node.data.tuple_field.base = cm_u_lower_expr(state,
            expr->data.tuple_field.base);
        node.data.tuple_field.index = expr->data.tuple_field.index;
        break;
    case CM_AST_EXPR_INDEX:
        node.kind = CM_U_EXPR_INDEX;
        node.data.index.base = cm_u_lower_expr(state, expr->data.index.base);
        node.data.index.index = cm_u_lower_expr(state,
            expr->data.index.index);
        break;
    case CM_AST_EXPR_UNARY: {
        const CmInternedString *op = cm_ast_get_string(state->ast,
            expr->data.unary.operator_name);
        CmUExprId operand = cm_u_lower_expr(state, expr->data.unary.operand);
        if (op != NULL && op->len != 0u && op->bytes[0] == '&') {
            /* `&`, `&mut`, `&&`, `&&mut`: references, doubled for `&&`. */
            int is_mutable = op->len >= 4u
                && memcmp(op->bytes + op->len - 3u, "mut", 3u) == 0;
            int doubled = op->len >= 2u && op->bytes[1] == '&';
            node.kind = CM_U_EXPR_REF;
            node.data.ref.operand = operand;
            node.data.ref.is_mutable = is_mutable;
            if (doubled) {
                CmUExprId inner = cm_u_push_expr(state, &node);
                memset(&node, 0, sizeof(node));
                node.kind = CM_U_EXPR_REF;
                node.span = cm_u_span(state, expr->span);
                node.ast = id;
                node.data.ref.operand = inner;
            }
            break;
        }
        node.kind = CM_U_EXPR_UNARY;
        node.data.unary.operand = operand;
        node.data.unary.op = op != NULL && op->len == 1u && op->bytes[0] == '!'
            ? CM_U_UNARY_NOT : op != NULL && op->len == 1u
                && op->bytes[0] == '*' ? CM_U_UNARY_DEREF : CM_U_UNARY_NEG;
        break;
    }
    case CM_AST_EXPR_RAW_REFERENCE:
        node.kind = CM_U_EXPR_REF;
        node.data.ref.operand = cm_u_lower_expr(state,
            expr->data.raw_reference.operand);
        node.data.ref.is_raw = 1;
        node.data.ref.is_mutable = expr->data.raw_reference.kind
            == CM_AST_RAW_REFERENCE_MUT;
        break;
    case CM_AST_EXPR_BINARY:
    case CM_AST_EXPR_ASSIGN: {
        CmUBinaryOp op = CM_U_BINARY_ADD;
        int is_assign_op = 0;
        int known = cm_u_binary_op(state->ast, expr->data.binary.operator_name,
            &op, &is_assign_op);
        CmUExprId left = cm_u_lower_expr(state, expr->data.binary.left);
        CmUExprId right = cm_u_lower_expr(state, expr->data.binary.right);
        if (expr->kind == CM_AST_EXPR_ASSIGN) {
            node.kind = known && is_assign_op ? CM_U_EXPR_ASSIGN_OP
                : CM_U_EXPR_ASSIGN;
            node.data.assign.target = left;
            node.data.assign.value = right;
            node.data.assign.op = op;
        } else {
            node.kind = CM_U_EXPR_BINARY;
            node.data.binary.op = op;
            node.data.binary.left = left;
            node.data.binary.right = right;
            if (!known) cm_u_fail(state, "unknown binary operator");
        }
        break;
    }
    case CM_AST_EXPR_CAST:
        node.kind = CM_U_EXPR_CAST;
        node.data.cast.value = cm_u_lower_expr(state, expr->data.cast.value);
        node.data.cast.type = cm_u_type_ref(state, expr->data.cast.type);
        break;
    case CM_AST_EXPR_TRY:
        node.kind = CM_U_EXPR_TRY;
        node.data.try_expr.operand = cm_u_lower_expr(state,
            expr->data.try_expr.operand);
        break;
    case CM_AST_EXPR_RANGE:
        node.kind = CM_U_EXPR_RANGE;
        node.data.range.start = cm_u_lower_expr(state, expr->data.range.start);
        node.data.range.end = cm_u_lower_expr(state, expr->data.range.end);
        node.data.range.is_inclusive = expr->data.range.is_inclusive;
        break;
    case CM_AST_EXPR_LET:
        node.kind = CM_U_EXPR_LET;
        node.data.let_expr.initializer = cm_u_lower_expr(state,
            expr->data.let_expr.initializer);
        node.data.let_expr.pattern = cm_u_lower_pat(state,
            expr->data.let_expr.pattern);
        break;
    case CM_AST_EXPR_RETURN:
    case CM_AST_EXPR_BREAK:
    case CM_AST_EXPR_CONTINUE:
        node.kind = expr->kind == CM_AST_EXPR_RETURN ? CM_U_EXPR_RETURN
            : expr->kind == CM_AST_EXPR_BREAK ? CM_U_EXPR_BREAK
            : CM_U_EXPR_CONTINUE;
        node.data.flow.label = cm_u_intern_ast(state, expr->data.flow.label);
        node.data.flow.value = cm_u_lower_expr(state, expr->data.flow.value);
        break;
    case CM_AST_EXPR_IF: {
        size_t saved = state->scope_count;
        node.kind = CM_U_EXPR_IF;
        node.data.if_expr.condition = cm_u_lower_expr(state,
            expr->data.if_expr.condition);
        /* `if let`: the pattern's bindings scope over the then branch. */
        node.data.if_expr.pattern = cm_u_lower_pat(state,
            expr->data.if_expr.pattern);
        node.data.if_expr.then_expr = cm_u_lower_expr(state,
            expr->data.if_expr.then_expr);
        state->scope_count = saved;
        node.data.if_expr.else_expr = cm_u_lower_expr(state,
            expr->data.if_expr.else_expr);
        break;
    }
    case CM_AST_EXPR_MATCH: {
        uint32_t count = expr->data.match_expr.arm_count;
        CmUMatchArm *arms = (CmUMatchArm *)cm_u_alloc(state, count,
            sizeof(*arms));
        node.kind = CM_U_EXPR_MATCH;
        node.data.match_expr.scrutinee = cm_u_lower_expr(state,
            expr->data.match_expr.scrutinee);
        for (index = 0u; index < count; ++index) {
            const CmAstMatchArm *arm = &expr->data.match_expr.arms[index];
            size_t saved = state->scope_count;
            arms[index].pattern = cm_u_lower_pat(state, arm->pattern);
            if (arm->guard_pattern != CM_AST_PATTERN_NONE) {
                /* `if let` guard: represent as a LET expression. */
                CmUExpr let_node;
                memset(&let_node, 0, sizeof(let_node));
                let_node.kind = CM_U_EXPR_LET;
                let_node.span = cm_u_span(state, arm->guard_span);
                let_node.data.let_expr.initializer = cm_u_lower_expr(state,
                    arm->guard_initializer);
                let_node.data.let_expr.pattern = cm_u_lower_pat(state,
                    arm->guard_pattern);
                arms[index].guard = cm_u_push_expr(state, &let_node);
            } else {
                arms[index].guard = cm_u_lower_expr(state, arm->guard);
            }
            arms[index].body = cm_u_lower_expr(state, arm->body);
            state->scope_count = saved;
        }
        node.data.match_expr.arms = arms;
        node.data.match_expr.arm_count = count;
        break;
    }
    case CM_AST_EXPR_LOOP:
        node.kind = CM_U_EXPR_LOOP;
        node.data.loop_expr.label = cm_u_intern_ast(state,
            expr->data.loop_expr.label);
        node.data.loop_expr.body = cm_u_lower_expr(state,
            expr->data.loop_expr.body);
        break;
    case CM_AST_EXPR_WHILE: {
        size_t saved = state->scope_count;
        node.kind = CM_U_EXPR_WHILE;
        node.data.while_expr.condition = cm_u_lower_expr(state,
            expr->data.while_expr.condition);
        node.data.while_expr.pattern = cm_u_lower_pat(state,
            expr->data.while_expr.pattern);
        node.data.while_expr.body = cm_u_lower_expr(state,
            expr->data.while_expr.body);
        state->scope_count = saved;
        break;
    }
    case CM_AST_EXPR_FOR: {
        size_t saved = state->scope_count;
        node.kind = CM_U_EXPR_FOR;
        node.data.for_expr.iterable = cm_u_lower_expr(state,
            expr->data.for_expr.iterable);
        node.data.for_expr.pattern = cm_u_lower_pat(state,
            expr->data.for_expr.pattern);
        node.data.for_expr.body = cm_u_lower_expr(state,
            expr->data.for_expr.body);
        state->scope_count = saved;
        break;
    }
    case CM_AST_EXPR_CLOSURE: {
        size_t saved = state->scope_count;
        uint32_t count = expr->data.closure.parameter_count;
        CmUClosureParam *parameters = (CmUClosureParam *)cm_u_alloc(state,
            count, sizeof(*parameters));
        node.kind = CM_U_EXPR_CLOSURE;
        for (index = 0u; index < count; ++index) {
            parameters[index].type = cm_u_type_ref(state,
                expr->data.closure.parameters[index].type);
            parameters[index].pattern = cm_u_lower_pat(state,
                expr->data.closure.parameters[index].pattern);
        }
        node.data.closure.parameters = parameters;
        node.data.closure.parameter_count = count;
        node.data.closure.return_type = cm_u_type_ref(state,
            expr->data.closure.return_type);
        node.data.closure.is_move = expr->data.closure.is_move;
        node.data.closure.body = cm_u_lower_expr(state,
            expr->data.closure.body);
        state->scope_count = saved;
        break;
    }
    case CM_AST_EXPR_TUPLE:
    case CM_AST_EXPR_ARRAY:
        if (expr->kind == CM_AST_EXPR_ARRAY
            && expr->data.list.repeat_value != CM_AST_EXPR_NONE) {
            node.kind = CM_U_EXPR_ARRAY_REPEAT;
            node.data.repeat.value = cm_u_lower_expr(state,
                expr->data.list.repeat_value);
            node.data.repeat.length = cm_u_lower_expr(state,
                expr->data.list.repeat_length);
            break;
        }
        node.kind = expr->kind == CM_AST_EXPR_TUPLE ? CM_U_EXPR_TUPLE
            : CM_U_EXPR_ARRAY;
        if (expr->kind == CM_AST_EXPR_TUPLE
            && expr->data.list.element_count == 0u) {
            node.kind = CM_U_EXPR_LITERAL;
            node.data.literal.kind = CM_U_LITERAL_UNIT;
            break;
        }
        node.data.list.elements = cm_u_lower_expr_list(state,
            expr->data.list.elements, expr->data.list.element_count);
        node.data.list.element_count = expr->data.list.element_count;
        break;
    case CM_AST_EXPR_STRUCT: {
        const CmAstPath *path = cm_ast_get_path(state->ast,
            expr->data.struct_expr.path);
        uint32_t count = expr->data.struct_expr.field_count;
        CmUField *fields = (CmUField *)cm_u_alloc(state, count,
            sizeof(*fields));
        node.kind = CM_U_EXPR_STRUCT;
        node.data.struct_expr.ast = cm_u_path_ref(state,
            expr->data.struct_expr.path);
        if (!cm_u_resolve_path(state, path, 1,
                &node.data.struct_expr.resolution))
            state->body->unresolved_paths += 1u;
        for (index = 0u; index < count; ++index) {
            const CmAstExprField *field = &expr->data.struct_expr.fields[index];
            fields[index].name = cm_u_intern_ast(state, field->name);
            if (field->value != CM_AST_EXPR_NONE) {
                fields[index].value = cm_u_lower_expr(state, field->value);
            } else {
                /* `S { x }` shorthand reads local `x`. */
                CmUExpr local;
                memset(&local, 0, sizeof(local));
                local.kind = CM_U_EXPR_PATH;
                local.span = node.span;
                local.data.path.segments = (CmInternId *)cm_u_alloc(state,
                    1u, sizeof(CmInternId));
                local.data.path.segments[0] = fields[index].name;
                local.data.path.segment_count = 1u;
                local.data.path.resolution.local = cm_u_find_local(state,
                    fields[index].name);
                local.data.path.resolution.kind =
                    local.data.path.resolution.local == CM_U_LOCAL_NONE
                    ? CM_U_RESOLVED_UNRESOLVED : CM_U_RESOLVED_LOCAL;
                local.data.path.resolution.rest_from = 1u;
                fields[index].value = cm_u_push_expr(state, &local);
            }
        }
        node.data.struct_expr.fields = fields;
        node.data.struct_expr.field_count = count;
        node.data.struct_expr.base = cm_u_lower_expr(state,
            expr->data.struct_expr.base);
        break;
    }
    case CM_AST_EXPR_MACRO: {
        const CmAstPath *path = cm_ast_get_path(state->ast,
            expr->data.macro_expr.path);
        CmInternId last = path != NULL && path->segment_count != 0u
            ? path->segments[path->segment_count - 1u].name
            : CM_INTERN_ID_NONE;
        node.data.retained.text = cm_u_intern_ast(state,
            expr->data.macro_expr.arguments);
        if (cm_u_ast_string_is(state->ast, last, "asm")
            || cm_u_ast_string_is(state->ast, last, "naked_asm")
            || cm_u_ast_string_is(state->ast, last, "global_asm")) {
            node.kind = CM_U_EXPR_ASM;
            state->body->retained_macros += 1u;
        } else if (cm_u_ast_string_is(state->ast, last, "offset_of")) {
            node.kind = CM_U_EXPR_OFFSET_OF;
            state->body->retained_macros += 1u;
        } else {
            node.kind = CM_U_EXPR_UNSUPPORTED;
            cm_u_fail(state, "unexpanded macro invocation in body");
        }
        break;
    }
    default:
        node.kind = CM_U_EXPR_UNSUPPORTED;
        cm_u_fail(state, "unsupported expression form");
        break;
    }
    return cm_u_push_expr(state, &node);
}

/* ------------------------------------------------------------------ */
/* Bodies                                                               */

/* The AST item whose body/initializer is exactly `root` (attributes and
 * doc comments make span-based matching unreliable). */
static const CmAstItem *cm_u_find_ast_item(const CmAst *ast,
    CmAstExprId root, CmAstItemId *out_id)
{
    size_t index;
    for (index = 0u; index < ast->items.len; ++index) {
        const CmAstItem *item = (const CmAstItem *)cm_vec_at_const(
            &ast->items, index);
        if (item == NULL) continue;
        if ((item->kind == CM_AST_ITEM_FUNCTION
                && item->data.function_item.body == root)
            || ((item->kind == CM_AST_ITEM_CONST
                    || item->kind == CM_AST_ITEM_STATIC)
                && item->data.value_item.initializer == root)) {
            *out_id = (CmAstItemId)(index + 1u);
            return item;
        }
    }
    return NULL;
}

static void cm_u_lower_body(CmULowerState *state, CmHirBodyId body_id,
    const CmHirBody *hir_body, CmUBody *out)
{
    const CmHirDefinition *definition;
    const CmHirItem *item;
    CmModuleId module = CM_MODULE_NONE;
    const CmAst *ast = NULL;
    const CmAstItem *ast_item;
    CmAstItemId ast_item_id = CM_AST_ITEM_NONE;
    CmAstExprId root;
    uint32_t index;

    memset(out, 0, sizeof(*out));
    out->hir_body = body_id;
    out->owner = hir_body->owner;
    cm_vec_init(&out->expressions, sizeof(CmUExpr));
    cm_vec_init(&out->patterns, sizeof(CmUPat));
    cm_vec_init(&out->statements, sizeof(CmUStmt));
    cm_vec_init(&out->locals, sizeof(CmULocal));
    if (hir_body->state != CM_HIR_BODY_UNLOWERED
        || hir_body->origin.kind != CM_HIR_BODY_ORIGIN_ITEM_SOURCE
        || hir_body->source_expression_id == CM_AST_EXPR_NONE) {
        out->status = CM_U_BODY_NO_SOURCE;
        return;
    }
    definition = cm_hir_lookup_definition(state->hir,
        hir_body->origin.definition);
    item = definition == NULL || definition->kind != CM_HIR_DEFINITION_ITEM
        ? NULL : cm_hir_get_item(state->hir, definition->entity.item_id);
    if (item == NULL
        || cm_hir_module_map_lookup_module(state->modules, state->graph,
            state->revision, state->hir, item->owner_module, &module)
            != CM_HIR_MODULE_MAP_OK
        || !cm_module_graph_borrow_ast(state->graph, module, &ast)
        || ast == NULL) {
        out->status = CM_U_BODY_NO_SOURCE;
        return;
    }
    state->body = out;
    state->ast = ast;
    state->source = hir_body->source;
    state->module = module;
    state->item = item;
    state->parent_item = NULL;
    state->scope_count = 0u;
    state->nested_count = 0u;
    state->failure = NULL;
    if (!cm_hir_def_id_is_none(item->parent_definition)) {
        const CmHirDefinition *parent = cm_hir_lookup_definition(state->hir,
            item->parent_definition);
        if (parent != NULL && parent->kind == CM_HIR_DEFINITION_ITEM)
            state->parent_item = cm_hir_get_item(state->hir,
                parent->entity.item_id);
    }
    root = hir_body->source_expression_id;
    ast_item = cm_u_find_ast_item(ast, root, &ast_item_id);
    if (ast_item != NULL && ast_item->kind == CM_AST_ITEM_FUNCTION) {
        uint32_t count = ast_item->data.function_item.parameter_count;
        out->parameters = (CmUPatId *)cm_u_alloc(state, count,
            sizeof(*out->parameters));
        out->parameter_types = (CmUAstTypeRef *)cm_u_alloc(state, count,
            sizeof(*out->parameter_types));
        out->parameter_count = count;
        for (index = 0u; index < count; ++index) {
            const CmAstFunctionParam *parameter =
                &ast_item->data.function_item.parameters[index];
            out->parameter_types[index] = cm_u_type_ref(state,
                parameter->type);
            if (parameter->is_self) {
                CmUPat self_pat;
                memset(&self_pat, 0, sizeof(self_pat));
                self_pat.kind = CM_U_PAT_BINDING;
                self_pat.span = cm_u_span(state, ast_item->span);
                self_pat.data.binding.local = cm_u_add_local(state,
                    cm_u_intern_text(state, "self"), 0, 0, self_pat.span);
                out->parameters[index] = cm_u_push_pat(state, &self_pat);
            } else {
                out->parameters[index] = cm_u_lower_pat(state,
                    parameter->pattern);
            }
        }
        out->return_type = cm_u_type_ref(state,
            ast_item->data.function_item.return_type);
    }
    out->root = cm_u_lower_expr(state, root);
    if (state->failure != NULL) {
        out->status = CM_U_BODY_FAILED;
        out->failure = state->failure;
    } else {
        out->status = CM_U_BODY_LOWERED;
    }
}

CmUBodyLowerResult cm_ubody_lower_all(CmUBodySet *set,
    const CmHirContext *hir, const CmModuleGraph *graph,
    CmModuleGraphRevision revision, const CmImportResolver *imports,
    const CmHirModuleMap *modules)
{
    CmUBodyLowerResult result;
    CmULowerState *state;
    size_t index;
    memset(&result, 0, sizeof(result));
    if (set == NULL || hir == NULL || graph == NULL || imports == NULL
        || modules == NULL) return result;
    state = (CmULowerState *)cm_alloc_zeroed(1u, sizeof(*state));
    state->set = set;
    state->hir = hir;
    state->graph = graph;
    state->revision = revision;
    state->imports = imports;
    state->modules = modules;
    cm_u_build_item_keys(state);
    cm_u_build_source_asts(state);
    cm_vec_clear(&set->bodies);
    for (index = 0u; index < hir->bodies.len; ++index) {
        const CmHirBody *hir_body = (const CmHirBody *)cm_vec_at_const(
            &hir->bodies, index);
        CmUBody body;
        result.bodies += 1u;
        cm_u_lower_body(state, (CmHirBodyId)(index + 1u), hir_body, &body);
        switch (body.status) {
        case CM_U_BODY_LOWERED:
            result.lowered += 1u;
            break;
        case CM_U_BODY_NO_SOURCE:
            result.no_source += 1u;
            break;
        case CM_U_BODY_FAILED:
        default:
            result.failed += 1u;
            if (result.first_failure == NULL) {
                result.first_failure = body.failure;
                result.first_failure_body = (CmHirBodyId)(index + 1u);
            }
            break;
        }
        result.expressions += body.expressions.len;
        result.unresolved_paths += body.unresolved_paths;
        result.nested_items += body.nested_items;
        result.retained_macros += body.retained_macros;
        cm_vec_push(&set->bodies, &body);
    }
    cm_free(state->source_asts);
    cm_free(state->item_keys);
    cm_free(state);
    return result;
}
