#include "cm/hir/ty.h"
#include "cm/alloc.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Arena                                                                */

static CmTyId cm_ty_intern(CmTyArena *arena, const CmTy *ty)
{
    unsigned char key[64 + 4u * 64u];
    size_t length = 0u;
    uint32_t index;
    int inserted = 0;
    CmTyId id;
    CmTyId *slot;
    CmTy copy;
    if (ty->count <= 64u) {
        memcpy(key + length, &ty->kind, sizeof(ty->kind));
        length += sizeof(ty->kind);
        memcpy(key + length, &ty->a, sizeof(ty->a));
        length += sizeof(ty->a);
        memcpy(key + length, &ty->b, sizeof(ty->b));
        length += sizeof(ty->b);
        memcpy(key + length, &ty->def, sizeof(ty->def));
        length += sizeof(ty->def);
        memcpy(key + length, &ty->def2, sizeof(ty->def2));
        length += sizeof(ty->def2);
        memcpy(key + length, &ty->lo, sizeof(ty->lo));
        length += sizeof(ty->lo);
        memcpy(key + length, &ty->hi, sizeof(ty->hi));
        length += sizeof(ty->hi);
        memcpy(key + length, &ty->count, sizeof(ty->count));
        length += sizeof(ty->count);
        for (index = 0u; index < ty->count; ++index) {
            memcpy(key + length, &ty->children[index], sizeof(CmTyId));
            length += sizeof(CmTyId);
        }
        slot = (CmTyId *)cm_map_get(&arena->intern, key, length);
        if (slot != NULL) return *slot;
    }
    /*
     * Each type owns its child array.  A single shared growable block would
     * invalidate every previously handed-out child pointer when it grew.
     */
    copy = *ty;
    if (ty->count != 0u) {
        copy.children = (CmTyId *)cm_alloc_zeroed(ty->count, sizeof(CmTyId));
        memcpy(copy.children, ty->children, ty->count * sizeof(CmTyId));
    } else {
        copy.children = NULL;
    }
    cm_vec_push(&arena->types, &copy);
    id = (CmTyId)arena->types.len;
    if (ty->count <= 64u)
        (void)cm_map_insert(&arena->intern, key, length, &id, &inserted);
    return id;
}

void cm_ty_arena_init(CmTyArena *arena)
{
    memset(arena, 0, sizeof(*arena));
    cm_vec_init(&arena->types, sizeof(CmTy));
    cm_vec_init(&arena->vars, sizeof(CmTyVar));
    cm_vec_init(&arena->undo, sizeof(CmTyUndoEntry));
    cm_map_init(&arena->intern, sizeof(CmTyId));
    arena->error = cm_ty_simple(arena, CM_TY_ERROR, 0u, 0u);
    arena->unit = cm_ty_tuple(arena, NULL, 0u);
    arena->never = cm_ty_simple(arena, CM_TY_NEVER, 0u, 0u);
    arena->boolean = cm_ty_simple(arena, CM_TY_BOOL, 0u, 0u);
    arena->character = cm_ty_simple(arena, CM_TY_CHAR, 0u, 0u);
    arena->str = cm_ty_simple(arena, CM_TY_STR, 0u, 0u);
    arena->usize = cm_ty_int(arena, CM_HIR_INT_USIZE);
    arena->isize = cm_ty_int(arena, CM_HIR_INT_ISIZE);
    arena->u8 = cm_ty_int(arena, CM_HIR_INT_U8);
    arena->i32 = cm_ty_int(arena, CM_HIR_INT_I32);
    arena->f64 = cm_ty_float(arena, CM_HIR_FLOAT_F64);
    arena->lifetime = cm_ty_simple(arena, CM_TY_LIFETIME, 0u, 0u);
    arena->const_unknown = cm_ty_simple(arena, CM_TY_CONST_UNKNOWN, 0u, 0u);
}

void cm_ty_arena_destroy(CmTyArena *arena)
{
    size_t index;
    for (index = 0u; index < arena->types.len; ++index) {
        CmTy *ty = (CmTy *)cm_vec_at(&arena->types, index);
        cm_free(ty->children);
    }
    cm_map_destroy(&arena->intern);
    cm_vec_destroy(&arena->vars);
    cm_vec_destroy(&arena->undo);
    cm_vec_destroy(&arena->types);
}

const CmTy *cm_ty_get(const CmTyArena *arena, CmTyId id)
{
    if (id == CM_TY_NONE || (size_t)id > arena->types.len) return NULL;
    return (const CmTy *)cm_vec_at_const(&arena->types, (size_t)id - 1u);
}

static CmTyId cm_ty_make(CmTyArena *arena, const CmTy *ty)
{
    return cm_ty_intern(arena, ty);
}

CmTyId cm_ty_simple(CmTyArena *arena, CmTyKind kind, uint32_t a, uint32_t b)
{
    CmTy ty;
    memset(&ty, 0, sizeof(ty));
    ty.kind = kind;
    ty.a = a;
    ty.b = b;
    return cm_ty_make(arena, &ty);
}

