#ifndef CMRUSTC_CM_HIR_TY_H
#define CMRUSTC_CM_HIR_TY_H
#include "cm/buf.h"
#include "cm/hir/model.h"
#include "cm/map.h"
#include "cm/vec.h"
#include <stddef.h>
#include <stdint.h>

/*
 * Typeck types (M9-04).  A hash-consed arena of structural types mirroring
 * the HIR type kinds, plus inference variables with union-find bindings.
 * Lifetimes are carried positionally as a dummy kind so generic argument
 * lists line up with HIR generic parameter lists; they are never solved.
 * Everything is lenient: ERROR unifies with anything and NEVER is
 * compatible with anything.
 */

typedef uint32_t CmTyId;
#define CM_TY_NONE ((CmTyId)0)

typedef enum CmTyKind {
    CM_TY_ERROR = 0,
    CM_TY_INFER,        /* a = variable index, b = CmHirInferenceKind */
    CM_TY_NEVER,
    CM_TY_BOOL,
    CM_TY_CHAR,
    CM_TY_STR,
    CM_TY_INT,          /* a = CmHirIntType */
    CM_TY_FLOAT,        /* a = CmHirFloatType */
    CM_TY_REF,          /* children[0] pointee, a = mutable */
    CM_TY_PTR,          /* children[0] pointee, a = mutable */
    CM_TY_TUPLE,        /* children = elements; zero elements is unit */
    CM_TY_ARRAY,        /* children[0] element, children[1] length const */
    CM_TY_SLICE,        /* children[0] element */
    CM_TY_FN_PTR,       /* children = params..., last = return; a = unsafe */
    CM_TY_FN_DEF,       /* def, children = generic args */
    CM_TY_ADT,          /* def, children = generic args */
    CM_TY_PARAM,        /* a = CmHirGenericParamId */
    CM_TY_SELF,         /* def = owning trait/impl */
    CM_TY_PROJECTION,   /* children[0] self, def = trait, a = assoc def index,
                           children[1..] trait args; assoc_def in def2 */
    CM_TY_DYN,          /* def = principal trait (or none), children = args */
    CM_TY_CLOSURE,      /* a = body id, b = expression id */
    CM_TY_FOREIGN,      /* def */
    CM_TY_OPAQUE,       /* def; treated as an inference variable holder */
    CM_TY_CONST,        /* const generic argument: lo/hi value */
    CM_TY_CONST_PARAM,  /* a = CmHirGenericParamId */
    CM_TY_CONST_UNKNOWN,
    CM_TY_LIFETIME      /* positional placeholder */
} CmTyKind;

typedef struct CmTy {
    CmTyKind kind;
    uint32_t a;
    uint32_t b;
    CmHirDefId def;
    CmHirDefId def2;
    uint32_t count;
    CmTyId *children;
    uint64_t lo;
    uint64_t hi;
} CmTy;

typedef struct CmTyVar {
    uint32_t parent;   /* union-find parent (self when root) */
    CmTyId binding;    /* CM_TY_NONE while unbound */
    CmHirInferenceKind kind;
} CmTyVar;

typedef struct CmTyUndoEntry {
    uint32_t variable;
    uint32_t old_parent;
    CmTyId old_binding;
    CmHirInferenceKind old_kind;
} CmTyUndoEntry;

typedef struct CmTyArena {
    CmVec types;   /* CmTy */
    CmVec vars;    /* CmTyVar */
    CmVec undo;    /* CmTyUndoEntry: every var mutation, for rollback */
    CmMap intern;  /* key bytes -> CmTyId */
    CmTyId unit;
    CmTyId error;
    CmTyId never;
    CmTyId boolean;
    CmTyId character;
    CmTyId str;
    CmTyId usize;
    CmTyId isize;
    CmTyId u8;
    CmTyId i32;
    CmTyId f64;
    CmTyId lifetime;
    CmTyId const_unknown;
} CmTyArena;

