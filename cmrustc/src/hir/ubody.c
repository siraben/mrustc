#include "cm/hir/ubody.h"
#include "cm/alloc.h"
#include "cm/arena.h"
#include <stdio.h>
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
    /* Enclosing body-local `mod name { .. }`, or NONE: its items are
     * named `module::item`, never bare. */
    CmInternId module;
} CmUNestedItem;

#define CM_U_NESTED_LIMIT 512u
#define CM_U_BODY_USE_LIMIT 128u
#define CM_U_USE_SEG_LIMIT 16u

/*
 * One body-local `use` binding.  Segment views borrow the AST's interned
 * use-tree text, which outlives lowering.  A glob entry has no name.
 */
typedef struct CmUBodyUse {
    const unsigned char *segment_bytes[CM_U_USE_SEG_LIMIT];
    uint32_t segment_lengths[CM_U_USE_SEG_LIMIT];
    uint32_t segment_count;
    const unsigned char *name_bytes;
    uint32_t name_length;
    int is_glob;
    int absolute;
} CmUBodyUse;

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
    /* The local crate's own bundle; active fields swap per body (M9-03). */
    const CmModuleGraph *local_graph;
    CmModuleGraphRevision local_revision;
    const CmImportResolver *local_imports;
    const CmHirModuleMap *local_modules;
    const CmUBodyDependency *dependencies;
    size_t dependency_count;
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
    CmUBodyUse body_uses[CM_U_BODY_USE_LIMIT];
    size_t body_use_count;
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
        if (item == NULL || item->ast_source == 0u) continue;
        state->item_keys[count].source = item->ast_source;
        state->item_keys[count].start = item->ast_item;
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

static void cm_u_ingest_graph_asts(CmULowerState *state,
    const CmModuleGraph *graph, size_t *used)
{
    size_t count = cm_module_graph_module_count(graph);
    size_t index;
    for (index = 0u; index < count; ++index) {
        CmResolveModuleInfo info;
        const CmAst *ast = NULL;
        size_t seen;
        if (!cm_module_graph_get_module_at(graph, index, &info)
            || !cm_module_graph_borrow_ast(graph, info.id, &ast)
            || ast == NULL) continue;
        for (seen = 0u; seen < *used; ++seen)
            if (state->source_asts[seen].source == info.source) break;
        if (seen != *used) continue;
        state->source_asts[*used].source = info.source;
        state->source_asts[*used].ast = ast;
        state->source_asts[*used].module = info.id;
        *used += 1u;
    }
}

static void cm_u_build_source_asts(CmULowerState *state)
{
    size_t count = cm_module_graph_module_count(state->graph);
    size_t dependency_index;
    size_t used = 0u;
    for (dependency_index = 0u; dependency_index < state->dependency_count;
         ++dependency_index)
        count += cm_module_graph_module_count(
            state->dependencies[dependency_index].graph);
    state->source_asts = (CmUSourceAst *)cm_alloc_zeroed(count + 1u,
        sizeof(*state->source_asts));
    cm_u_ingest_graph_asts(state, state->graph, &used);
    for (dependency_index = 0u; dependency_index < state->dependency_count;
         ++dependency_index)
        cm_u_ingest_graph_asts(state,
            state->dependencies[dependency_index].graph, &used);
    state->source_ast_count = used;
}

/* Map a resolver binding to a HIR definition. */
static int cm_u_debug_enabled(void)
{
    static int cached = -1;
    if (cached < 0) cached = getenv("CM_UBODY_DEBUG") != NULL;
    return cached;
}

static void cm_u_debug_path(const CmULowerState *state, const CmAstPath *path,
    const char *stage)
{
    uint32_t index;
    if (!cm_u_debug_enabled() || path == NULL) return;
    fprintf(stderr, "ubody-debug source=%u module=%u %s path=",
        (unsigned)state->source, (unsigned)state->module, stage);
    for (index = 0u; index < path->segment_count; ++index) {
        const CmInternedString *name = cm_ast_get_string(state->ast,
            path->segments[index].name);
        fprintf(stderr, "%s%.*s", index == 0u ? "" : "::",
            name == NULL ? 0 : (int)name->len,
            name == NULL ? "" : (const char *)name->bytes);
    }
    fputc(10, stderr);
}