CmTyId cm_ty_int(CmTyArena *arena, CmHirIntType kind)
{
    return cm_ty_simple(arena, CM_TY_INT, (uint32_t)kind, 0u);
}

CmTyId cm_ty_float(CmTyArena *arena, CmHirFloatType kind)
{
    return cm_ty_simple(arena, CM_TY_FLOAT, (uint32_t)kind, 0u);
}

static CmTyId cm_ty_one_child(CmTyArena *arena, CmTyKind kind, CmTyId child,
    uint32_t a)
{
    CmTy ty;
    CmTyId children[1];
    memset(&ty, 0, sizeof(ty));
    ty.kind = kind;
    ty.a = a;
    children[0] = child;
    ty.children = children;
    ty.count = 1u;
    return cm_ty_make(arena, &ty);
}

CmTyId cm_ty_ref(CmTyArena *arena, CmTyId pointee, int mutable)
{
    return cm_ty_one_child(arena, CM_TY_REF, pointee, mutable ? 1u : 0u);
}

CmTyId cm_ty_ptr(CmTyArena *arena, CmTyId pointee, int mutable)
{
    return cm_ty_one_child(arena, CM_TY_PTR, pointee, mutable ? 1u : 0u);
}

CmTyId cm_ty_slice(CmTyArena *arena, CmTyId element)
{
    return cm_ty_one_child(arena, CM_TY_SLICE, element, 0u);
}

CmTyId cm_ty_tuple(CmTyArena *arena, const CmTyId *elements, uint32_t count)
{
    CmTy ty;
    memset(&ty, 0, sizeof(ty));
    ty.kind = CM_TY_TUPLE;
    ty.children = (CmTyId *)elements;
    ty.count = count;
    return cm_ty_make(arena, &ty);
}

CmTyId cm_ty_array(CmTyArena *arena, CmTyId element, CmTyId length)
{
    CmTy ty;
    CmTyId children[2];
    memset(&ty, 0, sizeof(ty));
    ty.kind = CM_TY_ARRAY;
    children[0] = element;
    children[1] = length;
    ty.children = children;
    ty.count = 2u;
    return cm_ty_make(arena, &ty);
}

CmTyId cm_ty_fn_ptr(CmTyArena *arena, const CmTyId *params, uint32_t count,
    CmTyId return_type, int is_unsafe)
{
    CmTy ty;
    CmTyId *children = (CmTyId *)cm_alloc_zeroed(count + 1u, sizeof(CmTyId));
    CmTyId id;
    memset(&ty, 0, sizeof(ty));
    ty.kind = CM_TY_FN_PTR;
    ty.a = is_unsafe ? 1u : 0u;
    if (count != 0u) memcpy(children, params, count * sizeof(CmTyId));
    children[count] = return_type;
    ty.children = children;
    ty.count = count + 1u;
    id = cm_ty_make(arena, &ty);
    cm_free(children);
    return id;
}

CmTyId cm_ty_dyn(CmTyArena *arena, CmHirDefId principal,
    const CmTyId *args, uint32_t count, uint32_t principal_count,
    CmHirDefId assoc_def)
{
    CmTy ty;
    memset(&ty, 0, sizeof(ty));
    ty.kind = CM_TY_DYN;
    ty.def = principal;
    ty.def2 = assoc_def;
    ty.a = principal_count;
    ty.children = (CmTyId *)args;
    ty.count = count;
    return cm_ty_make(arena, &ty);
}

CmTyId cm_ty_with_def(CmTyArena *arena, CmTyKind kind, CmHirDefId def,
    const CmTyId *args, uint32_t count)
{
    CmTy ty;
    memset(&ty, 0, sizeof(ty));
    ty.kind = kind;
    ty.def = def;
    ty.children = (CmTyId *)args;
    ty.count = count;
    return cm_ty_make(arena, &ty);
}

CmTyId cm_ty_param(CmTyArena *arena, CmHirGenericParamId parameter)
{
    return cm_ty_simple(arena, CM_TY_PARAM, (uint32_t)parameter, 0u);
}

CmTyId cm_ty_const_value(CmTyArena *arena, uint64_t lo, uint64_t hi)
{
    CmTy ty;
    memset(&ty, 0, sizeof(ty));
    ty.kind = CM_TY_CONST;
    ty.lo = lo;
    ty.hi = hi;
    return cm_ty_make(arena, &ty);
}

