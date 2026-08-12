#ifndef CMRUSTC_CM_HIR_TYPECK_H
#define CMRUSTC_CM_HIR_TYPECK_H

#include "cm/hir/model.h"

/* Scratch type IDs are one-based and belong to exactly one typeck session. */
typedef uint32_t CmTypeckTypeId;

#define CM_TYPECK_TYPE_NONE ((CmTypeckTypeId)0u)

typedef enum CmTypeckStatus {
    CM_TYPECK_OK = 0,
    CM_TYPECK_INVALID_ARGUMENT,
    CM_TYPECK_INVALID_ID,
    CM_TYPECK_INVALID_SNAPSHOT,
    CM_TYPECK_UNSUPPORTED_HIR_TYPE,
    CM_TYPECK_UNSUPPORTED_CONSTANT,
    CM_TYPECK_KIND_CONFLICT,
    CM_TYPECK_TYPE_MISMATCH,
    CM_TYPECK_OCCURS_CHECK,
    CM_TYPECK_UNRESOLVED,
    CM_TYPECK_OVERFLOW,
    CM_TYPECK_HIR_FAILURE
} CmTypeckStatus;

typedef enum CmTypeckTypeKind {
    CM_TYPECK_TYPE_VARIABLE = 0,
    CM_TYPECK_TYPE_NEVER,
    CM_TYPECK_TYPE_UNIT,
    CM_TYPECK_TYPE_BOOL,
    CM_TYPECK_TYPE_CHAR,
    CM_TYPECK_TYPE_STR,
    CM_TYPECK_TYPE_INTEGER,
    CM_TYPECK_TYPE_FLOAT,
    CM_TYPECK_TYPE_REFERENCE,
    CM_TYPECK_TYPE_RAW_POINTER,
    CM_TYPECK_TYPE_TUPLE,
    CM_TYPECK_TYPE_ARRAY,
    CM_TYPECK_TYPE_SLICE,
    CM_TYPECK_TYPE_FN_POINTER,
    CM_TYPECK_TYPE_ADT,
    CM_TYPECK_TYPE_PARAMETER,
    CM_TYPECK_TYPE_PROJECTION
} CmTypeckTypeKind;

typedef struct CmTypeckConst {
    CmHirConstArgKind kind;
    CmTypeckTypeId type;
    union {
        struct {
            uint64_t low_bits;
            uint64_t high_bits;
        } value;
        CmHirGenericParamId parameter;
    } data;
} CmTypeckConst;

typedef struct CmTypeckGenericArg {
    CmHirGenericArgKind kind;
    union {
        CmHirRegion lifetime;
        CmTypeckTypeId type;
        CmTypeckConst constant;
    } data;
} CmTypeckGenericArg;

typedef struct CmTypeckNamedType {
    CmHirDefId definition;
    CmTypeckGenericArg *arguments;
    uint32_t argument_count;
} CmTypeckNamedType;

/*
 * One authenticated substitution environment. `arguments` is ordered by the
 * exact generic-parameter indices owned by `parameter_owner`. `Self` is a
 * separate binding because an associated item's parameters and its enclosing
 * trait/impl Self have different owners.
 */
typedef struct CmTypeckInstantiation {
    /* Authenticated owner of every scratch TypeId stored below. */
    const void *typeck_state;
    uint64_t typeck_lifetime_id;
    CmHirDefId parameter_owner;
    const CmTypeckGenericArg *arguments;
    uint32_t argument_count;
    CmHirDefId self_owner;
    CmTypeckTypeId self_type;
} CmTypeckInstantiation;

/*
 * Session-owned structural term. Variable terms are created only through
 * cm_typeck_new_variable. cm_typeck_add_type deeply copies every array.
 */
typedef struct CmTypeckType {
    CmTypeckTypeKind kind;
    CmSpan span;
    union {
        struct {
            uint32_t variable;
            CmHirInferenceKind class_kind;
        } variable;
        CmHirIntType integer_type;
        CmHirFloatType float_type;
        struct {
            CmHirRegion region;
            CmTypeckTypeId pointee;
            CmHirMutability mutability;
        } reference_type;
        struct {
            CmTypeckTypeId pointee;
            CmHirMutability mutability;
        } raw_pointer_type;
        struct {
            CmTypeckTypeId *elements;
            uint32_t element_count;
        } tuple_type;
        struct {
            CmTypeckTypeId element;
            CmTypeckConst length;
        } array_type;
        struct {
            CmTypeckTypeId element;
        } slice_type;
        struct {
            CmTypeckTypeId *parameters;
            uint32_t parameter_count;
            CmTypeckTypeId return_type;
            CmInternId abi;
            CmHirSafety safety;
            int is_variadic;
        } fn_pointer_type;
        CmTypeckNamedType named_type;
        struct {
            CmHirGenericParamId parameter;
        } parameter_type;
        struct {
            CmTypeckTypeId self_type;
            CmTypeckNamedType trait_type;
            CmTypeckNamedType associated_type;
        } projection_type;
    } data;
} CmTypeckType;

