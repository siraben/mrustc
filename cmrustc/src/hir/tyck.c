#include "cm/hir/tyck.h"
#include "cm/alloc.h"
#include "cm/buf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Lenient inference typeck (M9-04).  See tyck.h.
 */

#define CM_TYCK_MAX_ARGS 64u
#define CM_TYCK_PENDING_ROUNDS 6u

/* ------------------------------------------------------------------ */
/* Global index                                                         */

typedef struct CmTyckImpl {
    const CmHirItem *item;
    CmTyId self_pattern;     /* impl self type with generics as PARAM */
    CmHirDefId trait_def;    /* none for inherent impls */
    int has_trait;
} CmTyckImpl;

typedef struct CmTyckChild {
    CmHirDefId parent;
    const CmHirItem *item;
} CmTyckChild;

typedef struct CmTyckState {
    CmTyckSet *set;
    CmTyArena *arena;
    const CmHirContext *hir;
    const CmUBodySet *ubodies;
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
    CmTyckImpl *impls;
    size_t impl_count;
    CmTyckChild *children; /* sorted by parent def */
    size_t child_count;
    /* source id -> AST, for items reached through re-exports. */
    struct { CmSourceId source; const CmAst *ast; } *source_asts;
    size_t source_ast_count;
    /* Lazily resolved `core::ops` range types. */
    int range_resolved;
    CmHirDefId range_full;
    CmHirDefId range;
    CmHirDefId range_from;
    CmHirDefId range_to;
    CmHirDefId range_inclusive;
    CmHirDefId range_to_inclusive;
    CmTyckResult result;
} CmTyckState;

typedef struct CmTyckInstance {
    CmHirGenericParamId parameters[CM_TYCK_MAX_ARGS];
    CmTyId types[CM_TYCK_MAX_ARGS];
    uint32_t count;
    CmTySubst subst;
} CmTyckInstance;

/* Per-body environment. */
typedef struct CmTyckLoop {
    CmInternId label;
    CmTyId type;
    int is_loop; /* `loop` may break with a value */
} CmTyckLoop;

typedef struct CmTyckEnv {
    CmTyckState *state;
    const CmUBody *ub;
    CmTyckBody *out;
    const CmHirItem *item;
    const CmHirItem *parent;
    CmTyId self_type;
    CmModuleId module;
    const CmAst *ast;
    CmSourceId source;
    CmTyId return_type;
    CmTyckInstance self_subst;
    CmTyckLoop loops[64];
    size_t loop_count;
    CmUExprId *pending;
    size_t pending_count;
    size_t pending_capacity;
    int in_pending_pass;
    int progressed;
} CmTyckEnv;

static int cm_tyck_debug_budget = -1;

static void cm_tyck_debug_pair(CmTyckEnv *env, const char *what, CmTyId a,
    CmTyId b)
{
    CmStrBuf left;
    CmStrBuf right;
    const char *limit;
    if (getenv("CM_TYCK_DEBUG") == NULL) return;
    if (cm_tyck_debug_budget < 0) {
        limit = getenv("CM_TYCK_DEBUG_LIMIT");
        cm_tyck_debug_budget = limit == NULL ? 40 : atoi(limit);
    }
    if (cm_tyck_debug_budget == 0) return;
    cm_tyck_debug_budget -= 1;
    cm_str_buf_init(&left);
    cm_str_buf_init(&right);
    cm_ty_print(env->state->arena, env->state->hir, a, &left);
    cm_ty_print(env->state->arena, env->state->hir, b, &right);
    fprintf(stderr, "TYCK %s: %s vs %s\n", what, cm_str_buf_c_str(&left),
        cm_str_buf_c_str(&right));
    cm_str_buf_destroy(&right);
    cm_str_buf_destroy(&left);
}

static void cm_tyck_debug_span(CmTyckEnv *env, const CmUExpr *expr)
{
    const CmInternedString *name = NULL;
    if (getenv("CM_TYCK_DEBUG") == NULL || cm_tyck_debug_budget <= 0) return;
    if (env->item != NULL)
        name = cm_interner_get(&env->state->hir->strings, env->item->name);
    fprintf(stderr, "TYCK   at source=%lu %lu..%lu fn=%.*s\n",
        (unsigned long)expr->span.source, (unsigned long)expr->span.start,
        (unsigned long)expr->span.end,
        name == NULL ? 1 : (int)name->len,
        name == NULL ? "?" : (const char *)name->bytes);
}

static void cm_tyck_error(CmTyckEnv *env, const char *reason)
{
    CmTyckResult *result = &env->state->result;
    size_t index;
    env->out->error_nodes += 1u;
    if (env->out->first_error == NULL) env->out->first_error = reason;
    for (index = 0u; index < result->error_class_count; ++index)
        if (strcmp(result->error_classes[index].reason, reason) == 0) {
            result->error_classes[index].count += 1u;
            return;
        }
    if (index < CM_TYCK_ERROR_CLASSES) {
        result->error_classes[index].reason = reason;
        result->error_classes[index].count = 1u;
        result->error_class_count += 1u;
    }
}

/* ------------------------------------------------------------------ */
/* Set                                                                  */

void cm_tyck_set_init(CmTyckSet *set)
{
    memset(set, 0, sizeof(*set));
    cm_ty_arena_init(&set->arena);
    cm_vec_init(&set->bodies, sizeof(CmTyckBody));
    cm_vec_init(&set->storage, sizeof(CmTyId));
}

void cm_tyck_set_destroy(CmTyckSet *set)
{
    size_t index;
    for (index = 0u; index < set->bodies.len; ++index) {
        CmTyckBody *body = (CmTyckBody *)cm_vec_at(&set->bodies, index);
        cm_free(body->expr_types);
        cm_free(body->pat_types);
        cm_free(body->local_types);
    }
    cm_vec_destroy(&set->storage);
    cm_vec_destroy(&set->bodies);
    cm_ty_arena_destroy(&set->arena);
}

const CmTyckBody *cm_tyck_get(const CmTyckSet *set, CmHirBodyId body)
{
    if (body == CM_HIR_BODY_NONE || (size_t)body > set->bodies.len)
        return NULL;
    return (const CmTyckBody *)cm_vec_at_const(&set->bodies,
        (size_t)body - 1u);
}

/* ------------------------------------------------------------------ */
/* Names and items                                                      */

static int cm_tyck_name_is(const CmTyckState *state, CmInternId hir_name,
    CmInternId ubody_name)
{
    const CmInternedString *a = cm_interner_get(&state->hir->strings,
        hir_name);
    const CmInternedString *b = cm_interner_get(&state->ubodies->strings,
        ubody_name);
    return a != NULL && b != NULL && a->len == b->len
        && memcmp(a->bytes, b->bytes, a->len) == 0;
}

/* Intern a compiler-known name into the ubody interner (append-only,
 * so the const cast is safe): lookups must not depend on whether the
 * name happens to appear textually in some body. */
static CmInternId cm_tyck_intern_text(const CmTyckState *state,
    const char *text, size_t length)
{
    return cm_interner_intern((CmInterner *)&state->ubodies->strings,
        (const unsigned char *)text, length);
}

static const CmHirItem *cm_tyck_item(const CmTyckState *state, CmHirDefId def)
{
    const CmHirDefinition *record = cm_hir_lookup_definition(state->hir, def);
    if (record == NULL || record->kind != CM_HIR_DEFINITION_ITEM) return NULL;
    return cm_hir_get_item(state->hir, record->entity.item_id);
}

static const CmHirItem *cm_tyck_parent_item(const CmTyckState *state,
    const CmHirItem *item)
{
    if (item == NULL || cm_hir_def_id_is_none(item->parent_definition))
        return NULL;
    return cm_tyck_item(state, item->parent_definition);
}

static int cm_tyck_child_compare(const void *left, const void *right)
{
    const CmTyckChild *a = (const CmTyckChild *)left;
    const CmTyckChild *b = (const CmTyckChild *)right;
    if (a->parent.crate_id != b->parent.crate_id)
        return a->parent.crate_id < b->parent.crate_id ? -1 : 1;
    if (a->parent.index != b->parent.index)
        return a->parent.index < b->parent.index ? -1 : 1;
    return 0;
}

/* First child index of `parent`, or child_count when none. */
static size_t cm_tyck_children_start(const CmTyckState *state,
    CmHirDefId parent)
{
    size_t low = 0u;
    size_t high = state->child_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        const CmTyckChild *child = &state->children[middle];
        if (child->parent.crate_id < parent.crate_id
            || (child->parent.crate_id == parent.crate_id
                && child->parent.index < parent.index))
            low = middle + 1u;
        else
            high = middle;
    }
    return low;
}

static const CmHirItem *cm_tyck_child_named(const CmTyckState *state,
    CmHirDefId parent, CmInternId ubody_name, CmHirItemKind kind_or_any)
{
    size_t index = cm_tyck_children_start(state, parent);
    for (; index < state->child_count; ++index) {
        const CmTyckChild *child = &state->children[index];
        if (!cm_hir_def_id_equal(child->parent, parent)) break;
        if ((kind_or_any == (CmHirItemKind)-1 || child->item->kind
                == kind_or_any)
            && cm_tyck_name_is(state, child->item->name, ubody_name))
            return child->item;
    }
    return NULL;
}

static void cm_tyck_ingest_graph_asts(CmTyckState *state,
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
        *used += 1u;
    }
}

static void cm_tyck_build_source_asts(CmTyckState *state)
{
    size_t count = cm_module_graph_module_count(state->graph);
    size_t dependency_index;
    size_t used = 0u;
    for (dependency_index = 0u; dependency_index < state->dependency_count;
         ++dependency_index)
        count += cm_module_graph_module_count(
            state->dependencies[dependency_index].graph);
    state->source_asts = (void *)cm_alloc_zeroed(count + 1u,
        sizeof(*state->source_asts));
    cm_tyck_ingest_graph_asts(state, state->graph, &used);
    for (dependency_index = 0u; dependency_index < state->dependency_count;
         ++dependency_index)
        cm_tyck_ingest_graph_asts(state,
            state->dependencies[dependency_index].graph, &used);
    state->source_ast_count = used;
}

/*
 * The HIR item declared by a resolver binding, matched by AST span.  The
 * declaring AST is found by source id: a re-exported item is declared in a
 * different module than the one the path resolved from.
 */
static const CmHirItem *cm_tyck_item_from_binding(const CmTyckState *state,
    const CmResolvedBinding *binding)
{
    size_t index;
    if (binding->declaration.item == CM_AST_ITEM_NONE) return NULL;
    for (index = 0u; index < state->hir->items.len; ++index) {
        const CmHirItem *item = (const CmHirItem *)cm_vec_at_const(
            &state->hir->items, index);
        if (item != NULL
            && item->ast_source == binding->declaration.source
            && item->ast_item == binding->declaration.item) return item;
    }
    return NULL;
}

/* Resolve a crate-root-relative type path such as `ops::Range`. */
static int cm_tyck_lookup_type_path(CmTyckState *state,
    const char *const *segments, size_t count, CmHirDefId *out)
{
    CmResolvePathSegmentView views[8];
    CmResolvedBinding binding;
    CmModuleId root;
    const CmHirItem *item;
    size_t index;
    if (count > 8u || !cm_module_graph_get_root(state->graph, &root))
        return 0;
    for (index = 0u; index < count; ++index) {
        views[index].bytes = (const unsigned char *)segments[index];
        views[index].length = strlen(segments[index]);
    }
    if (cm_import_resolve_path_checked(state->imports, state->graph,
            state->revision, root, 0, views, count,
            CM_RESOLVE_NAMESPACE_TYPE, &binding) != CM_IMPORT_LOOKUP_OK)
        return 0;
    item = cm_tyck_item_from_binding(state, &binding);
    if (item == NULL) return 0;
    *out = item->definition;
    return 1;
}

static void cm_tyck_resolve_ranges(CmTyckState *state)
{
    static const char *const full[] = { "ops", "RangeFull" };
    static const char *const half[] = { "ops", "Range" };
    static const char *const from[] = { "ops", "RangeFrom" };
    static const char *const to[] = { "ops", "RangeTo" };
    static const char *const inclusive[] = { "ops", "RangeInclusive" };
    static const char *const to_inclusive[] = { "ops", "RangeToInclusive" };
    if (state->range_resolved) return;
    state->range_resolved = 1;
    (void)cm_tyck_lookup_type_path(state, full, 2u, &state->range_full);
    (void)cm_tyck_lookup_type_path(state, half, 2u, &state->range);
    (void)cm_tyck_lookup_type_path(state, from, 2u, &state->range_from);
    (void)cm_tyck_lookup_type_path(state, to, 2u, &state->range_to);
    (void)cm_tyck_lookup_type_path(state, inclusive, 2u,
        &state->range_inclusive);
    (void)cm_tyck_lookup_type_path(state, to_inclusive, 2u,
        &state->range_to_inclusive);
}

static const CmHirItem *cm_tyck_child_named_hir(const CmTyckState *state,
    CmHirDefId parent, CmInternId hir_name)
{
    size_t index = cm_tyck_children_start(state, parent);
    const CmInternedString *wanted = cm_interner_get(&state->hir->strings,
        hir_name);
    if (wanted == NULL) return NULL;
    for (; index < state->child_count; ++index) {
        const CmTyckChild *child = &state->children[index];
        const CmInternedString *name;
        if (!cm_hir_def_id_equal(child->parent, parent)) break;
        name = cm_interner_get(&state->hir->strings, child->item->name);
        if (name != NULL && name->len == wanted->len
            && memcmp(name->bytes, wanted->bytes, name->len) == 0)
            return child->item;
    }
    return NULL;
}

static void cm_tyck_build_index(CmTyckState *state)
{
    size_t index;
    size_t impls = 0u;
    size_t children = 0u;
    for (index = 0u; index < state->hir->items.len; ++index) {
        const CmHirItem *item = (const CmHirItem *)cm_vec_at_const(
            &state->hir->items, index);
        if (item == NULL) continue;
        if (item->kind == CM_HIR_ITEM_IMPL) impls += 1u;
        if (!cm_hir_def_id_is_none(item->parent_definition)) children += 1u;
    }
    state->impls = (CmTyckImpl *)cm_alloc_zeroed(impls + 1u,
        sizeof(*state->impls));
    state->children = (CmTyckChild *)cm_alloc_zeroed(children + 1u,
        sizeof(*state->children));
    for (index = 0u; index < state->hir->items.len; ++index) {
        const CmHirItem *item = (const CmHirItem *)cm_vec_at_const(
            &state->hir->items, index);
        if (item == NULL) continue;
        if (item->kind == CM_HIR_ITEM_IMPL) {
            CmTyckImpl *entry = &state->impls[state->impl_count++];
            entry->item = item;
            entry->self_pattern = cm_ty_from_hir(state->arena, state->hir,
                item->data.impl_item.self_type);
            entry->has_trait = item->data.impl_item.has_trait;
            entry->trait_def = item->data.impl_item.trait_type.definition;
        }
        if (!cm_hir_def_id_is_none(item->parent_definition)) {
            CmTyckChild *child = &state->children[state->child_count++];
            child->parent = item->parent_definition;
            child->item = item;
        }
    }
    qsort(state->children, state->child_count, sizeof(*state->children),
        cm_tyck_child_compare);
}

/* ------------------------------------------------------------------ */
/* Generic instantiation                                                */


static void cm_tyck_instance_init(CmTyckInstance *instance, CmTyId self_type)
{
    memset(instance, 0, sizeof(*instance));
    instance->subst.parameters = instance->parameters;
    instance->subst.types = instance->types;
    instance->subst.count = 0u;
    instance->subst.self_type = self_type;
}

/*
 * The substitution points into the instance's own arrays, so a copied
 * instance must re-point it before use.
 */
static const CmTySubst *cm_tyck_subst_of(CmTyckInstance *instance)
{
    instance->subst.parameters = instance->parameters;
    instance->subst.types = instance->types;
    instance->subst.count = instance->count;
    return &instance->subst;
}

static void cm_tyck_instance_add(CmTyckInstance *instance,
    CmHirGenericParamId parameter, CmTyId type)
{
    if (instance->count == CM_TYCK_MAX_ARGS) return;
    instance->parameters[instance->count] = parameter;
    instance->types[instance->count] = type;
    instance->count += 1u;
    instance->subst.count = instance->count;
}

/* Fresh variables for every generic parameter of `item` (own only). */
static void cm_tyck_instance_fresh(CmTyckEnv *env, CmTyckInstance *instance,
    const CmHirItem *item)
{
    uint32_t index;
    if (item == NULL || item->generic_parameter_start
            == CM_HIR_GENERIC_PARAM_NONE) return;
    for (index = 0u; index < item->generic_parameter_count; ++index) {
        CmHirGenericParamId id = item->generic_parameter_start + index;
        const CmHirGenericParam *parameter = cm_hir_get_generic_param(
            env->state->hir, id);
        CmTyId fresh;
        if (parameter == NULL) continue;
        if (parameter->kind == CM_HIR_GENERIC_LIFETIME)
            fresh = env->state->arena->lifetime;
        else if (parameter->kind == CM_HIR_GENERIC_CONST)
            fresh = env->state->arena->const_unknown;
        else
            fresh = cm_ty_fresh(env->state->arena, CM_HIR_INFER_GENERAL);
        cm_tyck_instance_add(instance, id, fresh);
    }
}

/* Bind `item`'s own generics from an explicit type list (FN_DEF args). */
static void cm_tyck_instance_from_args(CmTyckInstance *instance,
    const CmHirItem *item, const CmHirItem *parent, const CmTyId *args,
    uint32_t count)
{
    uint32_t used = 0u;
    uint32_t index;
    if (parent != NULL
        && parent->generic_parameter_start != CM_HIR_GENERIC_PARAM_NONE) {
        for (index = 0u; index < parent->generic_parameter_count
                && used < count; ++index)
            cm_tyck_instance_add(instance,
                parent->generic_parameter_start + index, args[used++]);
    }
    if (item != NULL
        && item->generic_parameter_start != CM_HIR_GENERIC_PARAM_NONE) {
        for (index = 0u; index < item->generic_parameter_count
                && used < count; ++index)
            cm_tyck_instance_add(instance,
                item->generic_parameter_start + index, args[used++]);
    }
}

/* FN_DEF type for `item` with parent + own generics instantiated fresh. */
static CmTyId cm_tyck_fn_def(CmTyckEnv *env, const CmHirItem *item,
    const CmHirItem *parent, CmTyId self_type, CmTyckInstance *out_instance)
{
    CmTyckInstance instance;
    /* A trait method reached as a plain definition: Self is inferred from
     * use, and the call site expects it in the first argument slot. */
    if (parent != NULL && parent->kind == CM_HIR_ITEM_TRAIT
        && self_type == CM_TY_NONE) {
        CmTyId args[CM_TYCK_MAX_ARGS];
        uint32_t index;
        CmTyId self = cm_ty_fresh(env->state->arena, CM_HIR_INFER_GENERAL);
        cm_tyck_instance_init(&instance, self);
        cm_tyck_instance_fresh(env, &instance, parent);
        cm_tyck_instance_fresh(env, &instance, item);
        if (out_instance != NULL) *out_instance = instance;
        args[0] = self;
        for (index = 0u; index < instance.count
                && index + 1u < CM_TYCK_MAX_ARGS; ++index)
            args[index + 1u] = instance.types[index];
        return cm_ty_with_def(env->state->arena, CM_TY_FN_DEF,
            item->definition, args, instance.count + 1u);
    }
    cm_tyck_instance_init(&instance, self_type);
    cm_tyck_instance_fresh(env, &instance, parent);
    cm_tyck_instance_fresh(env, &instance, item);
    if (out_instance != NULL) *out_instance = instance;
    return cm_ty_with_def(env->state->arena, CM_TY_FN_DEF, item->definition,
        instance.types, instance.count);
}

/* ADT type for a struct/enum/union item with fresh generics. */
static CmTyId cm_tyck_adt_fresh(CmTyckEnv *env, const CmHirItem *item,
    CmTyckInstance *out_instance)
{
    CmTyckInstance instance;
    cm_tyck_instance_init(&instance, CM_TY_NONE);
    cm_tyck_instance_fresh(env, &instance, item);
    if (out_instance != NULL) *out_instance = instance;
    return cm_ty_with_def(env->state->arena, CM_TY_ADT, item->definition,
        instance.types, instance.count);
}

/* Rebuild the instance of an ADT/FN_DEF type from its stored arguments. */
static void cm_tyck_instance_of_type(CmTyckEnv *env, CmTyId type,
    const CmHirItem *item, const CmHirItem *parent, CmTyId self_type,
    CmTyckInstance *out)
{
    const CmTy *ty = cm_ty_get(env->state->arena, type);
    cm_tyck_instance_init(out, self_type);
    if (ty == NULL) return;
    cm_tyck_instance_from_args(out, item, parent, ty->children, ty->count);
}

/* ------------------------------------------------------------------ */
/* Non-binding structural match                                         */

/*
 * Does `pattern` (an impl self type with PARAM wildcards) match `type`?
 * Inference variables in `type` match anything; nothing is bound.
 */