CmTyId cm_ty_projection(CmTyArena *arena, CmTyId self, CmHirDefId trait,
    const CmTyId *trait_args, uint32_t trait_arg_count,
    CmHirDefId associated, const CmTyId *assoc_args,
    uint32_t assoc_arg_count)
{
    CmTy ty;
    CmTyId *children = (CmTyId *)cm_alloc_zeroed(
        trait_arg_count + assoc_arg_count + 1u, sizeof(CmTyId));
    CmTyId id;
    memset(&ty, 0, sizeof(ty));
    ty.kind = CM_TY_PROJECTION;
    ty.def = trait;
    ty.def2 = associated;
    ty.b = trait_arg_count;
    children[0] = self;
    if (trait_arg_count != 0u)
        memcpy(children + 1, trait_args, trait_arg_count * sizeof(CmTyId));
    if (assoc_arg_count != 0u)
        memcpy(children + 1 + trait_arg_count, assoc_args,
            assoc_arg_count * sizeof(CmTyId));
    ty.children = children;
    ty.count = trait_arg_count + assoc_arg_count + 1u;
    id = cm_ty_make(arena, &ty);
    cm_free(children);
    return id;
}

CmTyId cm_ty_closure(CmTyArena *arena, uint32_t body, uint32_t expression)
{
    return cm_ty_simple(arena, CM_TY_CLOSURE, body, expression);
}

CmTyId cm_ty_closure_with(CmTyArena *arena, uint32_t body,
    uint32_t expression, const CmTyId *children, uint32_t count)
{
    CmTy ty;
    memset(&ty, 0, sizeof(ty));
    ty.kind = CM_TY_CLOSURE;
    ty.a = body;
    ty.b = expression;
    ty.children = (CmTyId *)children;
    ty.count = count;
    return cm_ty_make(arena, &ty);
}

/* ------------------------------------------------------------------ */
/* Inference variables                                                  */

CmTyId cm_ty_fresh(CmTyArena *arena, CmHirInferenceKind kind)
{
    CmTyVar var;
    uint32_t index = (uint32_t)arena->vars.len;
    var.parent = index;
    var.binding = CM_TY_NONE;
    var.kind = kind;
    cm_vec_push(&arena->vars, &var);
    return cm_ty_simple(arena, CM_TY_INFER, index, (uint32_t)kind);
}

static uint32_t cm_ty_var_root(CmTyArena *arena, uint32_t index)
{
    CmTyVar *var = (CmTyVar *)cm_vec_at(&arena->vars, index);
    while (var->parent != index) {
        CmTyVar *parent = (CmTyVar *)cm_vec_at(&arena->vars, var->parent);
        var->parent = parent->parent; /* path halving */
        index = var->parent;
        var = (CmTyVar *)cm_vec_at(&arena->vars, index);
    }
    return index;
}

CmTyId cm_ty_resolve(CmTyArena *arena, CmTyId id)
{
    for (;;) {
        const CmTy *ty = cm_ty_get(arena, id);
        uint32_t root;
        CmTyVar *var;
        if (ty == NULL || ty->kind != CM_TY_INFER) return id;
        root = cm_ty_var_root(arena, ty->a);
        var = (CmTyVar *)cm_vec_at(&arena->vars, root);
        if (var->binding == CM_TY_NONE) {
            if (root == ty->a) return id;
            return cm_ty_simple(arena, CM_TY_INFER, root, (uint32_t)var->kind);
        }
        id = var->binding;
    }
}

static CmTyId cm_ty_resolve_deep_at(CmTyArena *arena, CmTyId id,
    unsigned int depth)
{
    const CmTy *ty;
    CmTy copy;
    CmTyId children[64];
    uint32_t index;
    int changed = 0;
    if (depth > 256u) return id;
    id = cm_ty_resolve(arena, id);
    ty = cm_ty_get(arena, id);
    if (ty == NULL || ty->count == 0u || ty->count > 64u) return id;
    copy = *ty;
    for (index = 0u; index < copy.count; ++index) {
        children[index] = cm_ty_resolve_deep_at(arena,
            cm_ty_get(arena, id)->children[index], depth + 1u);
        if (children[index] != cm_ty_get(arena, id)->children[index])
            changed = 1;
    }
    if (!changed) return id;
    copy.children = children;
    return cm_ty_make(arena, &copy);
}

CmTyId cm_ty_resolve_deep(CmTyArena *arena, CmTyId id)
{
    return cm_ty_resolve_deep_at(arena, id, 0u);
}

static int cm_ty_has_infer_at(CmTyArena *arena, CmTyId id, unsigned int depth)
{
    const CmTy *ty;
    uint32_t index;
    if (depth > 10000u) return 0;
    id = cm_ty_resolve(arena, id);
    ty = cm_ty_get(arena, id);
    if (ty == NULL) return 0;
    if (ty->kind == CM_TY_INFER) return 1;
    {
        uint32_t count = ty->count;
        for (index = 0u; index < count; ++index)
            if (cm_ty_has_infer_at(arena,
                    cm_ty_get(arena, id)->children[index], depth + 1u))
                return 1;
    }
    return 0;
}

int cm_ty_has_infer(CmTyArena *arena, CmTyId id)
{
    return cm_ty_has_infer_at(arena, id, 0u);
}