static int cm_u_binding_definition(const CmULowerState *state,
    const CmResolvedBinding *binding, CmUResolution *out)
{
    CmHirDefId definition;
    if (binding->variant.enumeration.item != CM_AST_ITEM_NONE) {
        CmHirDefId enum_definition;
        const CmHirDefinition *record;
        size_t index;
        if (!cm_u_find_item_definition(state,
                binding->variant.enumeration.source,
                binding->variant.enumeration.item, &enum_definition))
            return 0;
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
    if (binding->declaration.item == CM_AST_ITEM_NONE) {
        if (cm_u_debug_enabled())
            fprintf(stderr, "ubody-debug binding without declaration:"
                " kind=%d module=%u target=%u dep=%u import=%d reexport=%d\n",
                (int)binding->item_kind, (unsigned)binding->module,
                (unsigned)binding->target_module,
                (unsigned)binding->dependency, binding->is_import,
                binding->is_reexport);
        return 0;
    }
    if (!cm_u_find_item_definition(state, binding->declaration.source,
            binding->declaration.item, &definition)) {
        if (cm_u_debug_enabled())
            fprintf(stderr, "ubody-debug binding source=%u item=%u:"
                " no HIR definition\n",
                (unsigned)binding->declaration.source,
                (unsigned)binding->declaration.item);
        return 0;
    }
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
/* ------------------------------------------------------------------ */
/* Body-local `use` declarations                                        */

static void cm_u_use_skip_space(const unsigned char *text, size_t length,
    size_t *pos)
{
    while (*pos < length && (text[*pos] == ' ' || text[*pos] == '\t'
            || text[*pos] == '\n' || text[*pos] == '\r'))
        *pos += 1u;
}

static int cm_u_use_ident_byte(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_' || c == '#';
}

static void cm_u_use_emit(CmULowerState *state,
    const unsigned char **seg_bytes, const uint32_t *seg_lengths,
    uint32_t seg_count, int absolute, int is_glob,
    const unsigned char *alias_bytes, uint32_t alias_length)
{
    CmUBodyUse *entry;
    uint32_t index;
    if (state->body_use_count == CM_U_BODY_USE_LIMIT) return;
    if (!is_glob && alias_bytes == NULL && seg_count == 0u) return;
    if (alias_bytes != NULL && alias_length == 1u && alias_bytes[0] == '_')
        return;
    entry = &state->body_uses[state->body_use_count];
    memset(entry, 0, sizeof(*entry));
    for (index = 0u; index < seg_count; ++index) {
        entry->segment_bytes[index] = seg_bytes[index];
        entry->segment_lengths[index] = seg_lengths[index];
    }
    entry->segment_count = seg_count;
    entry->absolute = absolute;
    entry->is_glob = is_glob;
    if (!is_glob) {
        uint32_t name_from = seg_count - 1u;
        if (alias_bytes != NULL) {
            entry->name_bytes = alias_bytes;
            entry->name_length = alias_length;
        } else {
            /* `path::to::mod::self` imports the module under its name. */
            if (seg_lengths[name_from] == 4u
                && memcmp(seg_bytes[name_from], "self", 4u) == 0) {
                if (name_from == 0u) return;
                name_from -= 1u;
                entry->segment_count = seg_count - 1u;
            }
            entry->name_bytes = seg_bytes[name_from];
            entry->name_length = seg_lengths[name_from];
        }
    }
    state->body_use_count += 1u;
}

/* Mirrors the resolver's textual use-tree grammar, leniently. */
static void cm_u_use_parse(CmULowerState *state, const unsigned char *text,
    size_t length, size_t *pos, const unsigned char **seg_bytes,
    uint32_t *seg_lengths, uint32_t seg_count, int absolute, int depth)
{
    if (depth > 8) return;
    cm_u_use_skip_space(text, length, pos);
    if (seg_count == 0u && *pos + 1u < length && text[*pos] == ':'
        && text[*pos + 1u] == ':') {
        absolute = 1;
        *pos += 2u;
        cm_u_use_skip_space(text, length, pos);
    }
    for (;;) {
        if (*pos < length && text[*pos] == '{') {
            *pos += 1u;
            for (;;) {
                cm_u_use_skip_space(text, length, pos);
                if (*pos >= length) return;
                if (text[*pos] == '}') { *pos += 1u; return; }
                cm_u_use_parse(state, text, length, pos, seg_bytes,
                    seg_lengths, seg_count, absolute, depth + 1);
                cm_u_use_skip_space(text, length, pos);
                if (*pos < length && text[*pos] == ',') *pos += 1u;
            }
        }
        if (*pos < length && text[*pos] == '*') {
            *pos += 1u;
            cm_u_use_emit(state, seg_bytes, seg_lengths, seg_count,
                absolute, 1, NULL, 0u);
            return;
        }
        if (*pos >= length || !cm_u_use_ident_byte(text[*pos])) return;
        {
            size_t start = *pos;
            while (*pos < length && cm_u_use_ident_byte(text[*pos]))
                *pos += 1u;
            if (seg_count == CM_U_USE_SEG_LIMIT) return;
            seg_bytes[seg_count] = text + start;
            seg_lengths[seg_count] = (uint32_t)(*pos - start);
            seg_count += 1u;
        }
        cm_u_use_skip_space(text, length, pos);
        if (*pos + 1u < length && text[*pos] == ':'
            && text[*pos + 1u] == ':') {
            *pos += 2u;
            cm_u_use_skip_space(text, length, pos);
            continue;
        }
        if (*pos + 1u < length && text[*pos] == 'a' && text[*pos + 1u] == 's'
            && (*pos + 2u >= length
                || !cm_u_use_ident_byte(text[*pos + 2u]))) {
            size_t start;
            *pos += 2u;
            cm_u_use_skip_space(text, length, pos);
            start = *pos;
            while (*pos < length && cm_u_use_ident_byte(text[*pos]))
                *pos += 1u;
            cm_u_use_emit(state, seg_bytes, seg_lengths, seg_count,
                absolute, 0, text + start, (uint32_t)(*pos - start));
            return;
        }
        cm_u_use_emit(state, seg_bytes, seg_lengths, seg_count, absolute, 0,
            NULL, 0u);
        return;
    }
}

static void cm_u_collect_body_use(CmULowerState *state,
    const CmAstItem *item)
{
    const CmInternedString *tree = cm_ast_get_string(state->ast,
        item->data.use_item.tree);
    const unsigned char *seg_bytes[CM_U_USE_SEG_LIMIT];
    uint32_t seg_lengths[CM_U_USE_SEG_LIMIT];
    size_t pos = 0u;
    if (tree == NULL) return;
    cm_u_use_parse(state, tree->bytes, tree->len, &pos, seg_bytes,
        seg_lengths, 0u, 0, 0);
}

static const CmUBodyUse *cm_u_body_use_named(const CmULowerState *state,
    CmInternId first)
{
    const CmInternedString *name = cm_ast_get_string(state->ast, first);
    size_t index;
    if (name == NULL) return NULL;
    for (index = state->body_use_count; index != 0u; --index) {
        const CmUBodyUse *entry = &state->body_uses[index - 1u];
        if (!entry->is_glob && entry->name_length == name->len
            && memcmp(entry->name_bytes, name->bytes, name->len) == 0)
            return entry;
    }
    return NULL;
}

/*
 * Resolve `entry segments + path[skip..]` through module-level import
 * resolution, mapping any associated tail back onto the original path.
 */
static int cm_u_resolve_substituted(CmULowerState *state,
    const CmUBodyUse *entry, const CmAstPath *path, uint32_t skip,
    int prefer_type, CmUResolution *out)
{
    CmResolvePathSegmentView views[64];
    CmResolvedBinding binding;
    uint32_t total = 0u;
    uint32_t index;
    uint32_t original_count = path->segment_count;
    int attempt;
    if (entry->segment_count + (original_count - skip) > 64u) return 0;
    for (index = 0u; index < entry->segment_count; ++index) {
        views[total].bytes = entry->segment_bytes[index];
        views[total].length = entry->segment_lengths[index];
        total += 1u;
    }
    for (index = skip; index < original_count; ++index) {
        const CmInternedString *name = cm_ast_get_string(state->ast,
            path->segments[index].name);
        if (name == NULL) return 0;
        views[total].bytes = name->bytes;
        views[total].length = name->len;
        total += 1u;
    }
    if (total == 0u) return 0;
    for (attempt = 0; attempt < 2; ++attempt) {
        CmResolveNamespace ns = (attempt == 0) == (prefer_type != 0)
            ? CM_RESOLVE_NAMESPACE_TYPE : CM_RESOLVE_NAMESPACE_VALUE;
        if (cm_import_resolve_path_checked(state->imports, state->graph,
                state->revision, state->module, entry->absolute, views,
                total, ns, &binding) == CM_IMPORT_LOOKUP_OK
            && cm_u_binding_definition(state, &binding, out)) {
            out->rest_from = original_count;
            return 1;
        }
    }
    /* Type prefix + associated tail, only where the tail stays within the
     * original path segments. */
    for (index = total - 1u; index != 0u; --index) {
        CmUResolution prefix;
        if (index < entry->segment_count) break;
        memset(&prefix, 0, sizeof(prefix));
        if (cm_import_resolve_path_checked(state->imports, state->graph,
                state->revision, state->module, entry->absolute, views,
                index, CM_RESOLVE_NAMESPACE_TYPE, &binding)
                == CM_IMPORT_LOOKUP_OK
            && cm_u_binding_definition(state, &binding, &prefix)
            && prefix.kind == CM_U_RESOLVED_DEFINITION) {
            out->kind = CM_U_RESOLVED_TYPE_ASSOC;
            out->definition = prefix.definition;
            out->rest_from = skip + (index - entry->segment_count);
            return 1;
        }
    }
    return 0;
}

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
        CmInternId second = count >= 2u
            ? cm_u_intern_ast(state, path->segments[1].name)
            : CM_INTERN_ID_NONE;
        size_t nested = state->nested_count;
        while (nested != 0u) {
            uint32_t consumed;
            nested -= 1u;
            if (state->nested[nested].module == CM_INTERN_ID_NONE) {
                if (state->nested[nested].name != name) continue;
                consumed = 1u;
            } else {
                /* `sigpipe::DEFAULT` names an item of a body-local mod. */
                if (count < 2u || state->nested[nested].module != name
                    || state->nested[nested].name != second) continue;
                consumed = 2u;
            }
            out->kind = CM_U_RESOLVED_NESTED_ITEM;
            out->nested_source = state->source;
            out->nested_item = state->nested[nested].item;
            out->rest_from = consumed;
            {
                /* Body-local consts and statics are lowered as body_local
                 * HIR items: resolve them as definitions so their values
                 * are read like module-level ones. */
                const CmAstItem *nested_item = cm_ast_get_item(state->ast,
                    state->nested[nested].item);
                CmHirDefId definition;
                if (nested_item != NULL
                    && (nested_item->kind == CM_AST_ITEM_CONST
                        || nested_item->kind == CM_AST_ITEM_STATIC)
                    && cm_u_find_item_definition(state, state->source,
                        state->nested[nested].item, &definition)) {
                    out->kind = CM_U_RESOLVED_DEFINITION;
                    out->definition = definition;
                }
                /* A body-local enum's variants (`STATX_STATE::Present`
                 * in std's try_statx) are reserved HIR variant records
                 * naming the enum item. */
                if (nested_item != NULL
                    && nested_item->kind == CM_AST_ITEM_ENUM
                    && count > consumed
                    && cm_u_find_item_definition(state, state->source,
                        state->nested[nested].item, &definition)) {
                    const CmHirDefinition *record =
                        cm_hir_lookup_definition(state->hir, definition);
                    CmInternId variant_name = cm_u_intern_ast(state,
                        path->segments[consumed].name);
                    uint32_t variant;
                    for (variant = 0u; record != NULL
                            && record->kind == CM_HIR_DEFINITION_ITEM
                            && variant < nested_item->data.enum_item
                                .variant_count; ++variant) {
                        size_t scan;
                        if (cm_u_intern_ast(state, nested_item->data
                                .enum_item.variants[variant].name)
                            != variant_name) continue;
                        for (scan = 0u; scan < state->hir->definitions.len;
                             ++scan) {
                            const CmHirDefinition *candidate =
                                (const CmHirDefinition *)cm_vec_at_const(
                                    &state->hir->definitions, scan);
                            if (candidate == NULL
                                || candidate->kind
                                    != CM_HIR_DEFINITION_ENUM_VARIANT
                                || candidate->entity.enum_variant
                                    .enum_item_id
                                    != record->entity.item_id
                                || candidate->entity.enum_variant
                                    .variant_index != variant) continue;
                            out->kind = CM_U_RESOLVED_VARIANT;
                            out->definition = candidate->id;
                            out->rest_from = consumed + 1u;
                            return 1;
                        }
                        break;
                    }
                }
            }
            return 1;
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
    if (!path->absolute && state->body_use_count != 0u) {
        const CmUBodyUse *entry = cm_u_body_use_named(state,
            path->segments[0].name);
        if (entry != NULL && cm_u_resolve_substituted(state, entry, path,
                1u, prefer_type, out))
            return 1;
    }
    {
        int first_type = prefer_type;
        int attempt;
        for (attempt = 0; attempt < 2; ++attempt) {
            CmResolveNamespace ns = (attempt == 0) == (first_type != 0)
                ? CM_RESOLVE_NAMESPACE_TYPE : CM_RESOLVE_NAMESPACE_VALUE;
            CmImportLookupStatus status = cm_import_resolve_path_checked(
                state->imports, state->graph, state->revision, state->module,
                path->absolute, views, count, ns, &binding);
            if (status == CM_IMPORT_LOOKUP_OK
                && cm_u_binding_definition(state, &binding, out)) {
                out->rest_from = count;
                return 1;
            }
            if (cm_u_debug_enabled() && count > 1u)
                fprintf(stderr, "ubody-debug lookup ns=%d status=%d"
                    " source=%u item=%u dep=%u\n", (int)ns, (int)status,
                    (unsigned)binding.declaration.source,
                    (unsigned)binding.declaration.item,
                    (unsigned)binding.dependency);
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
    if (!path->absolute) {
        size_t use_index;
        for (use_index = state->body_use_count; use_index != 0u;
                --use_index) {
            const CmUBodyUse *entry = &state->body_uses[use_index - 1u];
            if (!entry->is_glob) continue;
            if (cm_u_resolve_substituted(state, entry, path, 0u,
                    prefer_type, out))
                return 1;
        }
    }
    out->kind = CM_U_RESOLVED_UNRESOLVED;
    out->rest_from = 0u;
    cm_u_debug_path(state, path, "unresolved");
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

/* Code point of a `'c'` / `b'c'` literal body (between the quotes):
 * plain, `\n`-style escapes, `\x41`, `\u{1F600}`; 0 if malformed. */
static uint64_t cm_u_decode_char(const char *body, size_t length)
{
    size_t at;
    uint64_t value = 0u;
    if (length == 0u) return 0u;
    if (body[0] != '\\') {
        /* UTF-8 decode of the first scalar. */
        unsigned char lead = (unsigned char)body[0];
        size_t extra = lead >= 0xF0u ? 3u : lead >= 0xE0u ? 2u
            : lead >= 0xC0u ? 1u : 0u;
        value = extra == 0u ? lead : extra == 1u ? (lead & 0x1Fu)
            : extra == 2u ? (lead & 0x0Fu) : (lead & 0x07u);
        for (at = 1u; at <= extra && at < length; ++at)
            value = (value << 6) | ((unsigned char)body[at] & 0x3Fu);
        return value;
    }
    if (length < 2u) return 0u;
    switch (body[1]) {
    case 'n': return 10u;
    case 't': return 9u;
    case 'r': return 13u;
    case '0': return 0u;
    case '\\': return 92u;
    case '\'': return 39u;
    case '"': return 34u;
    case 'x':
    case 'u': {
        for (at = body[1] == 'u' ? 3u : 2u; at < length; ++at) {
            char c = body[at];
            if (c == '}') break;
            if (c == '_') continue;
            value <<= 4;
            if (c >= '0' && c <= '9') value |= (uint64_t)(c - '0');
            else if (c >= 'a' && c <= 'f') value |= (uint64_t)(10 + c - 'a');
            else if (c >= 'A' && c <= 'F') value |= (uint64_t)(10 + c - 'A');
            else return 0u;
        }
        return value;
    }
    default: return 0u;
    }
}

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
            if (length >= 4u)
                node->data.literal.value_low = cm_u_decode_char(text + 2,
                    length - 3u);
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
        if (length >= 3u)
            node->data.literal.value_low = cm_u_decode_char(text + 1,
                length - 2u);
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
    case CM_AST_PATTERN_REST:
        /* `..` — as a binding subpattern (`rest @ ..`) it marks the
         * binding as a subslice binding; WILD would lose that. */
        node.kind = CM_U_PAT_REST;
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
        size_t saved_uses = state->body_use_count;
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
            if (item == NULL) continue;
            if (item->kind == CM_AST_ITEM_USE) {
                cm_u_collect_body_use(state, item);
                continue;
            }
            if (item->kind == CM_AST_ITEM_EXTERN_BLOCK) {
                /* A body-local `extern { fn f(); }` (core's panic_fmt
                 * names its panic_impl this way) exposes its declarations
                 * by name in the block. */
                uint32_t child;
                for (child = 0u; child < item->data.extern_block_item
                        .item_count; ++child) {
                    CmAstItemId child_id =
                        item->data.extern_block_item.items[child];
                    const CmAstItem *foreign = cm_ast_get_item(state->ast,
                        child_id);
                    if (foreign == NULL || foreign->name == CM_INTERN_ID_NONE
                        || state->nested_count == CM_U_NESTED_LIMIT)
                        continue;
                    state->nested[state->nested_count].name =
                        cm_u_intern_ast(state, foreign->name);
                    state->nested[state->nested_count].item = child_id;
                    state->nested[state->nested_count].module =
                        CM_INTERN_ID_NONE;
                    state->nested_count += 1u;
                }
                continue;
            }
            if (item->kind == CM_AST_ITEM_MODULE) {
                /* A body-local `mod m { .. }`: its items are visible as
                 * `m::item` throughout the block. */
                uint32_t child;
                if (item->name == CM_INTERN_ID_NONE) continue;
                for (child = 0u; child < item->data.module_item.item_count;
                     ++child) {
                    CmAstItemId child_id = item->data.module_item.items[child];
                    const CmAstItem *member = cm_ast_get_item(state->ast,
                        child_id);
                    if (member == NULL || member->name == CM_INTERN_ID_NONE
                        || state->nested_count == CM_U_NESTED_LIMIT)
                        continue;
                    state->nested[state->nested_count].name =
                        cm_u_intern_ast(state, member->name);
                    state->nested[state->nested_count].item = child_id;
                    state->nested[state->nested_count].module =
                        cm_u_intern_ast(state, item->name);
                    state->nested_count += 1u;
                }
                continue;
            }
            if (item->name == CM_INTERN_ID_NONE
                || state->nested_count == CM_U_NESTED_LIMIT) continue;
            state->nested[state->nested_count].name = cm_u_intern_ast(state,
                item->name);
            state->nested[state->nested_count].item =
                stmt->data.item_stmt.item;
            state->nested[state->nested_count].module = CM_INTERN_ID_NONE;
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
        state->body_use_count = saved_uses;
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
            if (getenv("CMRUSTC_UBODY_DEBUG") != NULL) {
                const CmInternedString *text = last == CM_INTERN_ID_NONE
                    ? NULL : cm_ast_get_string(state->ast, last);
                fprintf(stderr, "UBODY unexpanded macro %.*s! segments=%u\n",
                    text == NULL ? 1 : (int)text->len,
                    text == NULL ? "?" : (const char *)text->bytes,
                    path == NULL ? 0u : (unsigned)path->segment_count);
            }
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
    /* Select the graph bundle owning this body's crate (M9-03). */
    state->graph = state->local_graph;
    state->revision = state->local_revision;
    state->imports = state->local_imports;
    state->modules = state->local_modules;
    if (item != NULL
        && cm_hir_module_map_lookup_module(state->modules, state->graph,
            state->revision, state->hir, item->owner_module, &module)
            != CM_HIR_MODULE_MAP_OK) {
        size_t dependency_index;
        for (dependency_index = 0u;
             dependency_index < state->dependency_count;
             ++dependency_index) {
            const CmUBodyDependency *dependency =
                &state->dependencies[dependency_index];
            if (cm_hir_module_map_lookup_module(dependency->modules,
                    dependency->graph, dependency->revision, state->hir,
                    item->owner_module, &module)
                    == CM_HIR_MODULE_MAP_OK) {
                state->graph = dependency->graph;
                state->revision = dependency->revision;
                state->imports = dependency->imports;
                state->modules = dependency->modules;
                break;
            }
        }
        if (dependency_index == state->dependency_count) item = NULL;
    }
    if (item == NULL
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
    const CmHirModuleMap *modules, const CmUBodyDependency *dependencies,
    size_t dependency_count)
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
    state->local_graph = graph;
    state->local_revision = revision;
    state->local_imports = imports;
    state->local_modules = modules;
    state->dependencies = dependencies;
    state->dependency_count = dependency_count;
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