static int cm_tyck_matches(CmTyckEnv *env, CmTyId pattern, CmTyId type)
{
    CmTyArena *arena = env->state->arena;
    const CmTy *p;
    const CmTy *t;
    uint32_t index;
    pattern = cm_ty_resolve(arena, pattern);
    type = cm_ty_resolve(arena, type);
    if (pattern == type) return 1;
    p = cm_ty_get(arena, pattern);
    t = cm_ty_get(arena, type);
    if (p == NULL || t == NULL) return 0;
    if (p->kind == CM_TY_PARAM || p->kind == CM_TY_CONST_PARAM
        || p->kind == CM_TY_INFER || p->kind == CM_TY_ERROR
        || p->kind == CM_TY_CONST_UNKNOWN || p->kind == CM_TY_LIFETIME
        || p->kind == CM_TY_OPAQUE) return 1;
    if (t->kind == CM_TY_INFER || t->kind == CM_TY_ERROR
        || t->kind == CM_TY_CONST_UNKNOWN || t->kind == CM_TY_LIFETIME
        || t->kind == CM_TY_CONST_PARAM
        /* An unresolved projection is an unknown: it may well be the
         * pattern's type (`Option<&T>` vs `Option<<I as
         * Iterator>::Item>`). */
        || t->kind == CM_TY_PROJECTION
        || t->kind == CM_TY_NEVER) return 1;
    if (p->kind != t->kind) return 0;
    if (p->a != t->a && p->kind != CM_TY_CLOSURE) return 0;
    if (!cm_hir_def_id_equal(p->def, t->def)
        || !cm_hir_def_id_equal(p->def2, t->def2)) return 0;
    if (p->lo != t->lo || p->hi != t->hi) return 0;
    if (p->count != t->count) return 0;
    {
        uint32_t count = p->count;
        for (index = 0u; index < count; ++index)
            if (!cm_tyck_matches(env,
                    cm_ty_get(arena, pattern)->children[index],
                    cm_ty_get(arena, type)->children[index])) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Impl and trait item lookup                                           */

/* Method named `name` declared directly on `trait_def` or a supertrait. */
static const CmHirItem *cm_tyck_trait_method(CmTyckEnv *env,
    CmHirDefId trait_def, CmInternId name, unsigned int depth,
    CmHirDefId *out_trait)
{
    const CmHirItem *found = cm_tyck_child_named(env->state, trait_def, name,
        (CmHirItemKind)-1);
    const CmHirItem *trait_item;
    uint32_t index;
    if (found != NULL) {
        *out_trait = trait_def;
        return found;
    }
    if (depth > 8u) return NULL;
    trait_item = cm_tyck_item(env->state, trait_def);
    if (trait_item == NULL || trait_item->kind != CM_HIR_ITEM_TRAIT)
        return NULL;
    for (index = 0u; index < trait_item->data.trait_item.supertrait_count;
            ++index) {
        found = cm_tyck_trait_method(env,
            trait_item->data.trait_item.supertraits[index].trait_type
                .definition, name, depth + 1u, out_trait);
        if (found != NULL) return found;
    }
    return NULL;
}

typedef struct CmTyckFound {
    const CmHirItem *item;       /* the fn/const item */
    const CmHirItem *parent;     /* impl or trait */
    CmTyckInstance instance;     /* parent + item generics */
    CmTyId self_type;            /* the concrete self type used */
    int via_trait_declaration;   /* found on a trait, not an impl */
} CmTyckFound;

/*
 * Associated value (fn or const) named `name` for `self_type`: inherent
 * impls first, then trait impls, then trait bounds for parameters, then
 * dyn principals.  Binds the chosen impl's generics against `self_type`.
 */
/*
 * passes: 1 = inherent impls, 2 = trait impls/bounds/dyn, 3 = both.
 * skip: return the (skip+1)-th impl candidate; predicate/dyn fallbacks
 * participate only at skip 0.
 */
static CmTyId cm_tyck_normalize(CmTyckEnv *env, CmTyId type,
    unsigned int depth);

static int cm_tyck_lookup_assoc_in(CmTyckEnv *env, CmTyId self_type,
    CmInternId name, CmTyckFound *out, unsigned int passes,
    unsigned int skip)
{
    CmTyArena *arena = env->state->arena;
    size_t index;
    const CmTy *self;
    int pass;
    memset(out, 0, sizeof(*out));
    /* Normalize the receiver first — a projection returned by a method
     * signature (`get_unchecked` yielding `<usize as SliceIndex<[T]>>
     * ::Output`) must resolve before candidate matching, including one
     * reference/pointer layer (btree's `(&mut *p).assume_init_mut()`).
     * All lookup callers, including the method-call candidate loop,
     * come through here. */
    self_type = cm_tyck_normalize(env, self_type, 0u);
    self = cm_ty_get(arena, cm_ty_resolve(arena, self_type));
    if (self != NULL && (self->kind == CM_TY_REF
            || self->kind == CM_TY_PTR)) {
        CmTyId pointee = self->children[0];
        int mutability = (int)self->a;
        int is_reference = self->kind == CM_TY_REF;
        CmTyId normalized = cm_tyck_normalize(env, pointee, 0u);
        if (normalized != pointee) {
            self_type = is_reference
                ? cm_ty_ref(arena, normalized, mutability)
                : cm_ty_ptr(arena, normalized, mutability);
        }
    }
    self_type = cm_ty_resolve(arena, self_type);
    self = cm_ty_get(arena, self_type);
    if (self == NULL || self->kind == CM_TY_INFER) return 0;
    /* Pass 0: inherent impls.  Pass 1: specific trait impls that
     * provide the item themselves.  Pass 2: blanket (bare-parameter
     * self) trait impls with the item — `impl<T: Clone>
     * SpecArrayClone for T` must not shadow a type's own impl.
     * Pass 3: declaration fallbacks. */
    for (pass = 0; pass < 4; ++pass) {
        if ((pass == 0 && (passes & 1u) == 0u)
            || (pass != 0 && (passes & 2u) == 0u)) continue;
        for (index = 0u; index < env->state->impl_count; ++index) {
            const CmTyckImpl *impl = &env->state->impls[index];
            const CmHirItem *child;
            CmHirDefId owner_trait;
            if ((pass == 0) != (impl->has_trait == 0)) continue;
            if (pass == 1 || pass == 2) {
                const CmTy *pattern = cm_ty_get(arena, cm_ty_resolve(arena,
                    impl->self_pattern));
                int blanket;
                while (pattern != NULL && (pattern->kind == CM_TY_REF
                        || pattern->kind == CM_TY_PTR))
                    pattern = cm_ty_get(arena, cm_ty_resolve(arena,
                        pattern->children[0]));
                blanket = pattern != NULL
                    && (pattern->kind == CM_TY_PARAM
                        || pattern->kind == CM_TY_SELF);
                if (blanket != (pass == 2)) continue;
            }
            if (!cm_tyck_matches(env, impl->self_pattern, self_type)) continue;
            if (impl->has_trait) {
                child = cm_tyck_child_named(env->state, impl->item->definition,
                    name, (CmHirItemKind)-1);
                if (child == NULL) {
                    const CmHirItem *declared;
                    if (pass == 1 || pass == 2) continue;
                    declared = cm_tyck_trait_method(env,
                        impl->trait_def, name, 0u, &owner_trait);
                    if (declared == NULL) continue;
                    /* Default method: instantiate the trait declaration
                     * with Self = self_type. */
                    if (skip != 0u) {
                        skip -= 1u;
                        continue;
                    }
                    cm_tyck_instance_init(&out->instance, self_type);
                    cm_tyck_instance_fresh(env, &out->instance,
                        cm_tyck_item(env->state, owner_trait));
                    cm_tyck_instance_fresh(env, &out->instance, declared);
                    out->item = declared;
                    out->parent = cm_tyck_item(env->state, owner_trait);
                    out->self_type = self_type;
                    out->via_trait_declaration = 1;
                    return 1;
                }
                if (pass == 3) continue;
            } else {
                child = cm_tyck_child_named(env->state, impl->item->definition,
                    name, (CmHirItemKind)-1);
                if (child == NULL) continue;
            }
            if (skip != 0u) {
                skip -= 1u;
                continue;
            }
            cm_tyck_instance_init(&out->instance, self_type);
            cm_tyck_instance_fresh(env, &out->instance, impl->item);
            (void)cm_ty_unify(arena, self_type, cm_ty_subst(arena,
                impl->self_pattern, cm_tyck_subst_of(&out->instance)));
            cm_tyck_instance_fresh(env, &out->instance, child);
            out->item = child;
            out->parent = impl->item;
            out->self_type = self_type;
            return 1;
        }
    }
    if (skip != 0u) return 0;
    /* Trait bounds in scope: `T: Trait`, and equally
     * `Simd<T, N>: SimdUint` or `P::Searcher: Searcher` — the subject is
     * matched structurally, with its generic parameters as wildcards. */
    if ((passes & 2u) != 0u) {
        const CmHirItem *owners[2];
        int owner;
        owners[0] = env->item;
        owners[1] = env->parent;
        for (owner = 0; owner < 2; ++owner) {
            const CmHirItem *item = owners[owner];
            uint32_t predicate;
            if (item == NULL) continue;
            for (predicate = 0u; predicate < item->predicate_count;
                    ++predicate) {
                const CmHirTraitPredicate *pred = &item->predicates[predicate];
                CmTyId subject = cm_ty_from_hir(arena, env->state->hir,
                    pred->subject);
                CmHirDefId owner_trait;
                const CmHirItem *declared;
                {
                    CmTyKind subject_kind = cm_ty_get(arena,
                        cm_ty_resolve(arena, subject))->kind;
                    if (cm_ty_resolve(arena, subject) != self_type
                        && !(subject_kind == CM_TY_SELF
                            && self->kind == CM_TY_SELF)
                        /* ADT- and projection-shaped subjects
                         * (`Simd<T, N>: SimdUint`, `P::Searcher:
                         * Searcher`) match structurally.  Bare params
                         * and reference-topped subjects (`&mut I:
                         * Iterator`) must match exactly, else the bound
                         * would apply to every (reference) receiver. */
                        && ((subject_kind != CM_TY_ADT
                                && subject_kind != CM_TY_PROJECTION)
                            || !cm_tyck_matches(env, subject, self_type)))
                        continue;
                }
                declared = cm_tyck_trait_method(env,
                    pred->trait_type.definition, name, 0u, &owner_trait);
                if (declared == NULL) continue;
                cm_tyck_instance_init(&out->instance, self_type);
                cm_tyck_instance_fresh(env, &out->instance,
                    cm_tyck_item(env->state, owner_trait));
                cm_tyck_instance_fresh(env, &out->instance, declared);
                out->item = declared;
                out->parent = cm_tyck_item(env->state, owner_trait);
                out->self_type = self_type;
                out->via_trait_declaration = 1;
                return 1;
            }
        }
        /* `Self` inside a trait: the trait's own items. */
        if (self->kind == CM_TY_SELF && env->parent != NULL
            && env->parent->kind == CM_HIR_ITEM_TRAIT) {
            CmHirDefId owner_trait;
            const CmHirItem *declared = cm_tyck_trait_method(env,
                env->parent->definition, name, 0u, &owner_trait);
            if (declared != NULL) {
                cm_tyck_instance_init(&out->instance, self_type);
                cm_tyck_instance_fresh(env, &out->instance,
                    cm_tyck_item(env->state, owner_trait));
                cm_tyck_instance_fresh(env, &out->instance, declared);
                out->item = declared;
                out->parent = cm_tyck_item(env->state, owner_trait);
                out->self_type = self_type;
                out->via_trait_declaration = 1;
                return 1;
            }
        }
    }
    /* A projection receiver: the associated type's declared bounds
     * (`trait Pattern { type Searcher: Searcher<'a>; }`) supply methods. */
    if ((passes & 2u) != 0u && self->kind == CM_TY_PROJECTION) {
        const CmHirItem *associated = cm_tyck_item(env->state, self->def2);
        if (associated != NULL
            && associated->kind == CM_HIR_ITEM_TYPE_ALIAS) {
            uint32_t bound;
            for (bound = 0u;
                    bound < associated->data.type_alias_item.bound_count;
                    ++bound) {
                const CmHirAssociatedTypeBound *type_bound =
                    &associated->data.type_alias_item.bounds[bound];
                CmHirDefId owner_trait;
                const CmHirItem *declared = cm_tyck_trait_method(env,
                    type_bound->trait_type.definition, name, 0u,
                    &owner_trait);
                if (declared == NULL) continue;
                cm_tyck_instance_init(&out->instance, self_type);
                cm_tyck_instance_fresh(env, &out->instance,
                    cm_tyck_item(env->state, owner_trait));
                cm_tyck_instance_fresh(env, &out->instance, declared);
                out->item = declared;
                out->parent = cm_tyck_item(env->state, owner_trait);
                out->self_type = self_type;
                out->via_trait_declaration = 1;
                return 1;
            }
        }
    }
    if ((passes & 2u) != 0u && self->kind == CM_TY_DYN
        && !cm_hir_def_id_is_none(self->def)) {
        CmHirDefId owner_trait;
        const CmHirItem *declared = cm_tyck_trait_method(env, self->def, name,
            0u, &owner_trait);
        if (declared != NULL) {
            cm_tyck_instance_init(&out->instance, self_type);
            cm_tyck_instance_fresh(env, &out->instance,
                cm_tyck_item(env->state, owner_trait));
            cm_tyck_instance_fresh(env, &out->instance, declared);
            out->item = declared;
            out->parent = cm_tyck_item(env->state, owner_trait);
            out->self_type = self_type;
            out->via_trait_declaration = 1;
            return 1;
        }
    }
    return 0;
}

static int cm_tyck_lookup_assoc(CmTyckEnv *env, CmTyId self_type,
    CmInternId name, CmTyckFound *out)
{
    return cm_tyck_lookup_assoc_in(env, self_type, name, out, 3u, 0u);
}

/* Is `def` one of the callable traits? */
static int cm_tyck_is_fn_trait(CmTyckEnv *env, CmHirDefId def)
{
    const CmHirItem *item = cm_tyck_item(env->state, def);
    const CmInternedString *name;
    if (item == NULL || item->kind != CM_HIR_ITEM_TRAIT) return 0;
    name = cm_interner_get(&env->state->hir->strings, item->name);
    return name != NULL
        && ((name->len == 2u && memcmp(name->bytes, "Fn", 2u) == 0)
            || (name->len == 5u && memcmp(name->bytes, "FnMut", 5u) == 0)
            || (name->len == 6u && memcmp(name->bytes, "FnOnce", 6u) == 0));
}

/*
 * A callable bound on `callee` (a generic parameter or `Self`): the
 * predicate's trait argument tuple gives the parameter types and its
 * `Output` equality the return type.
 */
static int cm_tyck_fn_bound(CmTyckEnv *env, CmTyId callee, CmTyId *params,
    uint32_t limit, uint32_t *out_count, CmTyId *out_return)
{
    CmTyArena *arena = env->state->arena;
    const CmHirItem *owners[2];
    int owner;
    callee = cm_ty_resolve(arena, callee);
    owners[0] = env->item;
    owners[1] = env->parent;
    for (owner = 0; owner < 2; ++owner) {
        const CmHirItem *item = owners[owner];
        uint32_t index;
        if (item == NULL) continue;
        for (index = 0u; index < item->predicate_count; ++index) {
            const CmHirTraitPredicate *pred = &item->predicates[index];
            CmTyId subject = cm_ty_subst(arena, cm_ty_from_hir(arena,
                env->state->hir, pred->subject),
                cm_tyck_subst_of(&env->self_subst));
            const CmTy *arguments;
            if (cm_ty_resolve(arena, subject) != callee) continue;
            if (!cm_tyck_is_fn_trait(env, pred->trait_type.definition))
                continue;
            *out_count = 0u;
            *out_return = arena->unit;
            if (pred->trait_type.argument_count != 0u
                && pred->trait_type.arguments[0].kind
                    == CM_HIR_GENERIC_ARG_TYPE) {
                CmTyId tuple = cm_ty_subst(arena, cm_ty_from_hir(arena,
                    env->state->hir, pred->trait_type.arguments[0].data.type),
                    cm_tyck_subst_of(&env->self_subst));
                arguments = cm_ty_get(arena, cm_ty_resolve(arena, tuple));
                if (arguments != NULL && arguments->kind == CM_TY_TUPLE) {
                    *out_count = arguments->count > limit ? limit
                        : arguments->count;
                    memcpy(params, arguments->children,
                        *out_count * sizeof(CmTyId));
                }
            }
            if (pred->equality_count != 0u)
                *out_return = cm_ty_subst(arena, cm_ty_from_hir(arena,
                    env->state->hir, pred->equalities[0].value),
                    cm_tyck_subst_of(&env->self_subst));
            return 1;
        }
    }
    return 0;
}

static int cm_tyck_debug_fn_matches(CmTyckEnv *env);

/*
 * Normalize `<Self as Trait>::Assoc`: through a matching impl's associated
 * type when the self type is concrete, or through an equality on a bound
 * when it is a parameter.  Unresolvable projections are returned as-is and
 * treated leniently by unification.
 */
static CmTyId cm_tyck_normalize(CmTyckEnv *env, CmTyId type,
    unsigned int depth)
{
    CmTyArena *arena = env->state->arena;
    const CmTy *projection;
    CmTyId self_type;
    const CmTy *self;
    const CmHirItem *associated;
    size_t index;
    if (depth > 8u) return type;
    CmHirDefId trait_def;
    CmHirDefId assoc_def;
    CmTyId first_child;
    CmTyKind self_kind;
    CmTyId projection_args[8];
    uint32_t projection_arg_count;
    CmTyId assoc_args[8];
    uint32_t assoc_arg_count;
    type = cm_ty_resolve(arena, type);
    projection = cm_ty_get(arena, type);
    if (projection == NULL || projection->kind != CM_TY_PROJECTION
        || projection->count == 0u) return type;
    /* Copy before recursing: type creation can move the arena. */
    trait_def = projection->def;
    assoc_def = projection->def2;
    first_child = projection->children[0];
    {
        uint32_t trait_arg_total = projection->b;
        uint32_t assoc_total = projection->count > trait_arg_total + 1u
            ? projection->count - 1u - trait_arg_total : 0u;
        projection_arg_count = trait_arg_total > 8u ? 8u : trait_arg_total;
        memcpy(projection_args, projection->children + 1u,
            projection_arg_count * sizeof(CmTyId));
        assoc_arg_count = assoc_total > 8u ? 8u : assoc_total;
        memcpy(assoc_args,
            projection->children + 1u + trait_arg_total,
            assoc_arg_count * sizeof(CmTyId));
    }
    self_type = cm_tyck_normalize(env, first_child, depth + 1u);
    self_type = cm_ty_resolve(arena, self_type);
    self = cm_ty_get(arena, self_type);
    associated = cm_tyck_item(env->state, assoc_def);
    if (self == NULL || associated == NULL) {
        if (cm_tyck_debug_fn_matches(env))
            fprintf(stderr, "TYCK norm-silent assoc=%d self=%d"
                " tdef=(%u,%u) adef=(%u,%u)\n",
                associated != NULL, self != NULL,
                (unsigned)trait_def.crate_id, (unsigned)trait_def.index,
                (unsigned)assoc_def.crate_id, (unsigned)assoc_def.index);
        return type;
    }
    self_kind = self->kind;
    if (self_kind == CM_TY_INFER) {
        if (cm_tyck_debug_fn_matches(env))
            fprintf(stderr, "TYCK norm-silent self-infer\n");
        return type;
    }
    /* An unresolved-projection self must not scan impls: `<<I::Item as
     * Try>::Residual as Residual>::TryType` would leniently match the
     * first Residual impl (the Option flavor) and normalize Result-typed
     * bodies to Option.  Leave it symbolic; coercion treats unresolved
     * projections leniently. */
    if (self_kind == CM_TY_PROJECTION) return type;
    if (self_kind == CM_TY_PARAM || self_kind == CM_TY_SELF) {
        const CmHirItem *owners[2];
        int owner;
        owners[0] = env->item;
        owners[1] = env->parent;
        for (owner = 0; owner < 2; ++owner) {
            const CmHirItem *item = owners[owner];
            uint32_t predicate;
            if (item == NULL) continue;
            for (predicate = 0u; predicate < item->predicate_count;
                    ++predicate) {
                const CmHirTraitPredicate *pred = &item->predicates[predicate];
                uint32_t equality;
                if (!cm_hir_def_id_equal(pred->trait_type.definition,
                        trait_def)) continue;
                if (cm_ty_resolve(arena, cm_ty_from_hir(arena,
                        env->state->hir, pred->subject))
                    != cm_ty_resolve(arena, self_type)) continue;
                for (equality = 0u; equality < pred->equality_count;
                        ++equality)
                    if (cm_hir_def_id_equal(
                            pred->equalities[equality].associated_type,
                            assoc_def)) {
                        CmTyId equality_value = cm_ty_from_hir(arena,
                            env->state->hir,
                            pred->equalities[equality].value);
                        if (equality_value == CM_TY_NONE) continue;
                        return cm_tyck_normalize(env, cm_ty_subst(arena,
                            equality_value,
                            cm_tyck_subst_of(&env->self_subst)),
                            depth + 1u);
                    }
            }
        }
        return type;
    }
    /* Specific impls first: a blanket `impl<T, U: Into<T>> TryFrom<U>
     * for T` must not shadow `impl TryFrom<u16> for u8`. */
    {
        int scan;
        for (scan = 0; scan < 2; ++scan)
        for (index = 0u; index < env->state->impl_count; ++index) {
            const CmTyckImpl *impl = &env->state->impls[index];
            const CmHirItem *definition;
            CmTyckInstance instance;
            const CmTy *pattern;
            int blanket;
            if (!impl->has_trait
                || !cm_hir_def_id_equal(impl->trait_def, trait_def))
                continue;
            pattern = cm_ty_get(arena, cm_ty_resolve(arena,
                impl->self_pattern));
            while (pattern != NULL && (pattern->kind == CM_TY_REF
                    || pattern->kind == CM_TY_PTR))
                pattern = cm_ty_get(arena, cm_ty_resolve(arena,
                    pattern->children[0]));
            blanket = pattern != NULL && (pattern->kind == CM_TY_PARAM
                || pattern->kind == CM_TY_SELF);
            if (blanket != (scan == 1)) continue;
            if (!cm_tyck_matches(env, impl->self_pattern, self_type))
                continue;
            /* Parameterized traits: `SliceIndex<[T]>` and
             * `SliceIndex<str>` both exist for Range; match the
             * projection's trait arguments against the impl's. */
            if (projection_arg_count != 0u
                && impl->item->kind == CM_HIR_ITEM_IMPL) {
                const CmHirNamedType *impl_trait =
                    &impl->item->data.impl_item.trait_type;
                CmTyId impl_args[8];
                uint32_t impl_arg_count = cm_ty_args_from_hir(arena,
                    env->state->hir, impl_trait->arguments,
                    impl_trait->argument_count, impl_args, 8u);
                uint32_t argument;
                int argument_mismatch = 0;
                for (argument = 0u; argument < impl_arg_count
                        && argument < projection_arg_count; ++argument) {
                    /* A projection argument that is itself generic
                     * (param, Self, projection, infer, const) cannot
                     * disambiguate impls — treat it as a wildcard. */
                    const CmTy *pa = cm_ty_get(arena, cm_ty_resolve(arena,
                        projection_args[argument]));
                    if (pa == NULL || pa->kind == CM_TY_PARAM
                        || pa->kind == CM_TY_SELF
                        || pa->kind == CM_TY_PROJECTION
                        || pa->kind == CM_TY_INFER
                        || pa->kind == CM_TY_CONST
                        || pa->kind == CM_TY_CONST_PARAM
                        || pa->kind == CM_TY_CONST_UNKNOWN
                        || pa->kind == CM_TY_LIFETIME) continue;
                    if (!cm_tyck_matches(env, impl_args[argument],
                            projection_args[argument])) {
                        argument_mismatch = 1;
                        break;
                    }
                }
                if (argument_mismatch) continue;
            }
            definition = cm_tyck_child_named_hir(env->state,
                impl->item->definition, associated->name);
            if (definition == NULL
                || definition->kind != CM_HIR_ITEM_TYPE_ALIAS) continue;
            {
                CmTyId target_type = cm_ty_from_hir(arena, env->state->hir,
                    definition->data.type_alias_item.target);
                if (target_type == CM_TY_NONE) continue;
                if (scan == 1 && getenv("CM_TYCK_DEBUG") != NULL) {
                    /* Blanket fallback: report why no specific impl
                     * matched. */
                    size_t scan0_trait = 0u;
                    size_t scan0_self = 0u;
                    size_t probe_index;
                    for (probe_index = 0u;
                            probe_index < env->state->impl_count;
                            ++probe_index) {
                        const CmTyckImpl *other =
                            &env->state->impls[probe_index];
                        const CmTy *other_pattern;
                        if (!other->has_trait
                            || !cm_hir_def_id_equal(other->trait_def,
                                trait_def)) continue;
                        other_pattern = cm_ty_get(arena, cm_ty_resolve(
                            arena, other->self_pattern));
                        while (other_pattern != NULL
                                && (other_pattern->kind == CM_TY_REF
                                    || other_pattern->kind == CM_TY_PTR))
                            other_pattern = cm_ty_get(arena,
                                cm_ty_resolve(arena,
                                    other_pattern->children[0]));
                        if (other_pattern != NULL
                            && (other_pattern->kind == CM_TY_PARAM
                                || other_pattern->kind == CM_TY_SELF))
                            continue;
                        scan0_trait += 1u;
                        if (!cm_tyck_matches(env, other->self_pattern,
                                self_type)) continue;
                        scan0_self += 1u;
                        {
                            const CmHirItem *other_child =
                                cm_tyck_child_named_hir(env->state,
                                    other->item->definition,
                                    associated->name);
                            if (other_child == NULL)
                                fprintf(stderr, "TYCK scan0-reject"
                                    " reason=no-child\n");
                            else if (other_child->kind
                                    != CM_HIR_ITEM_TYPE_ALIAS)
                                fprintf(stderr, "TYCK scan0-reject"
                                    " reason=child-kind=%d\n",
                                    (int)other_child->kind);
                            else if (cm_ty_from_hir(arena, env->state->hir,
                                    other_child->data.type_alias_item
                                        .target) == CM_TY_NONE)
                                fprintf(stderr, "TYCK scan0-reject"
                                    " reason=target-none\n");
                            else
                                fprintf(stderr, "TYCK scan0-reject"
                                    " reason=arg-filter\n");
                        }
                    }
                    fprintf(stderr, "TYCK normalize-blanket scan0_trait="
                        "%lu scan0_self=%lu ",
                        (unsigned long)scan0_trait,
                        (unsigned long)scan0_self);
                    cm_tyck_debug_pair(env, "projection", type, self_type);
                }
                cm_tyck_instance_init(&instance, self_type);
                cm_tyck_instance_fresh(env, &instance, impl->item);
                /* A generic associated type's own parameters bind to
                 * the projection's trailing GAT arguments
                 * (`Self::Cast<T>` -> `type Cast<T2> = Simd<T2, N>`). */
                cm_tyck_instance_fresh(env, &instance, definition);
                {
                    uint32_t own = definition->generic_parameter_count;
                    uint32_t base = instance.count >= own
                        ? instance.count - own : instance.count;
                    uint32_t garg;
                    for (garg = 0u; garg < assoc_arg_count
                            && base + garg < instance.count; ++garg)
                        (void)cm_ty_unify(arena,
                            instance.types[base + garg],
                            assoc_args[garg]);
                }
                (void)cm_ty_unify(arena, self_type, cm_ty_subst(arena,
                    impl->self_pattern, cm_tyck_subst_of(&instance)));
                /* Bind the impl's trait arguments against the
                 * projection's: `SliceIndex<[T]> for usize` normalizing
                 * `<usize as SliceIndex<[MaybeUninit<V>]>>::Output` must
                 * bind T, or Output substitutes to an unbound fresh
                 * variable and downstream method lookup starves. */
                if (impl->item->kind == CM_HIR_ITEM_IMPL
                    && projection_arg_count != 0u) {
                    const CmHirNamedType *bind_trait =
                        &impl->item->data.impl_item.trait_type;
                    CmTyId bind_args[8];
                    uint32_t bind_count = cm_ty_args_from_hir(arena,
                        env->state->hir, bind_trait->arguments,
                        bind_trait->argument_count, bind_args, 8u);
                    uint32_t bind_index;
                    for (bind_index = 0u; bind_index < bind_count
                            && bind_index < projection_arg_count;
                            ++bind_index)
                        (void)cm_ty_unify(arena, cm_ty_subst(arena,
                            bind_args[bind_index],
                            cm_tyck_subst_of(&instance)),
                            projection_args[bind_index]);
                }
                return cm_tyck_normalize(env, cm_ty_subst(arena,
                    target_type, cm_tyck_subst_of(&instance)), depth + 1u);
            }
        }
    }
    if (getenv("CM_TYCK_DEBUG") != NULL
        && (self_kind == CM_TY_ADT || cm_tyck_debug_fn_matches(env))) {
        size_t by_trait = 0u;
        size_t by_self = 0u;
        size_t no_child = 0u;
        for (index = 0u; index < env->state->impl_count; ++index) {
            const CmTyckImpl *impl = &env->state->impls[index];
            if (!impl->has_trait
                || !cm_hir_def_id_equal(impl->trait_def, trait_def))
                continue;
            by_trait += 1u;
            if (!cm_tyck_matches(env, impl->self_pattern, self_type))
                continue;
            by_self += 1u;
            if (cm_tyck_child_named_hir(env->state,
                    impl->item->definition, associated->name) == NULL)
                no_child += 1u;
        }
        fprintf(stderr, "TYCK normalize-miss impls_for_trait=%lu"
            " self_matched=%lu missing_child=%lu ",
            (unsigned long)by_trait, (unsigned long)by_self,
            (unsigned long)no_child);
        cm_tyck_debug_pair(env, "projection", type, self_type);
    }
    return type;
}

/* Signature of a found fn: parameter types (receiver excluded/included
 * as declared) and return type, substituted. */
static uint32_t cm_tyck_signature(CmTyckEnv *env, const CmHirItem *fn,
    const CmTySubst *subst, CmTyId *params, uint32_t limit, CmTyId *out_ret)
{
    const CmHirFunctionSignature *signature;
    uint32_t index;
    uint32_t count;
    CmTyArena *arena = env->state->arena;
    if (fn == NULL || fn->kind != CM_HIR_ITEM_FUNCTION) {
        *out_ret = arena->error;
        return 0u;
    }
    signature = &fn->data.function_item.signature;
    count = signature->parameter_count;
    if (count > limit) count = limit;
    for (index = 0u; index < count; ++index)
        params[index] = cm_ty_subst(arena, cm_ty_from_hir(arena,
            env->state->hir, signature->parameters[index].type), subst);
    *out_ret = cm_ty_subst(arena, cm_ty_from_hir(arena, env->state->hir,
        signature->return_type), subst);
    return count;
}

/* ------------------------------------------------------------------ */
/* Fields                                                               */

static CmTyId cm_tyck_field_type(CmTyckEnv *env, CmTyId adt, CmInternId name,
    int *out_found)
{
    CmTyArena *arena = env->state->arena;
    const CmTy *ty = cm_ty_get(arena, cm_ty_resolve(arena, adt));
    const CmHirItem *item;
    CmTyckInstance instance;
    uint32_t index;
    *out_found = 0;
    if (ty == NULL || ty->kind != CM_TY_ADT) return arena->error;
    item = cm_tyck_item(env->state, ty->def);
    if (item == NULL || (item->kind != CM_HIR_ITEM_STRUCT
            && item->kind != CM_HIR_ITEM_UNION)) return arena->error;
    cm_tyck_instance_of_type(env, cm_ty_resolve(arena, adt), item, NULL,
        CM_TY_NONE, &instance);
    for (index = 0u; index < item->data.aggregate_item.field_count; ++index) {
        const CmHirField *field = &item->data.aggregate_item.fields[index];
        if (cm_tyck_name_is(env->state, field->name, name)) {
            *out_found = 1;
            return cm_ty_subst(arena, cm_ty_from_hir(arena, env->state->hir,
                field->type), cm_tyck_subst_of(&instance));
        }
    }
    return arena->error;
}

static CmTyId cm_tyck_tuple_field_type(CmTyckEnv *env, CmTyId base,
    uint32_t position, int *out_found)
{
    CmTyArena *arena = env->state->arena;
    const CmTy *ty = cm_ty_get(arena, cm_ty_resolve(arena, base));
    *out_found = 0;
    if (ty == NULL) return arena->error;
    if (ty->kind == CM_TY_TUPLE) {
        if (position >= ty->count) return arena->error;
        *out_found = 1;
        return ty->children[position];
    }
    if (ty->kind == CM_TY_ADT) {
        const CmHirItem *item = cm_tyck_item(env->state, ty->def);
        CmTyckInstance instance;
        if (item == NULL || item->kind != CM_HIR_ITEM_STRUCT
            || position >= item->data.aggregate_item.field_count)
            return arena->error;
        cm_tyck_instance_of_type(env, cm_ty_resolve(arena, base), item, NULL,
            CM_TY_NONE, &instance);
        *out_found = 1;
        return cm_ty_subst(arena, cm_ty_from_hir(arena, env->state->hir,
            item->data.aggregate_item.fields[position].type),
            cm_tyck_subst_of(&instance));
    }
    return arena->error;
}

/* Strip references (and, later, Deref) from a receiver type. */
static CmTyId cm_tyck_autoderef(CmTyckEnv *env, CmTyId type, unsigned int steps)
{
    CmTyArena *arena = env->state->arena;
    const CmTy *ty;
    type = cm_ty_resolve(arena, type);
    ty = cm_ty_get(arena, type);
    if (ty == NULL || steps == 0u) return type;
    if (ty->kind == CM_TY_REF || ty->kind == CM_TY_PTR)
        return cm_tyck_autoderef(env, ty->children[0], steps - 1u);
    return type;
}

/* ------------------------------------------------------------------ */
/* AST types written in bodies                                          */

static CmTyId cm_tyck_ast_type(CmTyckEnv *env, CmAstTypeId type_id);

static CmTyId cm_tyck_ast_path_type(CmTyckEnv *env, CmAstPathId path_id)
{
    CmTyArena *arena = env->state->arena;
    const CmAstPath *path = cm_ast_get_path(env->ast, path_id);
    CmResolvePathSegmentView views[CM_TYCK_MAX_ARGS];
    uint32_t count;
    uint32_t index;
    CmResolvedBinding binding;
    static const char *const primitives[] = { NULL, "bool", "char", "str",
        "i8", "i16", "i32", "i64", "i128", "isize", "u8", "u16", "u32",
        "u64", "u128", "usize", "f16", "f32", "f64", "f128" };
    if (path == NULL || path->segment_count == 0u
        || path->segment_count > CM_TYCK_MAX_ARGS) return arena->error;
    count = path->segment_count;
    if (count == 1u && !path->absolute) {
        const CmInternedString *name = cm_ast_get_string(env->ast,
            path->segments[0].name);
        const CmHirItem *owners[2];
        int owner;
        if (name == NULL) return arena->error;
        if (name->len == 4u && memcmp(name->bytes, "Self", 4u) == 0)
            return env->self_type;
        for (index = 1u; index < sizeof(primitives) / sizeof(primitives[0]);
                ++index)
            if (name->len == strlen(primitives[index])
                && memcmp(name->bytes, primitives[index], name->len) == 0) {
                switch ((CmHirPrimitiveKind)index) {
                case CM_HIR_PRIMITIVE_BOOL: return arena->boolean;
                case CM_HIR_PRIMITIVE_CHAR: return arena->character;
                case CM_HIR_PRIMITIVE_STR: return arena->str;
                case CM_HIR_PRIMITIVE_F16: return cm_ty_float(arena,
                    CM_HIR_FLOAT_F16);
                case CM_HIR_PRIMITIVE_F32: return cm_ty_float(arena,
                    CM_HIR_FLOAT_F32);
                case CM_HIR_PRIMITIVE_F64: return arena->f64;
                case CM_HIR_PRIMITIVE_F128: return cm_ty_float(arena,
                    CM_HIR_FLOAT_F128);
                default:
                    return cm_ty_int(arena,
                        (CmHirIntType)(index - CM_HIR_PRIMITIVE_I8));
                }
            }
        owners[0] = env->item;
        owners[1] = env->parent;
        for (owner = 0; owner < 2; ++owner) {
            const CmHirItem *item = owners[owner];
            uint32_t generic;
            if (item == NULL || item->generic_parameter_start
                    == CM_HIR_GENERIC_PARAM_NONE) continue;
            for (generic = 0u; generic < item->generic_parameter_count;
                    ++generic) {
                CmHirGenericParamId id = item->generic_parameter_start
                    + generic;
                const CmHirGenericParam *parameter = cm_hir_get_generic_param(
                    env->state->hir, id);
                const CmInternedString *hir_name = parameter == NULL ? NULL
                    : cm_interner_get(&env->state->hir->strings,
                        parameter->name);
                if (hir_name != NULL && hir_name->len == name->len
                    && memcmp(hir_name->bytes, name->bytes, name->len) == 0)
                    return parameter->kind == CM_HIR_GENERIC_CONST
                        ? cm_ty_simple(arena, CM_TY_CONST_PARAM, (uint32_t)id,
                            0u)
                        : cm_ty_param(arena, id);
            }
        }
    }
    for (index = 0u; index < count; ++index) {
        const CmInternedString *name = cm_ast_get_string(env->ast,
            path->segments[index].name);
        if (name == NULL) return arena->error;
        views[index].bytes = name->bytes;
        views[index].length = name->len;
    }
    if (cm_import_resolve_path_checked(env->state->imports, env->state->graph,
            env->state->revision, env->module, path->absolute, views, count,
            CM_RESOLVE_NAMESPACE_TYPE, &binding) == CM_IMPORT_LOOKUP_OK) {
        /* Locate the HIR item by its declaring AST span; borrow the
         * declaring AST through the graph module of that source. */
        const CmAstPathSegment *last = &path->segments[count - 1u];
        size_t item_index;
        if (binding.primitive_kind != CM_RESOLVE_PRIMITIVE_NONE) {
            switch ((CmHirPrimitiveKind)binding.primitive_kind) {
            case CM_HIR_PRIMITIVE_BOOL: return arena->boolean;
            case CM_HIR_PRIMITIVE_CHAR: return arena->character;
            case CM_HIR_PRIMITIVE_STR: return arena->str;
            case CM_HIR_PRIMITIVE_F16: return cm_ty_float(arena,
                CM_HIR_FLOAT_F16);
            case CM_HIR_PRIMITIVE_F32: return cm_ty_float(arena,
                CM_HIR_FLOAT_F32);
            case CM_HIR_PRIMITIVE_F64: return arena->f64;
            case CM_HIR_PRIMITIVE_F128: return cm_ty_float(arena,
                CM_HIR_FLOAT_F128);
            default:
                return cm_ty_int(arena, (CmHirIntType)(
                    (uint32_t)binding.primitive_kind - CM_HIR_PRIMITIVE_I8));
            }
        }
        {
            const CmHirItem *item = cm_tyck_item_from_binding(env->state,
                &binding);
            if (item == NULL) return cm_ty_fresh(arena,
                CM_HIR_INFER_GENERAL);
            (void)item_index;
            if (item->kind == CM_HIR_ITEM_STRUCT || item->kind
                    == CM_HIR_ITEM_ENUM || item->kind == CM_HIR_ITEM_UNION) {
                CmTyckInstance instance;
                CmTyId adt = cm_tyck_adt_fresh(env, item, &instance);
                uint32_t arg;
                uint32_t used = 0u;
                /* Explicit generic arguments unify with the fresh ones. */
                for (arg = 0u; arg < last->argument_count
                        && used < instance.count; ++arg) {
                    const CmAstGenericArg *ga = &last->arguments[arg];
                    if (ga->kind == CM_AST_GENERIC_TYPE) {
                        while (used < instance.count
                            && cm_ty_get(arena, instance.types[used])->kind
                                == CM_TY_LIFETIME) used += 1u;
                        if (used < instance.count)
                            (void)cm_ty_unify(arena, instance.types[used++],
                                cm_tyck_ast_type(env, ga->type));
                    }
                }
                return adt;
            }
            if (item->kind == CM_HIR_ITEM_TYPE_ALIAS) {
                CmTyckInstance instance;
                cm_tyck_instance_init(&instance, CM_TY_NONE);
                cm_tyck_instance_fresh(env, &instance, item);
                return cm_ty_subst(arena, cm_ty_from_hir(arena,
                    env->state->hir, item->data.type_alias_item.target),
                    cm_tyck_subst_of(&instance));
            }
            if (item->kind == CM_HIR_ITEM_TRAIT)
                return cm_ty_with_def(arena, CM_TY_DYN, item->definition,
                    NULL, 0u);
            if (item->kind == CM_HIR_ITEM_EXTERN_TYPE)
                return cm_ty_with_def(arena, CM_TY_FOREIGN, item->definition,
                    NULL, 0u);
        }
    }
    /* `T::Assoc`, `Self::Assoc`, unresolvable: an inference variable. */
    return cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
}

static CmTyId cm_tyck_ast_type(CmTyckEnv *env, CmAstTypeId type_id)
{
    CmTyArena *arena = env->state->arena;
    const CmAstType *type = cm_ast_get_type(env->ast, type_id);
    if (type == NULL) return cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
    switch (type->kind) {
    case CM_AST_TYPE_INFER:
        return cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
    case CM_AST_TYPE_NEVER:
        return arena->never;
    case CM_AST_TYPE_PATH:
        return cm_tyck_ast_path_type(env, type->path);
    case CM_AST_TYPE_REFERENCE:
        return cm_ty_ref(arena, cm_tyck_ast_type(env, type->child),
            type->is_mutable);
    case CM_AST_TYPE_POINTER:
        return cm_ty_ptr(arena, cm_tyck_ast_type(env, type->child),
            type->is_mutable);
    case CM_AST_TYPE_TUPLE: {
        CmTyId elements[CM_TYCK_MAX_ARGS];
        uint32_t index;
        uint32_t count = type->element_count;
        if (count > CM_TYCK_MAX_ARGS) return arena->error;
        for (index = 0u; index < count; ++index)
            elements[index] = cm_tyck_ast_type(env,
                cm_ast_get_type(env->ast, type_id)->elements[index]);
        return cm_ty_tuple(arena, elements, count);
    }
    case CM_AST_TYPE_SLICE:
        return cm_ty_slice(arena, cm_tyck_ast_type(env, type->child));
    case CM_AST_TYPE_ARRAY: {
        CmTyId element = cm_tyck_ast_type(env, type->child);
        const CmInternedString *text = cm_ast_get_string(env->ast,
            cm_ast_get_type(env->ast, type_id)->text);
        CmTyId length = arena->const_unknown;
        if (text != NULL && text->len != 0u && text->bytes[0] >= '0'
            && text->bytes[0] <= '9') {
            uint64_t value = 0u;
            size_t index;
            for (index = 0u; index < text->len; ++index) {
                unsigned char c = text->bytes[index];
                if (c == '_') continue;
                if (c < '0' || c > '9') break;
                value = value * 10u + (uint64_t)(c - '0');
            }
            length = cm_ty_const_value(arena, value, 0u);
        }
        return cm_ty_array(arena, element, length);
    }
    case CM_AST_TYPE_FUNCTION: {
        CmTyId params[CM_TYCK_MAX_ARGS];
        uint32_t index;
        uint32_t count = type->element_count;
        CmTyId ret;
        if (count > CM_TYCK_MAX_ARGS - 1u) return arena->error;
        for (index = 0u; index < count; ++index)
            params[index] = cm_tyck_ast_type(env,
                cm_ast_get_type(env->ast, type_id)->elements[index]);
        ret = cm_ast_get_type(env->ast, type_id)->child == CM_AST_TYPE_NONE
            ? arena->unit
            : cm_tyck_ast_type(env, cm_ast_get_type(env->ast, type_id)->child);
        return cm_ty_fn_ptr(arena, params, count, ret,
            cm_ast_get_type(env->ast, type_id)->is_unsafe);
    }
    case CM_AST_TYPE_DYN_TRAIT:
    case CM_AST_TYPE_IMPL_TRAIT:
        if (type->bound_count != 0u && type->bounds != NULL
            && type->bounds[0].trait_type != CM_AST_TYPE_NONE
            && type->kind == CM_AST_TYPE_DYN_TRAIT) {
            CmTyId principal = cm_tyck_ast_type(env,
                type->bounds[0].trait_type);
            const CmTy *pt = cm_ty_get(arena, principal);
            if (pt != NULL && pt->kind == CM_TY_DYN) return principal;
        }
        return cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
    case CM_AST_TYPE_PROJECTION:
    case CM_AST_TYPE_OTHER:
    default:
        return cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
    }
}

/* ------------------------------------------------------------------ */
/* Expressions and patterns                                             */

static void cm_tyck_set_expr(CmTyckEnv *env, CmUExprId id, CmTyId type)
{
    if (id != CM_U_EXPR_NONE) env->out->expr_types[id] = type;
}

static CmTyId cm_tyck_expr(CmTyckEnv *env, CmUExprId id, CmTyId expected);
static void cm_tyck_pat(CmTyckEnv *env, CmUPatId id, CmTyId expected);

/*
 * Item type of an iterable: `into_iter` (when present) then `next`, whose
 * `Option<Item>` return names the item.  Avoids modelling IntoIterator's
 * associated types before trait solving exists.
 */
static int cm_tyck_iterator_item(CmTyckEnv *env, CmTyId iterable,
    CmTyId item_type)
{
    CmTyArena *arena = env->state->arena;
    CmTyId iterator = iterable;
    CmTyckFound found;
    CmInternId into_iter = cm_tyck_intern_text(env->state,
        "into_iter", 9u);
    CmInternId next = cm_tyck_intern_text(env->state,
        "next", 4u);
    CmTyId params[CM_TYCK_MAX_ARGS];
    CmTyId ret;
    const CmTy *option;
    if (cm_ty_get(arena, cm_ty_resolve(arena, iterable))->kind == CM_TY_INFER)
        return 0;
    if (into_iter != CM_INTERN_ID_NONE
        && cm_tyck_lookup_assoc(env, iterable, into_iter, &found)
        && found.item->kind == CM_HIR_ITEM_FUNCTION) {
        (void)cm_tyck_signature(env, found.item,
            cm_tyck_subst_of(&found.instance), params, CM_TYCK_MAX_ARGS,
            &ret);
        iterator = ret;
    }
    if (next == CM_INTERN_ID_NONE
        || !cm_tyck_lookup_assoc(env, iterator, next, &found)
        || found.item->kind != CM_HIR_ITEM_FUNCTION) return 0;
    (void)cm_tyck_signature(env, found.item,
        cm_tyck_subst_of(&found.instance), params, CM_TYCK_MAX_ARGS, &ret);
    ret = cm_ty_resolve(arena, ret);
    option = cm_ty_get(arena, ret);
    if (option == NULL || option->kind != CM_TY_ADT) return 0;
    {
        CmTyId arguments[CM_TYCK_MAX_ARGS];
        uint32_t count = option->count > CM_TYCK_MAX_ARGS
            ? CM_TYCK_MAX_ARGS : option->count;
        uint32_t index;
        memcpy(arguments, option->children, count * sizeof(CmTyId));
        for (index = 0u; index < count; ++index) {
            const CmTy *argument = cm_ty_get(arena, cm_ty_resolve(arena,
                arguments[index]));
            if (argument != NULL && argument->kind == CM_TY_LIFETIME)
                continue;
            return cm_ty_unify(arena, item_type, arguments[index]);
        }
    }
    return 0;
}

static void cm_tyck_push_pending(CmTyckEnv *env, CmUExprId id)
{
    size_t index;
    for (index = 0u; index < env->pending_count; ++index)
        if (env->pending[index] == id) return;
    if (env->pending_count == env->pending_capacity) {
        size_t capacity = env->pending_capacity == 0u ? 64u
            : env->pending_capacity * 2u;
        CmUExprId *grown = (CmUExprId *)cm_alloc_zeroed(capacity,
            sizeof(CmUExprId));
        if (env->pending_count != 0u)
            memcpy(grown, env->pending, env->pending_count
                * sizeof(CmUExprId));
        cm_free(env->pending);
        env->pending = grown;
        env->pending_capacity = capacity;
    }
    env->pending[env->pending_count++] = id;
}

static CmTyId cm_tyck_literal(CmTyckEnv *env, const CmUExpr *expr,
    CmTyId expected)
{
    CmTyArena *arena = env->state->arena;
    const CmInternedString *suffix = cm_interner_get(
        &env->state->ubodies->strings, expr->data.literal.suffix);
    static const struct { const char *text; CmHirIntType kind; } ints[] = {
        { "i8", CM_HIR_INT_I8 }, { "i16", CM_HIR_INT_I16 },
        { "i32", CM_HIR_INT_I32 }, { "i64", CM_HIR_INT_I64 },
        { "i128", CM_HIR_INT_I128 }, { "isize", CM_HIR_INT_ISIZE },
        { "u8", CM_HIR_INT_U8 }, { "u16", CM_HIR_INT_U16 },
        { "u32", CM_HIR_INT_U32 }, { "u64", CM_HIR_INT_U64 },
        { "u128", CM_HIR_INT_U128 }, { "usize", CM_HIR_INT_USIZE }
    };
    size_t index;
    (void)expected;
    switch (expr->data.literal.kind) {
    case CM_U_LITERAL_INTEGER:
        if (suffix != NULL) {
            for (index = 0u; index < sizeof(ints) / sizeof(ints[0]); ++index)
                if (suffix->len == strlen(ints[index].text)
                    && memcmp(suffix->bytes, ints[index].text, suffix->len)
                        == 0)
                    return cm_ty_int(arena, ints[index].kind);
            if (suffix->len >= 2u && suffix->bytes[0] == 'f')
                return suffix->len == 3u && suffix->bytes[1] == '3'
                    ? cm_ty_float(arena, CM_HIR_FLOAT_F32) : arena->f64;
        }
        return cm_ty_fresh(arena, CM_HIR_INFER_INTEGER);
    case CM_U_LITERAL_FLOAT:
        if (suffix != NULL && suffix->len == 3u
            && memcmp(suffix->bytes, "f32", 3u) == 0)
            return cm_ty_float(arena, CM_HIR_FLOAT_F32);
        if (suffix != NULL && suffix->len == 3u
            && memcmp(suffix->bytes, "f64", 3u) == 0)
            return arena->f64;
        return cm_ty_fresh(arena, CM_HIR_INFER_FLOAT);
    case CM_U_LITERAL_BOOL:
        return arena->boolean;
    case CM_U_LITERAL_CHAR:
        return arena->character;
    case CM_U_LITERAL_STRING:
        return cm_ty_ref(arena, arena->str, 0);
    case CM_U_LITERAL_BYTE:
        return arena->u8;
    case CM_U_LITERAL_BYTE_STRING:
        return cm_ty_ref(arena, cm_ty_array(arena, arena->u8,
            arena->const_unknown), 0);
    case CM_U_LITERAL_C_STRING:
        return cm_ty_ref(arena, cm_ty_fresh(arena, CM_HIR_INFER_GENERAL), 0);
    case CM_U_LITERAL_UNIT:
    default:
        return arena->unit;
    }
}

/* Constructor-like callee: tuple struct or tuple variant. */
typedef struct CmTyckCtor {
    const CmHirItem *adt;      /* struct or enum item */
    const CmHirField *fields;
    uint32_t field_count;
    CmHirAggregateForm form;
    int valid;
} CmTyckCtor;

static CmTyckCtor cm_tyck_ctor_of(CmTyckEnv *env, const CmUResolution *res)
{
    CmTyckCtor ctor;
    memset(&ctor, 0, sizeof(ctor));
    if (res->kind == CM_U_RESOLVED_DEFINITION) {
        const CmHirItem *item = cm_tyck_item(env->state, res->definition);
        if (item != NULL && item->kind == CM_HIR_ITEM_STRUCT) {
            ctor.adt = item;
            ctor.fields = item->data.aggregate_item.fields;
            ctor.field_count = item->data.aggregate_item.field_count;
            ctor.form = item->data.aggregate_item.form;
            ctor.valid = 1;
        }
    } else if (res->kind == CM_U_RESOLVED_VARIANT) {
        const CmHirDefinition *record = cm_hir_lookup_definition(
            env->state->hir, res->definition);
        if (record != NULL && record->kind == CM_HIR_DEFINITION_ENUM_VARIANT) {
            const CmHirItem *item = cm_hir_get_item(env->state->hir,
                record->entity.enum_variant.enum_item_id);
            uint32_t variant = record->entity.enum_variant.variant_index;
            if (item != NULL && item->kind == CM_HIR_ITEM_ENUM
                && variant < item->data.enum_item.variant_count) {
                const CmHirVariant *v = &item->data.enum_item.variants[variant];
                ctor.adt = item;
                ctor.fields = v->fields;
                ctor.field_count = v->field_count;
                ctor.form = v->form;
                ctor.valid = 1;
            }
        }
    }
    return ctor;
}

/* Type of a resolved value path; `out_found` may carry an assoc lookup. */
static CmTyId cm_tyck_path_type(CmTyckEnv *env, const CmUExpr *expr,
    CmUExprId id)
{
    CmTyArena *arena = env->state->arena;
    const CmUResolution *res = &expr->data.path.resolution;
    CmInternId last = expr->data.path.segment_count == 0u ? CM_INTERN_ID_NONE
        : expr->data.path.segments[expr->data.path.segment_count - 1u];
    switch (res->kind) {
    case CM_U_RESOLVED_LOCAL:
        if (res->local < env->ub->locals.len)
            return env->out->local_types[res->local];
        cm_tyck_error(env, "local index out of range");
        return arena->error;
    case CM_U_RESOLVED_DEFINITION: {
        const CmHirItem *item = cm_tyck_item(env->state, res->definition);
        if (item == NULL) {
            cm_tyck_error(env, "path names no HIR item");
            return arena->error;
        }
        if (item->kind == CM_HIR_ITEM_FUNCTION)
            return cm_tyck_fn_def(env, item, cm_tyck_parent_item(env->state,
                item), CM_TY_NONE, NULL);
        if (item->kind == CM_HIR_ITEM_CONST || item->kind == CM_HIR_ITEM_STATIC)
            return cm_ty_from_hir(arena, env->state->hir,
                item->data.value_item.type);
        if (item->kind == CM_HIR_ITEM_STRUCT) {
            CmTyckInstance instance;
            CmTyId adt = cm_tyck_adt_fresh(env, item, &instance);
            if (item->data.aggregate_item.form == CM_HIR_AGGREGATE_UNIT)
                return adt;
            if (item->data.aggregate_item.form == CM_HIR_AGGREGATE_TUPLE) {
                CmTyId params[CM_TYCK_MAX_ARGS];
                uint32_t index;
                uint32_t count = item->data.aggregate_item.field_count;
                if (count > CM_TYCK_MAX_ARGS - 1u) return arena->error;
                for (index = 0u; index < count; ++index)
                    params[index] = cm_ty_subst(arena, cm_ty_from_hir(arena,
                        env->state->hir,
                        item->data.aggregate_item.fields[index].type),
                        cm_tyck_subst_of(&instance));
                return cm_ty_fn_ptr(arena, params, count, adt, 0);
            }
            cm_tyck_error(env, "named struct used as a value");
            return arena->error;
        }
        cm_tyck_error(env, "unsupported item kind in value position");
        return arena->error;
    }
    case CM_U_RESOLVED_VARIANT: {
        CmTyckCtor ctor = cm_tyck_ctor_of(env, res);
        CmTyckInstance instance;
        CmTyId adt;
        if (!ctor.valid) {
            cm_tyck_error(env, "variant path names no enum variant");
            return arena->error;
        }
        adt = cm_tyck_adt_fresh(env, ctor.adt, &instance);
        if (ctor.form == CM_HIR_AGGREGATE_UNIT) return adt;
        if (ctor.form == CM_HIR_AGGREGATE_TUPLE) {
            CmTyId params[CM_TYCK_MAX_ARGS];
            uint32_t index;
            if (ctor.field_count > CM_TYCK_MAX_ARGS - 1u) return arena->error;
            for (index = 0u; index < ctor.field_count; ++index)
                params[index] = cm_ty_subst(arena, cm_ty_from_hir(arena,
                    env->state->hir, ctor.fields[index].type),
                    cm_tyck_subst_of(&instance));
            return cm_ty_fn_ptr(arena, params, ctor.field_count, adt, 0);
        }
        cm_tyck_error(env, "struct variant used as a value");
        return arena->error;
    }
    case CM_U_RESOLVED_SELF_TYPE:
    case CM_U_RESOLVED_TYPE_ASSOC:
    case CM_U_RESOLVED_PRIMITIVE:
    case CM_U_RESOLVED_GENERIC_PARAM: {
        CmTyId self_type;
        CmTyckFound found;
        if (res->kind == CM_U_RESOLVED_SELF_TYPE) {
            self_type = env->self_type;
        } else if (res->kind == CM_U_RESOLVED_PRIMITIVE) {
            switch (res->primitive) {
            case CM_HIR_PRIMITIVE_BOOL: self_type = arena->boolean; break;
            case CM_HIR_PRIMITIVE_CHAR: self_type = arena->character; break;
            case CM_HIR_PRIMITIVE_STR: self_type = arena->str; break;
            case CM_HIR_PRIMITIVE_F16:
                self_type = cm_ty_float(arena, CM_HIR_FLOAT_F16); break;
            case CM_HIR_PRIMITIVE_F32:
                self_type = cm_ty_float(arena, CM_HIR_FLOAT_F32); break;
            case CM_HIR_PRIMITIVE_F64: self_type = arena->f64; break;
            case CM_HIR_PRIMITIVE_F128:
                self_type = cm_ty_float(arena, CM_HIR_FLOAT_F128); break;
            default:
                self_type = cm_ty_int(arena, (CmHirIntType)(
                    (uint32_t)res->primitive - CM_HIR_PRIMITIVE_I8));
                break;
            }
        } else if (res->kind == CM_U_RESOLVED_GENERIC_PARAM) {
            self_type = cm_ty_param(arena, res->generic_parameter);
        } else {
            const CmHirItem *item = cm_tyck_item(env->state, res->definition);
            if (item == NULL) {
                cm_tyck_error(env, "associated path base names no item");
                return arena->error;
            }
            if (item->kind == CM_HIR_ITEM_ENUM && res->rest_from
                    + 1u == expr->data.path.segment_count) {
                /* `Enum::Variant` through a type alias or import. */
                uint32_t variant;
                for (variant = 0u; variant < item->data.enum_item.variant_count;
                        ++variant) {
                    const CmHirVariant *v =
                        &item->data.enum_item.variants[variant];
                    if (cm_tyck_name_is(env->state, v->name, last)) {
                        CmUResolution alias;
                        CmUExpr fake = *expr;
                        memset(&alias, 0, sizeof(alias));
                        alias.kind = CM_U_RESOLVED_VARIANT;
                        alias.definition = v->definition;
                        fake.data.path.resolution = alias;
                        return cm_tyck_path_type(env, &fake, id);
                    }
                }
            }
            if (item->kind == CM_HIR_ITEM_TRAIT) {
                /* `Trait::method`: Self is inferred from use. */
                CmHirDefId owner_trait;
                const CmHirItem *declared = cm_tyck_trait_method(env,
                    item->definition, last, 0u, &owner_trait);
                if (declared != NULL) {
                    CmTyckInstance instance;
                    CmTyId self = cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
                    cm_tyck_instance_init(&instance, self);
                    cm_tyck_instance_fresh(env, &instance,
                        cm_tyck_item(env->state, owner_trait));
                    cm_tyck_instance_fresh(env, &instance, declared);
                    /* Encode Self as the first argument slot. */
                    {
                        CmTyId args[CM_TYCK_MAX_ARGS];
                        uint32_t index;
                        args[0] = self;
                        for (index = 0u; index < instance.count
                                && index + 1u < CM_TYCK_MAX_ARGS; ++index)
                            args[index + 1u] = instance.types[index];
                        return cm_ty_with_def(arena, CM_TY_FN_DEF,
                            declared->definition, args, instance.count + 1u);
                    }
                }
                cm_tyck_error(env, "trait has no such associated item");
                return arena->error;
            }
            if (item->kind == CM_HIR_ITEM_STRUCT || item->kind
                    == CM_HIR_ITEM_ENUM || item->kind == CM_HIR_ITEM_UNION) {
                self_type = cm_tyck_adt_fresh(env, item, NULL);
            } else if (item->kind == CM_HIR_ITEM_TYPE_ALIAS) {
                CmTyckInstance instance;
                cm_tyck_instance_init(&instance, CM_TY_NONE);
                cm_tyck_instance_fresh(env, &instance, item);
                self_type = cm_ty_subst(arena, cm_ty_from_hir(arena,
                    env->state->hir, item->data.type_alias_item.target),
                    cm_tyck_subst_of(&instance));
                {
                    const CmTy *st = cm_ty_get(arena,
                        cm_ty_resolve(arena, self_type));
                    if (st != NULL && st->kind == CM_TY_ADT
                        && res->rest_from + 1u
                            == expr->data.path.segment_count) {
                        const CmHirItem *enum_item = cm_tyck_item(env->state,
                            st->def);
                        uint32_t variant;
                        if (enum_item != NULL
                            && enum_item->kind == CM_HIR_ITEM_ENUM)
                            for (variant = 0u; variant < enum_item->data
                                    .enum_item.variant_count; ++variant) {
                                const CmHirVariant *v = &enum_item->data
                                    .enum_item.variants[variant];
                                if (cm_tyck_name_is(env->state, v->name,
                                        last)) {
                                    CmUResolution alias;
                                    CmUExpr fake = *expr;
                                    memset(&alias, 0, sizeof(alias));
                                    alias.kind = CM_U_RESOLVED_VARIANT;
                                    alias.definition = v->definition;
                                    fake.data.path.resolution = alias;
                                    return cm_tyck_path_type(env, &fake, id);
                                }
                            }
                    }
                }
            } else {
                self_type = cm_ty_with_def(arena, CM_TY_FOREIGN,
                    item->definition, NULL, 0u);
            }
        }
        if (res->rest_from >= expr->data.path.segment_count) {
            /* `Self` / `Type` alone in value position: unit struct. */
            return self_type;
        }
        if (cm_tyck_lookup_assoc(env, self_type, last, &found)) {
            if (found.item->kind == CM_HIR_ITEM_FUNCTION)
                return cm_ty_with_def(arena, CM_TY_FN_DEF,
                    found.item->definition, found.instance.types,
                    found.instance.count);
            if (found.item->kind == CM_HIR_ITEM_CONST)
                return cm_ty_subst(arena, cm_ty_from_hir(arena,
                    env->state->hir, found.item->data.value_item.type),
                    cm_tyck_subst_of(&found.instance));
            cm_tyck_error(env, "associated item is not a value");
            return arena->error;
        }
        if (cm_ty_has_infer(arena, self_type)
            && cm_ty_get(arena, cm_ty_resolve(arena, self_type))->kind
                == CM_TY_INFER) {
            cm_tyck_push_pending(env, id);
            return cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
        }
        if (getenv("CM_TYCK_DEBUG") != NULL) {
            const CmInternedString *assoc_name = cm_interner_get(
                &env->state->ubodies->strings, last);
            fprintf(stderr, "TYCK assoc-name=%.*s ",
                assoc_name == NULL ? 1 : (int)assoc_name->len,
                assoc_name == NULL ? "?"
                    : (const char *)assoc_name->bytes);
        }
        cm_tyck_debug_pair(env, "assoc-value-on", self_type, self_type);
        cm_tyck_error(env, "associated value not found");
        return arena->error;
    }
    case CM_U_RESOLVED_NESTED_ITEM: {
        /* Items declared inside a body are not HIR items; their types come
         * straight from the AST declaration. */
        const CmAstItem *nested = cm_ast_get_item(env->ast,
            res->nested_item);
        if (nested == NULL) {
            cm_tyck_error(env, "body-local item is unavailable");
            return arena->error;
        }
        if (nested->kind == CM_AST_ITEM_FUNCTION) {
            CmTyId params[CM_TYCK_MAX_ARGS];
            uint32_t index;
            uint32_t count = nested->data.function_item.parameter_count;
            CmTyId ret;
            if (count > CM_TYCK_MAX_ARGS - 1u) count = CM_TYCK_MAX_ARGS - 1u;
            for (index = 0u; index < count; ++index)
                params[index] = cm_tyck_ast_type(env,
                    nested->data.function_item.parameters[index].type);
            ret = nested->data.function_item.return_type == CM_AST_TYPE_NONE
                ? arena->unit
                : cm_tyck_ast_type(env,
                    nested->data.function_item.return_type);
            return cm_ty_fn_ptr(arena, params, count, ret, 0);
        }
        if (nested->kind == CM_AST_ITEM_CONST
            || nested->kind == CM_AST_ITEM_STATIC)
            return cm_tyck_ast_type(env, nested->data.value_item.type);
        /* Nested structs/enums have no HIR definition to name yet. */
        return cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
    }
    case CM_U_RESOLVED_UNRESOLVED:
    default:
        /* A body-local fn's ubody starts with an empty nested scope, so
         * references to sibling body-local fns (task.rs's clone_waker /
         * wake / drop_waker vtable set, btree's self-recursive
         * clone_subtree) arrive unresolved.  Accept a unique same-source
         * body-local function by name. */
        if (expr->data.path.segment_count == 1u && env->item != NULL
            && env->item->ast_source != 0u) {
            const CmHirItem *found = NULL;
            int ambiguous = 0;
            size_t scan;
            for (scan = 0u; scan < env->state->hir->items.len; ++scan) {
                const CmHirItem *cand = (const CmHirItem *)cm_vec_at_const(
                    &env->state->hir->items, scan);
                if (cand == NULL || cand->kind != CM_HIR_ITEM_FUNCTION
                    || cand->ast_source != env->item->ast_source
                    || !cm_hir_def_id_is_none(cand->parent_definition))
                    continue;
                if (!cm_tyck_name_is(env->state, cand->name,
                        expr->data.path.segments[0])) continue;
                if (found != NULL) { ambiguous = 1; break; }
                found = cand;
            }
            if (found != NULL && !ambiguous)
                return cm_tyck_fn_def(env, found,
                    cm_tyck_parent_item(env->state, found), CM_TY_NONE,
                    NULL);
        }
        if (getenv("CM_TYCK_DEBUG") != NULL) {
            uint32_t seg;
            fprintf(stderr, "TYCK unresolved-value kind=%d segs=",
                (int)res->kind);
            for (seg = 0u; seg < expr->data.path.segment_count; ++seg) {
                const CmInternedString *sn = cm_interner_get(
                    &env->state->ubodies->strings,
                    expr->data.path.segments[seg]);
                fprintf(stderr, "%.*s%s",
                    sn == NULL ? 1 : (int)sn->len,
                    sn == NULL ? "?" : (const char *)sn->bytes,
                    seg + 1u < expr->data.path.segment_count ? "::" : "");
            }
            fprintf(stderr, " ");
            cm_tyck_debug_span(env, expr);
        }
        cm_tyck_error(env, "unresolved value path");
        return arena->error;
    }
}

static int cm_tyck_coerce(CmTyckEnv *env, CmTyId actual, CmTyId expected);

static CmTyId cm_tyck_join(CmTyckEnv *env, CmTyId a, CmTyId b)
{
    CmTyArena *arena = env->state->arena;
    const CmTy *ta = cm_ty_get(arena, cm_ty_resolve(arena, a));
    const CmTy *tb = cm_ty_get(arena, cm_ty_resolve(arena, b));
    if (ta != NULL && ta->kind == CM_TY_NEVER) return b;
    if (tb != NULL && tb->kind == CM_TY_NEVER) return a;
    if (!cm_ty_unify(arena, a, b)) {
        /* Reference coercions: `&mut T` to `&T` is the common case. */
        if (ta != NULL && tb != NULL && ta->kind == CM_TY_REF
            && tb->kind == CM_TY_REF)
            (void)cm_ty_unify(arena, ta->children[0], tb->children[0]);
        else if (!cm_tyck_coerce(env, b, a) && !cm_tyck_coerce(env, a, b))
            {
            cm_tyck_debug_pair(env, "branch", a, b);
            cm_tyck_error(env, "branch types do not unify");
        }
    }
    return a;
}

/* Unify with lenient coercions: `&mut T` -> `&T`, `!` -> anything,
 * `&[T; N]` -> `&[T]`, fn item -> fn pointer. */
/*
 * Deep lenient structural equality: unify where possible, and accept any
 * position occupied by a projection, generic parameter, `Self`, opaque, or
 * const argument on either side — the input is assumed valid and trait
 * solving is not modelled.  Reference/pointer mutability and fn-pointer
 * unsafety are ignored.  Children arrays are type-owned, so capturing them
 * before recursing is safe even though the arena vector may move.
 */
static int cm_tyck_lenient_eq(CmTyckEnv *env, CmTyId left, CmTyId right,
    unsigned int depth)
{
    CmTyArena *arena = env->state->arena;
    const CmTy *a;
    const CmTy *e;
    CmTyKind ak;
    CmTyKind ek;
    uint32_t count;
    const CmTyId *a_children;
    const CmTyId *e_children;
    uint32_t index;
    if (depth > 32u) return 1;
    left = cm_tyck_normalize(env, left, 0u);
    right = cm_tyck_normalize(env, right, 0u);
    if (cm_ty_unify(arena, left, right)) return 1;
    a = cm_ty_get(arena, cm_ty_resolve(arena, left));
    e = cm_ty_get(arena, cm_ty_resolve(arena, right));
    if (a == NULL || e == NULL) return 0;
    ak = a->kind;
    ek = e->kind;
    if (ak == CM_TY_PROJECTION || ek == CM_TY_PROJECTION
        || ak == CM_TY_PARAM || ek == CM_TY_PARAM
        || ak == CM_TY_SELF || ek == CM_TY_SELF
        || ak == CM_TY_OPAQUE || ek == CM_TY_OPAQUE
        || ak == CM_TY_ERROR || ek == CM_TY_ERROR
        || ak == CM_TY_NEVER || ek == CM_TY_NEVER
        || ak == CM_TY_INFER || ek == CM_TY_INFER
        || ak == CM_TY_CONST || ek == CM_TY_CONST
        || ak == CM_TY_CONST_PARAM || ek == CM_TY_CONST_PARAM
        || ak == CM_TY_CONST_UNKNOWN || ek == CM_TY_CONST_UNKNOWN)
        return 1;
    /* `&&T` against `&T`: strip the extra reference. */
    if (ak == CM_TY_REF && ek == CM_TY_REF) {
        const CmTy *ap = cm_ty_get(arena, cm_ty_resolve(arena,
            a->children[0]));
        if (ap != NULL && ap->kind == CM_TY_REF)
            return cm_tyck_lenient_eq(env, cm_ty_get(arena,
                cm_ty_resolve(arena, left))->children[0], right,
                depth + 1u);
    }
    if (ak != ek) return 0;
    if ((ak == CM_TY_ADT || ak == CM_TY_FN_DEF || ak == CM_TY_FOREIGN
            || ak == CM_TY_DYN)
        && !cm_hir_def_id_equal(a->def, e->def)) return 0;
    if ((ak == CM_TY_INT || ak == CM_TY_FLOAT) && a->a != e->a) return 0;
    if (a->count != e->count) return 0;
    count = a->count;
    a_children = a->children;
    e_children = e->children;
    for (index = 0u; index < count; ++index)
        if (!cm_tyck_lenient_eq(env, a_children[index], e_children[index],
                depth + 1u)) return 0;
    return 1;
}

static CmTyId cm_tyck_user_deref(CmTyckEnv *env, CmTyId type);

static int cm_tyck_coerce(CmTyckEnv *env, CmTyId actual, CmTyId expected)
{
    CmTyArena *arena = env->state->arena;
    const CmTy *a;
    const CmTy *e;
    if (expected == CM_TY_NONE) return 1;
    if (cm_ty_unify(arena, actual, expected)) return 1;
    actual = cm_tyck_normalize(env, actual, 0u);
    expected = cm_tyck_normalize(env, expected, 0u);
    if (cm_ty_unify(arena, actual, expected)) return 1;
    a = cm_ty_get(arena, cm_ty_resolve(arena, actual));
    e = cm_ty_get(arena, cm_ty_resolve(arena, expected));
    if (a == NULL || e == NULL) {
        if (getenv("CM_TYCK_DEBUG") != NULL)
            fprintf(stderr, "TYCK coerce-null actual=%lu expected=%lu\n",
                (unsigned long)actual, (unsigned long)expected);
        return 0;
    }
    /* A projection that cannot be normalized yet is accepted: the input is
     * assumed to be valid Rust and trait solving is not modelled. */
    if (a->kind == CM_TY_PROJECTION || e->kind == CM_TY_PROJECTION) return 1;
    if (a->kind == CM_TY_PARAM || e->kind == CM_TY_PARAM
        || a->kind == CM_TY_SELF || e->kind == CM_TY_SELF) return 1;
    if (a->kind == CM_TY_REF && e->kind == CM_TY_REF) {
        const CmTy *ap = cm_ty_get(arena, cm_ty_resolve(arena,
            a->children[0]));
        const CmTy *ep = cm_ty_get(arena, cm_ty_resolve(arena,
            e->children[0]));
        if (ap != NULL && ep != NULL && ap->kind == CM_TY_ARRAY
            && ep->kind == CM_TY_SLICE)
            return cm_ty_unify(arena, ap->children[0], ep->children[0]);
        if (ap != NULL && ep != NULL && ep->kind == CM_TY_DYN) return 1;
        /* Unsize through a reference to one ADT application:
         * `&PolymorphicIter<[T; N]>` coerces to `&PolymorphicIter<[T]>`
         * (core's array iter unsize helpers). */
        if (ap != NULL && ep != NULL && ap->kind == CM_TY_ADT
            && ep->kind == CM_TY_ADT
            && cm_hir_def_id_equal(ap->def, ep->def)
            && cm_tyck_coerce(env, a->children[0], e->children[0]))
            return 1;
        a = cm_ty_get(arena, cm_ty_resolve(arena, actual));
        e = cm_ty_get(arena, cm_ty_resolve(arena, expected));
        if (a == NULL || e == NULL) return 0;
        if (cm_ty_unify(arena, a->children[0], e->children[0])) return 1;
        /* Deref coercion at argument position: `&Vec<u8>` passed where
         * `&[u8]` is expected goes through the pointee's `deref`. */
        if (ap != NULL && ap->kind == CM_TY_ADT) {
            CmTyId derefed = cm_tyck_user_deref(env, a->children[0]);
            a = cm_ty_get(arena, cm_ty_resolve(arena, actual));
            e = cm_ty_get(arena, cm_ty_resolve(arena, expected));
            if (a == NULL || e == NULL) return 0;
            if (derefed != CM_TY_NONE
                && cm_ty_unify(arena, derefed, e->children[0])) return 1;
        }
        return cm_tyck_lenient_eq(env, actual, expected, 0u);
    }
    if ((a->kind == CM_TY_PTR || a->kind == CM_TY_REF)
        && e->kind == CM_TY_PTR) {
        /* Pointer unsizing binds the slice element: btree's
         * `let keys: *const [_] = &raw const (*leaf).keys;` must give
         * the elided slice its `[MaybeUninit<K>; CAP]` element or every
         * later `get_unchecked` projection stays unresolved. */
        const CmTy *ap = cm_ty_get(arena, cm_ty_resolve(arena,
            a->children[0]));
        const CmTy *ep = cm_ty_get(arena, cm_ty_resolve(arena,
            e->children[0]));
        if (ap != NULL && ep != NULL && ap->kind == CM_TY_ARRAY
            && ep->kind == CM_TY_SLICE)
            (void)cm_ty_unify(arena, ap->children[0], ep->children[0]);
        else
            (void)cm_ty_unify(arena, a->children[0], e->children[0]);
        return 1;
    }
    if (a->kind == CM_TY_FN_DEF && e->kind == CM_TY_FN_PTR) return 1;
    if (a->kind == CM_TY_CLOSURE && e->kind == CM_TY_FN_PTR) return 1;
    /* Unsize leniency (M9): `[T; N]` coerces to `[T]`, directly and
     * argument-wise through one ADT application (`Box<[T; 1], A>` vs
     * `Box<[T], A>`, alloc's into_boxed_slice family). */
    if (a->kind == CM_TY_ARRAY && e->kind == CM_TY_SLICE)
        return cm_ty_unify(arena, a->children[0], e->children[0]);
    if (a->kind == CM_TY_ADT && e->kind == CM_TY_ADT
        && cm_hir_def_id_equal(a->def, e->def)
        && a->count == e->count) {
        uint32_t child;
        int all = 1;
        for (child = 0u; child < a->count && all; ++child) {
            uint32_t child_index = child;
            CmTyId left_child = a->children[child_index];
            CmTyId right_child = e->children[child_index];
            if (!cm_tyck_coerce(env, left_child, right_child)) all = 0;
            /* Re-fetch: coercion can create types and move the arena. */
            a = cm_ty_get(arena, cm_ty_resolve(arena, actual));
            e = cm_ty_get(arena, cm_ty_resolve(arena, expected));
            if (a == NULL || e == NULL) return 0;
        }
        if (all) return 1;
    }
    return cm_tyck_lenient_eq(env, actual, expected, 0u);
}

static int cm_tyck_method_args_compatible(CmTyckEnv *env,
    const CmTyckFound *found, const CmTyId *arg_types, uint32_t arg_count);

/* CM_TYCK_DEBUG_FN=<name>: trace candidate decisions inside that fn. */
static int cm_tyck_debug_fn_matches(CmTyckEnv *env)
{
    const char *filter = getenv("CM_TYCK_DEBUG_FN");
    const CmInternedString *name;
    if (filter == NULL || env->item == NULL) return 0;
    name = cm_interner_get(&env->state->hir->strings, env->item->name);
    return name != NULL && name->len == strlen(filter)
        && memcmp(name->bytes, filter, name->len) == 0;
}

/* One user-Deref step: `<T as Deref>::deref(&T) -> &Target`; NONE when
 * no deref impl is visible. */
static CmTyId cm_tyck_user_deref(CmTyckEnv *env, CmTyId type)
{
    CmTyArena *arena = env->state->arena;
    CmTyckFound found;
    CmInternId method = cm_tyck_intern_text(env->state, "deref", 5u);
    CmTyId params[CM_TYCK_MAX_ARGS];
    CmTyId ret;
    const CmTy *rt;
    if (!cm_tyck_lookup_assoc(env, type, method, &found)
        || found.item->kind != CM_HIR_ITEM_FUNCTION) return CM_TY_NONE;
    (void)cm_tyck_signature(env, found.item,
        cm_tyck_subst_of(&found.instance), params, CM_TYCK_MAX_ARGS, &ret);
    ret = cm_tyck_normalize(env, ret, 0u);
    rt = cm_ty_get(arena, cm_ty_resolve(arena, ret));
    if (rt != NULL && rt->kind == CM_TY_REF) return rt->children[0];
    return ret == CM_TY_NONE ? CM_TY_NONE : ret;
}

static CmTyId cm_tyck_primitive_ty(CmTyckEnv *env,
    CmHirPrimitiveKind primitive)
{
    CmTyArena *arena = env->state->arena;
    switch (primitive) {
    case CM_HIR_PRIMITIVE_BOOL: return arena->boolean;
    case CM_HIR_PRIMITIVE_CHAR: return arena->character;
    case CM_HIR_PRIMITIVE_STR: return arena->str;
    case CM_HIR_PRIMITIVE_F16: return cm_ty_float(arena, CM_HIR_FLOAT_F16);
    case CM_HIR_PRIMITIVE_F32: return cm_ty_float(arena, CM_HIR_FLOAT_F32);
    case CM_HIR_PRIMITIVE_F64: return arena->f64;
    case CM_HIR_PRIMITIVE_F128: return cm_ty_float(arena,
        CM_HIR_FLOAT_F128);
    case CM_HIR_PRIMITIVE_NONE: return CM_TY_NONE;
    default:
        return cm_ty_int(arena, (CmHirIntType)((uint32_t)primitive
            - CM_HIR_PRIMITIVE_I8));
    }
}

static CmAstTypeId expr_qualified_self_type(CmTyckEnv *env,
    const CmUExpr *callee_expr)
{
    (void)env;
    return callee_expr->data.qualified_path.self_type.type;
}

static CmTyId cm_tyck_call(CmTyckEnv *env, const CmUExpr *expr,
    CmUExprId id, CmTyId expected)
{
    CmTyArena *arena = env->state->arena;
    const CmUExpr *callee_expr = cm_ubody_get_expr(env->ub,
        expr->data.call.callee);
    CmTyId callee;
    const CmTy *ct;
    CmTyId params[CM_TYCK_MAX_ARGS];
    uint32_t count = 0u;
    CmTyId ret = CM_TY_NONE;
    uint32_t index;
    int known = 0;
    CmTyId call_arg_types[CM_TYCK_MAX_ARGS];
    uint32_t call_arg_count = 0u;
    (void)id;
    callee = cm_tyck_expr(env, expr->data.call.callee, CM_TY_NONE);
    /* Type the arguments early (closures deferred) so overlapping
     * associated-fn impls can be told apart: `<u64>::from(mant)` must
     * pick `From<u32>`, not `From<NonZero<u32>>`. */
    {
        uint32_t argument;
        for (argument = 0u; argument < expr->data.call.argument_count;
                ++argument) {
            const CmUExpr *arg_expr = cm_ubody_get_expr(env->ub,
                expr->data.call.arguments[argument]);
            CmTyId typed = CM_TY_NONE;
            if (arg_expr == NULL || arg_expr->kind != CM_U_EXPR_CLOSURE)
                typed = cm_tyck_expr(env,
                    expr->data.call.arguments[argument], CM_TY_NONE);
            if (argument < CM_TYCK_MAX_ARGS)
                call_arg_types[argument] = typed;
        }
        call_arg_count = expr->data.call.argument_count > CM_TYCK_MAX_ARGS
            ? CM_TYCK_MAX_ARGS : expr->data.call.argument_count;
    }
    ct = cm_ty_get(arena, cm_ty_resolve(arena, callee));
    if (ct != NULL && ct->kind == CM_TY_FN_DEF) {
        const CmHirItem *fn = cm_tyck_item(env->state, ct->def);
        const CmHirItem *parent = cm_tyck_parent_item(env->state, fn);
        CmTyckInstance instance;
        CmTyId self_type = CM_TY_NONE;
        const CmTyId *args = ct->children;
        uint32_t arg_count = ct->count;
        if (parent != NULL && parent->kind == CM_HIR_ITEM_TRAIT
            && arg_count != 0u) {
            /* Trait method FN_DEF: first slot is Self. */
            self_type = args[0];
            args += 1;
            arg_count -= 1u;
        } else if (parent != NULL && parent->kind == CM_HIR_ITEM_IMPL) {
            CmTyckInstance impl_instance;
            cm_tyck_instance_init(&impl_instance, CM_TY_NONE);
            cm_tyck_instance_from_args(&impl_instance, NULL, parent, args,
                arg_count);
            self_type = cm_ty_subst(arena, cm_ty_from_hir(arena,
                env->state->hir, parent->data.impl_item.self_type),
                cm_tyck_subst_of(&impl_instance));
        }
        cm_tyck_instance_init(&instance, self_type);
        cm_tyck_instance_from_args(&instance, fn, parent, args, arg_count);
        count = cm_tyck_signature(env, fn, cm_tyck_subst_of(&instance), params,
            CM_TYCK_MAX_ARGS, &ret);
        known = 1;
    } else if (ct != NULL && ct->kind == CM_TY_FN_PTR) {
        count = ct->count - 1u;
        for (index = 0u; index < count; ++index)
            params[index] = ct->children[index];
        ret = ct->children[ct->count - 1u];
        known = 1;
    } else if (ct != NULL && ct->kind == CM_TY_CLOSURE) {
        /* Closure call: parameters from the closure expression. */
        const CmUExpr *closure = cm_ubody_get_expr(env->ub,
            (CmUExprId)ct->b);
        if (closure != NULL && closure->kind == CM_U_EXPR_CLOSURE) {
            count = closure->data.closure.parameter_count;
            for (index = 0u; index < count && index < CM_TYCK_MAX_ARGS; ++index)
                params[index] = env->out->pat_types[
                    closure->data.closure.parameters[index].pattern];
            ret = env->out->expr_types[closure->data.closure.body];
            known = 1;
        }
    } else if (ct != NULL && ct->kind == CM_TY_INFER) {
        cm_tyck_push_pending(env, id);
    } else if (ct != NULL && ct->kind == CM_TY_ERROR) {
        /* already reported */
    } else if (ct != NULL && cm_tyck_fn_bound(env, callee, params,
            CM_TYCK_MAX_ARGS, &count, &ret)) {
        known = 1;
    } else if (ct != NULL && ct->kind == CM_TY_DYN) {
        /* `dyn Fn(..) -> R` objects: arguments are typed loosely. */
    } else if (ct != NULL) {
        CmTyckFound found;
        CmInternId call_name = cm_tyck_intern_text(env->state, "call", 4u);
        CmInternId call_mut = cm_tyck_intern_text(env->state, "call_mut", 8u);
        CmInternId call_once = cm_tyck_intern_text(env->state, "call_once", 9u);
        int resolved = 0;
        int attempt;
        if (callee_expr != NULL && callee_expr->kind == CM_U_EXPR_PATH) {
            /* Tuple or unit constructor called as a function. */
            CmTyckCtor ctor = cm_tyck_ctor_of(env,
                &callee_expr->data.path.resolution);
            if (ctor.valid) {
                CmTyckInstance instance;
                CmTyId adt = cm_tyck_adt_fresh(env, ctor.adt, &instance);
                count = ctor.field_count > CM_TYCK_MAX_ARGS
                    ? CM_TYCK_MAX_ARGS : ctor.field_count;
                for (index = 0u; index < count; ++index)
                    params[index] = cm_ty_subst(arena, cm_ty_from_hir(arena,
                        env->state->hir, ctor.fields[index].type),
                        cm_tyck_subst_of(&instance));
                ret = adt;
                known = 1;
                resolved = 1;
            }
        }
        if (!resolved && ct->kind == CM_TY_ADT) {
            /* `Self(v)` and ctors through aliases: the callee already has
             * the ADT type; take fields from its struct item. */
            const CmHirItem *item = cm_tyck_item(env->state, ct->def);
            if (item != NULL && item->kind == CM_HIR_ITEM_STRUCT) {
                CmTyckInstance instance;
                CmTyId adt = cm_ty_resolve(arena, callee);
                cm_tyck_instance_of_type(env, adt, item, NULL, CM_TY_NONE,
                    &instance);
                count = item->data.aggregate_item.field_count
                        > CM_TYCK_MAX_ARGS
                    ? CM_TYCK_MAX_ARGS
                    : item->data.aggregate_item.field_count;
                for (index = 0u; index < count; ++index)
                    params[index] = cm_ty_subst(arena, cm_ty_from_hir(arena,
                        env->state->hir,
                        item->data.aggregate_item.fields[index].type),
                        cm_tyck_subst_of(&instance));
                ret = adt;
                known = 1;
                resolved = 1;
            }
        }
        for (attempt = 0; attempt < 3 && !resolved; ++attempt) {
            CmInternId name = attempt == 0 ? call_name
                : attempt == 1 ? call_mut : call_once;
            if (name == CM_INTERN_ID_NONE) continue;
            if (cm_tyck_lookup_assoc(env, callee, name, &found)
                && found.item->kind == CM_HIR_ITEM_FUNCTION) {
                CmTyId signature_params[CM_TYCK_MAX_ARGS];
                uint32_t signature_count = cm_tyck_signature(env, found.item,
                    cm_tyck_subst_of(&found.instance), signature_params,
                    CM_TYCK_MAX_ARGS, &ret);
                const CmTy *tuple = signature_count >= 2u
                    ? cm_ty_get(arena, cm_ty_resolve(arena,
                        signature_params[1])) : NULL;
                count = 0u;
                if (tuple != NULL && tuple->kind == CM_TY_TUPLE) {
                    count = tuple->count > CM_TYCK_MAX_ARGS
                        ? CM_TYCK_MAX_ARGS : tuple->count;
                    memcpy(params, tuple->children,
                        count * sizeof(CmTyId));
                }
                known = 1;
                resolved = 1;
            }
        }
        if (!resolved) {
            if (getenv("CM_TYCK_DEBUG") != NULL && callee_expr != NULL)
                fprintf(stderr, "TYCK non-function callee-kind=%d\n",
                    (int)callee_expr->kind);
            if (getenv("CM_TYCK_DEBUG") != NULL && callee_expr != NULL
                && callee_expr->kind == CM_U_EXPR_PATH) {
                uint32_t seg;
                fprintf(stderr, "TYCK non-function path kind=%d ",
                    (int)callee_expr->data.path.resolution.kind);
                for (seg = 0u; seg
                        < callee_expr->data.path.segment_count; ++seg) {
                    const CmInternedString *t = cm_interner_get(
                        &env->state->ubodies->strings,
                        callee_expr->data.path.segments[seg]);
                    fprintf(stderr, "%s%.*s", seg == 0u ? "" : "::",
                        t == NULL ? 0 : (int)t->len,
                        t == NULL ? "" : (const char *)t->bytes);
                }
                fputc(10, stderr);
            }
            cm_tyck_debug_pair(env, "non-function callee", callee, callee);
            cm_tyck_debug_span(env, expr);
            cm_tyck_error(env, "call through non-function type");
        }
    }
    if (callee_expr != NULL && callee_expr->kind == CM_U_EXPR_PATH
        && (callee_expr->data.path.resolution.kind == CM_U_RESOLVED_VARIANT
            || callee_expr->data.path.resolution.kind
                == CM_U_RESOLVED_DEFINITION)
        && !known) {
        /* Constructor whose FN_PTR view failed above: type args anyway. */
    }
    if (known && callee_expr != NULL
        && callee_expr->kind == CM_U_EXPR_PATH
        && (callee_expr->data.path.resolution.kind
                == CM_U_RESOLVED_TYPE_ASSOC
            || callee_expr->data.path.resolution.kind
                == CM_U_RESOLVED_PRIMITIVE
            || callee_expr->data.path.resolution.kind
                == CM_U_RESOLVED_SELF_TYPE)
        && callee_expr->data.path.segment_count >= 2u) {
        /* `usize::from(x)`: retry overlapping assoc-fn impls by the
         * argument types, like qualified paths. */
        uint32_t argument;
        int compatible = 1;
        for (argument = 0u; argument < call_arg_count && argument < count;
                ++argument) {
            if (call_arg_types[argument] == CM_TY_NONE) continue;
            if (!cm_tyck_matches(env, params[argument],
                    call_arg_types[argument])
                && !cm_tyck_matches(env, call_arg_types[argument],
                    params[argument])) {
                compatible = 0;
                break;
            }
        }
        if (!compatible) {
            const CmUResolution *res = &callee_expr->data.path.resolution;
            CmTyId path_self = CM_TY_NONE;
            CmInternId path_name = callee_expr->data.path.segments[
                callee_expr->data.path.segment_count - 1u];
            if (res->kind == CM_U_RESOLVED_SELF_TYPE) {
                path_self = env->self_type;
            } else if (res->kind == CM_U_RESOLVED_TYPE_ASSOC) {
                const CmHirItem *base_item = cm_tyck_item(env->state,
                    res->definition);
                if (base_item != NULL
                    && (base_item->kind == CM_HIR_ITEM_STRUCT
                        || base_item->kind == CM_HIR_ITEM_ENUM
                        || base_item->kind == CM_HIR_ITEM_UNION))
                    path_self = cm_tyck_adt_fresh(env, base_item, NULL);
            } else {
                path_self = cm_tyck_primitive_ty(env, res->primitive);
            }
            if (path_self != CM_TY_NONE) {
                unsigned int variant;
                for (variant = 1u; variant < 32u; ++variant) {
                    CmTyckFound retry;
                    size_t undo_mark = cm_ty_undo_mark(arena);
                    if (!cm_tyck_lookup_assoc_in(env, path_self,
                            path_name, &retry, 3u, variant)) break;
                    if (retry.item->kind == CM_HIR_ITEM_FUNCTION
                        && cm_tyck_method_args_compatible(env, &retry,
                            call_arg_types, call_arg_count)) {
                        count = cm_tyck_signature(env, retry.item,
                            cm_tyck_subst_of(&retry.instance), params,
                            CM_TYCK_MAX_ARGS, &ret);
                        break;
                    }
                    cm_ty_undo_to(arena, undo_mark);
                }
            }
        }
    }
    if (known && callee_expr != NULL
        && callee_expr->kind == CM_U_EXPR_QUALIFIED_PATH) {
        /* Overlapping assoc-fn impls: retry by argument types. */
        uint32_t argument;
        int compatible = 1;
        for (argument = 0u; argument < call_arg_count && argument < count;
                ++argument) {
            if (call_arg_types[argument] == CM_TY_NONE) continue;
            if (!cm_tyck_matches(env, params[argument],
                    call_arg_types[argument])
                && !cm_tyck_matches(env, call_arg_types[argument],
                    params[argument])) {
                compatible = 0;
                break;
            }
        }
        if (!compatible) {
            CmTyId qualified_self = cm_tyck_ast_type(env,
                expr_qualified_self_type(env, callee_expr));
            const CmAstPath *associated = cm_ast_get_path(env->ast,
                callee_expr->data.qualified_path.associated_path.path);
            CmInternId qualified_name = CM_INTERN_ID_NONE;
            if (associated != NULL && associated->segment_count != 0u) {
                const CmInternedString *text = cm_ast_get_string(env->ast,
                    associated->segments[associated->segment_count - 1u]
                        .name);
                if (text != NULL)
                    qualified_name = cm_tyck_intern_text(env->state,
                        (const char *)text->bytes, text->len);
            }
            if (qualified_name != CM_INTERN_ID_NONE) {
                unsigned int variant;
                for (variant = 1u; variant < 32u; ++variant) {
                    CmTyckFound retry;
                    size_t undo_mark = cm_ty_undo_mark(arena);
                    if (!cm_tyck_lookup_assoc_in(env, qualified_self,
                            qualified_name, &retry, 3u, variant)) break;
                    if (retry.item->kind == CM_HIR_ITEM_FUNCTION
                        && cm_tyck_method_args_compatible(env, &retry,
                            call_arg_types, call_arg_count)) {
                        count = cm_tyck_signature(env, retry.item,
                            cm_tyck_subst_of(&retry.instance), params,
                            CM_TYCK_MAX_ARGS, &ret);
                        break;
                    }
                    cm_ty_undo_to(arena, undo_mark);
                }
            }
        }
    }
    for (index = 0u; index < expr->data.call.argument_count; ++index) {
        CmTyId arg_expected = known && index < count ? params[index]
            : CM_TY_NONE;
        CmTyId actual = index < call_arg_count
                && call_arg_types[index] != CM_TY_NONE
            ? call_arg_types[index]
            : cm_tyck_expr(env, expr->data.call.arguments[index],
                arg_expected);
        if (known && index < count && !cm_tyck_coerce(env, actual,
                params[index])) {
            cm_tyck_debug_pair(env, "call argument", actual, params[index]);
            cm_tyck_debug_span(env, expr);
            cm_tyck_error(env, "call argument type mismatch");
        }
    }
    if (!known) return ct != NULL && ct->kind == CM_TY_ERROR ? arena->error
        : cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
    (void)expected;
    return ret;
}

/*
 * Non-binding check: could this candidate's parameters accept the
 * already-typed arguments?  CM_TY_NONE argument slots (deferred
 * closures) always fit.  Used to pick among overlapping impls
 * (`PartialEq<ByteStr> for [u8]` vs `PartialEq<[B]> for [A]`).
 */
static int cm_tyck_method_args_compatible(CmTyckEnv *env,
    const CmTyckFound *found, const CmTyId *arg_types, uint32_t arg_count)
{
    CmTyId params[CM_TYCK_MAX_ARGS];
    CmTyId ret;
    CmTyckFound copy = *found;
    uint32_t count = cm_tyck_signature(env, found->item,
        cm_tyck_subst_of(&copy.instance), params, CM_TYCK_MAX_ARGS, &ret);
    uint32_t first = found->item->data.function_item.signature.receiver
        != CM_HIR_RECEIVER_NONE ? 1u : 0u;
    uint32_t index;
    for (index = 0u; index < arg_count; ++index) {
        uint32_t slot = first + index;
        CmTyId param_type;
        CmTyId arg_type;
        if (slot >= count) break;
        if (arg_types[index] == CM_TY_NONE) {
            /* A deferred closure argument: the parameter must at least
             * be callable-shaped, not a scalar. */
            const CmTy *pt = cm_ty_get(env->state->arena,
                cm_ty_resolve(env->state->arena, params[slot]));
            if (pt != NULL && (pt->kind == CM_TY_INT
                    || pt->kind == CM_TY_FLOAT || pt->kind == CM_TY_BOOL
                    || pt->kind == CM_TY_CHAR || pt->kind == CM_TY_STR))
                return 0;
            continue;
        }
        param_type = params[slot];
        arg_type = arg_types[index];
        /* `&T` coerces to `*const T`: compare pointees across any
         * reference/pointer mix, as coercion will. */
        {
            const CmTy *pt = cm_ty_get(env->state->arena,
                cm_ty_resolve(env->state->arena, param_type));
            const CmTy *at = cm_ty_get(env->state->arena,
                cm_ty_resolve(env->state->arena, arg_type));
            if (pt != NULL && at != NULL
                && (pt->kind == CM_TY_REF || pt->kind == CM_TY_PTR)
                && (at->kind == CM_TY_REF || at->kind == CM_TY_PTR)
                && pt->kind != at->kind) {
                param_type = pt->children[0];
                arg_type = at->children[0];
            }
        }
        if (!cm_tyck_matches(env, param_type, arg_type)
            && !cm_tyck_matches(env, arg_type, param_type))
            return 0;
    }
    return 1;
}

static CmTyId cm_tyck_method_call(CmTyckEnv *env, const CmUExpr *expr,
    CmUExprId id)
{
    CmTyArena *arena = env->state->arena;
    CmTyId receiver = cm_tyck_expr(env, expr->data.method_call.receiver,
        CM_TY_NONE);
    CmTyId candidate;
    unsigned int step;
    CmTyckFound found;
    CmTyId params[CM_TYCK_MAX_ARGS];
    CmTyId arg_types[CM_TYCK_MAX_ARGS];
    uint32_t arg_count = 0u;
    uint32_t count;
    CmTyId ret;
    uint32_t index;
    uint32_t first_arg;
    receiver = cm_ty_resolve(arena, receiver);
    receiver = cm_tyck_normalize(env, receiver, 0u);
    if (cm_ty_get(arena, receiver)->kind == CM_TY_INFER) {
        cm_tyck_push_pending(env, id);
        for (index = 0u; index < expr->data.method_call.argument_count;
                ++index)
            (void)cm_tyck_expr(env, expr->data.method_call.arguments[index],
                CM_TY_NONE);
        return cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
    }
    /* Type the arguments first so overlapping impls can be told apart
     * by what the call actually passes.  Closures are deferred: their
     * parameter types come from the chosen method's expectation. */
    {
        uint32_t argument;
        for (argument = 0u; argument < expr->data.method_call.argument_count;
                ++argument) {
            const CmUExpr *arg_expr = cm_ubody_get_expr(env->ub,
                expr->data.method_call.arguments[argument]);
            CmTyId typed = CM_TY_NONE;
            if (arg_expr == NULL || arg_expr->kind != CM_U_EXPR_CLOSURE)
                typed = cm_tyck_expr(env,
                    expr->data.method_call.arguments[argument], CM_TY_NONE);
            if (argument < CM_TYCK_MAX_ARGS) arg_types[argument] = typed;
        }
        arg_count = expr->data.method_call.argument_count
                > CM_TYCK_MAX_ARGS ? CM_TYCK_MAX_ARGS
            : expr->data.method_call.argument_count;
    }
    {
        /* Inherent methods across every autoderef step win over trait
         * impls at any step: a blanket `impl<H: Hasher + ?Sized> Hasher
         * for &mut H` must not capture `DebugStruct::finish`.  Within a
         * step, the first candidate whose parameters accept the typed
         * arguments wins; the very first candidate is the fallback. */
        unsigned int phase;
        CmTyckFound fallback;
        CmTyId fallback_candidate = CM_TY_NONE;
        int have_fallback = 0;
        found.item = NULL;
        for (phase = 0u; phase < 2u && found.item == NULL; ++phase) {
            unsigned int mask = phase == 0u ? 1u : 2u;
            for (step = 0u; step < 6u; ++step) {
                CmTyKind candidate_kind;
                unsigned int variant;
                candidate = cm_tyck_autoderef(env, receiver, step);
                candidate = cm_tyck_normalize(env, candidate, 0u);
                for (variant = 0u; variant < 32u; ++variant) {
                    size_t undo_mark = cm_ty_undo_mark(arena);
                    int looked = cm_tyck_lookup_assoc_in(env, candidate,
                        expr->data.method_call.name, &found, mask,
                        variant);
                    if (cm_tyck_debug_fn_matches(env)) {
                        const CmTy *cd = cm_ty_get(arena,
                            cm_ty_resolve(arena, candidate));
                        const CmInternedString *mname = cm_interner_get(
                            &env->state->ubodies->strings,
                            expr->data.method_call.name);
                        int child_kind = -1;
                        long child_param = -1;
                        if (cd != NULL && cd->count != 0u) {
                            const CmTy *cc = cm_ty_get(arena,
                                cm_ty_resolve(arena, cd->children[0]));
                            if (cc != NULL) {
                                child_kind = (int)cc->kind;
                                if (cc->kind == CM_TY_PARAM)
                                    child_param = (long)cc->a;
                            }
                        }
                        const CmInternedString *pname = looked
                                && found.parent != NULL
                            ? cm_interner_get(&env->state->hir->strings,
                                found.parent->name)
                            : NULL;
                        fprintf(stderr, "TYCK trace m=%.*s phase=%u"
                            " step=%u variant=%u looked=%d kind=%d"
                            " child0=%d child0a=%ld gstart=%ld"
                            " gcount=%ld via=%d parent=%.*s ",
                            mname == NULL ? 1 : (int)mname->len,
                            mname == NULL ? "?"
                                : (const char *)mname->bytes,
                            phase, step, variant, looked,
                            looked ? (int)found.item->kind : -1,
                            child_kind, child_param,
                            looked ? (long)found.item
                                ->generic_parameter_start : -1,
                            looked ? (long)found.item
                                ->generic_parameter_count : -1,
                            looked ? found.via_trait_declaration : -1,
                            pname == NULL ? 1 : (int)pname->len,
                            pname == NULL ? "?"
                                : (const char *)pname->bytes);
                        cm_tyck_debug_pair(env, "cand", candidate,
                            candidate);
                        if (looked && found.parent != NULL
                            && found.parent->kind == CM_HIR_ITEM_IMPL) {
                            const CmHirItem *trait_item = cm_tyck_item(
                                env->state, found.parent
                                    ->data.impl_item.trait_type.definition);
                            const CmInternedString *tname = trait_item
                                    == NULL ? NULL
                                : cm_interner_get(&env->state->hir->strings,
                                    trait_item->name);
                            const CmInternedString *iname =
                                cm_interner_get(&env->state->hir->strings,
                                    found.item->name);
                            fprintf(stderr, "TYCK chosen trait=%.*s"
                                " item=%.*s ",
                                tname == NULL ? 1 : (int)tname->len,
                                tname == NULL ? "?"
                                    : (const char *)tname->bytes,
                                iname == NULL ? 1 : (int)iname->len,
                                iname == NULL ? "?"
                                    : (const char *)iname->bytes);
                            cm_tyck_debug_pair(env, "impl-self",
                                cm_ty_from_hir(arena, env->state->hir,
                                    found.parent
                                        ->data.impl_item.self_type),
                                candidate);
                        }
                    }
                    if (!looked
                        || found.item->kind != CM_HIR_ITEM_FUNCTION) {
                        cm_ty_undo_to(arena, undo_mark);
                        found.item = NULL;
                        break;
                    }
                    if (cm_tyck_method_args_compatible(env, &found,
                            arg_types, arg_count)) break;
                    if (!have_fallback) {
                        fallback = found;
                        fallback_candidate = candidate;
                        have_fallback = 1;
                    }
                    /* Undo the rejected candidate's receiver bindings. */
                    cm_ty_undo_to(arena, undo_mark);
                    found.item = NULL;
                }
                if (found.item != NULL) break;
                candidate_kind = cm_ty_get(arena, candidate)->kind;
                if (candidate_kind == CM_TY_ARRAY) {
                    /* Unsize `[T; N]` to `[T]` for slice methods. */
                    CmTyId slice = cm_ty_slice(arena,
                        cm_ty_get(arena, candidate)->children[0]);
                    if (cm_tyck_lookup_assoc_in(env, slice,
                            expr->data.method_call.name, &found, mask, 0u)
                        && found.item->kind == CM_HIR_ITEM_FUNCTION) {
                        candidate = slice;
                        break;
                    }
                    found.item = NULL;
                }
                if (candidate_kind != CM_TY_REF
                    && candidate_kind != CM_TY_PTR) {
                    /* One user-Deref step (`ByteStr` -> `[u8]`) before
                     * giving up on this chain. */
                    if (candidate_kind == CM_TY_ADT) {
                        CmTyId derefed = cm_tyck_user_deref(env,
                            candidate);
                        if (derefed != CM_TY_NONE) {
                            unsigned int deref_variant;
                            for (deref_variant = 0u; deref_variant < 32u;
                                    ++deref_variant) {
                                size_t deref_mark = cm_ty_undo_mark(arena);
                                if (!cm_tyck_lookup_assoc_in(env, derefed,
                                        expr->data.method_call.name,
                                        &found, mask, deref_variant)
                                    || found.item->kind
                                        != CM_HIR_ITEM_FUNCTION) {
                                    cm_ty_undo_to(arena, deref_mark);
                                    found.item = NULL;
                                    break;
                                }
                                if (cm_tyck_method_args_compatible(env,
                                        &found, arg_types, arg_count)) {
                                    candidate = derefed;
                                    break;
                                }
                                cm_ty_undo_to(arena, deref_mark);
                                found.item = NULL;
                            }
                        }
                    }
                    if (found.item != NULL) break;
                    candidate = CM_TY_NONE;
                    break;
                }
            }
        }
        if (found.item == NULL && have_fallback) {
            found = fallback;
            candidate = fallback_candidate;
        }
        if (found.item == NULL) candidate = CM_TY_NONE;
    }
    if (candidate == CM_TY_NONE || found.item == NULL) {
        const CmTy *rt = cm_ty_get(arena, cm_ty_resolve(arena,
            cm_tyck_normalize(env, receiver, 0u)));
        for (index = 0u; index < expr->data.method_call.argument_count;
                ++index)
            if (index >= arg_count || arg_types[index] == CM_TY_NONE)
                (void)cm_tyck_expr(env,
                    expr->data.method_call.arguments[index], CM_TY_NONE);
        /* A projection or parameter receiver whose traits we cannot see:
         * assume the method exists (the input is valid Rust). */
        if (rt != NULL && (rt->kind == CM_TY_PROJECTION
                || rt->kind == CM_TY_PARAM || rt->kind == CM_TY_SELF))
            return cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
        if (getenv("CM_TYCK_DEBUG") != NULL) {
            const CmInternedString *name = cm_interner_get(
                &env->state->ubodies->strings,
                expr->data.method_call.name);
            cm_tyck_debug_pair(env, name == NULL ? "method"
                : (const char *)name->bytes, receiver, receiver);
            cm_tyck_debug_span(env, expr);
        }
        cm_tyck_error(env, "method not found");
        return arena->error;
    }
    /* `x.cast::<u16>()`: bind the method's own generics (the last
     * instance entries) to the explicit turbofish arguments. */
    /* `x.cast::<u16>()`: bind the method's own type generics to the
     * explicit turbofish arguments, read straight from the AST
     * method-call node (ubody keeps each expression's AST id). */
    {
        const CmAstExpr *ast_expr = expr->ast == CM_AST_EXPR_NONE ? NULL
            : cm_ast_get_expr(env->ast, expr->ast);
        if (ast_expr != NULL && ast_expr->kind == CM_AST_EXPR_METHOD_CALL
            && ast_expr->data.method_call.generic_argument_count != 0u) {
            uint32_t own = found.item->generic_parameter_count;
            uint32_t base = found.instance.count >= own
                ? found.instance.count - own : 0u;
            uint32_t argument;
            uint32_t slot = base;
            uint32_t explicit_types = 0u;
            uint32_t own_types = 0u;
            for (argument = 0u;
                    argument < ast_expr->data.method_call
                        .generic_argument_count; ++argument)
                if (ast_expr->data.method_call.generic_arguments[argument]
                        .kind == CM_AST_GENERIC_TYPE) explicit_types += 1u;
            for (argument = 0u; argument < own; ++argument) {
                const CmHirGenericParam *parameter =
                    cm_hir_get_generic_param(env->state->hir,
                        found.item->generic_parameter_start + argument);
                if (parameter != NULL
                    && parameter->kind == CM_HIR_GENERIC_TYPE)
                    own_types += 1u;
            }
            for (argument = 0u; explicit_types == own_types
                    && argument < ast_expr->data.method_call
                        .generic_argument_count
                    && slot < found.instance.count; ++argument) {
                const CmAstGenericArg *garg = &ast_expr->data.method_call
                    .generic_arguments[argument];
                if (garg->kind != CM_AST_GENERIC_TYPE) continue;
                while (slot < found.instance.count) {
                    const CmHirGenericParam *parameter =
                        cm_hir_get_generic_param(env->state->hir,
                            found.instance.parameters[slot]);
                    if (parameter != NULL
                        && parameter->kind == CM_HIR_GENERIC_TYPE) break;
                    slot += 1u;
                }
                if (slot >= found.instance.count) break;
                (void)cm_ty_unify(arena, found.instance.types[slot],
                    cm_tyck_ast_type(env, garg->type));
                slot += 1u;
            }
        }
    }
    count = cm_tyck_signature(env, found.item, cm_tyck_subst_of(&found.instance), params,
        CM_TYCK_MAX_ARGS, &ret);
    /* The declared receiver parameter, if any, is the first parameter. */
    first_arg = found.item->data.function_item.signature.receiver
        != CM_HIR_RECEIVER_NONE ? 1u : 0u;
    for (index = 0u; index < expr->data.method_call.argument_count; ++index) {
        uint32_t slot = first_arg + index;
        CmTyId arg_expected = slot < count ? params[slot] : CM_TY_NONE;
        CmTyId actual = index < arg_count && arg_types[index] != CM_TY_NONE
            ? arg_types[index]
            : cm_tyck_expr(env, expr->data.method_call.arguments[index],
                arg_expected);
        if (slot < count && !cm_tyck_coerce(env, actual, params[slot]))
            {
                cm_tyck_debug_pair(env, "method argument", actual,
                    params[first_arg + index]);
                cm_tyck_debug_span(env, expr);
                cm_tyck_error(env, "method argument type mismatch");
            }
    }
    return ret;
}

static CmTyId cm_tyck_struct_literal(CmTyckEnv *env, const CmUExpr *expr)
{
    CmTyArena *arena = env->state->arena;
    const CmUResolution *res = &expr->data.struct_expr.resolution;
    CmTyId adt = CM_TY_NONE;
    const CmHirField *fields = NULL;
    uint32_t field_count = 0u;
    CmTyckInstance instance;
    uint32_t index;
    cm_tyck_instance_init(&instance, CM_TY_NONE);
    if (res->kind == CM_U_RESOLVED_DEFINITION) {
        const CmHirItem *item = cm_tyck_item(env->state, res->definition);
        if (item != NULL && (item->kind == CM_HIR_ITEM_STRUCT
                || item->kind == CM_HIR_ITEM_UNION)) {
            adt = cm_tyck_adt_fresh(env, item, &instance);
            fields = item->data.aggregate_item.fields;
            field_count = item->data.aggregate_item.field_count;
        }
    } else if (res->kind == CM_U_RESOLVED_VARIANT) {
        CmTyckCtor ctor = cm_tyck_ctor_of(env, res);
        if (ctor.valid) {
            adt = cm_tyck_adt_fresh(env, ctor.adt, &instance);
            fields = ctor.fields;
            field_count = ctor.field_count;
        }
    } else if (res->kind == CM_U_RESOLVED_SELF_TYPE) {
        const CmTy *st;
        adt = env->self_type;
        st = cm_ty_get(arena, cm_ty_resolve(arena, adt));
        if (st != NULL && st->kind == CM_TY_ADT) {
            const CmHirItem *item = cm_tyck_item(env->state, st->def);
            if (item != NULL && item->kind == CM_HIR_ITEM_STRUCT) {
                cm_tyck_instance_of_type(env, cm_ty_resolve(arena, adt), item,
                    NULL, CM_TY_NONE, &instance);
                fields = item->data.aggregate_item.fields;
                field_count = item->data.aggregate_item.field_count;
            }
        }
    } else if (res->kind == CM_U_RESOLVED_TYPE_ASSOC) {
        /* `Enum::Variant { .. }` through a type path. */
        const CmHirItem *item = cm_tyck_item(env->state, res->definition);
        CmInternId last = expr->data.struct_expr.ast.path == CM_AST_PATH_NONE
            ? CM_INTERN_ID_NONE : CM_INTERN_ID_NONE;
        (void)last;
        if (item != NULL && item->kind == CM_HIR_ITEM_ENUM) {
            /* The variant name is the last path segment; recover it from
             * the AST path. */
            const CmAstPath *path = cm_ast_get_path(env->ast,
                expr->data.struct_expr.ast.path);
            const CmInternedString *name = path == NULL
                    || path->segment_count == 0u ? NULL
                : cm_ast_get_string(env->ast,
                    path->segments[path->segment_count - 1u].name);
            uint32_t variant;
            for (variant = 0u; name != NULL && variant
                    < item->data.enum_item.variant_count; ++variant) {
                const CmHirVariant *v = &item->data.enum_item.variants[variant];
                const CmInternedString *vn = cm_interner_get(
                    &env->state->hir->strings, v->name);
                if (vn != NULL && vn->len == name->len
                    && memcmp(vn->bytes, name->bytes, vn->len) == 0) {
                    adt = cm_tyck_adt_fresh(env, item, &instance);
                    fields = v->fields;
                    field_count = v->field_count;
                    break;
                }
            }
        }
    }
    if (adt == CM_TY_NONE && res->kind == CM_U_RESOLVED_NESTED_ITEM) {
        /* Body-local structs are lowered as body_local HIR items; find
         * the definition by exact declaring-AST identity. */
        size_t scan;
        for (scan = 0u; scan < env->state->hir->items.len; ++scan) {
            const CmHirItem *item = (const CmHirItem *)cm_vec_at_const(
                &env->state->hir->items, scan);
            if (item == NULL || item->ast_source != res->nested_source
                || item->ast_item != res->nested_item) continue;
            if (item->kind == CM_HIR_ITEM_STRUCT
                || item->kind == CM_HIR_ITEM_UNION) {
                adt = cm_tyck_adt_fresh(env, item, &instance);
                fields = item->data.aggregate_item.fields;
                field_count = item->data.aggregate_item.field_count;
            }
            break;
        }
    }
    if (adt == CM_TY_NONE) {
        {
            cm_tyck_debug_pair(env, "struct-literal", adt, adt);
            cm_tyck_debug_span(env, expr);
            cm_tyck_error(env,
                "struct literal path is not a struct or variant");
        }
        for (index = 0u; index < expr->data.struct_expr.field_count; ++index)
            (void)cm_tyck_expr(env, expr->data.struct_expr.fields[index].value,
                CM_TY_NONE);
        if (expr->data.struct_expr.base != CM_U_EXPR_NONE)
            (void)cm_tyck_expr(env, expr->data.struct_expr.base, CM_TY_NONE);
        return arena->error;
    }
    for (index = 0u; index < expr->data.struct_expr.field_count; ++index) {
        const CmUField *field = &expr->data.struct_expr.fields[index];
        uint32_t f;
        CmTyId expected = CM_TY_NONE;
        for (f = 0u; f < field_count; ++f)
            if (cm_tyck_name_is(env->state, fields[f].name, field->name)) {
                expected = cm_ty_subst(arena, cm_ty_from_hir(arena,
                    env->state->hir, fields[f].type), cm_tyck_subst_of(&instance));
                break;
            }
        if (expected == CM_TY_NONE) cm_tyck_error(env, "unknown struct field");
        {
            CmTyId actual = cm_tyck_expr(env, field->value, expected);
            if (expected != CM_TY_NONE
                && !cm_tyck_coerce(env, actual, expected)) {
                cm_tyck_debug_pair(env, "struct-field", actual, expected);
                cm_tyck_debug_span(env, expr);
                cm_tyck_error(env, "struct field type mismatch");
            }
        }
    }
    if (expr->data.struct_expr.base != CM_U_EXPR_NONE)
        (void)cm_tyck_coerce(env, cm_tyck_expr(env,
            expr->data.struct_expr.base, adt), adt);
    return adt;
}

static CmTyId cm_tyck_binary(CmTyckEnv *env, const CmUExpr *expr, CmTyId expected)
{
    CmTyArena *arena = env->state->arena;
    CmUBinaryOp op = expr->data.binary.op;
    CmTyId left;
    CmTyId right;
    const CmTy *lt;
    int comparison = op >= CM_U_BINARY_EQ;
    int logical = op == CM_U_BINARY_AND || op == CM_U_BINARY_OR;
    int shift = op == CM_U_BINARY_SHL || op == CM_U_BINARY_SHR;
    if (logical) {
        (void)cm_tyck_coerce(env, cm_tyck_expr(env, expr->data.binary.left,
            arena->boolean), arena->boolean);
        (void)cm_tyck_coerce(env, cm_tyck_expr(env, expr->data.binary.right,
            arena->boolean), arena->boolean);
        return arena->boolean;
    }
    left = cm_tyck_expr(env, expr->data.binary.left,
        comparison || shift ? CM_TY_NONE : expected);
    right = cm_tyck_expr(env, expr->data.binary.right,
        shift ? CM_TY_NONE : left);
    left = cm_tyck_normalize(env, left, 0u);
    lt = cm_ty_get(arena, cm_ty_resolve(arena, left));
    if (lt != NULL && (lt->kind == CM_TY_INT || lt->kind == CM_TY_FLOAT
            || lt->kind == CM_TY_BOOL || lt->kind == CM_TY_CHAR
            || (lt->kind == CM_TY_INFER && lt->b != CM_HIR_INFER_GENERAL))) {
        if (!shift && !cm_ty_unify(arena, left, right)) {
            /* `&i32 == i32` and friends: strip references leniently. */
            const CmTy *rt = cm_ty_get(arena, cm_ty_resolve(arena, right));
            if (rt != NULL && rt->kind == CM_TY_REF)
                (void)cm_ty_unify(arena, left, rt->children[0]);
        }
        return comparison ? arena->boolean : left;
    }
    if (lt != NULL && lt->kind == CM_TY_REF) {
        /* `&T op &T` on primitives. */
        const CmTy *pt = cm_ty_get(arena, cm_ty_resolve(arena,
            lt->children[0]));
        if (pt != NULL && (pt->kind == CM_TY_INT || pt->kind == CM_TY_FLOAT
                || pt->kind == CM_TY_BOOL || pt->kind == CM_TY_CHAR
                || pt->kind == CM_TY_STR)) {
            (void)cm_ty_unify(arena, left, right);
            return comparison ? arena->boolean : lt->children[0];
        }
    }
    if (comparison) {
        /* PartialEq/PartialOrd on anything: bool. */
        (void)cm_ty_unify(arena, left, right);
        return arena->boolean;
    }
    /* Operator traits on ADTs (Add::add ...): resolved through the trait
     * impls by method name. */
    {
        static const char *const names[] = { "add", "sub", "mul", "div",
            "rem", NULL, NULL, "bitand", "bitor", "bitxor", "shl", "shr" };
        const char *name = (unsigned int)op < 12u ? names[op] : NULL;
        CmTyckFound found;
        if (name != NULL && lt != NULL && lt->kind != CM_TY_INFER) {
            CmInternId method = cm_tyck_intern_text(env->state, name, strlen(name));
            int have_operator = 0;
            if (method != CM_INTERN_ID_NONE) {
                /* Pick the first candidate whose right-hand parameter
                 * accepts the right operand: `impl<T> BitOr<NonZero<T>>
                 * for T` must not shadow `impl BitOr for Wrapping<u8>`. */
                unsigned int variant;
                CmTyckFound fallback;
                int have_fallback = 0;
                for (variant = 0u; variant < 32u; ++variant) {
                    size_t undo_mark = cm_ty_undo_mark(arena);
                    if (!cm_tyck_lookup_assoc_in(env, left, method, &found,
                            3u, variant)) break;
                    if (found.item->kind != CM_HIR_ITEM_FUNCTION) {
                        cm_ty_undo_to(arena, undo_mark);
                        break;
                    }
                    if (cm_tyck_method_args_compatible(env, &found, &right,
                            1u)) {
                        have_operator = 1;
                        break;
                    }
                    if (!have_fallback) {
                        fallback = found;
                        have_fallback = 1;
                    }
                    cm_ty_undo_to(arena, undo_mark);
                }
                if (!have_operator && have_fallback) {
                    found = fallback;
                    have_operator = 1;
                }
            }
            if (have_operator) {
                CmTyId params[CM_TYCK_MAX_ARGS];
                CmTyId ret;
                uint32_t count = cm_tyck_signature(env, found.item,
                    cm_tyck_subst_of(&found.instance), params, CM_TYCK_MAX_ARGS, &ret);
                if (count >= 2u) (void)cm_tyck_coerce(env, right, params[1]);
                if (getenv("CM_TYCK_DEBUG") != NULL) {
                    const CmTy *rt = cm_ty_get(arena,
                        cm_ty_resolve(arena, ret));
                    if (found.via_trait_declaration) {
                        fprintf(stderr, "TYCK op-via-decl %s ", name);
                        cm_tyck_debug_pair(env, "operand", left, ret);
                    } else if (rt != NULL
                            && rt->kind == CM_TY_PROJECTION) {
                        fprintf(stderr, "TYCK op-impl-proj %s parent=%s ",
                            name, found.parent == NULL ? "?"
                            : found.parent->kind == CM_HIR_ITEM_IMPL
                                ? "impl" : "other");
                        cm_tyck_debug_pair(env, "operand", left, ret);
                    }
                }
                return ret;
            }
        }
    }
    if (lt != NULL && lt->kind == CM_TY_INFER) return cm_ty_fresh(arena,
        CM_HIR_INFER_GENERAL);
    /* Arithmetic on a projection, generic parameter, `Self`, or const
     * parameter: the operator trait bound is assumed and the result keeps
     * the operand type. */
    if (lt != NULL && (lt->kind == CM_TY_PROJECTION
            || lt->kind == CM_TY_PARAM || lt->kind == CM_TY_SELF
            || lt->kind == CM_TY_CONST_PARAM
            || lt->kind == CM_TY_CONST_UNKNOWN
            || lt->kind == CM_TY_ERROR))
        return comparison ? arena->boolean : left;
    cm_tyck_debug_pair(env, "operator", left, right);
    cm_tyck_debug_span(env, expr);
    cm_tyck_error(env, "operator on unsupported type");
    return arena->error;
}

static CmTyId cm_tyck_expr(CmTyckEnv *env, CmUExprId id, CmTyId expected)
{
    CmTyArena *arena = env->state->arena;
    const CmUExpr *expr = cm_ubody_get_expr(env->ub, id);
    CmTyId result;
    uint32_t index;
    if (expr == NULL) return arena->unit;
    if (env->in_pending_pass && env->out->expr_types[id] != CM_TY_NONE
        && expr->kind != CM_U_EXPR_METHOD_CALL && expr->kind != CM_U_EXPR_FIELD
        && expr->kind != CM_U_EXPR_TUPLE_FIELD && expr->kind != CM_U_EXPR_INDEX
        && expr->kind != CM_U_EXPR_PATH && expr->kind != CM_U_EXPR_CALL
        && expr->kind != CM_U_EXPR_UNARY)
        return env->out->expr_types[id];
    switch (expr->kind) {
    case CM_U_EXPR_LITERAL:
        result = cm_tyck_literal(env, expr, expected);
        break;
    case CM_U_EXPR_PATH:
        result = cm_tyck_path_type(env, expr, id);
        break;
    case CM_U_EXPR_QUALIFIED_PATH: {
        /* `<T as Trait>::item` and `<T>::item`. */
        CmTyId self_type = expr->data.qualified_path.self_type.type
                == CM_AST_TYPE_NONE ? arena->error
            : cm_tyck_ast_type(env, expr->data.qualified_path.self_type.type);
        const CmAstPath *associated = cm_ast_get_path(env->ast,
            expr->data.qualified_path.associated_path.path);
        CmInternId name = CM_INTERN_ID_NONE;
        CmTyckFound found;
        result = arena->error;
        if (associated != NULL && associated->segment_count != 0u) {
            const CmInternedString *text = cm_ast_get_string(env->ast,
                associated->segments[associated->segment_count - 1u].name);
            /* Intern (not lookup): the associated name need not appear
             * anywhere else in a body.  The interner is append-only, so
             * this cast is safe. */
            if (text != NULL)
                name = cm_interner_intern(
                    (CmInterner *)&env->state->ubodies->strings,
                    text->bytes, text->len);
        }
        if (name != CM_INTERN_ID_NONE
            && cm_tyck_lookup_assoc(env, self_type, name, &found)) {
            if (found.item->kind == CM_HIR_ITEM_FUNCTION)
                result = cm_ty_with_def(arena, CM_TY_FN_DEF,
                    found.item->definition, found.instance.types,
                    found.instance.count);
            else if (found.item->kind == CM_HIR_ITEM_CONST)
                result = cm_ty_subst(arena, cm_ty_from_hir(arena,
                    env->state->hir, found.item->data.value_item.type),
                    cm_tyck_subst_of(&found.instance));
            else
                cm_tyck_error(env, "qualified path is not a value");
        } else if (cm_ty_get(arena, cm_ty_resolve(arena, self_type))->kind
                == CM_TY_INFER) {
            cm_tyck_push_pending(env, id);
            result = cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
        } else {
            cm_tyck_debug_pair(env, "qualified path", self_type, self_type);
            cm_tyck_debug_span(env, expr);
            cm_tyck_error(env, "qualified path not resolved");
        }
        break;
    }
    case CM_U_EXPR_BLOCK: {
        CmTyId last = arena->unit;
        int diverges = 0;
        for (index = 0u; index < expr->data.block.statement_count; ++index) {
            const CmUStmt *stmt = cm_ubody_get_stmt(env->ub,
                expr->data.block.statements[index]);
            if (stmt == NULL) continue;
            if (stmt->kind == CM_U_STMT_LET) {
                CmTyId declared = stmt->data.let_stmt.type.type
                        == CM_AST_TYPE_NONE ? CM_TY_NONE
                    : cm_tyck_ast_type(env, stmt->data.let_stmt.type.type);
                CmTyId init = stmt->data.let_stmt.initializer == CM_U_EXPR_NONE
                    ? CM_TY_NONE
                    : cm_tyck_expr(env, stmt->data.let_stmt.initializer,
                        declared);
                CmTyId pat_type = declared != CM_TY_NONE ? declared
                    : init != CM_TY_NONE ? init
                    : cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
                if (declared != CM_TY_NONE && init != CM_TY_NONE
                    && !cm_tyck_coerce(env, init, declared))
                    cm_tyck_error(env, "let initializer type mismatch");
                cm_tyck_pat(env, stmt->data.let_stmt.pattern, pat_type);
                if (stmt->data.let_stmt.else_block != CM_U_EXPR_NONE)
                    (void)cm_tyck_expr(env, stmt->data.let_stmt.else_block,
                        arena->never);
            } else if (stmt->kind == CM_U_STMT_EXPR) {
                CmTyId t = cm_tyck_expr(env, stmt->data.expr_stmt.expression,
                    stmt->data.expr_stmt.has_semicolon ? CM_TY_NONE
                    : (index + 1u == expr->data.block.statement_count
                        && expr->data.block.tail == CM_U_EXPR_NONE
                        ? expected : CM_TY_NONE));
                const CmTy *tt = cm_ty_get(arena, cm_ty_resolve(arena, t));
                if (tt != NULL && tt->kind == CM_TY_NEVER) diverges = 1;
                if (!stmt->data.expr_stmt.has_semicolon
                    && index + 1u == expr->data.block.statement_count
                    && expr->data.block.tail == CM_U_EXPR_NONE)
                    last = t;
            }
        }
        if (expr->data.block.tail != CM_U_EXPR_NONE)
            last = cm_tyck_expr(env, expr->data.block.tail, expected);
        else if (diverges && last == arena->unit)
            last = arena->never;
        result = last;
        break;
    }
    case CM_U_EXPR_CALL:
        result = cm_tyck_call(env, expr, id, expected);
        break;
    case CM_U_EXPR_METHOD_CALL:
        result = cm_tyck_method_call(env, expr, id);
        break;
    case CM_U_EXPR_FIELD: {
        CmTyId base = cm_tyck_expr(env, expr->data.field.base, CM_TY_NONE);
        unsigned int step;
        int found = 0;
        result = arena->error;
        for (step = 0u; step < 6u && !found; ++step) {
            CmTyId candidate = cm_tyck_autoderef(env, base, step);
            const CmTy *ct;
            candidate = cm_tyck_normalize(env, candidate, 0u);
            ct = cm_ty_get(arena, candidate);
            if (ct->kind == CM_TY_INFER) {
                cm_tyck_push_pending(env, id);
                result = cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
                found = 1;
                break;
            }
            if (ct->kind == CM_TY_PROJECTION || ct->kind == CM_TY_PARAM
                || ct->kind == CM_TY_SELF) {
                /* Field of an unresolved projection or parameter. */
                result = cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
                found = 1;
                break;
            }
            result = cm_tyck_field_type(env, candidate,
                expr->data.field.name, &found);
            if (cm_tyck_debug_fn_matches(env)) {
                const CmInternedString *fname = cm_interner_get(
                    &env->state->ubodies->strings,
                    expr->data.field.name);
                fprintf(stderr, "TYCK ftrace f=%.*s step=%u found=%d ",
                    fname == NULL ? 1 : (int)fname->len,
                    fname == NULL ? "?" : (const char *)fname->bytes,
                    step, found);
                cm_tyck_debug_pair(env, "field", candidate, result);
            }
            if (!found && ct->kind == CM_TY_ADT) {
                /* `ManuallyDrop<Waker>`-style wrappers: user Deref. */
                CmTyId derefed = cm_tyck_user_deref(env, candidate);
                if (derefed != CM_TY_NONE) {
                    result = cm_tyck_field_type(env, derefed,
                        expr->data.field.name, &found);
                    if (found) break;
                }
            }
            if (!found && ct->kind != CM_TY_REF && ct->kind != CM_TY_PTR)
                break;
        }
        if (!found) {
            cm_tyck_debug_pair(env, "field-of", base, base);
            cm_tyck_debug_span(env, expr);
            cm_tyck_error(env, "field not found");
        }
        break;
    }
    case CM_U_EXPR_TUPLE_FIELD: {
        CmTyId base = cm_tyck_expr(env, expr->data.tuple_field.base,
            CM_TY_NONE);
        unsigned int step;
        int found = 0;
        result = arena->error;
        for (step = 0u; step < 6u && !found; ++step) {
            CmTyId candidate = cm_tyck_autoderef(env, base, step);
            const CmTy *ct;
            candidate = cm_tyck_normalize(env, candidate, 0u);
            ct = cm_ty_get(arena, candidate);
            if (ct->kind == CM_TY_INFER) {
                cm_tyck_push_pending(env, id);
                result = cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
                found = 1;
                break;
            }
            if (ct->kind == CM_TY_PROJECTION || ct->kind == CM_TY_PARAM
                || ct->kind == CM_TY_SELF) {
                result = cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
                found = 1;
                break;
            }
            result = cm_tyck_tuple_field_type(env, candidate,
                expr->data.tuple_field.index, &found);
            if (!found && ct->kind != CM_TY_REF && ct->kind != CM_TY_PTR)
                break;
        }
        if (!found) {
            cm_tyck_debug_pair(env, "tuple-field-of", base, base);
            cm_tyck_debug_span(env, expr);
            cm_tyck_error(env, "tuple field not found");
        }
        break;
    }
    case CM_U_EXPR_INDEX: {
        CmTyId base = cm_tyck_expr(env, expr->data.index.base, CM_TY_NONE);
        CmTyId index_type = cm_tyck_expr(env, expr->data.index.index,
            CM_TY_NONE);
        unsigned int step;
        result = CM_TY_NONE;
        for (step = 0u; step < 6u; ++step) {
            CmTyId candidate = cm_tyck_autoderef(env, base, step);
            const CmTy *ct = cm_ty_get(arena, candidate);
            const CmTy *it = cm_ty_get(arena, cm_ty_resolve(arena,
                index_type));
            if (ct->kind == CM_TY_INFER) {
                cm_tyck_push_pending(env, id);
                result = cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
                break;
            }
            if ((ct->kind == CM_TY_ARRAY || ct->kind == CM_TY_SLICE)
                && it != NULL && (it->kind == CM_TY_INT
                    || (it->kind == CM_TY_INFER
                        && it->b == CM_HIR_INFER_INTEGER))) {
                (void)cm_ty_unify(arena, index_type, arena->usize);
                result = ct->children[0];
                break;
            }
            if (ct->kind == CM_TY_ARRAY || ct->kind == CM_TY_SLICE
                || ct->kind == CM_TY_STR) {
                /* Range indexing yields a slice/str. */
                result = ct->kind == CM_TY_STR ? arena->str
                    : cm_ty_slice(arena, ct->children[0]);
                break;
            }
            if (ct->kind != CM_TY_REF && ct->kind != CM_TY_PTR) {
                /* Index trait on ADTs: look up `index`. */
                CmTyckFound found;
                CmInternId method = cm_tyck_intern_text(env->state, "index", 5u);
                if (method != CM_INTERN_ID_NONE
                    && cm_tyck_lookup_assoc(env, candidate, method, &found)
                    && found.item->kind == CM_HIR_ITEM_FUNCTION) {
                    CmTyId params[CM_TYCK_MAX_ARGS];
                    CmTyId ret;
                    uint32_t count = cm_tyck_signature(env, found.item,
                        cm_tyck_subst_of(&found.instance), params, CM_TYCK_MAX_ARGS, &ret);
                    const CmTy *rt;
                    if (count >= 2u) (void)cm_tyck_coerce(env, index_type,
                        params[1]);
                    rt = cm_ty_get(arena, cm_ty_resolve(arena, ret));
                    result = rt != NULL && rt->kind == CM_TY_REF
                        ? rt->children[0] : ret;
                }
                break;
            }
        }
        if (result == CM_TY_NONE) {
            cm_tyck_error(env, "index on unsupported type");
            result = arena->error;
        }
        break;
    }
    case CM_U_EXPR_UNARY: {
        CmTyId operand = cm_tyck_expr(env, expr->data.unary.operand,
            expr->data.unary.op == CM_U_UNARY_DEREF ? CM_TY_NONE : expected);
        const CmTy *ot = cm_ty_get(arena, cm_ty_resolve(arena, operand));
        if (expr->data.unary.op == CM_U_UNARY_DEREF) {
            if (ot != NULL && (ot->kind == CM_TY_REF || ot->kind == CM_TY_PTR)) {
                result = ot->children[0];
            } else if (ot != NULL && ot->kind == CM_TY_INFER) {
                cm_tyck_push_pending(env, id);
                result = cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
            } else {
                /* Deref trait. */
                CmTyckFound found;
                CmInternId method = cm_tyck_intern_text(env->state, "deref", 5u);
                result = arena->error;
                if (method != CM_INTERN_ID_NONE && ot != NULL
                    && cm_tyck_lookup_assoc(env, operand, method, &found)
                    && found.item->kind == CM_HIR_ITEM_FUNCTION) {
                    CmTyId params[CM_TYCK_MAX_ARGS];
                    CmTyId ret;
                    const CmTy *rt;
                    (void)cm_tyck_signature(env, found.item,
                        cm_tyck_subst_of(&found.instance), params, CM_TYCK_MAX_ARGS, &ret);
                    rt = cm_ty_get(arena, cm_ty_resolve(arena, ret));
                    result = rt != NULL && rt->kind == CM_TY_REF
                        ? rt->children[0] : ret;
                } else if (ot->kind == CM_TY_PROJECTION
                        || ot->kind == CM_TY_PARAM
                        || ot->kind == CM_TY_SELF
                        || ot->kind == CM_TY_ERROR) {
                    /* Deref through an unknown Deref impl. */
                    result = cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
                } else if (ot->kind == CM_TY_INT || ot->kind == CM_TY_FLOAT
                        || ot->kind == CM_TY_BOOL
                        || ot->kind == CM_TY_CHAR) {
                    /* A `&`-pattern bound the reference away already. */
                    result = operand;
                } else {
                    cm_tyck_debug_pair(env, "deref", operand, operand);
                    cm_tyck_debug_span(env, expr);
                    cm_tyck_error(env, "deref of non-pointer");
                }
            }
        } else {
            result = operand;
            if (ot != NULL && ot->kind == CM_TY_ADT) {
                CmTyckFound found;
                CmInternId method = cm_tyck_intern_text(env->state,
                    expr->data.unary.op == CM_U_UNARY_NOT ? "not" : "neg", 3u);
                if (method != CM_INTERN_ID_NONE
                    && cm_tyck_lookup_assoc(env, operand, method, &found)
                    && found.item->kind == CM_HIR_ITEM_FUNCTION) {
                    CmTyId params[CM_TYCK_MAX_ARGS];
                    CmTyId ret;
                    (void)cm_tyck_signature(env, found.item,
                        cm_tyck_subst_of(&found.instance), params, CM_TYCK_MAX_ARGS, &ret);
                    result = ret;
                }
            }
        }
        break;
    }
    case CM_U_EXPR_REF: {
        const CmTy *et = expected == CM_TY_NONE ? NULL
            : cm_ty_get(arena, cm_ty_resolve(arena, expected));
        CmTyId inner_expected = et != NULL && (et->kind == CM_TY_REF
                || et->kind == CM_TY_PTR) ? et->children[0] : CM_TY_NONE;
        CmTyId operand = cm_tyck_expr(env, expr->data.ref.operand,
            inner_expected);
        result = expr->data.ref.is_raw
            ? cm_ty_ptr(arena, operand, expr->data.ref.is_mutable)
            : cm_ty_ref(arena, operand, expr->data.ref.is_mutable);
        break;
    }
    case CM_U_EXPR_BINARY:
        result = cm_tyck_binary(env, expr, expected);
        break;
    case CM_U_EXPR_ASSIGN:
    case CM_U_EXPR_ASSIGN_OP: {
        CmTyId target = cm_tyck_expr(env, expr->data.assign.target,
            CM_TY_NONE);
        CmTyId value = cm_tyck_expr(env, expr->data.assign.value,
            expr->kind == CM_U_EXPR_ASSIGN ? target : CM_TY_NONE);
        if (expr->kind == CM_U_EXPR_ASSIGN) {
            if (!cm_tyck_coerce(env, value, target)) {
                const CmUExpr *value_expr = cm_ubody_get_expr(env->ub,
                    expr->data.assign.value);
                if (getenv("CM_TYCK_DEBUG") != NULL && value_expr != NULL)
                    fprintf(stderr, "TYCK assign-value-kind=%d\n",
                        (int)value_expr->kind);
                cm_tyck_debug_pair(env, "assignment", value, target);
                cm_tyck_debug_pair(env, "assignment-normalized",
                    cm_tyck_normalize(env, value, 0u), target);
                cm_tyck_debug_span(env, expr);
                cm_tyck_error(env, "assignment type mismatch");
            }
        } else {
            const CmTy *tt = cm_ty_get(arena, cm_ty_resolve(arena, target));
            if (tt != NULL && (tt->kind == CM_TY_INT || tt->kind == CM_TY_FLOAT
                    || (tt->kind == CM_TY_INFER && tt->b
                        != CM_HIR_INFER_GENERAL))) {
                if (expr->data.assign.op != CM_U_BINARY_SHL
                    && expr->data.assign.op != CM_U_BINARY_SHR)
                    (void)cm_ty_unify(arena, target, value);
            }
        }
        result = arena->unit;
        break;
    }
    case CM_U_EXPR_CAST: {
        CmTyId target = cm_tyck_ast_type(env, expr->data.cast.type.type);
        (void)cm_tyck_expr(env, expr->data.cast.value, CM_TY_NONE);
        result = target;
        break;
    }
    case CM_U_EXPR_TRY: {
        /* `expr?`: Result<T, E> -> T, Option<T> -> T, lenient otherwise. */
        CmTyId operand = cm_tyck_expr(env, expr->data.try_expr.operand,
            CM_TY_NONE);
        const CmTy *ot = cm_ty_get(arena, cm_ty_resolve(arena, operand));
        if (ot != NULL && ot->kind == CM_TY_ADT && ot->count != 0u) {
            uint32_t first = 0u;
            while (first < ot->count && cm_ty_get(arena,
                    ot->children[first])->kind == CM_TY_LIFETIME) first += 1u;
            result = first < ot->count ? ot->children[first]
                : cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
        } else if (ot != NULL && ot->kind == CM_TY_INFER) {
            cm_tyck_push_pending(env, id);
            result = cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
        } else {
            result = cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
        }
        break;
    }
    case CM_U_EXPR_RANGE: {
        CmTyId start = expr->data.range.start == CM_U_EXPR_NONE ? CM_TY_NONE
            : cm_tyck_expr(env, expr->data.range.start, CM_TY_NONE);
        CmTyId end = expr->data.range.end == CM_U_EXPR_NONE ? CM_TY_NONE
            : cm_tyck_expr(env, expr->data.range.end, start);
        CmTyId element = start != CM_TY_NONE ? start
            : end != CM_TY_NONE ? end : CM_TY_NONE;
        CmHirDefId def;
        cm_tyck_resolve_ranges(env->state);
        if (start != CM_TY_NONE && end != CM_TY_NONE)
            (void)cm_ty_unify(arena, start, end);
        if (start == CM_TY_NONE && end == CM_TY_NONE)
            def = env->state->range_full;
        else if (start == CM_TY_NONE)
            def = expr->data.range.is_inclusive
                ? env->state->range_to_inclusive : env->state->range_to;
        else if (end == CM_TY_NONE)
            def = env->state->range_from;
        else
            def = expr->data.range.is_inclusive
                ? env->state->range_inclusive : env->state->range;
        if (cm_hir_def_id_is_none(def)) {
            cm_tyck_error(env, "range type not available");
            result = cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
        } else if (element == CM_TY_NONE) {
            result = cm_ty_with_def(arena, CM_TY_ADT, def, NULL, 0u);
        } else {
            result = cm_ty_with_def(arena, CM_TY_ADT, def, &element, 1u);
        }
        break;
    }
    case CM_U_EXPR_LET: {
        CmTyId init = cm_tyck_expr(env, expr->data.let_expr.initializer,
            CM_TY_NONE);
        cm_tyck_pat(env, expr->data.let_expr.pattern, init);
        result = arena->boolean;
        break;
    }
    case CM_U_EXPR_RETURN:
        if (expr->data.flow.value != CM_U_EXPR_NONE) {
            CmTyId value = cm_tyck_expr(env, expr->data.flow.value,
                env->return_type);
            if (!cm_tyck_coerce(env, value, env->return_type))
                cm_tyck_error(env, "return type mismatch");
        }
        result = arena->never;
        break;
    case CM_U_EXPR_BREAK: {
        size_t loop = env->loop_count;
        CmTyId value = CM_TY_NONE;
        if (expr->data.flow.value != CM_U_EXPR_NONE)
            value = cm_tyck_expr(env, expr->data.flow.value, CM_TY_NONE);
        while (loop != 0u) {
            loop -= 1u;
            if (expr->data.flow.label == CM_INTERN_ID_NONE
                || env->loops[loop].label == expr->data.flow.label) {
                if (value != CM_TY_NONE)
                    (void)cm_tyck_coerce(env, value, env->loops[loop].type);
                else if (env->loops[loop].is_loop)
                    (void)cm_ty_unify(arena, env->loops[loop].type,
                        arena->unit);
                break;
            }
        }
        result = arena->never;
        break;
    }
    case CM_U_EXPR_CONTINUE:
        result = arena->never;
        break;
    case CM_U_EXPR_IF: {
        CmTyId condition = cm_tyck_expr(env, expr->data.if_expr.condition,
            expr->data.if_expr.pattern == CM_U_PAT_NONE ? arena->boolean
            : CM_TY_NONE);
        CmTyId then_type;
        if (expr->data.if_expr.pattern != CM_U_PAT_NONE)
            cm_tyck_pat(env, expr->data.if_expr.pattern, condition);
        else
            (void)cm_tyck_coerce(env, condition, arena->boolean);
        then_type = cm_tyck_expr(env, expr->data.if_expr.then_expr,
            expr->data.if_expr.else_expr == CM_U_EXPR_NONE ? arena->unit
            : expected);
        if (expr->data.if_expr.else_expr != CM_U_EXPR_NONE) {
            CmTyId else_type = cm_tyck_expr(env, expr->data.if_expr.else_expr,
                expected != CM_TY_NONE ? expected : then_type);
            result = cm_tyck_join(env, then_type, else_type);
        } else {
            result = arena->unit;
        }
        break;
    }
    case CM_U_EXPR_MATCH: {
        CmTyId scrutinee = cm_tyck_expr(env, expr->data.match_expr.scrutinee,
            CM_TY_NONE);
        CmTyId joined = CM_TY_NONE;
        for (index = 0u; index < expr->data.match_expr.arm_count; ++index) {
            const CmUMatchArm *arm = &expr->data.match_expr.arms[index];
            CmTyId body;
            cm_tyck_pat(env, arm->pattern, scrutinee);
            if (arm->guard != CM_U_EXPR_NONE)
                (void)cm_tyck_expr(env, arm->guard, arena->boolean);
            body = cm_tyck_expr(env, arm->body,
                expected != CM_TY_NONE ? expected : joined);
            joined = joined == CM_TY_NONE ? body
                : cm_tyck_join(env, joined, body);
        }
        result = joined == CM_TY_NONE ? arena->never : joined;
        break;
    }
    case CM_U_EXPR_LOOP: {
        CmTyId loop_type = expected != CM_TY_NONE ? expected
            : cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
        if (env->loop_count < 64u) {
            env->loops[env->loop_count].label = expr->data.loop_expr.label;
            env->loops[env->loop_count].type = loop_type;
            env->loops[env->loop_count].is_loop = 1;
            env->loop_count += 1u;
        }
        (void)cm_tyck_expr(env, expr->data.loop_expr.body, arena->unit);
        if (env->loop_count != 0u) env->loop_count -= 1u;
        /* A loop with no break is `!`; leniently keep the variable. */
        result = loop_type;
        break;
    }
    case CM_U_EXPR_WHILE:
    case CM_U_EXPR_FOR: {
        if (env->loop_count < 64u) {
            env->loops[env->loop_count].label = expr->kind == CM_U_EXPR_WHILE
                ? expr->data.while_expr.label : expr->data.for_expr.label;
            env->loops[env->loop_count].type = arena->unit;
            env->loops[env->loop_count].is_loop = 0;
            env->loop_count += 1u;
        }
        if (expr->kind == CM_U_EXPR_WHILE) {
            CmTyId condition = cm_tyck_expr(env,
                expr->data.while_expr.condition,
                expr->data.while_expr.pattern == CM_U_PAT_NONE
                    ? arena->boolean : CM_TY_NONE);
            if (expr->data.while_expr.pattern != CM_U_PAT_NONE)
                cm_tyck_pat(env, expr->data.while_expr.pattern, condition);
            (void)cm_tyck_expr(env, expr->data.while_expr.body, arena->unit);
        } else {
            CmTyId iterable = cm_tyck_expr(env, expr->data.for_expr.iterable,
                CM_TY_NONE);
            /* Item type through IntoIterator/Iterator is resolved later;
             * arrays, slices and ranges get a direct answer. */
            const CmTy *it = cm_ty_get(arena, cm_ty_resolve(arena, iterable));
            CmTyId item_type = cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
            if (it != NULL && it->kind == CM_TY_REF) {
                const CmTy *pt = cm_ty_get(arena, cm_ty_resolve(arena,
                    it->children[0]));
                if (pt != NULL && (pt->kind == CM_TY_ARRAY
                        || pt->kind == CM_TY_SLICE))
                    (void)cm_ty_unify(arena, item_type,
                        cm_ty_ref(arena, pt->children[0], it->a != 0u));
                else if (!cm_tyck_iterator_item(env, iterable, item_type))
                    cm_tyck_error(env, "for loop iterable is not iterable");
            } else if (it != NULL && it->kind == CM_TY_ARRAY) {
                (void)cm_ty_unify(arena, item_type, it->children[0]);
            } else if (!cm_tyck_iterator_item(env, iterable, item_type)) {
                cm_tyck_error(env, "for loop iterable is not iterable");
            }
            cm_tyck_pat(env, expr->data.for_expr.pattern, item_type);
            (void)cm_tyck_expr(env, expr->data.for_expr.body, arena->unit);
        }
        if (env->loop_count != 0u) env->loop_count -= 1u;
        result = arena->unit;
        break;
    }
    case CM_U_EXPR_CLOSURE: {
        CmTyId ret = expr->data.closure.return_type.type == CM_AST_TYPE_NONE
            ? cm_ty_fresh(arena, CM_HIR_INFER_GENERAL)
            : cm_tyck_ast_type(env, expr->data.closure.return_type.type);
        for (index = 0u; index < expr->data.closure.parameter_count; ++index) {
            const CmUClosureParam *param = &expr->data.closure.parameters[index];
            CmTyId type = param->type.type == CM_AST_TYPE_NONE
                ? cm_ty_fresh(arena, CM_HIR_INFER_GENERAL)
                : cm_tyck_ast_type(env, param->type.type);
            cm_tyck_pat(env, param->pattern, type);
        }
        {
            CmTyId body = cm_tyck_expr(env, expr->data.closure.body, ret);
            (void)cm_tyck_coerce(env, body, ret);
        }
        result = cm_ty_closure(arena, (uint32_t)env->ub->hir_body, id);
        break;
    }
    case CM_U_EXPR_TUPLE: {
        CmTyId elements[CM_TYCK_MAX_ARGS];
        const CmTy *et = expected == CM_TY_NONE ? NULL
            : cm_ty_get(arena, cm_ty_resolve(arena, expected));
        uint32_t count = expr->data.list.element_count;
        if (count > CM_TYCK_MAX_ARGS) {
            cm_tyck_error(env, "tuple too wide");
            result = arena->error;
            break;
        }
        for (index = 0u; index < count; ++index)
            elements[index] = cm_tyck_expr(env,
                expr->data.list.elements[index],
                et != NULL && et->kind == CM_TY_TUPLE && index < et->count
                    ? et->children[index] : CM_TY_NONE);
        result = cm_ty_tuple(arena, elements, count);
        break;
    }
    case CM_U_EXPR_ARRAY: {
        const CmTy *et = expected == CM_TY_NONE ? NULL
            : cm_ty_get(arena, cm_ty_resolve(arena, expected));
        CmTyId element = et != NULL && et->kind == CM_TY_ARRAY
            ? et->children[0] : cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
        for (index = 0u; index < expr->data.list.element_count; ++index)
            (void)cm_tyck_coerce(env, cm_tyck_expr(env,
                expr->data.list.elements[index], element), element);
        result = cm_ty_array(arena, element,
            cm_ty_const_value(arena, expr->data.list.element_count, 0u));
        break;
    }
    case CM_U_EXPR_ARRAY_REPEAT: {
        const CmTy *et = expected == CM_TY_NONE ? NULL
            : cm_ty_get(arena, cm_ty_resolve(arena, expected));
        CmTyId element = cm_tyck_expr(env, expr->data.repeat.value,
            et != NULL && et->kind == CM_TY_ARRAY ? et->children[0]
            : CM_TY_NONE);
        (void)cm_tyck_expr(env, expr->data.repeat.length, arena->usize);
        result = cm_ty_array(arena, element, et != NULL
            && et->kind == CM_TY_ARRAY ? et->children[1]
            : arena->const_unknown);
        break;
    }
    case CM_U_EXPR_STRUCT:
        result = cm_tyck_struct_literal(env, expr);
        break;
    case CM_U_EXPR_ASM:
        result = arena->unit;
        break;
    case CM_U_EXPR_OFFSET_OF:
        result = arena->usize;
        break;
    case CM_U_EXPR_UNSUPPORTED:
    default:
        cm_tyck_error(env, "unsupported expression");
        result = arena->error;
        break;
    }
    if (expected != CM_TY_NONE && result != CM_TY_NONE) {
        const CmTy *rt = cm_ty_get(arena, cm_ty_resolve(arena, result));
        if (rt != NULL && rt->kind == CM_TY_INFER)
            (void)cm_ty_unify(arena, result, expected);
    }
    cm_tyck_set_expr(env, id, result);
    return result;
}

static void cm_tyck_pat(CmTyckEnv *env, CmUPatId id, CmTyId expected)
{
    CmTyArena *arena = env->state->arena;
    const CmUPat *pat = cm_ubody_get_pat(env->ub, id);
    uint32_t index;
    if (pat == NULL) return;
    if (expected == CM_TY_NONE) expected = cm_ty_fresh(arena,
        CM_HIR_INFER_GENERAL);
    env->out->pat_types[id] = expected;
    switch (pat->kind) {
    case CM_U_PAT_WILD:
    case CM_U_PAT_REST:
        break;
    case CM_U_PAT_BINDING: {
        CmTyId local_type = pat->data.binding.by_ref
            ? cm_ty_ref(arena, expected, pat->data.binding.is_mutable)
            : expected;
        if (pat->data.binding.local < env->ub->locals.len)
            env->out->local_types[pat->data.binding.local] = local_type;
        if (pat->data.binding.subpattern != CM_U_PAT_NONE)
            cm_tyck_pat(env, pat->data.binding.subpattern, expected);
        break;
    }
    case CM_U_PAT_LITERAL: {
        const CmInternedString *text = cm_interner_get(
            &env->state->ubodies->strings, pat->data.literal.text);
        CmTyId literal;
        if (text == NULL || text->len == 0u) break;
        if (text->bytes[0] == '"' || text->bytes[0] == 'r')
            literal = cm_ty_ref(arena, arena->str, 0);
        else if (text->bytes[0] == 'b' && text->len > 1u
            && text->bytes[1] == '\'')
            literal = arena->u8;
        else if (text->bytes[0] == 'b')
            literal = cm_ty_ref(arena, cm_ty_array(arena, arena->u8,
                arena->const_unknown), 0);
        else if (text->bytes[0] == '\'')
            literal = arena->character;
        else if ((text->len == 4u && memcmp(text->bytes, "true", 4u) == 0)
            || (text->len == 5u && memcmp(text->bytes, "false", 5u) == 0))
            literal = arena->boolean;
        else {
            size_t i;
            int is_float = 0;
            for (i = 0u; i < text->len; ++i)
                if (text->bytes[i] == '.' || ((text->bytes[i] == 'e'
                        || text->bytes[i] == 'E') && !(text->len > 1u
                        && text->bytes[1] == 'x'))) is_float = 1;
            literal = cm_ty_fresh(arena, is_float ? CM_HIR_INFER_FLOAT
                : CM_HIR_INFER_INTEGER);
        }
        {
            const CmTy *et = cm_ty_get(arena, cm_ty_resolve(arena, expected));
            if (et != NULL && et->kind == CM_TY_REF)
                (void)cm_ty_unify(arena, et->children[0], literal);
            else
                (void)cm_ty_unify(arena, expected, literal);
        }
        break;
    }
    case CM_U_PAT_PATH: {
        CmUExpr fake;
        CmTyId type;
        memset(&fake, 0, sizeof(fake));
        fake.kind = CM_U_EXPR_PATH;
        fake.data.path.resolution = pat->data.path.resolution;
        fake.data.path.segments = pat->data.path.segments;
        fake.data.path.segment_count = pat->data.path.segment_count;
        type = cm_tyck_path_type(env, &fake, CM_U_EXPR_NONE);
        {
            const CmTy *et = cm_ty_get(arena, cm_ty_resolve(arena, expected));
            if (et != NULL && et->kind == CM_TY_REF)
                (void)cm_ty_unify(arena, et->children[0], type);
            else if (!cm_ty_unify(arena, expected, type)) {
                cm_tyck_debug_pair(env, "path-pattern", type, expected);
                cm_tyck_error(env, "path pattern type mismatch");
            }
        }
        break;
    }
    case CM_U_PAT_TUPLE: {
        CmTyId elements[CM_TYCK_MAX_ARGS];
        uint32_t count = pat->data.list.pattern_count;
        const CmTy *et = cm_ty_get(arena, cm_ty_resolve(arena, expected));
        CmTyId inner = expected;
        if (et != NULL && et->kind == CM_TY_REF) {
            inner = et->children[0];
            et = cm_ty_get(arena, cm_ty_resolve(arena, inner));
        }
        if (pat->data.list.has_rest || count > CM_TYCK_MAX_ARGS) {
            /* `(a, .., z)`: only the known prefix/suffix are typed. */
            for (index = 0u; index < count; ++index)
                cm_tyck_pat(env, pat->data.list.patterns[index], CM_TY_NONE);
            break;
        }
        if (et != NULL && et->kind == CM_TY_TUPLE && et->count == count) {
            for (index = 0u; index < count; ++index)
                cm_tyck_pat(env, pat->data.list.patterns[index],
                    cm_ty_get(arena, cm_ty_resolve(arena, inner))
                        ->children[index]);
            break;
        }
        for (index = 0u; index < count; ++index) {
            elements[index] = cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
            cm_tyck_pat(env, pat->data.list.patterns[index], elements[index]);
        }
        (void)cm_ty_unify(arena, inner, cm_ty_tuple(arena, elements, count));
        break;
    }
    case CM_U_PAT_TUPLE_STRUCT:
    case CM_U_PAT_STRUCT: {
        CmTyckCtor ctor = cm_tyck_ctor_of(env, &pat->data.struct_pat.resolution);
        CmTyckInstance instance;
        CmTyId adt;
        const CmTy *et = cm_ty_get(arena, cm_ty_resolve(arena, expected));
        CmTyId inner = expected;
        if (et != NULL && et->kind == CM_TY_REF) inner = et->children[0];
        if (!ctor.valid
            && pat->data.struct_pat.resolution.kind == CM_U_RESOLVED_SELF_TYPE) {
            const CmTy *st = cm_ty_get(arena, cm_ty_resolve(arena,
                env->self_type));
            const CmHirItem *item = st == NULL || st->kind != CM_TY_ADT ? NULL
                : cm_tyck_item(env->state, st->def);
            if (item != NULL && item->kind == CM_HIR_ITEM_STRUCT) {
                ctor.adt = item;
                ctor.fields = item->data.aggregate_item.fields;
                ctor.field_count = item->data.aggregate_item.field_count;
                ctor.form = item->data.aggregate_item.form;
                ctor.valid = 1;
            }
        }
        if (!ctor.valid && pat->data.struct_pat.resolution.kind
                == CM_U_RESOLVED_TYPE_ASSOC) {
            /* `Enum::Variant(..)` via alias/import: find the variant name
             * from the AST path. */
            const CmHirItem *item = cm_tyck_item(env->state,
                pat->data.struct_pat.resolution.definition);
            const CmAstPath *path = cm_ast_get_path(env->ast,
                pat->data.struct_pat.ast.path);
            const CmInternedString *name = path == NULL
                    || path->segment_count == 0u ? NULL
                : cm_ast_get_string(env->ast,
                    path->segments[path->segment_count - 1u].name);
            uint32_t variant;
            if (item != NULL && item->kind == CM_HIR_ITEM_ENUM && name != NULL)
                for (variant = 0u; variant
                        < item->data.enum_item.variant_count; ++variant) {
                    const CmHirVariant *v =
                        &item->data.enum_item.variants[variant];
                    const CmInternedString *vn = cm_interner_get(
                        &env->state->hir->strings, v->name);
                    if (vn != NULL && vn->len == name->len
                        && memcmp(vn->bytes, name->bytes, vn->len) == 0) {
                        ctor.adt = item;
                        ctor.fields = v->fields;
                        ctor.field_count = v->field_count;
                        ctor.form = v->form;
                        ctor.valid = 1;
                        break;
                    }
                }
        }
        if (!ctor.valid) {
            cm_tyck_error(env, "struct pattern path is not a struct or variant");
            for (index = 0u; index < pat->data.struct_pat.pattern_count;
                    ++index)
                cm_tyck_pat(env, pat->data.struct_pat.patterns[index],
                    CM_TY_NONE);
            for (index = 0u; index < pat->data.struct_pat.field_count; ++index)
                cm_tyck_pat(env, pat->data.struct_pat.fields[index].pattern,
                    CM_TY_NONE);
            break;
        }
        adt = cm_tyck_adt_fresh(env, ctor.adt, &instance);
        (void)cm_ty_unify(arena, inner, adt);
        /* Re-derive the instance from the unified type so field types see
         * the scrutinee's arguments. */
        cm_tyck_instance_of_type(env, cm_ty_resolve(arena, adt), ctor.adt,
            NULL, CM_TY_NONE, &instance);
        if (pat->kind == CM_U_PAT_TUPLE_STRUCT) {
            uint32_t count = pat->data.struct_pat.pattern_count;
            uint32_t field = 0u;
            for (index = 0u; index < count; ++index) {
                const CmUPat *sub = cm_ubody_get_pat(env->ub,
                    pat->data.struct_pat.patterns[index]);
                if (sub != NULL && sub->kind == CM_U_PAT_REST) {
                    /* Skip to the suffix. */
                    field = ctor.field_count >= (count - index - 1u)
                        ? ctor.field_count - (count - index - 1u) : field;
                    cm_tyck_pat(env, pat->data.struct_pat.patterns[index],
                        CM_TY_NONE);
                    continue;
                }
                cm_tyck_pat(env, pat->data.struct_pat.patterns[index],
                    field < ctor.field_count
                        ? cm_ty_subst(arena, cm_ty_from_hir(arena,
                            env->state->hir, ctor.fields[field].type),
                            cm_tyck_subst_of(&instance))
                        : CM_TY_NONE);
                field += 1u;
            }
        } else {
            for (index = 0u; index < pat->data.struct_pat.field_count; ++index) {
                const CmUPatField *field = &pat->data.struct_pat.fields[index];
                uint32_t f;
                CmTyId type = CM_TY_NONE;
                for (f = 0u; f < ctor.field_count; ++f)
                    if (cm_tyck_name_is(env->state, ctor.fields[f].name,
                            field->name)) {
                        type = cm_ty_subst(arena, cm_ty_from_hir(arena,
                            env->state->hir, ctor.fields[f].type),
                            cm_tyck_subst_of(&instance));
                        break;
                    }
                if (type == CM_TY_NONE)
                    cm_tyck_error(env, "unknown field in struct pattern");
                cm_tyck_pat(env, field->pattern, type);
            }
        }
        break;
    }
    case CM_U_PAT_REF: {
        const CmTy *et = cm_ty_get(arena, cm_ty_resolve(arena, expected));
        CmTyId inner;
        if (et != NULL && et->kind == CM_TY_REF) {
            inner = et->children[0];
        } else {
            inner = cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
            (void)cm_ty_unify(arena, expected, cm_ty_ref(arena, inner,
                pat->data.ref.is_mutable));
        }
        cm_tyck_pat(env, pat->data.ref.pattern, inner);
        break;
    }
    case CM_U_PAT_SLICE: {
        const CmTy *et = cm_ty_get(arena, cm_ty_resolve(arena, expected));
        if (getenv("CM_TYCK_DEBUG") != NULL) {
            uint32_t debug_index;
            fprintf(stderr, "TYCK slice-pat count=%u has_rest=%d"
                " rest_index=%u kinds=",
                (unsigned)pat->data.list.pattern_count,
                (int)pat->data.list.has_rest,
                (unsigned)pat->data.list.rest_index);
            for (debug_index = 0u;
                    debug_index < pat->data.list.pattern_count;
                    ++debug_index) {
                const CmUPat *dsub = cm_ubody_get_pat(env->ub,
                    pat->data.list.patterns[debug_index]);
                fprintf(stderr, "%d", dsub == NULL ? -1 : (int)dsub->kind);
                if (dsub != NULL && dsub->kind == CM_U_PAT_BINDING) {
                    const CmUPat *dchild = cm_ubody_get_pat(env->ub,
                        dsub->data.binding.subpattern);
                    fprintf(stderr, "(sub=%u kind=%d)",
                        (unsigned)dsub->data.binding.subpattern,
                        dchild == NULL ? -1 : (int)dchild->kind);
                }
                fputc(44, stderr);
            }
            fputc(10, stderr);
            cm_tyck_debug_pair(env, "slice-pat-expected", expected,
                expected);
        }
        CmTyId element;
        int behind_ref = 0;
        int ref_mutable = 0;
        CmTyId rest_type;
        if (et != NULL && et->kind == CM_TY_REF) {
            behind_ref = 1;
            ref_mutable = (int)et->a;
            et = cm_ty_get(arena, cm_ty_resolve(arena, et->children[0]));
        }
        element = et != NULL && (et->kind == CM_TY_ARRAY
                || et->kind == CM_TY_SLICE) ? et->children[0]
            : cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
        /* `rest @ ..` binds the subslice, ref-wrapped by match
         * ergonomics when the scrutinee sat behind a reference. */
        rest_type = cm_ty_slice(arena, element);
        if (behind_ref)
            rest_type = cm_ty_ref(arena, rest_type, ref_mutable);
        for (index = 0u; index < pat->data.list.pattern_count; ++index) {
            const CmUPat *sub = cm_ubody_get_pat(env->ub,
                pat->data.list.patterns[index]);
            int is_rest = (pat->data.list.has_rest
                    && index == pat->data.list.rest_index)
                || (sub != NULL && (sub->kind == CM_U_PAT_REST
                    || (sub->kind == CM_U_PAT_BINDING
                        && cm_ubody_get_pat(env->ub,
                            sub->data.binding.subpattern) != NULL
                        && cm_ubody_get_pat(env->ub,
                            sub->data.binding.subpattern)->kind
                            == CM_U_PAT_REST)));
            cm_tyck_pat(env, pat->data.list.patterns[index],
                is_rest ? rest_type : element);
        }
        break;
    }
    case CM_U_PAT_OR:
        for (index = 0u; index < pat->data.list.pattern_count; ++index)
            cm_tyck_pat(env, pat->data.list.patterns[index], expected);
        break;
    case CM_U_PAT_RANGE:
        cm_tyck_pat(env, pat->data.range.start, expected);
        cm_tyck_pat(env, pat->data.range.end, expected);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Bodies                                                               */

static void cm_tyck_body(CmTyckState *state, CmHirBodyId body_id,
    const CmUBody *ub, CmTyckBody *out)
{
    CmTyArena *arena = state->arena;
    CmTyckEnv env;
    const CmHirBody *hir_body = cm_hir_get_body(state->hir, body_id);
    const CmHirItem *item;
    const CmHirFunctionSignature *signature = NULL;
    uint32_t index;
    size_t node;
    unsigned int round;

    memset(out, 0, sizeof(*out));
    out->body = body_id;
    if (ub == NULL || ub->status != CM_U_BODY_LOWERED || hir_body == NULL) {
        out->status = CM_TYCK_BODY_SKIPPED;
        return;
    }
    memset(&env, 0, sizeof(env));
    env.state = state;
    env.ub = ub;
    env.out = out;
    out->expr_types = (CmTyId *)cm_alloc_zeroed(ub->expressions.len + 1u,
        sizeof(CmTyId));
    out->pat_types = (CmTyId *)cm_alloc_zeroed(ub->patterns.len + 1u,
        sizeof(CmTyId));
    out->local_types = (CmTyId *)cm_alloc_zeroed(ub->locals.len + 1u,
        sizeof(CmTyId));
    item = cm_tyck_item(state, hir_body->origin.definition);
    env.item = item;
    env.parent = cm_tyck_parent_item(state, item);
    if (env.parent != NULL && env.parent->kind == CM_HIR_ITEM_IMPL)
        env.self_type = cm_ty_from_hir(arena, state->hir,
            env.parent->data.impl_item.self_type);
    else if (env.parent != NULL && env.parent->kind == CM_HIR_ITEM_TRAIT)
        env.self_type = cm_ty_with_def(arena, CM_TY_SELF,
            env.parent->definition, NULL, 0u);
    else
        env.self_type = arena->error;
    /* Select the graph bundle owning this body's crate (M9-03). */
    state->graph = state->local_graph;
    state->revision = state->local_revision;
    state->imports = state->local_imports;
    state->modules = state->local_modules;
    if (item != NULL && cm_hir_module_map_lookup_module(state->modules,
            state->graph, state->revision, state->hir, item->owner_module,
            &env.module) != CM_HIR_MODULE_MAP_OK) {
        size_t dependency_index;
        env.module = CM_MODULE_NONE;
        for (dependency_index = 0u;
             dependency_index < state->dependency_count;
             ++dependency_index) {
            const CmUBodyDependency *dependency =
                &state->dependencies[dependency_index];
            if (cm_hir_module_map_lookup_module(dependency->modules,
                    dependency->graph, dependency->revision, state->hir,
                    item->owner_module, &env.module)
                    == CM_HIR_MODULE_MAP_OK) {
                state->graph = dependency->graph;
                state->revision = dependency->revision;
                state->imports = dependency->imports;
                state->modules = dependency->modules;
                break;
            }
            env.module = CM_MODULE_NONE;
        }
    }
    if (env.module == CM_MODULE_NONE
        || !cm_module_graph_borrow_ast(state->graph, env.module, &env.ast)) {
        out->status = CM_TYCK_BODY_SKIPPED;
        return;
    }
    env.source = hir_body->source;
    for (index = 0u; index <= ub->locals.len; ++index)
        out->local_types[index] = cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);

    /* Parameters and return type from the HIR signature. */
    /* `Self` in a signature means the impl's self type (or the trait's
     * Self parameter); every signature type is substituted through it. */
    cm_tyck_instance_init(&env.self_subst, env.self_type);
    if (item != NULL && item->kind == CM_HIR_ITEM_FUNCTION) {
        uint32_t hir_index = 0u;
        signature = &item->data.function_item.signature;
        env.return_type = cm_ty_subst(arena, cm_ty_from_hir(arena,
            state->hir, signature->return_type), cm_tyck_subst_of(&env.self_subst));
        for (index = 0u; index < ub->parameter_count; ++index) {
            CmTyId type;
            if (index == 0u && signature->receiver != CM_HIR_RECEIVER_NONE
                && ub->parameter_count == signature->parameter_count + 1u) {
                /* Receiver not among the HIR parameters. */
                type = signature->receiver == CM_HIR_RECEIVER_VALUE
                    ? env.self_type
                    : cm_ty_ref(arena, env.self_type,
                        signature->receiver == CM_HIR_RECEIVER_REF_MUTABLE);
            } else if (hir_index < signature->parameter_count) {
                type = cm_ty_subst(arena, cm_ty_from_hir(arena, state->hir,
                    signature->parameters[hir_index].type),
                    cm_tyck_subst_of(&env.self_subst));
                hir_index += 1u;
            } else if (ub->parameter_types[index].type != CM_AST_TYPE_NONE) {
                type = cm_tyck_ast_type(&env, ub->parameter_types[index].type);
            } else {
                type = cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
            }
            cm_tyck_pat(&env, ub->parameters[index], type);
        }
    } else if (item != NULL && (item->kind == CM_HIR_ITEM_CONST
            || item->kind == CM_HIR_ITEM_STATIC)) {
        env.return_type = cm_ty_subst(arena, cm_ty_from_hir(arena,
            state->hir, item->data.value_item.type), cm_tyck_subst_of(&env.self_subst));
    } else {
        env.return_type = cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
    }
    out->return_type = env.return_type;

    {
        CmTyId root = cm_tyck_expr(&env, ub->root, env.return_type);
        if (!cm_tyck_coerce(&env, root, env.return_type)) {
            if (getenv("CM_TYCK_DEBUG") != NULL) {
                const CmTy *ra = cm_ty_get(arena, cm_ty_resolve(arena,
                    root));
                const CmTy *re = cm_ty_get(arena, cm_ty_resolve(arena,
                    env.return_type));
                fprintf(stderr, "TYCK sig-kinds a=%d e=%d ab=%lu eb=%lu"
                    " acount=%lu ecount=%lu adef=%lu:%lu edef=%lu:%lu\n",
                    ra == NULL ? -1 : (int)ra->kind,
                    re == NULL ? -1 : (int)re->kind,
                    ra == NULL ? 0ul : (unsigned long)ra->b,
                    re == NULL ? 0ul : (unsigned long)re->b,
                    ra == NULL ? 0ul : (unsigned long)ra->count,
                    re == NULL ? 0ul : (unsigned long)re->count,
                    ra == NULL ? 0ul : (unsigned long)ra->def.crate_id,
                    ra == NULL ? 0ul : (unsigned long)ra->def.index,
                    re == NULL ? 0ul : (unsigned long)re->def.crate_id,
                    re == NULL ? 0ul : (unsigned long)re->def.index);
            }
            cm_tyck_debug_pair(&env, "body signature", root,
                env.return_type);
            cm_tyck_debug_pair(&env, "sig-normalized",
                cm_tyck_normalize(&env, root, 0u),
                cm_tyck_normalize(&env, env.return_type, 0u));
            cm_tyck_debug_span(&env, cm_ubody_get_expr(ub, ub->root));
            cm_tyck_error(&env, "body type does not match its signature");
        }
    }
    /* Pending nodes: retry while something resolves. */
    env.in_pending_pass = 1;
    for (round = 0u; round < CM_TYCK_PENDING_ROUNDS
            && env.pending_count != 0u; ++round) {
        CmUExprId *list = env.pending;
        size_t count = env.pending_count;
        size_t before_errors = out->error_nodes;
        env.pending = NULL;
        env.pending_count = 0u;
        env.pending_capacity = 0u;
        for (node = 0u; node < count; ++node) {
            CmTyId previous = out->expr_types[list[node]];
            CmTyId now = cm_tyck_expr(&env, list[node], CM_TY_NONE);
            if (previous != CM_TY_NONE) (void)cm_ty_unify(arena, previous, now);
        }
        cm_free(list);
        (void)before_errors;
        if (round == 1u) cm_ty_apply_defaults(arena);
    }
    cm_free(env.pending);
    cm_ty_apply_defaults(arena);

    /* Census. */
    for (node = 1u; node <= ub->expressions.len; ++node) {
        CmTyId type = out->expr_types[node];
        const CmTy *ty;
        if (type == CM_TY_NONE) {
            out->unresolved_nodes += 1u;
            continue;
        }
        ty = cm_ty_get(arena, cm_ty_resolve(arena, type));
        if (ty == NULL) {
            out->unresolved_nodes += 1u;
            continue;
        }
        if (ty->kind == CM_TY_ERROR) continue; /* counted as error nodes */
        if (cm_ty_has_infer(arena, type)) out->unresolved_nodes += 1u;
    }
    out->status = out->unresolved_nodes == 0u && out->error_nodes == 0u
        ? CM_TYCK_BODY_TYPED : CM_TYCK_BODY_PARTIAL;
}

CmTyckResult cm_tyck_all(CmTyckSet *set, const CmHirContext *hir,
    const CmUBodySet *bodies, const CmModuleGraph *graph,
    CmModuleGraphRevision revision, const CmImportResolver *imports,
    const CmHirModuleMap *modules, const CmUBodyDependency *dependencies,
    size_t dependency_count)
{
    CmTyckState *state;
    CmTyckResult result;
    size_t index;
    memset(&result, 0, sizeof(result));
    if (set == NULL || hir == NULL || bodies == NULL) return result;
    state = (CmTyckState *)cm_alloc_zeroed(1u, sizeof(*state));
    state->set = set;
    state->arena = &set->arena;
    state->hir = hir;
    state->ubodies = bodies;
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
    cm_tyck_build_source_asts(state);
    cm_tyck_build_index(state);
    cm_vec_clear(&set->bodies);
    for (index = 0u; index < bodies->bodies.len; ++index) {
        const CmUBody *ub = (const CmUBody *)cm_vec_at_const(&bodies->bodies,
            index);
        CmTyckBody body;
        cm_tyck_body(state, (CmHirBodyId)(index + 1u), ub, &body);
        state->result.bodies += 1u;
        switch (body.status) {
        case CM_TYCK_BODY_TYPED: state->result.typed += 1u; break;
        case CM_TYCK_BODY_PARTIAL: state->result.partial += 1u; break;
        case CM_TYCK_BODY_SKIPPED:
        default: state->result.skipped += 1u; break;
        }
        state->result.expressions += ub == NULL ? 0u : ub->expressions.len;
        state->result.unresolved_nodes += body.unresolved_nodes;
        state->result.error_nodes += body.error_nodes;
        cm_vec_push(&set->bodies, &body);
    }
    result = state->result;
    cm_free(state->source_asts);
    cm_free(state->children);
    cm_free(state->impls);
    cm_free(state);
    return result;
}