static int cm_ty_var_accepts(CmHirInferenceKind kind, const CmTy *ty)
{
    switch (kind) {
    case CM_HIR_INFER_INTEGER:
        return ty->kind == CM_TY_INT || ty->kind == CM_TY_ERROR
            || (ty->kind == CM_TY_INFER && ty->b != CM_HIR_INFER_FLOAT);
    case CM_HIR_INFER_FLOAT:
        return ty->kind == CM_TY_FLOAT || ty->kind == CM_TY_ERROR
            || (ty->kind == CM_TY_INFER && ty->b != CM_HIR_INFER_INTEGER);
    case CM_HIR_INFER_GENERAL:
    default:
        return 1;
    }
}

/* Does the (root of the) variable occur in `target`?  Prevents infinite
 * types like `T = &T` from being formed by unification. */
static int cm_ty_occurs(CmTyArena *arena, uint32_t root, CmTyId target,
    unsigned int depth)
{
    const CmTy *ty;
    uint32_t index;
    if (depth > 10000u) return 1;
    target = cm_ty_resolve(arena, target);
    ty = cm_ty_get(arena, target);
    if (ty == NULL) return 0;
    if (ty->kind == CM_TY_INFER) return cm_ty_var_root(arena, ty->a) == root;
    {
        uint32_t count = ty->count;
        for (index = 0u; index < count; ++index)
            if (cm_ty_occurs(arena, root,
                    cm_ty_get(arena, target)->children[index], depth + 1u))
                return 1;
    }
    return 0;
}

static void cm_ty_undo_log(CmTyArena *arena, uint32_t variable)
{
    const CmTyVar *var = (const CmTyVar *)cm_vec_at(&arena->vars, variable);
    CmTyUndoEntry entry;
    if (var == NULL) return;
    entry.variable = variable;
    entry.old_parent = var->parent;
    entry.old_binding = var->binding;
    entry.old_kind = var->kind;
    (void)cm_vec_push(&arena->undo, &entry);
}

size_t cm_ty_undo_mark(const CmTyArena *arena)
{
    return arena->undo.len;
}

void cm_ty_undo_to(CmTyArena *arena, size_t mark)
{
    while (arena->undo.len > mark) {
        CmTyUndoEntry entry;
        CmTyVar *var;
        if (!cm_vec_pop(&arena->undo, &entry)) break;
        var = (CmTyVar *)cm_vec_at(&arena->vars, entry.variable);
        if (var == NULL) continue;
        var->parent = entry.old_parent;
        var->binding = entry.old_binding;
        var->kind = entry.old_kind;
    }
}

static int cm_ty_bind_var(CmTyArena *arena, uint32_t variable, CmTyId target)
{
    uint32_t root = cm_ty_var_root(arena, variable);
    CmTyVar *var = (CmTyVar *)cm_vec_at(&arena->vars, root);
    const CmTy *ty = cm_ty_get(arena, target);
    if (ty == NULL) return 0;
    if (ty->kind == CM_TY_INFER) {
        uint32_t other = cm_ty_var_root(arena, ty->a);
        CmTyVar *other_var;
        if (other == root) return 1;
        other_var = (CmTyVar *)cm_vec_at(&arena->vars, other);
        if (!cm_ty_var_accepts(var->kind, ty)) return 0;
        /* Merge: the more specific kind wins. */
        cm_ty_undo_log(arena, root);
        cm_ty_undo_log(arena, other);
        if (var->kind == CM_HIR_INFER_GENERAL) {
            var->parent = other;
        } else {
            other_var->parent = root;
            if (other_var->kind != CM_HIR_INFER_GENERAL)
                var->kind = other_var->kind;
        }
        return 1;
    }
    if (!cm_ty_var_accepts(var->kind, ty)) return 0;
    if (cm_ty_occurs(arena, root, target, 0u)) {
        /* Lenient: an infinite type is treated as an error, not a failure. */
        cm_ty_undo_log(arena, root);
        var = (CmTyVar *)cm_vec_at(&arena->vars, root);
        var->binding = arena->error;
        return 1;
    }
    cm_ty_undo_log(arena, root);
    var = (CmTyVar *)cm_vec_at(&arena->vars, root);
    var->binding = target;
    return 1;
}

int cm_ty_unify(CmTyArena *arena, CmTyId left, CmTyId right)
{
    const CmTy *a;
    const CmTy *b;
    uint32_t index;
    left = cm_ty_resolve(arena, left);
    right = cm_ty_resolve(arena, right);
    if (left == right) return 1;
    a = cm_ty_get(arena, left);
    b = cm_ty_get(arena, right);
    if (a == NULL || b == NULL) return 0;
    if (a->kind == CM_TY_INFER) return cm_ty_bind_var(arena, a->a, right);
    if (b->kind == CM_TY_INFER) return cm_ty_bind_var(arena, b->a, left);
    if (a->kind == CM_TY_ERROR || b->kind == CM_TY_ERROR) return 1;
    if (a->kind == CM_TY_NEVER || b->kind == CM_TY_NEVER) return 1;
    if (a->kind == CM_TY_OPAQUE || b->kind == CM_TY_OPAQUE) return 1;
    if (a->kind == CM_TY_CONST_UNKNOWN || b->kind == CM_TY_CONST_UNKNOWN)
        return 1;
    if (a->kind == CM_TY_LIFETIME || b->kind == CM_TY_LIFETIME) return 1;
    if (a->kind != b->kind) return 0;
    if (a->a != b->a && a->kind != CM_TY_CLOSURE) {
        /* Reference mutability is checked; `&mut T` vs `&T` fails here and
         * callers coerce explicitly. */
        return 0;
    }
    if (!cm_hir_def_id_equal(a->def, b->def)
        || !cm_hir_def_id_equal(a->def2, b->def2)) return 0;
    if (a->lo != b->lo || a->hi != b->hi) return 0;
    if (a->count != b->count) return 0;
    {
        uint32_t count = a->count;
        for (index = 0u; index < count; ++index) {
            CmTyId ca = cm_ty_get(arena, left)->children[index];
            CmTyId cb = cm_ty_get(arena, right)->children[index];
            if (!cm_ty_unify(arena, ca, cb)) return 0;
        }
    }
    return 1;
}