void cm_ty_arena_init(CmTyArena *arena);
void cm_ty_arena_destroy(CmTyArena *arena);
const CmTy *cm_ty_get(const CmTyArena *arena, CmTyId id);

CmTyId cm_ty_simple(CmTyArena *arena, CmTyKind kind, uint32_t a,
    uint32_t b);
CmTyId cm_ty_int(CmTyArena *arena, CmHirIntType kind);
CmTyId cm_ty_float(CmTyArena *arena, CmHirFloatType kind);
CmTyId cm_ty_ref(CmTyArena *arena, CmTyId pointee, int mutable);
CmTyId cm_ty_ptr(CmTyArena *arena, CmTyId pointee, int mutable);
CmTyId cm_ty_tuple(CmTyArena *arena, const CmTyId *elements, uint32_t count);
CmTyId cm_ty_array(CmTyArena *arena, CmTyId element, CmTyId length);
CmTyId cm_ty_slice(CmTyArena *arena, CmTyId element);
CmTyId cm_ty_fn_ptr(CmTyArena *arena, const CmTyId *params, uint32_t count,
    CmTyId return_type, int is_unsafe);
CmTyId cm_ty_with_def(CmTyArena *arena, CmTyKind kind, CmHirDefId def,
    const CmTyId *args, uint32_t count);
CmTyId cm_ty_param(CmTyArena *arena, CmHirGenericParamId parameter);
CmTyId cm_ty_const_value(CmTyArena *arena, uint64_t lo, uint64_t hi);
/* children = [self, trait args..., associated-type args...]; the
 * trait-arg count is stored in `b` so GAT arguments are recoverable. */
CmTyId cm_ty_projection(CmTyArena *arena, CmTyId self, CmHirDefId trait,
    const CmTyId *trait_args, uint32_t trait_arg_count,
    CmHirDefId associated, const CmTyId *assoc_args,
    uint32_t assoc_arg_count);
CmTyId cm_ty_closure(CmTyArena *arena, uint32_t body, uint32_t expression);

/* Fresh inference variable of the given kind. */
CmTyId cm_ty_fresh(CmTyArena *arena, CmHirInferenceKind kind);
/* Shallow-resolve: follow bound variables to their binding. */
CmTyId cm_ty_resolve(CmTyArena *arena, CmTyId id);
/* Deep-resolve: rebuild with all bound variables replaced. */
CmTyId cm_ty_resolve_deep(CmTyArena *arena, CmTyId id);
/* Does a deep-resolved type still contain unbound variables? */
int cm_ty_has_infer(CmTyArena *arena, CmTyId id);
/* Structural unification with variable binding; 1 on success. */
int cm_ty_unify(CmTyArena *arena, CmTyId left, CmTyId right);
/* Speculative unification: mark, try, and roll back rejected bindings. */
size_t cm_ty_undo_mark(const CmTyArena *arena);
void cm_ty_undo_to(CmTyArena *arena, size_t mark);
/* Bind every unbound integer/float variable to i32/f64. */
void cm_ty_apply_defaults(CmTyArena *arena);

/* Generic substitution: parameter id -> type. */
typedef struct CmTySubst {
    const CmHirGenericParamId *parameters;
    const CmTyId *types;
    uint32_t count;
    CmTyId self_type; /* replaces CM_TY_SELF; NONE keeps it */
} CmTySubst;

CmTyId cm_ty_subst(CmTyArena *arena, CmTyId id, const CmTySubst *subst);

/* Convert a HIR type (and HIR generic args) into arena types. */
CmTyId cm_ty_from_hir(CmTyArena *arena, const CmHirContext *hir,
    CmHirTypeId type);
uint32_t cm_ty_args_from_hir(CmTyArena *arena, const CmHirContext *hir,
    const CmHirGenericArg *args, uint32_t count, CmTyId *out, uint32_t limit);

/* Debug rendering. */
void cm_ty_print(CmTyArena *arena, const CmHirContext *hir, CmTyId id,
    CmStrBuf *out);

#endif