/*
 * Opaque, body/session-owned scratch state. An external rewind of the source
 * HIR context invalidates the session; destroy remains valid afterward.
 */
typedef struct CmTypeckContext {
    void *state;
} CmTypeckContext;

/* Stack-disciplined capability. Only the newest active snapshot may close. */
typedef struct CmTypeckSnapshot {
    const CmTypeckContext *owner;
    uint64_t lifetime_id;
    uint64_t snapshot_id;
    int active;
} CmTypeckSnapshot;

typedef struct CmTypeckFreezeResult {
    CmTypeckStatus status;
    CmHirTypeId type;
    CmHirStatus hir_status;
    size_t added_type_count;
} CmTypeckFreezeResult;

void cm_typeck_context_init(CmTypeckContext *context,
    const CmHirContext *hir);
/* Make this context reject later proof-relevant HIR mutation. */
void cm_typeck_context_track_hir_semantic_generation(
    CmTypeckContext *context);
void cm_typeck_context_destroy(CmTypeckContext *context);
/* Initialize an empty instantiation capability for this exact session. */
void cm_typeck_instantiation_init(const CmTypeckContext *context,
    CmTypeckInstantiation *instantiation);

CmTypeckStatus cm_typeck_snapshot(CmTypeckContext *context,
    CmTypeckSnapshot *out_snapshot);
CmTypeckStatus cm_typeck_rollback(CmTypeckContext *context,
    CmTypeckSnapshot *snapshot);
CmTypeckStatus cm_typeck_commit(CmTypeckContext *context,
    CmTypeckSnapshot *snapshot);

CmTypeckStatus cm_typeck_import_hir_type(CmTypeckContext *context,
    CmHirTypeId hir_type, CmTypeckTypeId *out_type);
/*
 * Import while replacing only parameters owned by `parameter_owner` and Self
 * owned by `self_owner`. Foreign parameters stay rigid. An unmatched Self is
 * rejected because the scratch model has no unauthenticated Self term.
 * Rejection is atomic. A successful named result is session-owned.
 */
CmTypeckStatus cm_typeck_instantiate_hir_type(CmTypeckContext *context,
    CmHirTypeId hir_type, const CmTypeckInstantiation *instantiation,
    CmTypeckTypeId *out_type);
CmTypeckStatus cm_typeck_instantiate_hir_named(CmTypeckContext *context,
    const CmHirNamedType *named,
    const CmTypeckInstantiation *instantiation,
    CmTypeckNamedType *out_named);
CmTypeckStatus cm_typeck_add_type(CmTypeckContext *context,
    const CmTypeckType *type, CmTypeckTypeId *out_type);
CmTypeckStatus cm_typeck_new_variable(CmTypeckContext *context,
    CmHirInferenceKind class_kind, CmSpan span,
    CmTypeckTypeId *out_type);

/* Rejection is atomic, including every alias/binding made recursively. */
CmTypeckStatus cm_typeck_unify(CmTypeckContext *context,
    CmTypeckTypeId left, CmTypeckTypeId right);

/* Returns the deterministic canonical root or its structural binding. */
CmTypeckStatus cm_typeck_resolve(const CmTypeckContext *context,
    CmTypeckTypeId type, CmTypeckTypeId *out_type);
const CmTypeckType *cm_typeck_get_type(const CmTypeckContext *context,
    CmTypeckTypeId type);
size_t cm_typeck_type_count(const CmTypeckContext *context);
/* Source identity for clients that combine scratch terms with HIR indexes. */
const CmHirContext *cm_typeck_hir_context(const CmTypeckContext *context);
/* Authenticate a nominal ADT definition, arity, and ordered argument kinds. */
int cm_typeck_adt_is_valid(const CmTypeckContext *context,
    const CmTypeckNamedType *adt);
/* Authenticate every owner, argument kind, term, and optional Self binding. */
int cm_typeck_instantiation_is_valid(const CmTypeckContext *context,
    const CmTypeckInstantiation *instantiation);

/*
 * Freeze one fully solved term into `hir`. The caller must hold an active HIR
 * context mark for the same HIR context used to initialize this session.
 * A private nested mark makes every failed freeze leave HIR unchanged.
 */
CmTypeckFreezeResult cm_typeck_freeze_hir_type(CmTypeckContext *context,
    CmTypeckTypeId type, CmHirContext *hir,
    CmHirContextMark *caller_mark);

const char *cm_typeck_status_name(CmTypeckStatus status);

#endif