void cm_ty_apply_defaults(CmTyArena *arena)
{
    size_t index;
    for (index = 0u; index < arena->vars.len; ++index) {
        uint32_t root = cm_ty_var_root(arena, (uint32_t)index);
        CmTyVar *var = (CmTyVar *)cm_vec_at(&arena->vars, root);
        if (var->binding != CM_TY_NONE) continue;
        if (var->kind == CM_HIR_INFER_INTEGER) var->binding = arena->i32;
        else if (var->kind == CM_HIR_INFER_FLOAT) var->binding = arena->f64;
    }
}

/* ------------------------------------------------------------------ */
/* Substitution                                                         */

static CmTyId cm_ty_subst_at(CmTyArena *arena, CmTyId id,
    const CmTySubst *subst, unsigned int depth)
{
    const CmTy *ty;
    CmTy copy;
    CmTyId children[64];
    uint32_t index;
    int changed = 0;
    if (depth > 256u) return id;
    id = cm_ty_resolve(arena, id);
    ty = cm_ty_get(arena, id);
    if (ty == NULL) return id;
    if (ty->kind == CM_TY_PARAM || ty->kind == CM_TY_CONST_PARAM) {
        for (index = 0u; index < subst->count; ++index)
            if (subst->parameters[index] == (CmHirGenericParamId)ty->a)
                return subst->types[index];
        return id;
    }
    if (ty->kind == CM_TY_SELF && subst->self_type != CM_TY_NONE)
        return subst->self_type;
    if (ty->count == 0u || ty->count > 64u) return id;
    copy = *ty;
    for (index = 0u; index < copy.count; ++index) {
        CmTyId child = cm_ty_get(arena, id)->children[index];
        children[index] = cm_ty_subst_at(arena, child, subst, depth + 1u);
        if (children[index] != child) changed = 1;
    }
    if (!changed) return id;
    copy.children = children;
    return cm_ty_make(arena, &copy);
}

CmTyId cm_ty_subst(CmTyArena *arena, CmTyId id, const CmTySubst *subst)
{
    return cm_ty_subst_at(arena, id, subst, 0u);
}

/* ------------------------------------------------------------------ */
/* HIR conversion                                                       */

static CmTyId cm_ty_const_from_hir(CmTyArena *arena, const CmHirConstArg *arg)
{
    switch (arg->kind) {
    case CM_HIR_CONST_VALUE:
        return cm_ty_const_value(arena, arg->data.value.low_bits,
            arg->data.value.high_bits);
    case CM_HIR_CONST_PARAMETER:
        return cm_ty_simple(arena, CM_TY_CONST_PARAM,
            (uint32_t)arg->data.parameter, 0u);
    case CM_HIR_CONST_UNEVALUATED:
    case CM_HIR_CONST_INFER:
    case CM_HIR_CONST_ERROR:
    default:
        return arena->const_unknown;
    }
}

uint32_t cm_ty_args_from_hir(CmTyArena *arena, const CmHirContext *hir,
    const CmHirGenericArg *args, uint32_t count, CmTyId *out, uint32_t limit)
{
    uint32_t index;
    if (count > limit) count = limit;
    for (index = 0u; index < count; ++index) {
        switch (args[index].kind) {
        case CM_HIR_GENERIC_ARG_LIFETIME:
            out[index] = arena->lifetime;
            break;
        case CM_HIR_GENERIC_ARG_TYPE:
            out[index] = cm_ty_from_hir(arena, hir, args[index].data.type);
            break;
        case CM_HIR_GENERIC_ARG_CONST:
        default:
            out[index] = cm_ty_const_from_hir(arena,
                &args[index].data.constant);
            break;
        }
    }
    return count;
}

CmTyId cm_ty_from_hir(CmTyArena *arena, const CmHirContext *hir,
    CmHirTypeId type)
{
    const CmHirType *ty = cm_hir_get_type(hir, type);
    CmTyId args[64];
    uint32_t count;
    if (ty == NULL) return arena->error;
    switch (ty->kind) {
    case CM_HIR_TYPE_ERROR_KIND:
        return arena->error;
    case CM_HIR_TYPE_INFER_KIND:
        return cm_ty_fresh(arena, ty->data.infer_type.kind);
    case CM_HIR_TYPE_NEVER_KIND:
        return arena->never;
    case CM_HIR_TYPE_UNIT_KIND:
        return arena->unit;
    case CM_HIR_TYPE_BOOL_KIND:
        return arena->boolean;
    case CM_HIR_TYPE_CHAR_KIND:
        return arena->character;
    case CM_HIR_TYPE_STR_KIND:
        return arena->str;
    case CM_HIR_TYPE_INTEGER_KIND:
        return cm_ty_int(arena, ty->data.integer_type.kind);
    case CM_HIR_TYPE_FLOAT_KIND:
        return cm_ty_float(arena, ty->data.float_type.kind);
    case CM_HIR_TYPE_REFERENCE_KIND:
        return cm_ty_ref(arena, cm_ty_from_hir(arena, hir,
            ty->data.reference_type.pointee),
            ty->data.reference_type.mutability == CM_HIR_MUTABLE);
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        return cm_ty_ptr(arena, cm_ty_from_hir(arena, hir,
            ty->data.raw_pointer_type.pointee),
            ty->data.raw_pointer_type.mutability == CM_HIR_MUTABLE);
    case CM_HIR_TYPE_TUPLE_KIND: {
        uint32_t index;
        uint32_t n = ty->data.tuple_type.element_count;
        if (n > 64u) return arena->error;
        for (index = 0u; index < n; ++index)
            args[index] = cm_ty_from_hir(arena, hir,
                cm_hir_get_type(hir, type)->data.tuple_type.elements[index]);
        return cm_ty_tuple(arena, args, n);
    }
    case CM_HIR_TYPE_ARRAY_KIND: {
        CmTyId element = cm_ty_from_hir(arena, hir,
            ty->data.array_type.element);
        CmTyId length = cm_ty_const_from_hir(arena,
            &cm_hir_get_type(hir, type)->data.array_type.length);
        return cm_ty_array(arena, element, length);
    }
    case CM_HIR_TYPE_SLICE_KIND:
        return cm_ty_slice(arena, cm_ty_from_hir(arena, hir,
            ty->data.slice_type.element));
    case CM_HIR_TYPE_FN_POINTER_KIND: {
        uint32_t index;
        uint32_t n = ty->data.fn_pointer_type.parameter_count;
        CmTyId ret;
        if (n > 63u) return arena->error;
        for (index = 0u; index < n; ++index)
            args[index] = cm_ty_from_hir(arena, hir, cm_hir_get_type(hir,
                type)->data.fn_pointer_type.parameters[index]);
        ret = cm_ty_from_hir(arena, hir,
            cm_hir_get_type(hir, type)->data.fn_pointer_type.return_type);
        return cm_ty_fn_ptr(arena, args, n, ret,
            cm_hir_get_type(hir, type)->data.fn_pointer_type.safety
                == CM_HIR_UNSAFE);
    }
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
    case CM_HIR_TYPE_ADT_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND: {
        CmTyKind kind = ty->kind == CM_HIR_TYPE_FN_DEFINITION_KIND
            ? CM_TY_FN_DEF : ty->kind == CM_HIR_TYPE_FOREIGN_KIND
            ? CM_TY_FOREIGN : CM_TY_ADT;
        CmHirDefId def = ty->data.named_type.definition;
        count = cm_ty_args_from_hir(arena, hir, ty->data.named_type.arguments,
            ty->data.named_type.argument_count, args, 64u);
        if (ty->kind == CM_HIR_TYPE_ALIAS_APPLICATION_KIND) {
            /* Expand the alias: substitute its generics into its target. */
            const CmHirDefinition *record = cm_hir_lookup_definition(hir,
                def);
            const CmHirItem *alias = record == NULL
                    || record->kind != CM_HIR_DEFINITION_ITEM ? NULL
                : cm_hir_get_item(hir, record->entity.item_id);
            if (alias != NULL && alias->kind == CM_HIR_ITEM_TYPE_ALIAS) {
                CmHirGenericParamId parameters[64];
                CmTySubst subst;
                uint32_t index;
                uint32_t n = alias->generic_parameter_count;
                if (n > 64u) n = 64u;
                for (index = 0u; index < n; ++index)
                    parameters[index] = alias->generic_parameter_start + index;
                subst.parameters = parameters;
                subst.types = args;
                subst.count = n < count ? n : count;
                subst.self_type = CM_TY_NONE;
                return cm_ty_subst(arena, cm_ty_from_hir(arena, hir,
                    alias->data.type_alias_item.target), &subst);
            }
            return arena->error;
        }
        return cm_ty_with_def(arena, kind, def, args, count);
    }
    case CM_HIR_TYPE_SELF_KIND:
        return cm_ty_with_def(arena, CM_TY_SELF, ty->data.self_type.owner,
            NULL, 0u);
    case CM_HIR_TYPE_PARAMETER_KIND:
        return cm_ty_param(arena, ty->data.parameter_type.parameter);
    case CM_HIR_TYPE_PROJECTION_KIND: {
        CmTyId self = cm_ty_from_hir(arena, hir,
            ty->data.projection_type.self_type);
        const CmHirType *again = cm_hir_get_type(hir, type);
        count = cm_ty_args_from_hir(arena, hir,
            again->data.projection_type.trait_type.arguments,
            again->data.projection_type.trait_type.argument_count, args, 32u);
        {
            CmTyId assoc_args[16];
            uint32_t assoc_count = cm_ty_args_from_hir(arena, hir,
                cm_hir_get_type(hir, type)->data.projection_type
                    .associated_type.arguments,
                cm_hir_get_type(hir, type)->data.projection_type
                    .associated_type.argument_count, assoc_args, 16u);
            return cm_ty_projection(arena, self,
                cm_hir_get_type(hir, type)->data.projection_type
                    .trait_type.definition, args, count,
                cm_hir_get_type(hir, type)->data.projection_type
                    .associated_type.definition, assoc_args, assoc_count);
        }
    }
    case CM_HIR_TYPE_DYN_TRAIT_KIND: {
        CmHirDefId def;
        CmHirDefId assoc_def;
        uint32_t principal_count;
        uint32_t equality;
        memset(&def, 0, sizeof(def));
        memset(&assoc_def, 0, sizeof(assoc_def));
        if (ty->data.dyn_trait_type.has_principal) {
            def = ty->data.dyn_trait_type.principal_trait.definition;
            count = cm_ty_args_from_hir(arena, hir,
                ty->data.dyn_trait_type.principal_trait.arguments,
                ty->data.dyn_trait_type.principal_trait.argument_count, args,
                64u);
        } else {
            count = 0u;
        }
        principal_count = count;
        /* `dyn FnMut(A) -> R` binds `Output = R`: the equalities' values
         * follow the principal's arguments (`a` = their start, `def2` =
         * the first bound associated type). */
        for (equality = 0u; equality < ty->data.dyn_trait_type.equality_count
                && count < 64u; ++equality) {
            const CmHirAssociatedTypeEquality *eq =
                &ty->data.dyn_trait_type.equalities[equality];
            CmTyId value = cm_ty_from_hir(arena, hir, eq->value);
            ty = cm_hir_get_type(hir, type);
            if (equality == 0u) assoc_def = eq->associated_type;
            args[count++] = value;
        }
        return cm_ty_dyn(arena, def, args, count, principal_count, assoc_def);
    }
    case CM_HIR_TYPE_OPAQUE_KIND:
        return cm_ty_fresh(arena, CM_HIR_INFER_GENERAL);
    case CM_HIR_TYPE_CLOSURE_KIND:
        return cm_ty_closure(arena, 0u, (uint32_t)ty->data.closure_type.closure);
    default:
        return arena->error;
    }
}

/* ------------------------------------------------------------------ */
/* Printing                                                             */

static void cm_ty_print_def(const CmHirContext *hir, CmHirDefId def,
    CmStrBuf *out)
{
    const CmHirDefinition *record = cm_hir_lookup_definition(hir, def);
    const CmHirItem *item = record == NULL
            || record->kind != CM_HIR_DEFINITION_ITEM ? NULL
        : cm_hir_get_item(hir, record->entity.item_id);
    const CmInternedString *name = item == NULL ? NULL
        : cm_interner_get(&hir->strings, item->name);
    char number[32];
    if (name != NULL) {
        cm_str_buf_append_n(out, (const char *)name->bytes, name->len);
        return;
    }
    snprintf(number, sizeof(number), "def#%lu:%lu",
        (unsigned long)def.crate_id, (unsigned long)def.index);
    cm_str_buf_append(out, number);
}

void cm_ty_print(CmTyArena *arena, const CmHirContext *hir, CmTyId id,
    CmStrBuf *out)
{
    static const char *const ints[] = { "i8", "i16", "i32", "i64", "i128",
        "isize", "u8", "u16", "u32", "u64", "u128", "usize" };
    static const char *const floats[] = { "f16", "f32", "f64", "f128" };
    const CmTy *ty;
    uint32_t index;
    char number[48];
    id = cm_ty_resolve(arena, id);
    ty = cm_ty_get(arena, id);
    if (ty == NULL) {
        cm_str_buf_append(out, "<none>");
        return;
    }
    switch (ty->kind) {
    case CM_TY_ERROR: cm_str_buf_append(out, "{error}"); return;
    case CM_TY_INFER:
        snprintf(number, sizeof(number), ty->b == CM_HIR_INFER_INTEGER
            ? "{int:%lu}" : ty->b == CM_HIR_INFER_FLOAT ? "{float:%lu}"
            : "?%lu", (unsigned long)ty->a);
        cm_str_buf_append(out, number);
        return;
    case CM_TY_NEVER: cm_str_buf_append(out, "!"); return;
    case CM_TY_BOOL: cm_str_buf_append(out, "bool"); return;
    case CM_TY_CHAR: cm_str_buf_append(out, "char"); return;
    case CM_TY_STR: cm_str_buf_append(out, "str"); return;
    case CM_TY_INT:
        cm_str_buf_append(out, ty->a < 12u ? ints[ty->a] : "int?");
        return;
    case CM_TY_FLOAT:
        cm_str_buf_append(out, ty->a < 4u ? floats[ty->a] : "float?");
        return;
    case CM_TY_REF:
        cm_str_buf_append(out, ty->a ? "&mut " : "&");
        cm_ty_print(arena, hir, cm_ty_get(arena, id)->children[0], out);
        return;
    case CM_TY_PTR:
        cm_str_buf_append(out, ty->a ? "*mut " : "*const ");
        cm_ty_print(arena, hir, cm_ty_get(arena, id)->children[0], out);
        return;
    case CM_TY_TUPLE: {
        uint32_t count = ty->count;
        cm_str_buf_append(out, "(");
        for (index = 0u; index < count; ++index) {
            if (index != 0u) cm_str_buf_append(out, ", ");
            cm_ty_print(arena, hir, cm_ty_get(arena, id)->children[index],
                out);
        }
        cm_str_buf_append(out, count == 1u ? ",)" : ")");
        return;
    }
    case CM_TY_ARRAY:
        cm_str_buf_append(out, "[");
        cm_ty_print(arena, hir, cm_ty_get(arena, id)->children[0], out);
        cm_str_buf_append(out, "; ");
        cm_ty_print(arena, hir, cm_ty_get(arena, id)->children[1], out);
        cm_str_buf_append(out, "]");
        return;
    case CM_TY_SLICE:
        cm_str_buf_append(out, "[");
        cm_ty_print(arena, hir, cm_ty_get(arena, id)->children[0], out);
        cm_str_buf_append(out, "]");
        return;
    case CM_TY_FN_PTR: {
        uint32_t count = ty->count;
        cm_str_buf_append(out, "fn(");
        for (index = 0u; index + 1u < count; ++index) {
            if (index != 0u) cm_str_buf_append(out, ", ");
            cm_ty_print(arena, hir, cm_ty_get(arena, id)->children[index],
                out);
        }
        cm_str_buf_append(out, ") -> ");
        cm_ty_print(arena, hir,
            cm_ty_get(arena, id)->children[count - 1u], out);
        return;
    }
    case CM_TY_FN_DEF:
    case CM_TY_ADT:
    case CM_TY_FOREIGN:
    case CM_TY_DYN:
        if (ty->kind == CM_TY_DYN) cm_str_buf_append(out, "dyn ");
        if (ty->kind == CM_TY_FN_DEF) cm_str_buf_append(out, "fn ");
        cm_ty_print_def(hir, ty->def, out);
        if (ty->count != 0u) {
            uint32_t count = ty->count;
            cm_str_buf_append(out, "<");
            for (index = 0u; index < count; ++index) {
                if (index != 0u) cm_str_buf_append(out, ", ");
                cm_ty_print(arena, hir, cm_ty_get(arena, id)->children[index],
                    out);
            }
            cm_str_buf_append(out, ">");
        }
        return;
    case CM_TY_PARAM:
    case CM_TY_CONST_PARAM: {
        const CmHirGenericParam *parameter = cm_hir_get_generic_param(hir,
            (CmHirGenericParamId)ty->a);
        const CmInternedString *name = parameter == NULL ? NULL
            : cm_interner_get(&hir->strings, parameter->name);
        if (name != NULL)
            cm_str_buf_append_n(out, (const char *)name->bytes, name->len);
        else
            cm_str_buf_append(out, "P?");
        return;
    }
    case CM_TY_SELF: cm_str_buf_append(out, "Self"); return;
    case CM_TY_PROJECTION:
        cm_str_buf_append(out, "<");
        cm_ty_print(arena, hir, cm_ty_get(arena, id)->children[0], out);
        cm_str_buf_append(out, " as ");
        cm_ty_print_def(hir, ty->def, out);
        cm_str_buf_append(out, ">::");
        cm_ty_print_def(hir, ty->def2, out);
        return;
    case CM_TY_CLOSURE:
        snprintf(number, sizeof(number), "{closure#%lu/%lu}",
            (unsigned long)ty->a, (unsigned long)ty->b);
        cm_str_buf_append(out, number);
        return;
    case CM_TY_OPAQUE: cm_str_buf_append(out, "impl ?"); return;
    case CM_TY_CONST:
        snprintf(number, sizeof(number), "%lu", (unsigned long)ty->lo);
        cm_str_buf_append(out, number);
        return;
    case CM_TY_CONST_UNKNOWN: cm_str_buf_append(out, "N?"); return;
    case CM_TY_LIFETIME: cm_str_buf_append(out, "'_"); return;
    default: cm_str_buf_append(out, "<?>"); return;
    }
}
